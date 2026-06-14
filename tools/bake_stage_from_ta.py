#!/usr/bin/env python3
"""bake_stage_from_ta.py — bake the engine's OWN stage TA (_stage_gt/engine_ta.bin)
into a client-loadable stage object whose control words and textures are GROUNDED in
the engine, not synthesized. Closes re_kb 26's open item ("synthesized stage PVR
control words vs a live STG00 TA").

For each opaque-list polygon group in the engine TA we emit:
  - the REAL PVR control words: pcw / isp / tsp / tcw(surrogate) exactly as the engine
    submitted them (parse_engine_ta.walk -> 32B/64B-correct TA walk)
  - the engine's final SCREEN-space verts (sx, sy, 1/w) + UVs + intensity, so the
    stage renders the fully-assembled carrier (all 83 models, incl the 82 props the
    POL rip cannot place without runtime matrices)
  - the texture decoded straight from VRAM at the group's real TexAddr (decode_vram_tex),
    so each mesh binds the texture the engine actually sampled

OUTPUT (gitignored, scp-only):
  atlas/stages/STG0B_ta.json  { mode:"engine_ta", screenW,screenH,
      textures:[{surr,w,h,fmt,file}], meshes:[{surr,pcw,isp,tsp, tris:[{pos:[sx,sy,invw],uv,inten}]}] }
  atlas/stages/STG0B_ta_tNN.png  — VRAM-decoded textures
plus a mirror into web/test-atlas/stages/.

The client (stage-client.mjs _buildFromTA) emits these straight into the PVR2Renderer
parsed shape — the control words come from here, NOT _build's synth path.
"""
import os, sys, struct, json
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from parse_engine_ta import walk
from decode_vram_tex import decode as decode_vram

GT = os.path.join(REPO, "_stage_gt")
OUTDIR = os.path.join(REPO, "atlas", "stages")
WEBOUT = os.path.join(REPO, "web", "test-atlas", "stages")
W, H = 640, 480


def tex_params(tsp, tcw):
    texU = 8 << ((tsp >> 3) & 7)
    texV = 8 << (tsp & 7)
    pf = (tcw >> 27) & 7
    twiddled = ((tcw >> 26) & 1) == 0          # ScanOrder 0 = twiddled
    addr = tcw & 0x1FFFFF
    return addr, texU, texV, pf, twiddled


def strip_to_tris(verts):
    tris, run = [], []
    for v in verts:
        run.append(v)
        if v["eos"]:
            for i in range(len(run) - 2):
                tris.append((run[i + 1], run[i], run[i + 2]) if i % 2 else (run[i], run[i + 1], run[i + 2]))
            run = []
    if len(run) >= 3:
        for i in range(len(run) - 2):
            tris.append((run[i + 1], run[i], run[i + 2]) if i % 2 else (run[i], run[i + 1], run[i + 2]))
    return tris


def inten_of(base):
    f = struct.unpack("<f", struct.pack("<I", base & 0xFFFFFFFF))[0]
    return f if 0.0 <= f <= 4.0 else 1.0


def main():
    sid = (sys.argv[1] if len(sys.argv) > 1 else "0B").upper().zfill(2)
    ta = open(os.path.join(GT, "engine_ta.bin"), "rb").read()
    groups = walk(ta)
    os.makedirs(OUTDIR, exist_ok=True)
    os.makedirs(WEBOUT, exist_ok=True)

    # 1) collect distinct (texkey) -> surrogate + decode each from VRAM
    tex_surr = {}                              # texkey -> surr(1-based)
    tex_meta = []
    for g in groups:
        if not g["bits"]["Texture"] or not g["verts"]:
            continue
        if g["bits"]["ListType"] != 0:
            continue
        addr, tu, tv, pf, tw = tex_params(g["tsp"], g["tcw"])
        key = (addr, tu, tv, pf, tw)
        if key not in tex_surr:
            surr = len(tex_surr) + 1
            tex_surr[key] = surr
            fn = f"STG{sid}_ta_t{surr:02d}.png"
            try:
                im = decode_vram(addr, tu, tv, pf, tw)
                im.save(os.path.join(OUTDIR, fn))
                im.save(os.path.join(WEBOUT, fn))
            except Exception as e:
                print("  WARN decode", hex(addr), e); fn = None
            tex_meta.append({"surr": surr, "addr": addr, "w": tu, "h": tv,
                             "fmt": pf, "twiddled": tw, "file": fn})

    # 2) emit one mesh per group (its real control words + screen tris)
    meshes = []
    for g in groups:
        if not g["verts"] or g["bits"]["ListType"] != 0:
            continue
        textured = g["bits"]["Texture"]
        surr = 0
        if textured:
            addr, tu, tv, pf, tw = tex_params(g["tsp"], g["tcw"])
            surr = tex_surr.get((addr, tu, tv, pf, tw), 0)
        tris = []
        for a, b, c in strip_to_tris(g["verts"]):
            if max(a["z"], b["z"], c["z"]) <= 0:     # behind camera
                continue
            tri = []
            for v in (a, b, c):
                tri.append({"pos": [round(v["x"], 3), round(v["y"], 3), round(v["z"], 6)],
                            "uv": [round(v["u"], 5), round(v["v"], 5)],
                            "i": round(inten_of(v["base"]), 4)})
            tris.append(tri)
        if not tris:
            continue
        meshes.append({"surr": surr,
                       "pcw": g["pcw"], "isp": g["isp"], "tsp": g["tsp"], "tcw": g["tcw"],
                       "textured": textured, "tris": tris})

    out = {"mode": "engine_ta", "stage": sid, "screenW": W, "screenH": H,
           "source": "_stage_gt/engine_ta.bin (oracle MAPLECAST_DUMP_RAM, same-frame)",
           "textures": tex_meta, "meshes": meshes}
    jp = f"STG{sid}_ta.json"
    json.dump(out, open(os.path.join(OUTDIR, jp), "w"))
    json.dump(out, open(os.path.join(WEBOUT, jp), "w"))
    ntris = sum(len(m["tris"]) for m in meshes)
    print(f"STG{sid}_ta: {len(meshes)} meshes, {ntris} tris, {len(tex_meta)} VRAM textures -> {jp}")


if __name__ == "__main__":
    main()
