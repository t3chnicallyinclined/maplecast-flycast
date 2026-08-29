#!/usr/bin/env python3
"""Sanity check: decode actual sprite params+vertices from a mid-capture frame,
dump float values, and dump the distinct-XA lattice. Falsification test for the
dictionary histogram's FSM alignment."""
import struct, sys, collections
import zstandard as zstd

CAP = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap_prod_play.mirror.zcst"
DUMP_FRAME = int(sys.argv[2]) if len(sys.argv) > 2 else 2600

def read_framed(path):
    data = open(path, "rb").read()
    off, msgs = 0, []
    while off + 4 <= len(data):
        n = struct.unpack_from("<I", data, off)[0]; off += 4
        msgs.append(data[off:off+n]); off += n
    return msgs

def reconstruct(msgs):
    dctx = zstd.ZstdDecompressor()
    prev = None; frames = []
    for m in msgs:
        if len(m) < 8 or m[:4] != b"ZCST": continue
        usize = struct.unpack_from("<I", m, 4)[0]
        raw = dctx.decompress(m[8:], max_output_size=usize)
        if raw[:4] == b"SYNC": continue
        taSize, dps = struct.unpack_from("<II", raw, 72)
        p = 80
        if dps == taSize:
            buf = bytearray(raw[p:p+taSize])
        else:
            if prev is None: continue
            buf = bytearray(taSize)
            n = min(taSize, len(prev)); buf[:n] = prev[:n]
            q, end = p, p + dps
            while q + 4 <= end:
                o = struct.unpack_from("<I", raw, q)[0]; q += 4
                if o == 0xFFFFFFFF: break
                rl = struct.unpack_from("<H", raw, q)[0]; q += 2
                if o + rl <= taSize: buf[o:o+rl] = raw[q:q+rl]
                q += rl
        prev = buf
        frames.append(bytes(buf))
    return frames

frames = reconstruct(read_framed(CAP))
print(f"{len(frames)} frames")
xa_vals = set(); ya_vals = set()
dump_left = 12
fidx = min(DUMP_FRAME, len(frames)-1)

for fi, ta in enumerate(frames):
    taSize = len(ta)
    off=0; curList=-1; inPolyList=False; isSpr=False; haveParam=False; cObj=0
    sprseen = 0
    while off + 32 <= taSize:
        pcw = struct.unpack_from("<I", ta, off)[0]
        pt = (pcw >> 29) & 7
        if pt in (0,1,2,3,6):
            haveParam=False
            if pt==0: curList=-1; inPolyList=False
            off += 32; continue
        if pt == 4:
            lt=(pcw>>24)&7
            if curList==-1: curList=lt; inPolyList=lt in (0,2,4)
            if curList in (1,3): haveParam=False; off+=32; continue
            cObj=pcw&0xFF; isSpr=False; haveParam=True
            colType=(cObj>>4)&3; vol=(cObj>>6)&1
            if colType==2 and not vol and ((cObj>>2)&1): sz=64 if off+64<=taSize else 32
            elif colType>=1 and vol: sz=64 if off+64<=taSize else 32
            else: sz=32
            off+=sz; continue
        if pt == 5:
            lt=(pcw>>24)&7
            if curList==-1: curList=lt; inPolyList=lt in (0,2,4)
            cObj=pcw&0xFF; isSpr=True; haveParam=True
            if fi == fidx and dump_left > 0:
                w = struct.unpack_from("<8I", ta, off)
                print(f"[f{fi}] SPR param  PCW={w[0]:08x} ISP={w[1]:08x} TSP={w[2]:08x} TCW={w[3]:08x} base={w[4]:08x}")
            off+=32; continue
        if pt == 7:
            if not inPolyList or not haveParam: off+=32; continue
            tex=(cObj>>3)&1; colType=(cObj>>4)&3; vol=(cObj>>6)&1
            if isSpr and off+64<=taSize:
                fl = struct.unpack_from("<11f", ta, off+4)
                uv = struct.unpack_from("<3I", ta, off+52)
                xa_vals.add(fl[0]); ya_vals.add(fl[1])
                if fi == fidx and dump_left > 0:
                    dump_left -= 1
                    print(f"[f{fi}] SPR vtx A=({fl[0]:.4f},{fl[1]:.4f},{fl[2]:.6f}) B=({fl[3]:.4f},{fl[4]:.4f},{fl[5]:.6f}) C=({fl[6]:.4f},{fl[7]:.4f},{fl[8]:.6f}) D=({fl[9]:.4f},{fl[10]:.4f}) UV={uv[0]:08x},{uv[1]:08x},{uv[2]:08x}")
                sprseen += 1
                off+=64; continue
            if not tex: sz=32
            elif not vol: sz=64 if (colType==1 and off+64<=taSize) else 32
            else: sz=32
            off+=sz; continue
        off += 32

xs = sorted(xa_vals)
print(f"\ndistinct XA float values: {len(xs)}")
print("first 40:", ", ".join(f"{v:.4f}" for v in xs[:40]))
print("last 10:", ", ".join(f"{v:.4f}" for v in xs[-10:]))
# lattice check: differences between consecutive values
diffs = collections.Counter(round(b-a, 6) for a, b in zip(xs, xs[1:]))
print("top consecutive-diff values:", diffs.most_common(10))
ys = sorted(ya_vals)
print(f"\ndistinct YA float values: {len(ys)}")
print("first 30:", ", ".join(f"{v:.4f}" for v in ys[:30]))
