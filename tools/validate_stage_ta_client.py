#!/usr/bin/env python3
"""validate_stage_ta_client.py — offline proof that stage-client.mjs _buildFromTA
produces the engine's stage. Loads atlas/stages/STG0B_ta.json + STG0B_ta_tNN.png
EXACTLY as stage-client does (surrogate->texIndex, intensity->base, screen verts,
real control words) and software-rasterizes the opaque list. Output == the AFTER
client render. Diffed vs the engine-TA ground truth + a per-poly control-word table.
"""
import os, sys, json
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OUTDIR = os.path.join(REPO, "atlas", "stages")
GT = os.path.join(REPO, "_stage_gt")
W, H = 640, 480


def main():
    sid = (sys.argv[1] if len(sys.argv) > 1 else "0B").upper().zfill(2)
    data = json.load(open(os.path.join(OUTDIR, f"STG{sid}_ta.json")))
    # surr -> RGBA texture (client: _surrToTex[surr]=surr-1, _imgs[i] == textures[i])
    tex = {}
    for t in data["textures"]:
        if t["file"]:
            tex[t["surr"]] = np.asarray(Image.open(os.path.join(OUTDIR, t["file"])).convert("RGBA")).astype(np.float32) / 255.0
    addr2surr = {t["addr"]: t["surr"] for t in data["textures"]}
    fb = np.zeros((H, W, 3), np.float32)
    zb = np.full((H, W), -1e9, np.float32)
    for m in data["meshes"]:
        surr = addr2surr.get((m["tcw"] >> 0) & 0x1FFFFF, 0) if m["textured"] else 0
        t = tex.get(surr)
        for tri in m["tris"]:
            _raster(fb, zb, t, tri)
    img = (np.clip(fb, 0, 1) * 255).astype(np.uint8)
    out = os.path.join(GT, f"AFTER_ta_client_STG{sid}.png")
    Image.fromarray(img).save(out)
    print("wrote", out)

    # control-word diff vs the synthesized path, for a few polys
    print("\n--- per-poly control words: GROUNDED (engine TA) vs SYNTHESIZED (_build) ---")
    for m in data["meshes"][:5]:
        synth_pcw = (4 << 29) | ((1 if m["textured"] else 0) << 3) | (1 << 1)
        synth_tsp = (1 << 29) | (0 << 26) | (1 << 6)
        synth_isp = (6 << 29) | (0 << 27) | (0 << 26)
        print(f"  mesh surr={m['surr']:>2} tex={m['textured']}")
        print(f"    PCW  engine={m['pcw']:08x}  synth={synth_pcw:08x}  {'DIFF' if m['pcw']!=synth_pcw else 'ok'}")
        print(f"    ISP  engine={m['isp']:08x}  synth={synth_isp:08x}  {'DIFF' if m['isp']!=synth_isp else 'ok'}")
        print(f"    TSP  engine={m['tsp']:08x}  synth={synth_tsp:08x}  {'DIFF' if m['tsp']!=synth_tsp else 'ok'}")
        print(f"    TCW  engine={m['tcw']:08x}  (synth used a bare surrogate int, no fmt/addr)")


def _raster(fb, zb, tex, tri):
    a, b, c = tri
    pa, pb, pc = a["pos"], b["pos"], c["pos"]
    if max(pa[2], pb[2], pc[2]) <= 0:
        return
    xs = [pa[0], pb[0], pc[0]]; ys = [pa[1], pb[1], pc[1]]
    minx = max(0, int(min(xs))); maxx = min(W - 1, int(max(xs)))
    miny = max(0, int(min(ys))); maxy = min(H - 1, int(max(ys)))
    if minx > maxx or miny > maxy:
        return
    x0, y0 = pa[0], pa[1]; x1, y1 = pb[0], pb[1]; x2, y2 = pc[0], pc[1]
    den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
    if abs(den) < 1e-9:
        return
    uvs = [a["uv"], b["uv"], c["uv"]]
    # per-vertex BASE colour (the bake now carries the type-correct rgb; fall back to the
    # mono intensity for pre-fix atlases). Matches stage-client writeVtx + the shader's
    # ShadInstr=1 modulate (texture * base).
    def _rgb(v):
        if "rgb" in v:
            return [v["rgb"][0] / 255.0, v["rgb"][1] / 255.0, v["rgb"][2] / 255.0]
        i = v.get("i", 1)
        return [i, i, i]
    cols = [_rgb(a), _rgb(b), _rgb(c)]
    if tex is not None:
        th, tw = tex.shape[0], tex.shape[1]
    for py in range(miny, maxy + 1):
        for px in range(minx, maxx + 1):
            l0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den
            l1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den
            l2 = 1 - l0 - l1
            if l0 < -0.001 or l1 < -0.001 or l2 < -0.001:
                continue
            z = l0 * pa[2] + l1 * pb[2] + l2 * pc[2]
            if z <= zb[py, px]:
                continue
            br = l0 * cols[0][0] + l1 * cols[1][0] + l2 * cols[2][0]
            bg = l0 * cols[0][1] + l1 * cols[1][1] + l2 * cols[2][1]
            bb = l0 * cols[0][2] + l1 * cols[1][2] + l2 * cols[2][2]
            if tex is not None:
                u = l0 * uvs[0][0] + l1 * uvs[1][0] + l2 * uvs[2][0]
                v = l0 * uvs[0][1] + l1 * uvs[1][1] + l2 * uvs[2][1]
                tcol = tex[int((v % 1.0) * th) % th, int((u % 1.0) * tw) % tw]
                fb[py, px] = [tcol[0] * br, tcol[1] * bg, tcol[2] * bb]
            else:
                fb[py, px] = [br, bg, bb]
            zb[py, px] = z


if __name__ == "__main__":
    main()
