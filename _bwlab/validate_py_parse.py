#!/usr/bin/env python3
"""Python twin of validate_js_parse.mjs — same sampled frames, md5 + vert totals
from stage_share.py's reconstruction + span parser. Diff outputs to validate."""
import struct, sys, hashlib
import numpy as np
import zstandard as zstd
sys.path.insert(0, r"C:\Users\trist\projects\maplecast-flycast\_bwlab")
from stage_share import read_msgs, parse_spans

CAP = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap_prod_play.mirror.zcst"
dctx = zstd.ZstdDecompressor()
shadow = np.zeros(0, np.uint8); has_prev = False; k = -1
for m in read_msgs(CAP):
    if not (len(m) >= 8 and m[:4] == b"ZCST"): continue
    usize = struct.unpack_from("<I", m, 4)[0]
    raw = dctx.decompress(m[8:], max_output_size=usize)
    if raw[:4] == b"SYNC":
        has_prev = False   # FrameDecoder resets prevTA on SYNC
        continue
    fn = struct.unpack_from("<I", raw, 4)[0]
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
    k += 1
    if k % 500: continue
    b = shadow[:taSize].tobytes()
    md5 = hashlib.md5(b).hexdigest()
    spans = parse_spans(b, taSize)[0]
    verts = sum(s[5] for s in spans)
    print(f"{k} fn={fn} taSize={taSize} md5={md5} verts={verts}")
