#!/usr/bin/env python3
"""Round 2: TA dictionary with CANONICAL masking (TACANON dead-byte map applied),
dictionary growth curve, and per-dword variance of sprite global params + sprite
vertices (to locate residual cardinality). Read-only.
Dead-byte map from maplecast_mirror.cpp taCanonicalize (lines 514-572):
  ctrl paraType0: bytes 4..32 dead
  poly 32B colType 0/1 !vol: bytes 16..32 dead
  spr param: bytes 24..32 dead
  sprite vertex 64B: bytes 48..52 dead
  textured 64B vertex (colType1 !vol): bytes 24..32 dead
  non-tex colType0 32B vertex: bytes 16..24 + 28..32 dead
"""
import struct, sys, collections
import zstandard as zstd

CAP = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap.mirror.zcst"

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
        frameNum = struct.unpack_from("<I", raw, 4)[0]
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
        frames.append((frameNum, bytes(buf)))
    return frames

def main():
    frames = reconstruct(read_framed(CAP))
    n = len(frames)
    print(f"{n} frames reconstructed")

    KINDS = ("ctrl","poly","spr","vtx","vtx_spr")
    total = collections.Counter()
    distinct = {k: set() for k in KINDS}
    seen_ever = set()
    growth = []          # (frame_idx, total_distinct) sampled每 60 frames
    spr_dword = [set() for _ in range(8)]      # distinct values per dword of 32B spr param (canon-masked)
    sprv_dword = [set() for _ in range(16)]    # per dword of 64B sprite vertex (canon-masked)
    spr_uv = set()       # sprite vertex UV block bytes 52..64
    rep_prev_hits = rep_prev_total = 0
    rep_ever_hits = rep_ever_total = 0
    prev_set = None
    Z4 = b"\0"*4; Z8 = b"\0"*8; Z16 = b"\0"*16; Z28 = b"\0"*28

    H = hash
    for fi,(fn,ta) in enumerate(frames):
        taSize = len(ta)
        off = 0; curList=-1; inPolyList=False; isSpr=False; haveParam=False; cObj=0
        cur_set = set(); cur_blocks = []
        def emit(kind, b):
            h = H(b)
            total[kind] += 1; distinct[kind].add(h)
            cur_set.add(h); cur_blocks.append(h)
        while off + 32 <= taSize:
            pcw = struct.unpack_from("<I", ta, off)[0]
            pt = (pcw >> 29) & 7
            if pt in (0,1,2,3,6):
                haveParam=False
                if pt==0:
                    curList=-1; inPolyList=False
                    emit("ctrl", ta[off:off+4] + Z28)          # bytes 4..32 dead
                else:
                    emit("ctrl", ta[off:off+32])
                off += 32; continue
            if pt == 4:
                lt=(pcw>>24)&7
                if curList==-1: curList=lt; inPolyList=lt in (0,2,4)
                if curList in (1,3):
                    haveParam=False; emit("poly", ta[off:off+32]); off+=32; continue
                cObj=pcw&0xFF; isSpr=False; haveParam=True
                colType=(cObj>>4)&3; vol=(cObj>>6)&1
                if colType==2 and not vol and ((cObj>>2)&1): sz=64 if off+64<=taSize else 32
                elif colType>=1 and vol: sz=64 if off+64<=taSize else 32
                else: sz=32
                b = ta[off:off+sz]
                if sz==32 and colType in (0,1) and not vol:
                    b = b[:16] + Z16                          # bytes 16..32 dead
                emit("poly", b)
                off += sz; continue
            if pt == 5:
                lt=(pcw>>24)&7
                if curList==-1: curList=lt; inPolyList=lt in (0,2,4)
                cObj=pcw&0xFF; isSpr=True; haveParam=True
                b = ta[off:off+24] + Z8                       # bytes 24..32 dead
                emit("spr", b)
                for d in range(6):
                    spr_dword[d].add(struct.unpack_from("<I", b, d*4)[0])
                off += 32; continue
            if pt == 7:
                if not inPolyList or not haveParam:
                    emit("vtx", ta[off:off+32]); off += 32; continue
                tex=(cObj>>3)&1; colType=(cObj>>4)&3; vol=(cObj>>6)&1
                if isSpr and off+64<=taSize:
                    b = ta[off:off+48] + Z4 + ta[off+52:off+64]   # bytes 48..52 dead
                    emit("vtx_spr", b)
                    for d in list(range(12)) + [13,14,15]:
                        sprv_dword[d].add(struct.unpack_from("<I", b, d*4)[0])
                    spr_uv.add(b[52:64])
                    off += 64; continue
                if not tex:
                    sz=32
                    b = ta[off:off+32]
                    if colType==0:
                        b = b[:16] + Z8 + b[24:28] + Z4       # 16..24 + 28..32 dead
                    emit("vtx", b); off += 32; continue
                elif not vol:
                    sz = 64 if (colType==1 and off+64<=taSize) else 32
                    b = ta[off:off+sz]
                    if sz==64: b = b[:24] + Z8 + b[32:]       # 24..32 dead
                    emit("vtx", b); off += sz; continue
                else:
                    emit("vtx", ta[off:off+32]); off += 32; continue
            emit("ctrl", ta[off:off+32]); off += 32
        if prev_set is not None:
            for h in cur_blocks:
                rep_prev_total += 1
                if h in prev_set: rep_prev_hits += 1
                rep_ever_total += 1
                if h in seen_ever: rep_ever_hits += 1
        seen_ever |= cur_set
        prev_set = cur_set
        if fi % 60 == 0 or fi == n-1:
            growth.append((fi, len(seen_ever)))

    tot_all = sum(total.values())
    print()
    print("== CANONICAL (dead bytes masked) ==")
    print(f"{'kind':8s} {'occurrences':>12s} {'distinct':>10s} {'dedup':>8s}")
    for k in KINDS:
        occ=total[k]; d=len(distinct[k])
        print(f"{k:8s} {occ:>12,} {d:>10,} {occ/max(1,d):>7.1f}x")
    print(f"TOTAL distinct blocks (canon): {len(seen_ever):,}  (occurrences {tot_all:,})")
    print(f"global params (poly+spr) distinct: {len(distinct['poly']|distinct['spr']):,}")
    print(f"sprite-vertex UV blocks (bytes 52..64) distinct: {len(spr_uv):,}")
    print()
    print("sprite GLOBAL param — distinct values per dword (canon):")
    names=["PCW","ISP","TSP","TCW","baseColor","offsColor"]
    for d in range(6):
        print(f"  +{d*4:2d} {names[d]:10s}: {len(spr_dword[d]):,}")
    print()
    print("sprite VERTEX — distinct values per dword (canon, dword12=dead skipped):")
    vn=["PCW","XA","YA","ZA","XB","YB","ZB","XC","YC","ZC","XD","YD","-","UV_A","UV_B","UV_C"]
    for d in list(range(12))+[13,14,15]:
        print(f"  +{d*4:2d} {vn[d]:5s}: {len(sprv_dword[d]):,}")
    print()
    print(f"frame-to-frame content repeat (canon): prev {100*rep_prev_hits/max(1,rep_prev_total):.2f}%  ever {100*rep_ever_hits/max(1,rep_ever_total):.2f}%")
    print()
    print("dictionary growth (distinct blocks vs frame idx):")
    for fi,d in growth[::5] + [growth[-1]]:
        print(f"  f{fi:5d}: {d:,}")
    # growth rate over last 10 seconds
    per_sec = [(growth[i][1]-growth[i-1][1]) for i in range(1,len(growth))]
    if len(per_sec) >= 20:
        print(f"new distinct blocks/second: first 5s avg {sum(per_sec[:5])/5:,.0f}, last 10s avg {sum(per_sec[-10:])/10:,.0f}")

if __name__ == "__main__":
    main()
