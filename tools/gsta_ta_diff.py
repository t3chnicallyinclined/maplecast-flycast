#!/usr/bin/env python3
"""GSTA-vs-real TA per-sprite param diff.

Parses a flycast TA command buffer (the exact stream ta.cpp consumes) and extracts
every Sprite (ParaType=5) global param + its vertex block (4 screen corners, f16 UVs).
Aligns the GSTA-emitted TA (frame_<vframe>.bin) to the gold-standard mirror-client TA
(frame_<frameNum>.bin) and diffs PCW / ISP / TSP / TCW / corners / UVs per sprite.

Each diff pinpoints a fidelity bug:
  TCW palette-number bits (21:26)  -> PALETTE BANK
  TSP blend bits (31:26) / ListType -> BLEND
  UV f16 fractions                  -> UV / V-orientation
  missing sprites / corner deltas   -> FRAGMENTATION / coverage

Usage:
  python tools/gsta_ta_diff.py --real <dir_or_bin> --gsta <dir_or_bin> [--frame N] [--summary]

TA stream layout (per flycast core/hw/pvr/ta.cpp + ta_structs.h):
  PCW.full @ +0 : bits 31:29 ParaType, 26:24 ListType, obj_ctrl byte (bit0 UV16,bit3 Texture)
  Sprite global param: 32 bytes  [PCW][ISP][TSP][TCW][basecol]...
  Sprite vertex param: 64 bytes  [PCW=0xE0..][Ax Ay Az][Bx By Bz][Cx Cy Cz][Dx Dy][uv16 x3]
"""
import sys, os, struct, glob, argparse

def f16_to_f32(h):
    # UV stored as the high 16 bits of an f32 (per gstaEmitSpriteTA h16()).
    return struct.unpack('<f', struct.pack('<I', h << 16))[0]

def parse_ta(buf):
    """Return list of sprite dicts. Walks 32B records; a ParaType=5 starts a 96B sprite."""
    sprites = []
    o = 0
    n = len(buf)
    while o + 32 <= n:
        pcw = struct.unpack_from('<I', buf, o)[0]
        ptype = (pcw >> 29) & 7
        if ptype == 5:  # Sprite global param (32B) + vertex param (64B)
            if o + 96 > n:
                break
            isp = struct.unpack_from('<I', buf, o + 4)[0]
            tsp = struct.unpack_from('<I', buf, o + 8)[0]
            tcw = struct.unpack_from('<I', buf, o + 12)[0]
            basecol = struct.unpack_from('<I', buf, o + 16)[0]
            v = o + 32
            Ax, Ay, Az = struct.unpack_from('<fff', buf, v + 4)
            Bx, By, Bz = struct.unpack_from('<fff', buf, v + 16)
            Cx, Cy, Cz = struct.unpack_from('<fff', buf, v + 28)
            Dx, Dy = struct.unpack_from('<ff', buf, v + 40)
            # uv16: at v+52..v+64 -> (v0,u0),(v1,u1),(v2,u2)
            v0, u0, v1, u1, v2, u2 = struct.unpack_from('<6H', buf, v + 52)
            sprites.append(dict(
                pcw=pcw, list_type=(pcw >> 24) & 7, isp=isp, tsp=tsp, tcw=tcw,
                basecol=basecol,
                pal=(tcw >> 21) & 0x3F, texfmt=(tcw >> 27) & 7, texaddr=tcw & 0x1FFFFF,
                blend_src=(tsp >> 29) & 7, blend_dst=(tsp >> 26) & 7,
                A=(Ax, Ay), B=(Bx, By), C=(Cx, Cy), D=(Dx, Dy),
                u=(f16_to_f32(u0), f16_to_f32(u1), f16_to_f32(u2)),
                vv=(f16_to_f32(v0), f16_to_f32(v1), f16_to_f32(v2)),
            ))
            o += 96
        elif ptype == 0:  # End-of-list / null
            o += 32
        elif ptype in (4, 1, 2, 3, 6, 7):  # poly global / others: skip 32B and resync
            o += 32
        else:
            o += 32
    return sprites

def load(arg, frame):
    if os.path.isdir(arg):
        if frame is not None:
            p = os.path.join(arg, "frame_%06d.bin" % frame)
            if not os.path.exists(p):
                # tolerate small frame-counter offset: pick nearest existing
                cands = sorted(glob.glob(os.path.join(arg, "frame_*.bin")))
                if not cands:
                    sys.exit("no frames in %s" % arg)
                nums = [int(os.path.basename(c)[6:12]) for c in cands]
                near = min(nums, key=lambda x: abs(x - frame))
                p = os.path.join(arg, "frame_%06d.bin" % near)
                print("[align] %s: requested %d -> nearest %d" % (arg, frame, near))
            return open(p, 'rb').read(), int(os.path.basename(p)[6:12])
        cands = sorted(glob.glob(os.path.join(arg, "frame_*.bin")))
        # pick a mid frame with content
        for c in cands[len(cands)//2:]:
            d = open(c, 'rb').read()
            if len(d) > 96:
                return d, int(os.path.basename(c)[6:12])
        return open(cands[-1], 'rb').read(), int(os.path.basename(cands[-1])[6:12])
    return open(arg, 'rb').read(), -1

def fmt_sprite(s):
    return ("pcw=%08X list=%d isp=%08X tsp=%08X tcw=%08X pal=%2d fmt=%d addr=%05X "
            "blend=%d/%d A=(%.1f,%.1f) C=(%.1f,%.1f) u=%.3f..%.3f v=%.3f..%.3f" % (
        s['pcw'], s['list_type'], s['isp'], s['tsp'], s['tcw'], s['pal'], s['texfmt'],
        s['texaddr'], s['blend_src'], s['blend_dst'], s['A'][0], s['A'][1],
        s['C'][0], s['C'][1], min(s['u']), max(s['u']), min(s['vv']), max(s['vv'])))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--real', required=True)
    ap.add_argument('--gsta', required=True)
    ap.add_argument('--frame', type=int, default=None)
    ap.add_argument('--summary', action='store_true')
    a = ap.parse_args()

    rbuf, rfn = load(a.real, a.frame)
    gbuf, gfn = load(a.gsta, a.frame)
    R = parse_ta(rbuf)
    G = parse_ta(gbuf)
    print("REAL frame %d: %d bytes, %d sprites" % (rfn, len(rbuf), len(R)))
    print("GSTA frame %d: %d bytes, %d sprites" % (gfn, len(gbuf), len(G)))

    if a.summary:
        from collections import Counter
        print("\n-- REAL pal histogram --", Counter(s['pal'] for s in R))
        print("-- GSTA pal histogram --", Counter(s['pal'] for s in G))
        print("-- REAL tsp blend(src/dst) --", Counter((s['blend_src'], s['blend_dst']) for s in R))
        print("-- GSTA tsp blend(src/dst) --", Counter((s['blend_src'], s['blend_dst']) for s in G))
        print("-- REAL list_type --", Counter(s['list_type'] for s in R))
        print("-- GSTA list_type --", Counter(s['list_type'] for s in G))
        print("-- REAL texfmt --", Counter(s['texfmt'] for s in R))
        print("-- GSTA texfmt --", Counter(s['texfmt'] for s in G))
        return

    print("\n=== REAL sprites ===")
    for i, s in enumerate(R):
        print("R[%2d] %s" % (i, fmt_sprite(s)))
    print("\n=== GSTA sprites ===")
    for i, s in enumerate(G):
        print("G[%2d] %s" % (i, fmt_sprite(s)))

if __name__ == '__main__':
    main()
