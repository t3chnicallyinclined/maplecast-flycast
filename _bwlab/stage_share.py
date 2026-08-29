#!/usr/bin/env python3
"""stage_share.py — quantify the stage/background share of TA delta churn on a
.mirror.zcst capture, and the counterfactual wire with stage polys stripped.

Pipeline (single pass over ZCST frames, FrameDecoder-equivalent reconstruction):
  1. reconstruct each frame's TA buffer (keyframe / run-delta apply, SYNC reset)
  2. span-parse the TA command stream (port of web/webgpu/ta-parser.mjs control
     flow) recording [start,end) byte spans per polygon with listType/paraType/TCW
  3. classify: STAGE = opaque-list (lt==0) polys; PT/TR sprite quads; other
  4. attribute raw byte-diff (cur vs prev) to spans per class, bucket per second
  5. counterfactual: splice stage spans out, rebuild delta runs with the SERVER'S
     exact algorithm (maplecast_mirror.cpp:2094-2142: run<=65535, one <=8B
     gap-merge probe per run, keyframe on the same frames as captured), reassemble
     inner frames [hdr | payload | tail(checksum+pages, as captured)] and feed
     three streaming-zstd z3 wlog=24 flush-per-frame contexts (epoch resets
     replicated from the captured ZCS2 flags):
        leg A = original inner frames  (ZCS2 replication anchor)
        leg B = rebuilt runs, unstripped (validates the run-builder: payload
                byte-compared vs the captured payload)
        leg C = stage spans stripped
  6. stage translate fit: per consecutive pair with equal stage vert count,
     dx/dy per vertex, uniformity residual (translate-opcode feasibility)

Usage: python stage_share.py [capture] [--limit N] [--json out.json]
"""
import struct, sys, json, collections, time
import numpy as np
import zstandard as zstd

CAP = r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap_prod_play.mirror.zcst"
LIMIT = None
JSON_OUT = r"C:\Users\trist\projects\maplecast-flycast\_bwlab\stage_share_frames.json"
args = sys.argv[1:]
i = 0
while i < len(args):
    if args[i] == "--limit": LIMIT = int(args[i+1]); i += 2
    elif args[i] == "--json": JSON_OUT = args[i+1]; i += 2
    else: CAP = args[i]; i += 1

# class codes
C_CTRL, C_OP, C_PT_SPR, C_TR_SPR, C_PT_POLY, C_TR_POLY, C_MODVOL = 0, 1, 2, 3, 4, 5, 6
CLASS_NAMES = ["control/other", "opaque(stage)", "PT sprite", "TR sprite",
               "PT poly", "TR poly", "modvol"]

def read_msgs(path):
    data = open(path, "rb").read()
    off = 0
    while off + 4 <= len(data):
        n = struct.unpack_from("<I", data, off)[0]; off += 4
        yield data[off:off+n]; off += n

# ---------------------------------------------------------------- span parser
def parse_spans(buf_bytes, taSize):
    """Port of ta-parser.mjs command walk, recording byte spans.
    Returns spans list [(start, end, cls, paraType, tcw, nverts)] and
    stage vertex x-offsets (byte offset of the x float of each stage vertex;
    sprites contribute their 3 explicit verts A,B,C at +4,+16,+28)."""
    n4 = taSize >> 2
    u32 = np.frombuffer(buf_bytes, dtype="<u4", count=n4).tolist()
    spans = []
    stage_vx = []          # byte offsets of stage vertex x floats
    stage_v32 = []         # record starts of 32B non-sprite stage vertices
    stage_v64 = []         # record starts of 64B non-sprite stage vertices
    stage_p32c = []        # 32B op poly params w/ face color floats @16-32 (colType 1/2)
    stage_p64 = []         # 64B op poly params (face col @32-48, offset col @48-64)
    stage_tcws = set()     # TCWs seen in the opaque list this frame
    # dead (parser-ignored per HW spec + ta-parser.mjs read set) record starts, all lists
    dead = {"p32c01": [],   # 32B poly param colType0/1 !vol: bytes 16-32 unread
            "sprp": [],     # sprite param: bytes 24-32 (DMA bookkeeping words)
            "eol": [],      # EOL/control paraType 0: bytes 4-32 reserved
            "sprv": [],     # sprite vertex 64B: bytes 48-52 ignored
            "v64pad": [],   # 64B textured floating-color vertex: bytes 24-32 ignored
            "v32nt0": []}   # 32B non-tex packed-color vertex: 16-24 & 28-32 unread
    off = 0
    curList = -1
    inPolyList = False     # curPPList != null equivalent (lt in 0,2,4)
    cObj = 0; cTCW = 0; isSpr = False
    sp_start = -1; sp_cls = C_CTRL; sp_pt = 0; sp_tcw = 0; sp_nv = 0

    def close_span(end):
        nonlocal sp_start
        if sp_start >= 0:
            spans.append((sp_start, end, sp_cls, sp_pt, sp_tcw, sp_nv))
            sp_start = -1

    def cls_for(lt, paraType):
        if lt == 0: return C_OP
        if lt == 4: return C_PT_SPR if paraType == 5 else C_PT_POLY
        if lt == 2: return C_TR_SPR if paraType == 5 else C_TR_POLY
        return C_MODVOL

    ctrl_start = -1
    def open_ctrl(o):
        nonlocal ctrl_start
        if ctrl_start < 0: ctrl_start = o
    def close_ctrl(end):
        nonlocal ctrl_start
        if ctrl_start >= 0:
            spans.append((ctrl_start, end, C_CTRL, 0, 0, 0))
            ctrl_start = -1

    while off + 32 <= taSize:
        pcw = u32[off >> 2]
        paraType = (pcw >> 29) & 7
        if paraType in (0, 1, 2, 3, 6):
            close_span(off); open_ctrl(off)
            if paraType == 0:
                curList = -1; inPolyList = False
                dead["eol"].append(off)
            off += 32; continue
        if paraType == 4:  # polygon param
            close_span(off); close_ctrl(off)
            lt = (pcw >> 24) & 7
            if curList == -1:
                curList = lt
                inPolyList = curList in (0, 2, 4)
            if curList in (1, 3):  # mod volume list
                spans.append((off, off + 32, C_MODVOL, 4, 0, 0))
                off += 32; continue
            cObj = pcw & 0xFF
            cTCW = u32[(off >> 2) + 3]
            isSpr = False
            colType = (cObj >> 4) & 3; vol = (cObj >> 6) & 1
            if colType == 2 and not vol and ((cObj >> 2) & 1):
                sz = 64 if off + 64 <= taSize else 32
            elif colType >= 1 and vol:
                sz = 64 if off + 64 <= taSize else 32
            else:
                sz = 32
            sp_start = off; sp_cls = cls_for(curList, 4); sp_pt = 4
            sp_tcw = cTCW; sp_nv = 0
            if sp_cls == C_OP:
                stage_tcws.add(cTCW)
                if sz == 64: stage_p64.append(off)
                elif colType in (1, 2) and not vol: stage_p32c.append(off)
            if sz == 32 and colType in (0, 1) and not vol:
                dead["p32c01"].append(off)
            off += sz; continue
        if paraType == 5:  # sprite param
            close_span(off); close_ctrl(off)
            lt = (pcw >> 24) & 7
            if curList == -1:
                curList = lt
                inPolyList = curList in (0, 2, 4)
            cObj = pcw & 0xFF
            cTCW = u32[(off >> 2) + 3]
            isSpr = True
            sp_start = off; sp_cls = cls_for(curList, 5); sp_pt = 5
            sp_tcw = cTCW; sp_nv = 0
            if sp_cls == C_OP: stage_tcws.add(cTCW)
            dead["sprp"].append(off)
            off += 32; continue
        if paraType == 7:  # vertex
            if not inPolyList or sp_start < 0:
                open_ctrl(off); off += 32; continue
            tex = (cObj >> 3) & 1; colType = (cObj >> 4) & 3; vol = (cObj >> 6) & 1
            if isSpr and off + 64 <= taSize:
                if sp_cls == C_OP:
                    stage_vx.extend((off + 4, off + 16, off + 28))
                dead["sprv"].append(off)
                sp_nv += 4; off += 64; continue
            if not tex:
                sz = 32
                if colType == 0: dead["v32nt0"].append(off)
            elif not vol:
                sz = 64 if (colType == 1 and off + 64 <= taSize) else 32
                if sz == 64: dead["v64pad"].append(off)
            else:
                sz = 32
            if sp_cls == C_OP:
                stage_vx.append(off + 4)
                (stage_v32 if sz == 32 else stage_v64).append(off)
            sp_nv += 1
            off += sz; continue
        off += 32
    close_span(off); close_ctrl(off)
    if off < taSize:  # trailing sub-32B
        spans.append((off, taSize, C_CTRL, 0, 0, 0))
    return spans, stage_vx, stage_v32, stage_v64, stage_p32c, stage_p64, stage_tcws, dead

# dead byte ranges (lo,hi) relative to each record start, per group
DEAD_RANGES = {"p32c01": ((16, 32),), "sprp": ((24, 32),), "eol": ((4, 32),),
               "sprv": ((48, 52),), "v64pad": ((24, 32),), "v32nt0": ((16, 24), (28, 32))}

def build_deadmask(dead, taSize):
    m = np.zeros(taSize, bool)
    for g, starts in dead.items():
        if not starts: continue
        S = np.array(starts, np.int64)
        for lo, hi in DEAD_RANGES[g]:
            m[(S[:, None] + np.arange(lo, hi)).ravel()] = True
    return m

# ------------------------------------------------- server delta run builder
def build_runs(cur, prev):
    """EXACT port of maplecast_mirror.cpp:2097-2142. cur/prev np.uint8 arrays.
    Returns (payload bytes incl 0xFFFFFFFF terminator, nRuns, dataBytes)."""
    taSize = len(cur); commonSize = min(taSize, len(prev))
    if commonSize:
        neq = cur[:commonSize] != prev[:commonSize]
        changed_idx = np.flatnonzero(neq)
    else:
        changed_idx = np.empty(0, np.int64)
    # contiguous changed segments within [0, commonSize)
    if len(changed_idx):
        brk = np.flatnonzero(np.diff(changed_idx) > 1)
        seg_start = changed_idx[np.concatenate(([0], brk + 1))]
        seg_end = changed_idx[np.concatenate((brk, [len(changed_idx) - 1]))] + 1
    else:
        seg_start = seg_end = np.empty(0, np.int64)
    # beyond commonSize everything is "changed"
    starts = seg_start.tolist(); ends = seg_end.tolist()
    if taSize > commonSize:
        starts.append(commonSize); ends.append(taSize)
    out = []; nruns = 0; databytes = 0
    curb = cur.tobytes()
    si = 0; nseg = len(starts)
    i = 0
    while si < nseg:
        # skip-equal: next changed byte >= i
        while si < nseg and ends[si] <= i: si += 1
        if si >= nseg: break
        i = max(i, starts[si])
        runStart = i
        # consume changed (respect 65535 cap): changed region is [starts[si], ends[si])
        while True:
            # advance to end of current changed segment or cap
            i = min(ends[si], runStart + 65535)
            if i < runStart + 65535 and si + 1 < nseg and starts[si + 1] == i:
                si += 1; continue
            break
        # gap-merge probe: any change in [i, i+8)?
        if i < taSize:
            gapEnd = min(i + 8, taSize)
            more = si + 1 < nseg and starts[si + 1] < gapEnd
            if not more and si < nseg and ends[si] > i:  # still inside a segment (cap case)
                more = True
            if more:
                i = gapEnd
        fullLen = i - runStart
        if fullLen > 65535:
            i = runStart + 65535; fullLen = 65535
        out.append(struct.pack("<IH", runStart, fullLen))
        out.append(curb[runStart:i])
        nruns += 1; databytes += fullLen
        # loop continues from i
    out.append(b"\xFF\xFF\xFF\xFF")
    return b"".join(out), nruns, databytes

# ---------------------------------------------------------------- main pass
def main():
    t0 = time.time()
    zparams = zstd.ZstdCompressionParameters.from_level(3, window_log=24)
    def new_ctx(): return zstd.ZstdCompressor(compression_params=zparams).compressobj()
    dctx = zstd.ZstdDecompressor()

    # pass 0: index msgs, pair ZCS2 k-th with ZCST k-th
    zcst_msgs = []; zcs2_meta = []   # (len, epoch, flags)
    sync_positions = set()           # index into zcst_msgs stream AFTER which a SYNC arrived
    for m in read_msgs(CAP):
        if len(m) >= 8 and m[:4] == b"ZCST":
            usize = struct.unpack_from("<I", m, 4)[0]
            raw = dctx.decompress(m[8:], max_output_size=usize)
            if raw[:4] == b"SYNC":
                sync_positions.add(len(zcst_msgs))
                continue
            zcst_msgs.append(raw)
        elif len(m) >= 10 and m[:4] == b"ZCS2":
            zcs2_meta.append((len(m), m[4], m[5]))
    n_all = len(zcst_msgs)
    print(f"[pass0] ZCST delta frames={n_all}  ZCS2 msgs={len(zcs2_meta)}  SYNCs at stream idx {sorted(sync_positions)}  ({time.time()-t0:.1f}s)")
    if LIMIT: zcst_msgs = zcst_msgs[:LIMIT]

    ctxA, ctxB, ctxC, ctxD, ctxE = new_ctx(), new_ctx(), new_ctx(), new_ctx(), new_ctx()
    FLUSH = zstd.COMPRESSOBJ_FLUSH_BLOCK

    shadow = np.zeros(0, np.uint8)
    has_prev = False
    prev_buf = None; prev_spans = None; prev_stage_vx = None
    prev_stripped = None; prev_masked = None; prev_ms = None
    first_fn = None
    frames = []                     # per-frame record dicts
    runbuilder_exact = runbuilder_diff = 0
    dropped = 0
    epoch_resets = 0

    tr_pairs = 0; tr_count_mismatch = 0
    tr_exact = 0; tr_le_half = 0; tr_gt_half = 0
    tr_static = 0                    # dx==dy==0 exact
    tr_maxres_list = []
    # within-stage churn by field
    FIELD_NAMES = ["param/other", "x", "y", "z", "uv", "vert base col", "vert offs col", "param face col", "DEAD (ignored)"]
    stage_field_chg = np.zeros(9, np.int64)
    stage_field_chg_sec = collections.defaultdict(lambda: np.zeros(9, np.int64))
    tcw_frames = collections.Counter()   # op-list TCW -> #frames present
    stage_sizes = []

    dead_group_chg = collections.Counter()   # dead churn by group
    for k, raw in enumerate(zcst_msgs):
        if k in sync_positions:
            has_prev = False; prev_buf = None; prev_stripped = None
            prev_masked = None; prev_ms = None
        frameNum = struct.unpack_from("<I", raw, 4)[0]
        taSize, dps = struct.unpack_from("<II", raw, 72)
        hdr = raw[:80]
        payload = raw[80:80+dps]
        tail = raw[80+dps:]
        keyframe = (dps == taSize)

        if keyframe:
            if len(shadow) < taSize:
                ns = np.zeros(taSize, np.uint8); ns[:len(shadow)] = shadow; shadow = ns
            shadow[:taSize] = np.frombuffer(payload, np.uint8)
            has_prev = True
        elif not has_prev:
            dropped += 1
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
        curb = cur.tobytes()
        if first_fn is None: first_fn = frameNum
        sec = (frameNum - first_fn) // 60

        spans, stage_vx, stage_v32, stage_v64, stage_p32c, stage_p64, stage_tcws, dead = parse_spans(curb, taSize)
        for t in stage_tcws: tcw_frames[t] += 1
        deadmask = build_deadmask(dead, taSize)
        # classmap + per-class span bytes
        classmap = np.zeros(taSize, np.uint8)
        span_bytes = [0]*7; span_polys = [0]*7; span_verts = [0]*7
        for (s, e, c, pt, tcw, nv) in spans:
            classmap[s:e] = c
            span_bytes[c] += e - s
            if pt: span_polys[c] += 1; span_verts[c] += nv

        rec = {"k": k, "fn": frameNum, "sec": int(sec), "taSize": taSize,
               "kf": keyframe, "span_bytes": span_bytes,
               "span_polys": span_polys, "span_verts": span_verts,
               "wire_dps": dps, "tail": len(tail)}

        # ---- delta attribution vs prev frame (raw byte diff)
        if prev_buf is not None and not keyframe:
            common = min(taSize, len(prev_buf))
            D = np.zeros(taSize, bool)
            if common:
                D[:common] = cur[:common] != prev_buf[:common]
            D[common:] = True
            ch = np.bincount(classmap[D], minlength=7).tolist()
            rec["chg_bytes"] = ch
            rec["chg_total"] = int(D.sum())
            dc = np.bincount(classmap[D & deadmask], minlength=7).tolist()
            rec["chg_dead"] = dc
            # dead churn by group (which ignored-byte family is flipping)
            for g, starts in dead.items():
                if not starts: continue
                S = np.array(starts, np.int64)
                gm = np.zeros(taSize, bool)
                for lo, hi in DEAD_RANGES[g]:
                    gm[(S[:, None] + np.arange(lo, hi)).ravel()] = True
                dead_group_chg[g] += int((D & gm).sum())
            # within-stage field attribution (non-sprite stage verts, 32B + 64B layouts,
            # + poly-param face-color blocks)
            if stage_v32 or stage_v64 or stage_p32c or stage_p64:
                fieldmap = np.zeros(taSize, np.uint8)
                if stage_v32:  # packed/intensity 32B: x@4 y@8 z@12 uv@16-24 bcol@24-28 ocol@28-32
                    S = np.array(stage_v32, np.int64)
                    for code, lo, hi in ((1,4,8),(2,8,12),(3,12,16),(4,16,24),(5,24,28),(6,28,32)):
                        fieldmap[(S[:, None] + np.arange(lo, hi)).ravel()] = code
                if stage_v64:  # intensity colType1 64B: x@4 y@8 z@12 uv@16-24 bcol@32-48 ocol@48-64
                    S = np.array(stage_v64, np.int64)
                    for code, lo, hi in ((1,4,8),(2,8,12),(3,12,16),(4,16,24),(5,32,48),(6,48,64)):
                        fieldmap[(S[:, None] + np.arange(lo, hi)).ravel()] = code
                if stage_p32c:  # face base color floats @16-32
                    S = np.array(stage_p32c, np.int64)
                    fieldmap[(S[:, None] + np.arange(16, 32)).ravel()] = 7
                if stage_p64:   # face base col @32-48, offset col @48-64
                    S = np.array(stage_p64, np.int64)
                    fieldmap[(S[:, None] + np.arange(32, 64)).ravel()] = 7
                fieldmap[deadmask] = 8   # parser-ignored bytes trump field labels
                sel = D & (classmap == C_OP)
                fc = np.bincount(fieldmap[sel], minlength=9)
                stage_field_chg += fc
                stage_field_chg_sec[int(sec)] += fc
        else:
            rec["chg_bytes"] = None

        # ---- stage translate fit
        if prev_buf is not None and not keyframe and prev_stage_vx is not None:
            if len(stage_vx) and len(stage_vx) == len(prev_stage_vx):
                f32c = np.frombuffer(curb, "<f4")
                f32p = np.frombuffer(prev_buf.tobytes(), "<f4")
                idx = np.array(stage_vx, np.int64) >> 2
                idxp = np.array(prev_stage_vx, np.int64) >> 2
                dx = f32c[idx] - f32p[idxp]
                dy = f32c[idx + 1] - f32p[idxp + 1]
                mdx = float(np.median(dx)); mdy = float(np.median(dy))
                res = max(float(np.abs(dx - mdx).max()), float(np.abs(dy - mdy).max()))
                tr_pairs += 1
                tr_maxres_list.append(res)
                if mdx == 0.0 and mdy == 0.0 and res == 0.0: tr_static += 1
                if res == 0.0: tr_exact += 1
                elif res <= 0.5: tr_le_half += 1
                else: tr_gt_half += 1
                rec["tr_res"] = res; rec["tr_d"] = [mdx, mdy]
            elif len(stage_vx) or len(prev_stage_vx):
                tr_count_mismatch += 1

        # ---- counterfactual buffers
        stage_mask = classmap == C_OP
        stripped = cur[~stage_mask]
        masked = cur.copy(); masked[deadmask] = 0          # leg D: dead bytes canonicalized
        maskstrip = masked[~stage_mask]                     # leg E: canonicalized + stage stripped
        rec["stripped_size"] = int(len(stripped))
        rec["dead_bytes"] = int(deadmask.sum())
        stage_sizes.append(span_bytes[C_OP])

        # ---- leg payloads
        if keyframe:
            payB = curb; payC = stripped.tobytes()
            payD = masked.tobytes(); payE = maskstrip.tobytes()
            rec["runsB"] = rec["runsC"] = None
        else:
            payB, nrB, dbB = build_runs(cur, prev_buf)
            payC, nrC, dbC = build_runs(stripped, prev_stripped) if prev_stripped is not None else (stripped.tobytes(), 0, len(stripped))
            payD = build_runs(masked, prev_masked)[0] if prev_masked is not None else masked.tobytes()
            payE = build_runs(maskstrip, prev_ms)[0] if prev_ms is not None else maskstrip.tobytes()
            rec["runsB"] = [nrB, dbB, len(payB)]
            rec["runsC"] = [nrC, dbC, len(payC)] if prev_stripped is not None else None
            if payB == payload: runbuilder_exact += 1
            else: runbuilder_diff += 1

        hdrB = bytearray(hdr); struct.pack_into("<I", hdrB, 76, len(payB))
        hdrC = bytearray(hdr)
        struct.pack_into("<I", hdrC, 72, len(stripped))
        struct.pack_into("<I", hdrC, 76, len(payC))
        hdrD = bytearray(hdr); struct.pack_into("<I", hdrD, 76, len(payD))
        hdrE = bytearray(hdr)
        struct.pack_into("<I", hdrE, 72, len(maskstrip))
        struct.pack_into("<I", hdrE, 76, len(payE))
        frameA = raw
        frameB = bytes(hdrB) + payB + tail
        frameC = bytes(hdrC) + payC + tail
        frameD = bytes(hdrD) + payD + tail
        frameE = bytes(hdrE) + payE + tail

        # epoch reset replication (from captured ZCS2 flags, paired by index)
        if k < len(zcs2_meta) and (zcs2_meta[k][2] & 1):
            ctxA, ctxB, ctxC, ctxD, ctxE = new_ctx(), new_ctx(), new_ctx(), new_ctx(), new_ctx()
            epoch_resets += 1
        outA = len(ctxA.compress(frameA)) + len(ctxA.flush(FLUSH))
        outB = len(ctxB.compress(frameB)) + len(ctxB.flush(FLUSH))
        outC = len(ctxC.compress(frameC)) + len(ctxC.flush(FLUSH))
        outD = len(ctxD.compress(frameD)) + len(ctxD.flush(FLUSH))
        outE = len(ctxE.compress(frameE)) + len(ctxE.flush(FLUSH))
        rec["zA"] = outA; rec["zB"] = outB; rec["zC"] = outC
        rec["zD"] = outD; rec["zE"] = outE
        rec["zcs2_bytes"] = zcs2_meta[k][0] if k < len(zcs2_meta) else None

        frames.append(rec)
        prev_buf = cur; prev_spans = spans; prev_stage_vx = stage_vx
        prev_stripped = stripped; prev_masked = masked; prev_ms = maskstrip

        if k % 500 == 0:
            print(f"  frame {k}/{len(zcst_msgs)} fn={frameNum} taSize={taSize} "
                  f"op={span_bytes[C_OP]}B/{span_polys[C_OP]}p ({time.time()-t0:.0f}s)")

    print(f"[pass1] done {len(frames)} frames, dropped {dropped}, epoch resets {epoch_resets} ({time.time()-t0:.0f}s)")
    print(f"[validate] run-builder payload byte-exact vs wire: {runbuilder_exact}/{runbuilder_exact+runbuilder_diff}")

    json.dump({"frames": frames,
               "stage_field_chg_sec": {str(s): v.tolist() for s, v in stage_field_chg_sec.items()},
               "translate": {"pairs": tr_pairs, "exact": tr_exact, "static": tr_static,
                              "le_half": tr_le_half, "gt_half": tr_gt_half,
                              "count_mismatch": tr_count_mismatch,
                              "maxres_p50_p90_p99_max": [float(np.percentile(tr_maxres_list, p)) for p in (50, 90, 99)] + [float(max(tr_maxres_list))] if tr_maxres_list else []}},
              open(JSON_OUT, "w"))
    print(f"[out] {JSON_OUT}")

    # ---------------- aggregate report ----------------
    dur = (frames[-1]["fn"] - frames[0]["fn"] + 1) / 60.0
    mbps = lambda b: b * 8 / dur / 1e6
    print(f"\n== capture: {len(frames)} frames, {dur:.2f}s ==")

    tot_span = [sum(f["span_bytes"][c] for f in frames) for c in range(7)]
    tot_polys = [sum(f["span_polys"][c] for f in frames) for c in range(7)]
    tot_verts = [sum(f["span_verts"][c] for f in frames) for c in range(7)]
    tot_ta = sum(f["taSize"] for f in frames)
    print("\n== TA buffer composition (per-frame averages) ==")
    for c in range(7):
        print(f"  {CLASS_NAMES[c]:16s} {tot_span[c]/len(frames):10.0f} B/f "
              f"{tot_polys[c]/len(frames):7.1f} polys/f {tot_verts[c]/len(frames):8.1f} verts/f "
              f"({100*tot_span[c]/tot_ta:5.1f}% of TA)")

    dfr = [f for f in frames if f["chg_bytes"]]
    tot_chg = [sum(f["chg_bytes"][c] for f in dfr) for c in range(7)]
    tot_dead = [sum(f["chg_dead"][c] for f in dfr) for c in range(7)]
    allchg = sum(tot_chg); alldead = sum(tot_dead)
    print(f"\n== raw byte-diff churn attribution ({len(dfr)} delta frames, {allchg:,} changed B total; "
          f"{alldead:,} = {100*alldead/allchg:.1f}% in parser-IGNORED bytes) ==")
    print(f"  {'class':16s} {'total chg':>12s} {'%':>7s} {'B/frame':>8s} {'dead chg':>12s} {'dead%of class':>13s}")
    for c in range(7):
        print(f"  {CLASS_NAMES[c]:16s} {tot_chg[c]:>12,} {100*tot_chg[c]/allchg:6.2f}% {tot_chg[c]/len(dfr):8.0f} "
              f"{tot_dead[c]:>12,} {100*tot_dead[c]/max(1,tot_chg[c]):12.1f}%")
    print(f"  dead churn by group: " + ", ".join(f"{g}:{v:,}" for g, v in dead_group_chg.most_common()))

    # per-second
    secs = sorted(set(f["sec"] for f in frames))
    per_sec = {}
    for s in secs:
        fs = [f for f in frames if f["sec"] == s]
        dfs = [f for f in fs if f["chg_bytes"]]
        chg = [sum(f["chg_bytes"][c] for f in dfs) for c in range(7)]
        stage_span = sum(f["span_bytes"][C_OP] for f in dfs)
        per_sec[s] = {
            "frames": len(fs),
            "chg": chg,
            "stage_frac_of_span": chg[C_OP] / stage_span if stage_span else 0.0,
            "zA": sum(f["zA"] for f in fs), "zB": sum(f["zB"] for f in fs),
            "zC": sum(f["zC"] for f in fs), "zD": sum(f["zD"] for f in fs),
            "zE": sum(f["zE"] for f in fs),
            "zcs2": sum(f["zcs2_bytes"] or 0 for f in fs),
        }
    moving = [s for s in secs if per_sec[s]["stage_frac_of_span"] > 0.20]
    static = [s for s in secs if s not in moving]
    print(f"\n== per-second: {len(moving)} camera-moving seconds (stage chg > 20% of stage span), {len(static)} static ==")
    def agg(ss, key):
        return sum(per_sec[s][key] for s in ss) / max(1, len(ss))
    for label, ss in (("moving", moving), ("static", static)):
        if not ss: continue
        chg = [sum(per_sec[s]["chg"][c] for s in ss) for c in range(7)]
        t = sum(chg)
        print(f"  [{label}] avg changed B/s by class: total {t/len(ss):,.0f}")
        for c in range(7):
            print(f"     {CLASS_NAMES[c]:16s} {chg[c]/len(ss):>10,.0f} B/s  {100*chg[c]/t:6.2f}%")
        print(f"     zcs2 measured {agg(ss,'zcs2')*8/1e6:.3f} Mbps | legA {agg(ss,'zA')*8/1e6:.3f} | legB {agg(ss,'zB')*8/1e6:.3f} | "
              f"legC(strip) {agg(ss,'zC')*8/1e6:.3f} | legD(deadmask) {agg(ss,'zD')*8/1e6:.3f} | legE(mask+strip) {agg(ss,'zE')*8/1e6:.3f}")

    totA = sum(f["zA"] for f in frames); totB = sum(f["zB"] for f in frames); totC = sum(f["zC"] for f in frames)
    totD = sum(f["zD"] for f in frames); totE = sum(f["zE"] for f in frames)
    tot2 = sum(f["zcs2_bytes"] or 0 for f in frames)
    print(f"\n== streaming z3 wlog24 flush/frame (epoch resets replicated: {epoch_resets}) ==")
    print(f"  ZCS2 measured on wire       : {tot2:>12,} B = {mbps(tot2):.3f} Mbps (incl 10B/frame hdr)")
    print(f"  leg A original frames       : {totA:>12,} B = {mbps(totA):.3f} Mbps")
    print(f"  leg B rebuilt runs          : {totB:>12,} B = {mbps(totB):.3f} Mbps")
    print(f"  leg C stage-stripped        : {totC:>12,} B = {mbps(totC):.3f} Mbps  (-{100*(totA-totC)/totA:.1f}% vs A)")
    print(f"  leg D dead-byte canonicalize: {totD:>12,} B = {mbps(totD):.3f} Mbps  (-{100*(totA-totD)/totA:.1f}% vs A)")
    print(f"  leg E canonicalize + strip  : {totE:>12,} B = {mbps(totE):.3f} Mbps  (-{100*(totA-totE)/totA:.1f}% vs A)")

    # spike seconds by ZCS2 measured
    ranked = sorted(secs, key=lambda s: -per_sec[s]["zcs2"])
    print(f"\n== top-10 ZCS2 spike seconds ==")
    print(f"  {'sec':>4} {'zcs2 Mbps':>9} {'legA':>7} {'legC':>7} {'legD':>7} {'legE':>7} {'stage%span':>10} {'stage chg B/s':>13} {'total chg B/s':>13}")
    for s in ranked[:10]:
        p = per_sec[s]
        print(f"  {s:>4} {p['zcs2']*8/1e6:>9.3f} {p['zA']*8/1e6:>7.3f} {p['zC']*8/1e6:>7.3f} {p['zD']*8/1e6:>7.3f} {p['zE']*8/1e6:>7.3f} "
              f"{100*p['stage_frac_of_span']:>9.1f}% {p['chg'][C_OP]:>13,} {sum(p['chg']):>13,}")

    tot_stage_chg = int(stage_field_chg.sum())
    print(f"\n== within-stage churn by field ({tot_stage_chg:,} changed stage B) ==")
    for c in range(9):
        print(f"  {FIELD_NAMES[c]:15s} {int(stage_field_chg[c]):>12,} B  {100*stage_field_chg[c]/max(1,tot_stage_chg):6.2f}%")

    stable99 = sum(1 for t, c in tcw_frames.items() if c >= 0.99 * len(frames))
    print(f"\n== op-list TCW stability ==")
    print(f"  distinct op TCWs across capture: {len(tcw_frames)}; present in >=99% of frames: {stable99}")
    print(f"  top-10: " + ", ".join(f"{t:08x}:{c}" for t, c in tcw_frames.most_common(10)))
    print(f"  stage span bytes/frame: min={min(stage_sizes):,} max={max(stage_sizes):,} mean={sum(stage_sizes)/len(stage_sizes):,.0f}")

    print(f"\n== stage translate fit ({tr_pairs} pairs, {tr_count_mismatch} vert-count mismatches) ==")
    if tr_pairs:
        print(f"  static (d=0, res=0): {tr_static} ({100*tr_static/tr_pairs:.1f}%)")
        print(f"  exact single-translate (res==0): {tr_exact} ({100*tr_exact/tr_pairs:.1f}%)")
        print(f"  res<=0.5px: {tr_le_half} ({100*tr_le_half/tr_pairs:.1f}%)   res>0.5px: {tr_gt_half} ({100*tr_gt_half/tr_pairs:.1f}%)")
        if tr_maxres_list:
            print(f"  max-residual percentiles p50/p90/p99/max: "
                  + "/".join(f"{float(np.percentile(tr_maxres_list,p)):.4f}" for p in (50,90,99))
                  + f"/{max(tr_maxres_list):.4f} px")

if __name__ == "__main__":
    main()
