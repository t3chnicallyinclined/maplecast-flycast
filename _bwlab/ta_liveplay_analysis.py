#!/usr/bin/env python3
"""Live-play capture analysis — docs/TA-DICT-WIRE-PLAN.md §3b test order steps 1-3.

Input: a cap_wss.mjs capture taken with the 2026-07-14 local rig config
(MAPLECAST_TACANON=2 -> the TA is already canon-masked on the wire;
MAPLECAST_ZSTREAM_CAM=1 -> per-frame M1/M2 camera in the CLEAR ZCS2 header).

Outputs, in order:
  [1] ACTIVITY VERIFICATION (the idle-corpus lesson): % of frames whose camera
      matrix changed, distinct camera count. The idle corpus's tell was a
      FROZEN camera — refuse to conclude anything from an idle capture.
  [2] CHURN DECOMPOSITION: every never-seen dictionary block attributed to
      body / stage / HUD / other-textured (effects) / untextured / control,
      using the in-tree classifiers (body banks {82,83,88,89}; stage TCW words
      {9fc00,a0000}; HUD words {9be00,80000,9de00..9e900} per re_kb/67).
  [3] WHOLE-BLOCK DICT WIRE on real play (the honest v1 number).
  [4] PLAYERS-ONLY DICT WIRE (body+control blocks only) — the endgame TA-side.
  [5] REPROJECTION TEST: for each NEW vertex block, zero its position floats
      and re-look-up. High hit-rate => churn is pure position motion (camera
      reprojection / lattice movement) => prediction/split-stream wins.

Usage: python ta_liveplay_analysis.py [capture.zcst]
"""
import struct, sys, statistics
import zstandard as zstd

CAP = sys.argv[1] if len(sys.argv) > 1 else \
    r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap_liveplay.mirror.zcst"

BODY_BANKS = {0x82, 0x83, 0x88, 0x89}
STAGE_WORDS = {0x9FC00 >> 3, 0xA0000 >> 3}     # allowlist entries are (tcw & 0x1FFFFF) values
HUD_WORDS_EXACT = {0x9BE00 >> 3, 0x80000 >> 3}
HUD_RANGE = (0x9DE00 >> 3, 0x9E900 >> 3)

# NOTE: server allowlist compares (tcw & 0x1FFFFF) directly against 0x9fc00 etc.
# Those constants are already the TCW word-address field values — do NOT shift.
STAGE_WORDS = {0x9FC00, 0xA0000}
HUD_WORDS_EXACT = {0x9BE00, 0x80000}
HUD_RANGE = (0x9DE00, 0x9E900)

def classify_tcw(tcw):
    addr = tcw & 0x1FFFFF
    bank = (addr << 3) >> 15
    if bank in BODY_BANKS:
        return "body"
    if addr in STAGE_WORDS:
        return "stage"
    if addr in HUD_WORDS_EXACT or HUD_RANGE[0] <= addr <= HUD_RANGE[1]:
        return "hud"
    return "otherTex"

def read_framed(path):
    data = open(path, "rb").read()
    off, msgs = 0, []
    while off + 4 <= len(data):
        n = struct.unpack_from("<I", data, off)[0]; off += 4
        msgs.append(data[off:off+n]); off += n
    return msgs

def reconstruct_with_camera(msgs):
    """Legacy-chain TA frames + the camera from the ZCS2 msg that precedes each
    legacy frame (per-frame broadcast order: TDW1, ZCS2, legacy ZCST)."""
    dctx = zstd.ZstdDecompressor()
    prev = None; frames = []
    last_cam = None
    for m in msgs:
        if len(m) >= 142 and m[:4] == b"ZCS2" and (m[5] & 8):
            last_cam = bytes(m[10:142])   # sid u32 + M2 16f + M1 16f
            continue
        if len(m) < 8 or m[:4] != b"ZCST":
            continue
        usize = struct.unpack_from("<I", m, 4)[0]
        if usize > 64 << 20:
            continue
        try:
            raw = dctx.decompress(bytes(m[8:]), max_output_size=usize)
        except zstd.ZstdError:
            continue
        if len(raw) < 80 or raw[:4] in (b"SYNC", b"TDWS", b"STAF", b"CHRQ", b"MCSV", b"FSYN"):
            continue
        taSize, dps = struct.unpack_from("<II", raw, 72)
        if taSize > 8 << 20 or dps > 8 << 20:
            continue
        p = 80
        if dps == taSize:
            buf = bytearray(raw[p:p+taSize])
        else:
            if prev is None:
                continue
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
        frames.append((bytes(buf), last_cam))
    return frames

def walk_blocks(ta):
    """Yield (block_bytes, cls, kind) in emit order. cls per classify_tcw of the
    OWNING param (vertices inherit); kind in {'ctrl','param','vert'}.
    Same FSM as taCanonicalize / the server walker."""
    taSize = len(ta); off = 0
    curList = -1; inPoly = False; isSpr = False; haveP = False
    cObj = 0; curCls = "ctrl"
    while off + 32 <= taSize:
        pcw = struct.unpack_from("<I", ta, off)[0]
        pt = (pcw >> 29) & 7
        sz = 32; cls = "ctrl"; kind = "ctrl"
        if pt in (0, 1, 2, 3, 6):
            haveP = False; curCls = "ctrl"
            if pt == 0: curList = -1; inPoly = False
        elif pt == 4:
            lt = (pcw >> 24) & 7
            if curList == -1: curList = lt; inPoly = lt in (0, 2, 4)
            if curList in (1, 3):
                haveP = False; curCls = "ctrl"
            else:
                cObj = pcw & 0xFF; isSpr = False; haveP = True
                colType = (cObj >> 4) & 3; vol = (cObj >> 6) & 1
                if colType == 2 and not vol and ((cObj >> 2) & 1): sz = 64 if off+64 <= taSize else 32
                elif colType >= 1 and vol: sz = 64 if off+64 <= taSize else 32
                tcw = struct.unpack_from("<I", ta, off+12)[0]
                curCls = classify_tcw(tcw) if ((cObj >> 3) & 1) else "untex"
                cls = curCls; kind = "param"
        elif pt == 5:
            lt = (pcw >> 24) & 7
            if curList == -1: curList = lt; inPoly = lt in (0, 2, 4)
            cObj = pcw & 0xFF; isSpr = True; haveP = True
            tcw = struct.unpack_from("<I", ta, off+12)[0]
            curCls = classify_tcw(tcw)
            cls = curCls; kind = "param"
        elif pt == 7:
            if inPoly and haveP:
                tex = (cObj >> 3) & 1; colType = (cObj >> 4) & 3; vol = (cObj >> 6) & 1
                if isSpr and off + 64 <= taSize: sz = 64
                elif tex and not vol and colType == 1 and off + 64 <= taSize: sz = 64
                cls = curCls; kind = "vert"
            else:
                cls = "ctrl"; kind = "ctrl"
        yield ta[off:off+sz], cls, kind, isSpr
        off += sz

def zero_positions(b, kind, is_spr):
    """Vertex block with position floats zeroed (reprojection-test key)."""
    if kind != "vert":
        return None
    v = bytearray(b)
    if is_spr and len(b) == 64:
        v[4:48] = b"\0" * 44        # A xyz, B xyz, C xyz, D xy
    else:
        v[4:16] = b"\0" * 12        # x, y, z
    return bytes(v)

def main():
    msgs = read_framed(CAP)
    frames = reconstruct_with_camera(msgs)
    n = len(frames)
    if n < 100:
        print(f"only {n} frames reconstructed — capture unusable"); sys.exit(1)
    dur = n / 60.0
    print(f"{CAP}\n{n} frames = {dur:.1f}s  ({len(msgs)} raw msgs)")

    # [1] activity verification
    cams = [c for _, c in frames if c is not None]
    cam_changes = sum(1 for a, b in zip(cams, cams[1:]) if a != b)
    distinct_cams = len(set(cams))
    pct = 100.0 * cam_changes / max(1, len(cams) - 1)
    print(f"\n[1] ACTIVITY: camera changed on {cam_changes}/{len(cams)-1} frame-pairs "
          f"({pct:.1f}%), {distinct_cams} distinct matrices")
    verdict = "ACTIVE" if pct > 10 and distinct_cams > 50 else "SUSPECT-IDLE"
    print(f"    verdict: {verdict}" + ("" if verdict == "ACTIVE" else "  — DO NOT TRUST, recapture"))

    # [2]+[3]+[4]+[5] single pass
    full_dict = {}; body_dict = {}
    zero_dict = set()
    cctx = zstd.ZstdCompressor(level=3)
    comp_full = cctx.compressobj()
    cctx2 = zstd.ZstdCompressor(level=3)
    comp_body = cctx2.compressobj()
    wire_full = wire_body = 0
    new_by_cls = {}; per_frame_full = []
    reproj_hits = reproj_miss = 0
    reproj_by_cls = {}
    for ta, _cam in frames:
        refs_f = bytearray(); news_f = bytearray()
        refs_b = bytearray(); news_b = bytearray()
        nf = nb = 0
        for blk, cls, kind, is_spr in walk_blocks(ta):
            bid = full_dict.get(blk)
            if bid is None:
                bid = len(full_dict); full_dict[blk] = bid
                news_f.append(len(blk)); news_f += blk
                new_by_cls[cls] = new_by_cls.get(cls, 0) + len(blk)
                zk = zero_positions(blk, kind, is_spr)
                if zk is not None:
                    h = zk in zero_dict
                    reproj_hits += h; reproj_miss += (not h)
                    d = reproj_by_cls.setdefault(cls, [0, 0])
                    d[0] += h; d[1] += (not h)
                    zero_dict.add(zk)
            refs_f += struct.pack("<I", bid); nf += 1
            if cls in ("body", "ctrl"):
                bid2 = body_dict.get(blk)
                if bid2 is None:
                    bid2 = len(body_dict); body_dict[blk] = bid2
                    news_b.append(len(blk)); news_b += blk
                refs_b += struct.pack("<I", bid2); nb += 1
        pf = struct.pack("<II", nf, len(news_f)) + bytes(refs_f) + bytes(news_f)
        pb = struct.pack("<II", nb, len(news_b)) + bytes(refs_b) + bytes(news_b)
        c1 = comp_full.compress(pf) + comp_full.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)
        c2 = comp_body.compress(pb) + comp_body.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)
        wire_full += len(c1); wire_body += len(c2)
        per_frame_full.append(len(c1))

    tot_new = sum(new_by_cls.values())
    print(f"\n[2] NEW-BLOCK CHURN DECOMPOSITION ({tot_new:,} B total new content):")
    for cls in sorted(new_by_cls, key=new_by_cls.get, reverse=True):
        v = new_by_cls[cls]
        print(f"    {cls:9s} {v:>12,} B  ({100.0*v/tot_new:5.1f}%)")

    print(f"\n[3] WHOLE-BLOCK DICT WIRE (real play, masked): "
          f"{wire_full:,} B = {wire_full/dur*8/1e6:.3f} Mbps  "
          f"median {statistics.median(per_frame_full):.0f} B/frame  "
          f"p95 {sorted(per_frame_full)[int(0.95*n)]} B  dict {len(full_dict):,} blocks")

    print(f"\n[4] PLAYERS-ONLY DICT WIRE (body+ctrl blocks): "
          f"{wire_body:,} B = {wire_body/dur*8/1e6:.3f} Mbps  dict {len(body_dict):,} blocks")

    tot_v = reproj_hits + reproj_miss
    print(f"\n[5] REPROJECTION TEST (new vertex blocks whose position-zeroed twin "
          f"was already known): {reproj_hits:,}/{tot_v:,} = "
          f"{100.0*reproj_hits/max(1,tot_v):.1f}% position-only variants")
    for cls in sorted(reproj_by_cls, key=lambda c: -sum(reproj_by_cls[c])):
        h, m = reproj_by_cls[cls]
        print(f"    {cls:9s} {h:>10,}/{h+m:<10,} = {100.0*h/max(1,h+m):5.1f}%")

if __name__ == "__main__":
    main()
