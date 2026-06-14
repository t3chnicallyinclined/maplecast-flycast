#!/usr/bin/env python3
"""render_stage_before.py — reproduce the OLD synthesized stage render (the green
untextured plane): the POL-rip STG0B.json, placed model 0 only, projected through
the engine camera M1·M2, textured with rip texIndex (the synth _build path). This is
the BEFORE for the grounding fix."""
import os, sys, struct, json
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
GT = os.path.join(REPO, "_stage_gt")
OUTDIR = os.path.join(REPO, "atlas", "stages")
W, H = 640, 480
RAM_BASE = 0x8C000000


def mat16(ram, addr):
    o = addr - RAM_BASE
    return [struct.unpack_from("<f", ram, o + 4 * i)[0] for i in range(16)]


def to44(m):
    A = np.zeros((4, 4))
    for col in range(4):
        for row in range(4):
            A[row, col] = m[col * 4 + row]
    return A


def main():
    sid = "0B"
    ram = open(os.path.join(GT, "ram.bin"), "rb").read()
    X = to44(mat16(ram, 0x8C2D6B18)) @ to44(mat16(ram, 0x8C2D6AD8))
    J = json.load(open(os.path.join(OUTDIR, f"STG{sid}.json")))
    tex = {}
    for t in J["textures"]:
        p = os.path.join(OUTDIR, t["file"])
        if os.path.exists(p):
            tex[t["index"]] = np.asarray(Image.open(p).convert("RGBA")).astype(np.float32) / 255.0
    fb = np.zeros((H, W, 3), np.float32)
    zb = np.full((H, W), -1e9, np.float32)
    for m in J["meshes"]:
        if not m.get("placed", True):
            continue
        t = tex.get(m["texIndex"]) if m["texIndex"] < len(J["textures"]) else None
        for tri in m["tris"]:
            sv = []
            for v in tri:
                p = v["pos"]
                fx = X[0, 0]*p[0]+X[0, 1]*p[1]+X[0, 2]*p[2]+X[0, 3]
                fy = X[1, 0]*p[0]+X[1, 1]*p[1]+X[1, 2]*p[2]+X[1, 3]
                fw = X[3, 0]*p[0]+X[3, 1]*p[1]+X[3, 2]*p[2]+X[3, 3]
                inv = 1.0/(fw if fw else 1e-6)
                sv.append((fx*inv, fy*inv, inv, v["uv"], v["col"]))
            _raster(fb, zb, t, sv)
    img = (np.clip(fb, 0, 1)*255).astype(np.uint8)
    out = os.path.join(GT, f"BEFORE_synth_client_STG{sid}.png")
    Image.fromarray(img).save(out)
    print("wrote", out)


def _raster(fb, zb, tex, sv):
    if max(v[2] for v in sv) <= 0:
        return
    xs = [v[0] for v in sv]; ys = [v[1] for v in sv]
    minx = max(0, int(min(xs))); maxx = min(W-1, int(max(xs)))
    miny = max(0, int(min(ys))); maxy = min(H-1, int(max(ys)))
    if minx > maxx or miny > maxy:
        return
    x0, y0 = sv[0][0], sv[0][1]; x1, y1 = sv[1][0], sv[1][1]; x2, y2 = sv[2][0], sv[2][1]
    den = (y1-y2)*(x0-x2)+(x2-x1)*(y0-y2)
    if abs(den) < 1e-9:
        return
    if tex is not None:
        th, tw = tex.shape[0], tex.shape[1]
    for py in range(miny, maxy+1):
        for px in range(minx, maxx+1):
            l0 = ((y1-y2)*(px-x2)+(x2-x1)*(py-y2))/den
            l1 = ((y2-y0)*(px-x2)+(x0-x2)*(py-y2))/den
            l2 = 1-l0-l1
            if l0 < -.001 or l1 < -.001 or l2 < -.001:
                continue
            z = l0*sv[0][2]+l1*sv[1][2]+l2*sv[2][2]
            if z <= zb[py, px]:
                continue
            if tex is not None:
                u = l0*sv[0][3][0]+l1*sv[1][3][0]+l2*sv[2][3][0]
                v = l0*sv[0][3][1]+l1*sv[1][3][1]+l2*sv[2][3][1]
                c = tex[int((v % 1.0)*th) % th, int((u % 1.0)*tw) % tw]
                fb[py, px] = [c[0], c[1], c[2]]
            else:
                col = sv[0][4]
                fb[py, px] = [col[0]/255, col[1]/255, col[2]/255]
            zb[py, px] = z


if __name__ == "__main__":
    main()
