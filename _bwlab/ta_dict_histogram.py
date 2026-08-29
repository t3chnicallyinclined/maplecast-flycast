#!/usr/bin/env python3
"""TA-command dictionary measurement.

Reconstructs every per-frame raw TA parameter stream from a .mirror.zcst capture
(keyframe + delta-run apply, exactly mirroring maplecast_mirror.cpp:1605-1634),
walks it with the same FSM as taCanonicalize (maplecast_mirror.cpp:514-572),
and histograms distinct blocks to test the "bounded dictionary" hypothesis:

  - control params   (paraType 0/1/2/3/6, 32B)
  - global poly/spr params (paraType 4/5, 32/64B)  <- the PCW/ISP/TSP/TCW templates
  - vertex params    (paraType 7, 32/64B)          <- positions + UVs
  - vertex params with position floats MASKED      <- the non-positional residue
  - distinct position tuples, distinct TCWs
  - frame-to-frame content repeat rate (block in frame N present in frame N-1 / anywhere before)

READ-ONLY on the capture. Output = stdout only.
"""
import struct, sys, collections
import zstandard as zstd

CAP = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap.mirror.zcst"
MAX_FRAMES = int(sys.argv[2]) if len(sys.argv) > 2 else 0  # 0 = all

def read_framed(path):
    data = open(path, "rb").read()
    off, msgs = 0, []
    while off + 4 <= len(data):
        n = struct.unpack_from("<I", data, off)[0]; off += 4
        msgs.append(data[off:off+n]); off += n
    return msgs

def main():
    msgs = read_framed(CAP)
    dctx = zstd.ZstdDecompressor()

    # ---- reconstruct per-frame TA buffers ----
    prev = None
    frames = []   # list of (frameNum, bytes)
    for m in msgs:
        if len(m) < 8 or m[:4] != b"ZCST":
            continue
        usize = struct.unpack_from("<I", m, 4)[0]
        raw = dctx.decompress(m[8:], max_output_size=usize)
        if raw[:4] == b"SYNC":
            continue
        frameNum = struct.unpack_from("<I", raw, 4)[0]
        taSize, dps = struct.unpack_from("<II", raw, 72)
        p = 80
        if dps == taSize:
            buf = bytearray(raw[p:p+taSize])
        else:
            if prev is None:
                continue
            buf = bytearray(taSize)
            n = min(taSize, len(prev))
            buf[:n] = prev[:n]
            q, end = p, p + dps
            while q + 4 <= end:
                o = struct.unpack_from("<I", raw, q)[0]; q += 4
                if o == 0xFFFFFFFF: break
                rl = struct.unpack_from("<H", raw, q)[0]; q += 2
                if o + rl <= taSize:
                    buf[o:o+rl] = raw[q:q+rl]
                q += rl
        prev = buf
        frames.append((frameNum, bytes(buf)))
        if MAX_FRAMES and len(frames) >= MAX_FRAMES:
            break

    print(f"capture: {CAP}")
    print(f"reconstructed {len(frames)} TA frames, sizes {min(len(b) for _,b in frames)}..{max(len(b) for _,b in frames)} B")

    # ---- FSM walk (mirror of taCanonicalize) + histograms ----
    KINDS = ("ctrl", "poly", "spr", "vtx", "vtx_spr")
    total = collections.Counter()          # occurrences per kind
    distinct = {k: set() for k in KINDS}   # global distinct content (hash of full block)
    vtx_masked = set()                     # vertex blocks with position floats zeroed
    spr_pos = set()                        # sprite-vertex position tuple (bytes 4..48)
    tcws = set()                           # TCW word of poly/spr global params
    param_per_frame = []
    int_pos = 0; nonint_pos = 0            # position floats that are exact integers
    # frame-to-frame repeat
    prev_set = None
    rep_prev_hits = 0; rep_prev_total = 0  # block occurrences present in previous frame
    seen_ever = set()
    rep_ever_hits = 0; rep_ever_total = 0  # block occurrences (after frame 0) seen in ANY prior frame
    frame_distinct_sum = 0

    H = hash  # 64-bit siphash of bytes, stable within run

    for fi, (fn, ta) in enumerate(frames):
        taSize = len(ta)
        off = 0
        curList = -1
        inPolyList = False; isSpr = False; haveParam = False
        cObj = 0
        nparams = 0
        cur_set = set()
        cur_blocks = []   # (kind, hash) occurrences this frame

        def emit(kind, lo, sz):
            nonlocal nparams
            b = ta[lo:lo+sz]
            h = H(b)
            total[kind] += 1
            distinct[kind].add(h)
            cur_set.add(h)
            cur_blocks.append(h)
            nparams += 1
            return b, h

        while off + 32 <= taSize:
            pcw = struct.unpack_from("<I", ta, off)[0]
            paraType = (pcw >> 29) & 7
            if paraType in (0, 1, 2, 3, 6):
                haveParam = False
                if paraType == 0:
                    curList = -1; inPolyList = False
                emit("ctrl", off, 32)
                off += 32; continue
            if paraType == 4:
                lt = (pcw >> 24) & 7
                if curList == -1:
                    curList = lt; inPolyList = lt in (0, 2, 4)
                if curList in (1, 3):
                    haveParam = False
                    emit("poly", off, 32)
                    off += 32; continue
                cObj = pcw & 0xFF
                isSpr = False; haveParam = True
                colType = (cObj >> 4) & 3; vol = (cObj >> 6) & 1
                if colType == 2 and not vol and ((cObj >> 2) & 1):
                    sz = 64 if off + 64 <= taSize else 32
                elif colType >= 1 and vol:
                    sz = 64 if off + 64 <= taSize else 32
                else:
                    sz = 32
                b, h = emit("poly", off, sz)
                tcws.add(struct.unpack_from("<I", b, 12)[0])
                off += sz; continue
            if paraType == 5:
                lt = (pcw >> 24) & 7
                if curList == -1:
                    curList = lt; inPolyList = lt in (0, 2, 4)
                cObj = pcw & 0xFF
                isSpr = True; haveParam = True
                b, h = emit("spr", off, 32)
                tcws.add(struct.unpack_from("<I", b, 12)[0])
                off += 32; continue
            if paraType == 7:
                if not inPolyList or not haveParam:
                    emit("vtx", off, 32)
                    off += 32; continue
                tex = (cObj >> 3) & 1; colType = (cObj >> 4) & 3; vol = (cObj >> 6) & 1
                if isSpr and off + 64 <= taSize:
                    b, h = emit("vtx_spr", off, 64)
                    # masked = zero position floats bytes 4..48 (XA..YD)
                    mb = b[:4] + b"\0"*44 + b[48:]
                    vtx_masked.add(H(mb))
                    spr_pos.add(H(b[4:48]))
                    off += 64; continue
                if not tex:
                    sz = 32
                elif not vol:
                    sz = 64 if (colType == 1 and off + 64 <= taSize) else 32
                else:
                    sz = 32
                b, h = emit("vtx", off, sz)
                mb = b[:4] + b"\0"*12 + b[16:]  # zero X,Y,Z
                vtx_masked.add(H(mb))
                off += sz; continue
            emit("ctrl", off, 32)   # unknown -> bucket as ctrl
            off += 32

        param_per_frame.append(nparams)
        frame_distinct_sum += len(cur_set)
        if prev_set is not None:
            for h in cur_blocks:
                rep_prev_total += 1
                if h in prev_set: rep_prev_hits += 1
                rep_ever_total += 1
                if h in seen_ever: rep_ever_hits += 1
        seen_ever |= cur_set
        prev_set = cur_set

    # integer-position measurement done separately on a sample of sprite vertices
    # (kept out of hot loop): sample first 200 frames
    samp_int = samp_half = samp_other = 0
    for fn, ta in frames[:200]:
        off = 0; curList=-1; inPolyList=False; isSpr=False; haveParam=False; cObj=0
        taSize = len(ta)
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
                cObj=pcw&0xFF; isSpr=True; haveParam=True; off+=32; continue
            if pt == 7:
                if not inPolyList or not haveParam: off+=32; continue
                tex=(cObj>>3)&1; colType=(cObj>>4)&3; vol=(cObj>>6)&1
                if isSpr and off+64<=taSize:
                    fl = struct.unpack_from("<11f", ta, off+4)
                    for f in fl[:8]:   # XY of the 4 corners (skip Z triplet mix): actually XA YA ZA XB YB ZB XC YC ZC XD YD -> take x/y only
                        pass
                    # x/y coords are indices 0,1,3,4,6,7,9,10 ; z = 2,5,8
                    for i in (0,1,3,4,6,7,9,10):
                        f = fl[i]
                        if float(f).is_integer(): samp_int += 1
                        elif (f*2).is_integer(): samp_half += 1
                        else: samp_other += 1
                    off+=64; continue
                if not tex: sz=32
                elif not vol: sz=64 if (colType==1 and off+64<=taSize) else 32
                else: sz=32
                off+=sz; continue
            off += 32

    n = len(frames)
    tot_all = sum(total.values())
    print()
    print(f"== params/frame: mean {sum(param_per_frame)/n:.0f}, min {min(param_per_frame)}, max {max(param_per_frame)} ==")
    print(f"total blocks {tot_all:,} over {n} frames")
    print()
    print(f"{'kind':8s} {'occurrences':>12s} {'distinct(whole capture)':>24s} {'dedup ratio':>12s}")
    for k in KINDS:
        occ = total[k]; d = len(distinct[k])
        print(f"{k:8s} {occ:>12,} {d:>24,} {occ/max(1,d):>11.1f}x")
    print()
    print(f"global params (poly+spr) distinct = {len(distinct['poly'] | distinct['spr']):,}   <- the PCW/ISP/TSP/TCW 'template dictionary'")
    print(f"distinct TCW values (texture control words) = {len(tcws):,}")
    print(f"vertex blocks with POSITION FLOATS MASKED distinct = {len(vtx_masked):,}   <- non-positional vertex residue (UV/color patterns)")
    print(f"distinct sprite-vertex position tuples (44B XA..YD) = {len(spr_pos):,}")
    print()
    tot_xy = samp_int + samp_half + samp_other
    print(f"sprite-vertex x/y coord quantization (first 200 frames, {tot_xy:,} coords):")
    print(f"  exact integer: {samp_int:,} ({100*samp_int/max(1,tot_xy):.1f}%)   half-integer: {samp_half:,} ({100*samp_half/max(1,tot_xy):.1f}%)   other: {samp_other:,} ({100*samp_other/max(1,tot_xy):.1f}%)")
    print()
    print(f"== frame-to-frame content repeat (by block content, not offset) ==")
    print(f"block occurrences present in PREVIOUS frame: {rep_prev_hits:,}/{rep_prev_total:,} = {100*rep_prev_hits/max(1,rep_prev_total):.2f}%")
    print(f"block occurrences seen in ANY prior frame:  {rep_ever_hits:,}/{rep_ever_total:,} = {100*rep_ever_hits/max(1,rep_ever_total):.2f}%")
    print(f"avg distinct blocks per frame: {frame_distinct_sum/n:.0f} (vs {tot_all/n:.0f} occurrences/frame)")
    print(f"TOTAL distinct blocks whole capture: {len(seen_ever):,}")

if __name__ == "__main__":
    main()
