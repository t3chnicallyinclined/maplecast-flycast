#!/usr/bin/env python3
"""_probe_stage_world_render.py — validate the world-space TA bake by rasterizing the
client _buildFromTA geometry two ways and comparing to the engine ground-truth raster:

  BEFORE = the baked SCREEN coords (camera-static, what the old _buildFromTA used)
  AFTER  = the un-projected WORLD coords re-projected through the LIVE camera (M1.M2
           from _stage_gt/ram.bin) via the exact _projectEngine math in stage-client.mjs

If the world-space pipeline is correct, AFTER == BEFORE == engine truth (the live
camera IS the captured frame's camera here, so they must coincide). This is the
geometry gate for the prop-matrix fix (re_kb 26). Writes a side-by-side PNG.
"""
import os, sys, json, struct
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from decode_vram_tex import decode as decode_vram

GT = os.path.join(REPO, "_stage_gt")
W, H = 640, 480
RAM_BASE = 0x8C000000


def mat16(ram, addr):
    o = addr - RAM_BASE
    return [struct.unpack_from("<f", ram, o + 4 * i)[0] for i in range(16)]


def matmul_colmaj(X, Mnew):
    out = [0.0] * 16
    for col in range(4):
        for i in range(4):
            out[col * 4 + i] = (X[i] * Mnew[col * 4 + 0] + X[i + 4] * Mnew[col * 4 + 1]
                                + X[i + 8] * Mnew[col * 4 + 2] + X[i + 12] * Mnew[col * 4 + 3])
    return out


def proj_engine(X, x, y, z):
    fx = X[0]*x + X[4]*y + X[8]*z + X[12]
    fy = X[1]*x + X[5]*y + X[9]*z + X[13]
    fw = X[3]*x + X[7]*y + X[11]*z + X[15]
    inv = 1.0 / (fw if fw else 1e-6)
    return fx * inv, fy * inv, max(inv, 1e-9)


def raster(meshes, texmap, project):
    img = np.zeros((H, W, 3), np.uint8)
    zb = np.full((H, W), -1e9, np.float32)
    for m in meshes:
        tex = texmap.get(m["surr"])
        for tri in m["tris"]:
            P = []
            for v in tri:
                sx, sy, sz = project(v)
                P.append((sx, sy, sz, v["uv"][0], v["uv"][1], v.get("i", 1.0)))
            _fill(img, zb, P, tex)
    return img


def _fill(img, zb, P, tex):
    xs = [p[0] for p in P]; ys = [p[1] for p in P]
    if max(P[0][2], P[1][2], P[2][2]) <= 0:
        return
    minx = max(0, int(min(xs))); maxx = min(W - 1, int(max(xs)))
    miny = max(0, int(min(ys))); maxy = min(H - 1, int(max(ys)))
    if minx > maxx or miny > maxy:
        return
    (x0, y0, z0, u0, v0, i0) = P[0]; (x1, y1, z1, u1, v1, i1) = P[1]; (x2, y2, z2, u2, v2, i2) = P[2]
    d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
    if abs(d) < 1e-9:
        return
    th, tw = (tex.shape[0], tex.shape[1]) if tex is not None else (0, 0)
    for py in range(miny, maxy + 1):
        for px in range(minx, maxx + 1):
            a = ((y1 - y2) * (px + .5 - x2) + (x2 - x1) * (py + .5 - y2)) / d
            b = ((y2 - y0) * (px + .5 - x2) + (x0 - x2) * (py + .5 - y2)) / d
            c = 1 - a - b
            if a < -.001 or b < -.001 or c < -.001:
                continue
            z = a * z0 + b * z1 + c * z2
            if z <= zb[py, px]:
                continue
            zb[py, px] = z
            inten = a * i0 + b * i1 + c * i2
            if tex is not None and th:
                u = (a * u0 + b * u1 + c * u2) % 1.0
                vv = (a * v0 + b * v1 + c * v2) % 1.0
                col = tex[int(vv * (th - 1)), int(u * (tw - 1))][:3].astype(np.float32)
            else:
                col = np.array([180, 180, 180], np.float32)
            img[py, px] = np.clip(col * min(inten, 2.0), 0, 255).astype(np.uint8)


def main():
    sid = (sys.argv[1] if len(sys.argv) > 1 else "0B").upper().zfill(2)
    J = json.load(open(os.path.join(REPO, "atlas", "stages", f"STG{sid}_ta.json")))
    ram = open(os.path.join(GT, "ram.bin"), "rb").read()
    X = matmul_colmaj(mat16(ram, 0x8C2D6B18), mat16(ram, 0x8C2D6AD8))

    # decode the same VRAM textures the bake used, key by surr
    texmap = {}
    for t in J["textures"]:
        if not t.get("file"):
            continue
        try:
            im = decode_vram(t["addr"], t["w"], t["h"], t["fmt"], t["twiddled"])
            texmap[t["surr"]] = np.asarray(im.convert("RGB"))
        except Exception as e:
            print("  tex decode warn", e)

    before = raster(J["meshes"], texmap, lambda v: (v["pos"][0], v["pos"][1], v["pos"][2]))
    after = raster(J["meshes"], texmap, lambda v: proj_engine(X, *v["world"]))

    nb = int((before.sum(2) > 0).sum()); na = int((after.sum(2) > 0).sum())
    diff = int((np.abs(before.astype(int) - after.astype(int)).sum(2) > 12).sum())
    print(f"STG{sid}: BEFORE(screen) non-black px={nb}  AFTER(live-world) non-black px={na}")
    print(f"  per-pixel diff(>12/255) px={diff}  ({100*diff/(W*H):.3f}% of frame)")

    sbs = np.zeros((H, W * 2 + 8, 3), np.uint8)
    sbs[:, :W] = before
    sbs[:, W + 8:] = after
    out = os.path.join(GT, f"PROP_BEFORE_AFTER_STG{sid}.png")
    Image.fromarray(sbs).save(out)
    print("  wrote", out, "(left=baked screen, right=live-world reproject)")


if __name__ == "__main__":
    main()
