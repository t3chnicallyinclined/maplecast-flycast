#!/usr/bin/env python3
"""stage-render-preview.py — STATIC validation harness for stage-render.mjs.

Replicates the EXACT projection + control-word semantics of
web/webgpu/stage-render.mjs (DEFAULT_CAM, _project, modulate-shading, 1/w depth,
alpha blend) using a tiny CPU rasterizer, so the stage adapter can be eyeballed
WITHOUT a browser/WebGPU. Renders <out>/STGxx.json + tex_NN.png to a 640x480 PNG.

This is a VALIDATION SCAFFOLD only — production rendering goes through
pvr2-renderer.mjs in the browser. Keep DEFAULT_CAM here in sync with the .mjs.

Usage:
  python web/webgpu/stage-render-preview.py _stage_out/STG00 stage_preview.png \
      [fovDeg eyeZ targetZ]
"""
import sys, json, math
import numpy as np
from PIL import Image

SCREEN_W, SCREEN_H = 640, 480

# MUST mirror DEFAULT_CAM in stage-render.mjs
CAM = dict(fovDeg=30.0, eyeX=0.0, eyeY=0.0, eyeZ=9000.0,
           targetX=0.0, targetY=0.0, targetZ=-2000.0, depthScale=1.0)


def view_basis(cam):
    fx, fy, fz = cam['targetX']-cam['eyeX'], cam['targetY']-cam['eyeY'], cam['targetZ']-cam['eyeZ']
    fl = math.hypot(fx, fy, fz) or 1.0; fx, fy, fz = fx/fl, fy/fl, fz/fl
    rx, ry, rz = fz, 0.0, -fx           # right = up(0,1,0) × forward
    rl = math.hypot(rx, ry, rz) or 1.0; rx, ry, rz = rx/rl, ry/rl, rz/rl
    ux = fy*rz - fz*ry; uy = fz*rx - fx*rz; uz = fx*ry - fy*rx   # up = forward × right
    tanHalf = math.tan(math.radians(cam['fovDeg'])/2) or 1.0
    return dict(rx=rx, ry=ry, rz=rz, ux=ux, uy=uy, uz=uz, fx=fx, fy=fy, fz=fz,
                ex=cam['eyeX'], ey=cam['eyeY'], ez=cam['eyeZ'],
                tanHalf=tanHalf, aspect=SCREEN_W/SCREEN_H, depthScale=cam['depthScale'])


def project(b, x, y, z):
    dx, dy, dz = x-b['ex'], y-b['ey'], z-b['ez']
    vx = dx*b['rx']+dy*b['ry']+dz*b['rz']
    vy = dx*b['ux']+dy*b['uy']+dz*b['uz']
    vw = max(dx*b['fx']+dy*b['fy']+dz*b['fz'], 1e-3)
    ndcX = vx/(vw*b['tanHalf']*b['aspect'])
    ndcY = vy/(vw*b['tanHalf'])
    sx = (ndcX*0.5+0.5)*SCREEN_W
    sy = (1-(ndcY*0.5+0.5))*SCREEN_H
    sz = max((1.0/vw)*b['depthScale'], 1e-9)
    return sx, sy, sz, vw


def load_tex(dirpath, i):
    nn = f"{i:02d}"
    try:
        im = Image.open(f"{dirpath}/tex_{nn}.png").convert('RGBA')
        return np.asarray(im, dtype=np.float32)/255.0
    except Exception:
        return None


def sample(tex, u, v):
    h, w = tex.shape[0], tex.shape[1]
    # repeat wrap (matches sampler addressMode 'repeat')
    ui = int((u - math.floor(u)) * w) % w
    vi = int((v - math.floor(v)) * h) % h
    return tex[vi, ui]


def main():
    dirpath = sys.argv[1] if len(sys.argv) > 1 else "_stage_out/STG00"
    out = sys.argv[2] if len(sys.argv) > 2 else "stage_preview.png"
    cam = dict(CAM)
    if len(sys.argv) > 3: cam['fovDeg'] = float(sys.argv[3])
    if len(sys.argv) > 4: cam['eyeZ'] = float(sys.argv[4])
    if len(sys.argv) > 5: cam['targetZ'] = float(sys.argv[5])

    sid = dirpath.rstrip('/').split('/')[-1]
    data = json.load(open(f"{dirpath}/{sid}.json"))
    b = view_basis(cam)
    texs = [load_tex(dirpath, i) for i in range(len(data['textureList']))]

    color = np.zeros((SCREEN_H, SCREEN_W, 3), dtype=np.float32)
    depth = np.full((SCREEN_H, SCREEN_W), -1.0, dtype=np.float32)  # bigger z = nearer

    tri_count = 0
    for model in data['models']:
        for mesh in model['meshes']:
            ti = mesh['textureIndex']
            tex = texs[ti] if (0 <= ti < len(texs)) else None
            is_op = mesh.get('isOpaque', False)
            alpha = mesh.get('alpha', 1.0)
            is_trans = (not is_op) or alpha < 0.999
            for p in mesh['polygons']:
                idx = p.get('indices')
                if not idx or len(idx) < 3:
                    continue
                vs = p['vertices']
                for t in range(0, len(idx)-2, 3):
                    tri = [vs[idx[t+k]] for k in range(3)]
                    sp = [project(b, *v['position']) for v in tri]
                    # skip tris with any vertex behind camera (vw tiny)
                    if any(s[3] <= 1.0 for s in sp):
                        continue
                    raster_tri(color, depth, sp, tri, tex, is_trans, alpha)
                    tri_count += 1

    img = Image.fromarray(np.clip(color*255, 0, 255).astype(np.uint8), 'RGB')
    img.save(out)
    cov = int((depth > -1.0).sum())
    print(f"{sid}: rasterized {tri_count} tris, coverage {cov}/{SCREEN_W*SCREEN_H} "
          f"({100*cov/(SCREEN_W*SCREEN_H):.0f}%), cam={cam}")
    print(f"wrote {out}")


def raster_tri(color, depth, sp, tri, tex, is_trans, alpha):
    (x0, y0, z0, _), (x1, y1, z1, _), (x2, y2, z2, _) = sp
    minx = max(0, int(math.floor(min(x0, x1, x2))))
    maxx = min(SCREEN_W-1, int(math.ceil(max(x0, x1, x2))))
    miny = max(0, int(math.floor(min(y0, y1, y2))))
    maxy = min(SCREEN_H-1, int(math.ceil(max(y0, y1, y2))))
    if minx > maxx or miny > maxy:
        return
    area = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0)
    if abs(area) < 1e-6:
        return
    inv = 1.0/area
    c0 = tri[0].get('colors') or [1, 1, 1, 1]
    c1 = tri[1].get('colors') or [1, 1, 1, 1]
    c2 = tri[2].get('colors') or [1, 1, 1, 1]
    uv0 = tri[0].get('uv') or [0, 0]; uv1 = tri[1].get('uv') or [0, 0]; uv2 = tri[2].get('uv') or [0, 0]
    for py in range(miny, maxy+1):
        for px in range(minx, maxx+1):
            fx, fy = px+0.5, py+0.5
            w0 = ((x1-fx)*(y2-fy) - (x2-fx)*(y1-fy))*inv
            w1 = ((x2-fx)*(y0-fy) - (x0-fx)*(y2-fy))*inv
            w2 = 1.0-w0-w1
            if w0 < 0 or w1 < 0 or w2 < 0:
                if w0 > 0 or w1 > 0 or w2 > 0:  # outside (sign-consistent test)
                    continue
            z = w0*z0 + w1*z1 + w2*z2
            if z <= depth[py, px]:
                continue
            r = w0*c0[0] + w1*c1[0] + w2*c2[0]
            g = w0*c0[1] + w1*c1[1] + w2*c2[1]
            bl = w0*c0[2] + w1*c1[2] + w2*c2[2]
            a = (w0*c0[3] + w1*c1[3] + w2*c2[3])
            if tex is not None:
                u = w0*uv0[0] + w1*uv1[0] + w2*uv2[0]
                v = w0*uv0[1] + w1*uv1[1] + w2*uv2[1]
                tc = sample(tex, u, v)
                # ShadInstr=1 modulate: texel * vertex color
                r, g, bl = r*tc[0], g*tc[1], bl*tc[2]
                a = a*tc[3]
            if is_trans:
                a = min(max(a*alpha, 0.0), 1.0)
                src = np.array([r, g, bl], dtype=np.float32)
                color[py, px] = src*a + color[py, px]*(1-a)
                # translucent: zwrite disabled (don't update depth)
            else:
                color[py, px] = [min(r, 1), min(g, 1), min(bl, 1)]
                depth[py, px] = z


if __name__ == "__main__":
    main()
