#!/usr/bin/env python3
"""render_engine_ta.py — GROUND-TRUTH stage raster: rasterize the engine's own TA
stream (_stage_gt/engine_ta.bin) directly, sampling each polygon's REAL texture
decoded from VRAM at its real TexAddr, with the engine's real UVs + base colour.
This is what the stage MUST look like (the reference the client is validated against).

Walks groups via parse_engine_ta.walk; for each textured group, decode its VRAM
texture (TexAddr/size/fmt from TCW+TSP), strip its verts into tris, rasterize with
UV + modulate(base colour). Opaque list only (ListType 0). 640x480."""
import os, sys, struct
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from parse_engine_ta import walk
from decode_vram_tex import decode as decode_vram

GT = os.path.join(REPO, "_stage_gt")
W, H = 640, 480


def tex_params(tsp, tcw):
    texU = 8 << ((tsp >> 3) & 7)
    texV = 8 << (tsp & 7)
    pf = (tcw >> 27) & 7
    addr = tcw & 0x1FFFFF
    return addr, texU, texV, pf


def strip_to_tris(verts):
    # TA strip: triangle i uses verts i,i+1,i+2, ended by eos flag.
    tris = []
    run = []
    for v in verts:
        run.append(v)
        if v["eos"]:
            for i in range(len(run) - 2):
                if i % 2 == 0:
                    tris.append((run[i], run[i + 1], run[i + 2]))
                else:
                    tris.append((run[i + 1], run[i], run[i + 2]))
            run = []
    if len(run) >= 3:
        for i in range(len(run) - 2):
            if i % 2 == 0:
                tris.append((run[i], run[i + 1], run[i + 2]))
            else:
                tris.append((run[i + 1], run[i], run[i + 2]))
    return tris


def main():
    ta = open(os.path.join(GT, "engine_ta.bin"), "rb").read()
    groups = walk(ta)
    fb = np.zeros((H, W, 3), np.float32)
    zb = np.full((H, W), -1e9, np.float32)
    texcache = {}
    for g in groups:
        if not g["bits"]["Texture"] or not g["verts"]:
            continue
        if g["bits"]["ListType"] != 0:        # opaque only
            continue
        addr, tu, tv, pf = tex_params(g["tsp"], g["tcw"])
        key = (addr, tu, tv, pf)
        if key not in texcache:
            try:
                texcache[key] = np.asarray(decode_vram(addr, tu, tv, pf, True)).astype(np.float32) / 255.0
            except Exception:
                texcache[key] = None
        tex = texcache[key]
        if tex is None:
            continue
        for a, b, c in strip_to_tris(g["verts"]):
            _raster(fb, zb, tex, a, b, c)
    img = (np.clip(fb, 0, 1) * 255).astype(np.uint8)
    out = os.path.join(GT, "GROUNDTRUTH_engine_ta_STG0B.png")
    Image.fromarray(img).save(out)
    print("wrote", out)


def _raster(fb, zb, tex, a, b, c):
    pts = [(a["x"], a["y"]), (b["x"], b["y"]), (c["x"], c["y"])]
    zs = [a["z"], b["z"], c["z"]]
    uvs = [(a["u"], a["v"]), (b["u"], b["v"]), (c["u"], c["v"])]
    # Per-vertex BASE colour (ShadInstr=1 modulate => out = texture * base). parse_engine_ta
    # now decodes the TYPE-CORRECT per-vertex RGBA into v['rgba'] (vt5/6 floating colour
    # from the 2nd 32B half, vt7/8 intensity). The OLD code read +0x18 'base' as a float
    # intensity for EVERY type — for the deck (vt5) that was ignore_1 (~0.01), making the
    # deck render black in the "ground truth". Use the real RGBA so the deck modulates
    # by its true dark-grey ramp.
    bases = []
    for v in (a, b, c):
        rgba = v.get("rgba")
        if rgba is not None:
            bases.append((rgba[0], rgba[1], rgba[2]))
        else:
            inten = struct.unpack("<f", struct.pack("<I", v["base"] & 0xFFFFFFFF))[0]
            if not (0.0 <= inten <= 4.0):
                inten = 1.0
            bases.append((inten, inten, inten))
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    if max(zs) <= 0:
        return
    minx = max(0, int(min(xs))); maxx = min(W - 1, int(max(xs)))
    miny = max(0, int(min(ys))); maxy = min(H - 1, int(max(ys)))
    if minx > maxx or miny > maxy:
        return
    x0, y0 = pts[0]; x1, y1 = pts[1]; x2, y2 = pts[2]
    den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
    if abs(den) < 1e-9:
        return
    th, tw = tex.shape[0], tex.shape[1]
    for py in range(miny, maxy + 1):
        for px in range(minx, maxx + 1):
            l0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den
            l1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den
            l2 = 1 - l0 - l1
            if l0 < -0.001 or l1 < -0.001 or l2 < -0.001:
                continue
            z = l0 * zs[0] + l1 * zs[1] + l2 * zs[2]
            if z <= zb[py, px]:
                continue
            u = l0 * uvs[0][0] + l1 * uvs[1][0] + l2 * uvs[2][0]
            v = l0 * uvs[0][1] + l1 * uvs[1][1] + l2 * uvs[2][1]
            tx = int((u % 1.0) * tw) % tw
            ty = int((v % 1.0) * th) % th
            tcol = tex[ty, tx]
            br = l0 * bases[0][0] + l1 * bases[1][0] + l2 * bases[2][0]
            bg = l0 * bases[0][1] + l1 * bases[1][1] + l2 * bases[2][1]
            bb = l0 * bases[0][2] + l1 * bases[1][2] + l2 * bases[2][2]
            fb[py, px] = [tcol[0] * br, tcol[1] * bg, tcol[2] * bb]
            zb[py, px] = z


if __name__ == "__main__":
    main()
