#!/usr/bin/env python3
"""Debug: where inside the stage (op-list) spans do bytes actually change?
Reconstructs two consecutive frames and prints every changed 4-byte word in
op-list spans: span-relative offset, word index within the record, old/new."""
import struct, sys
import numpy as np
import zstandard as zstd
sys.path.insert(0, r"C:\Users\trist\projects\maplecast-flycast\_bwlab")
from stage_share import parse_spans, read_msgs, C_OP

CAP = r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap_prod_play.mirror.zcst"
TARGET_K = int(sys.argv[1]) if len(sys.argv) > 1 else 100

dctx = zstd.ZstdDecompressor()
shadow = np.zeros(0, np.uint8); has_prev = False
prev = None; k = -1
for m in read_msgs(CAP):
    if not (len(m) >= 8 and m[:4] == b"ZCST"): continue
    usize = struct.unpack_from("<I", m, 4)[0]
    raw = dctx.decompress(m[8:], max_output_size=usize)
    if raw[:4] == b"SYNC": continue
    k += 1
    taSize, dps = struct.unpack_from("<II", raw, 72)
    payload = raw[80:80+dps]
    if dps == taSize:
        if len(shadow) < taSize:
            ns = np.zeros(taSize, np.uint8); ns[:len(shadow)] = shadow; shadow = ns
        shadow[:taSize] = np.frombuffer(payload, np.uint8); has_prev = True
    elif not has_prev:
        continue
    else:
        if len(shadow) < taSize:
            ns = np.zeros(taSize, np.uint8); ns[:len(shadow)] = shadow; shadow = ns
        q = 0
        while q + 4 <= len(payload):
            o = struct.unpack_from("<I", payload, q)[0]; q += 4
            if o == 0xFFFFFFFF: break
            rl = struct.unpack_from("<H", payload, q)[0]; q += 2
            if o + rl <= taSize and q + rl <= len(payload):
                shadow[o:o+rl] = np.frombuffer(payload[q:q+rl], np.uint8)
            q += rl
    cur = shadow[:taSize].copy()
    if k == TARGET_K and prev is not None:
        curb = cur.tobytes(); prevb = prev.tobytes()
        spans, svx, sv32, sv64, sp32c, sp64, tcws = parse_spans(curb, taSize)
        print(f"frame k={k} taSize={taSize} v32={len(sv32)} v64={len(sv64)} p32c={len(sp32c)} p64={len(sp64)}")
        common = min(len(curb), len(prevb))
        D = np.frombuffer(curb[:common], np.uint8) != np.frombuffer(prevb[:common], np.uint8)
        chg = np.flatnonzero(D)
        op_spans = [s for s in spans if s[2] == C_OP]
        print(f"op spans: {len(op_spans)}; eg first 3: {[(s[0],s[1],hex(s[4]),s[5]) for s in op_spans[:3]]}")
        shown = 0
        for (s, e, c, pt, tcw, nv) in op_spans:
            in_span = chg[(chg >= s) & (chg < e)]
            if not len(in_span): continue
            words = sorted(set((int(i) - s) // 4 for i in in_span))
            print(f"span [{s},{e}) len={e-s} paraType={pt} tcw={tcw:08x} nv={nv}: {len(in_span)} chgB, {len(words)} words, span-rel words {words[:24]}")
            # vertex-relative interpretation: param size = 64 if s in sp64 else 32
            psz = 64 if s in set(sp64) else 32
            vstart = s + psz
            for w in words[:16]:
                boff = s + w*4
                a = struct.unpack_from('<I', prevb, boff)[0]
                b = struct.unpack_from('<I', curb, boff)[0]
                fa = struct.unpack_from('<f', prevb, boff)[0]
                fb = struct.unpack_from('<f', curb, boff)[0]
                vrel = (boff - vstart) % 64 if boff >= vstart else -1
                vidx = (boff - vstart) // 64 if boff >= vstart else -1
                print(f"    word {w} (byte {boff}, vert#{vidx} +{vrel}): {a:08x} -> {b:08x}   f32 {fa:.6g} -> {fb:.6g}")
            shown += 1
            if shown >= 12: break
        # total per-span churn ranking
        totals = []
        for (s, e, c, pt, tcw, nv) in op_spans:
            n = int(((chg >= s) & (chg < e)).sum())
            if n: totals.append((n, s, e, nv, tcw))
        totals.sort(reverse=True)
        print("\ntop churn op spans:", [(n, f"[{s},{e}) nv={nv} tcw={tcw:08x}") for n, s, e, nv, tcw in totals[:8]])
        break
    prev = cur
