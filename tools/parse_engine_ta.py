#!/usr/bin/env python3
"""
parse_engine_ta.py — walk the engine's RAW PowerVR TA parameter stream
(_stage_gt/engine_ta.bin = tctx->tad.thd_root..thd_data, dumped by the oracle
hook MAPLECAST_DUMP_RAM at the SAME frame as ram/vram/pvr_regs) and extract the
REAL per-polygon material control words: PCW, ISP/TSP, TSP, TCW + per-vertex
screen XY/depth/UV/base-colour.

This is the ground truth that GROUNDS stage-client.mjs (replaces the synthesized
control words flagged OPEN in re_kb 26). The stream is variable-width: a poly
header is 32B (type 0/1/3) or 64B (type 2/4), and its vertices are 32B or 64B
depending on the poly type — exactly flycast's TaTypeLut (core/hw/pvr/ta.cpp
poly_data_type_id / poly_header_type_size). We replicate that here so the walk is
byte-exact, not a fixed stride.

Para types (PCW bits 31..29):
  0 End_Of_List      1 User_Tile_Clip   2 Object_List_Set
  4 Polygon/ModVol   5 Sprite           7 Vertex

Usage:
  python3 tools/parse_engine_ta.py            # summary + first polys
  python3 tools/parse_engine_ta.py --json out.json   # full structured dump
"""
import os, sys, json, struct
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
GT = os.path.join(REPO, "_stage_gt")


def f(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


# --- flycast TaTypeLut, ported (core/hw/pvr/ta.cpp) ---
def pcw_bits(pcw):
    return {
        "UV_16bit": pcw & 1,
        "Gouraud": (pcw >> 1) & 1,
        "Offset": (pcw >> 2) & 1,
        "Texture": (pcw >> 3) & 1,
        "Col_Type": (pcw >> 4) & 3,
        "Volume": (pcw >> 6) & 1,
        "Shadow": (pcw >> 7) & 1,
        "ListType": (pcw >> 24) & 7,
        "EndOfStrip": (pcw >> 28) & 1,
        "ParaType": (pcw >> 29) & 7,
    }


def poly_data_type_id(b):
    if b["Texture"]:
        if b["Volume"] == 0:
            if b["Col_Type"] == 0:
                return 3 if b["UV_16bit"] == 0 else 4
            elif b["Col_Type"] == 1:
                return 5 if b["UV_16bit"] == 0 else 6
            else:
                return 7 if b["UV_16bit"] == 0 else 8
        else:
            if b["Col_Type"] == 0:
                return 11 if b["UV_16bit"] == 0 else 12
            elif b["Col_Type"] == 1:
                return 255
            else:
                return 13 if b["UV_16bit"] == 0 else 14
    else:
        if b["Volume"] == 0:
            if b["Col_Type"] == 0:
                return 0
            elif b["Col_Type"] == 1:
                return 1
            else:
                return 2
        else:
            if b["Col_Type"] == 0:
                return 9
            elif b["Col_Type"] == 1:
                return 255
            else:
                return 10


def poly_header_sz64(b):
    # returns True if the polygon HEADER is 64B (type 2 / type 4)
    if b["Volume"] == 0:
        if b["Col_Type"] < 2:
            return False
        elif b["Col_Type"] == 2:
            if b["Texture"] and b["Offset"]:
                return True
            return False
        else:
            return False
    else:
        if b["Col_Type"] == 0:
            return False
        elif b["Col_Type"] == 2:
            return True
        elif b["Col_Type"] == 3:
            return False
        return False


def vertex_sz64(b):
    vt = poly_data_type_id(b)
    return vt in (5, 6, 11, 12, 13, 14)


def walk(ta):
    """Yield polygon groups: {pcw, isp, tsp, tcw, paraType, bits, verts:[...]}."""
    n = len(ta)
    o = 0
    cur = None
    groups = []
    while o + 32 <= n:
        c = struct.unpack_from("<I", ta, o)[0]
        ptype = (c >> 29) & 7
        if ptype == 7:
            # vertex param — size depends on current poly
            v64 = cur and cur["_v64"]
            vt = cur["vt"] if cur is not None else None
            # screen X,Y,Z(1/w) at +4,+8,+0xC ; UV at +0x10,+0x14. The per-vertex BASE
            # COLOUR layout differs by vertex type (core/hw/pvr/ta_structs.h):
            #   vt3  (Packed Color, textured):  +0x18 BaseCol(ARGB8888 packed)
            #   vt5  (Floating Color, textured, 64B): +0x18,+0x1C ignored; the BASE colour
            #        is the second 32B half — +0x20 A, +0x24 R, +0x28 G, +0x2C B (floats 0..1)
            #   vt7  (Intensity, textured):     +0x18 BaseInt (float 0..1) — modulates FaceColor
            # The OLD walk read +0x18 as `base` for EVERY type — for vt5 that is `ignore_1`
            # (garbage, decoded to a bogus ~0.01 intensity that painted the deck black). We
            # now decode the real per-vertex RGBA so a textured Intensity/Floating mesh (the
            # carrier deck, vt5, ShadInstr=modulate) keeps its true dark-grey shading.
            x = f(struct.unpack_from("<I", ta, o + 4)[0])
            y = f(struct.unpack_from("<I", ta, o + 8)[0])
            z = f(struct.unpack_from("<I", ta, o + 12)[0])
            u = f(struct.unpack_from("<I", ta, o + 16)[0])
            v = f(struct.unpack_from("<I", ta, o + 20)[0])
            base = struct.unpack_from("<I", ta, o + 24)[0]   # vt3 BaseCol (kept for compat)
            rgba = None      # (r,g,b,a) floats 0..1 — the REAL modulation colour
            if vt == 5 or vt == 6:        # Floating Color (textured) — colour in 2nd half
                A = f(struct.unpack_from("<I", ta, o + 32 + 0)[0])
                R = f(struct.unpack_from("<I", ta, o + 32 + 4)[0])
                G = f(struct.unpack_from("<I", ta, o + 32 + 8)[0])
                B = f(struct.unpack_from("<I", ta, o + 32 + 12)[0])
                rgba = (R, G, B, A)
            elif vt == 7 or vt == 8:      # Intensity (textured) — single float, grey
                bi = f(base)
                if 0.0 <= bi <= 4.0:
                    rgba = (bi, bi, bi, 1.0)
            elif vt == 3 or vt == 4:      # Packed ARGB8888
                A = ((base >> 24) & 0xFF) / 255.0
                R = ((base >> 16) & 0xFF) / 255.0
                G = ((base >> 8) & 0xFF) / 255.0
                B = (base & 0xFF) / 255.0
                rgba = (R, G, B, A)
            if cur is not None:
                cur["verts"].append({"x": x, "y": y, "z": z, "u": u, "v": v, "base": base,
                                     "rgba": rgba, "eos": (c >> 28) & 1})
            o += 64 if v64 else 32
        elif ptype == 4 or ptype == 5:
            b = pcw_bits(c)
            isp = struct.unpack_from("<I", ta, o + 4)[0]
            tsp = struct.unpack_from("<I", ta, o + 8)[0]
            tcw = struct.unpack_from("<I", ta, o + 12)[0]
            cur = {"pcw": c, "isp": isp, "tsp": tsp, "tcw": tcw,
                   "paraType": ptype, "bits": b,
                   "vt": poly_data_type_id(b) if ptype == 4 else "sprite",
                   "_v64": (True if ptype == 5 else vertex_sz64(b)),
                   "verts": []}
            groups.append(cur)
            o += 64 if (ptype == 4 and poly_header_sz64(b)) else 32
        else:
            # 0 EOL, 1 user-tile-clip, 2 obj-list-set — all 32B, no poly
            cur = None
            o += 32
    return groups


def tsp_bits(tsp):
    return {
        "TexV": tsp & 7, "TexU": (tsp >> 3) & 7,
        "ShadInstr": (tsp >> 6) & 3, "FilterMode": (tsp >> 13) & 3,
        "UseAlpha": (tsp >> 20) & 1, "IgnoreTexA": (tsp >> 19) & 1,
        "ClampU": (tsp >> 15) & 1, "ClampV": (tsp >> 14) & 1,
        "FlipU": (tsp >> 17) & 1, "FlipV": (tsp >> 16) & 1,
        "DstInstr": (tsp >> 26) & 7, "SrcInstr": (tsp >> 29) & 7,
    }


def isp_bits(isp):
    return {"DepthMode": (isp >> 29) & 7, "CullMode": (isp >> 27) & 3,
            "ZWriteDis": (isp >> 26) & 1, "Texture": (isp >> 25) & 1,
            "Gouraud": (isp >> 23) & 1}


def tcw_bits(tcw):
    return {"TexAddr": tcw & 0x1FFFFF, "ScanOrder": (tcw >> 26) & 1,
            "PixelFmt": (tcw >> 27) & 7, "VQ": (tcw >> 30) & 1,
            "MipMapped": (tcw >> 31) & 1, "PalSelect": (tcw >> 21) & 0x3F}


def main():
    ta = open(os.path.join(GT, "engine_ta.bin"), "rb").read()
    groups = walk(ta)
    nverts = sum(len(g["verts"]) for g in groups)
    print(f"engine_ta.bin: {len(ta)} bytes, {len(groups)} poly groups, {nverts} verts")
    pt = Counter(g["paraType"] for g in groups)
    vt = Counter(g["vt"] for g in groups)
    tex = Counter((g["tcw"] & 0x1FFFFF) for g in groups if g["bits"]["Texture"])
    print("  para types:", dict(pt))
    print("  vertex types:", dict(vt))
    print(f"  distinct textured TexAddr: {len(tex)}  ->", sorted(tex)[:16])
    print(f"  textured groups: {sum(1 for g in groups if g['bits']['Texture'])}  "
          f"untextured: {sum(1 for g in groups if not g['bits']['Texture'])}")
    # show the first few real polygon groups (skip 0-vert headers)
    shown = 0
    for g in groups:
        if not g["verts"]:
            continue
        b = g["bits"]
        print(f"\n  GROUP pcw={g['pcw']:08x} type={g['vt']} list={b['ListType']} "
              f"tex={b['Texture']} gouraud={b['Gouraud']} offset={b['Offset']} "
              f"verts={len(g['verts'])}")
        print(f"    ISP={g['isp']:08x} {isp_bits(g['isp'])}")
        print(f"    TSP={g['tsp']:08x} {tsp_bits(g['tsp'])}")
        print(f"    TCW={g['tcw']:08x} {tcw_bits(g['tcw'])}")
        v0 = g["verts"][0]
        print(f"    v0 screen=({v0['x']:.1f},{v0['y']:.1f}) z={v0['z']:.4f} "
              f"uv=({v0['u']:.3f},{v0['v']:.3f}) base={v0['base']:08x}")
        shown += 1
        if shown >= 8:
            break

    if "--json" in sys.argv:
        out = sys.argv[sys.argv.index("--json") + 1]
        slim = [{"pcw": g["pcw"], "isp": g["isp"], "tsp": g["tsp"], "tcw": g["tcw"],
                 "paraType": g["paraType"], "vt": g["vt"], "bits": g["bits"],
                 "verts": g["verts"]} for g in groups if g["verts"]]
        json.dump(slim, open(out, "w"))
        print(f"\nwrote {len(slim)} groups -> {out}")


if __name__ == "__main__":
    main()
