#!/usr/bin/env python3
"""LIVE Phase-2 gate for the TDW1 dict wire (docs/TA-DICT-WIRE-PLAN.md).

Connects to a LOCAL flycast mirror server (ws://127.0.0.1:7200) and, per frame,
byte-compares:
  A) the TA buffer reconstructed from the legacy ZCST delta chain (ground truth
     — the shipping wire, decoded exactly like the production client), against
  B) the TA buffer reconstructed from the TDW1 content-addressed dict stream.

Frames are matched by frameNum (legacy inner off 4 == TDW1 inner off 0).
Start this script BEFORE the server (it retries the connect) so the TDW zstd
stream and dictionary are consumed from their very first message; otherwise it
waits for the next TDWS+streamStart pair (client join / SYNC).

Usage:  python tadict_gate_live.py [--frames N] [--url ws://127.0.0.1:7200]
Exit 0 = gate PASS (N frames compared, 0 mismatches). Read-only; renders nothing.
"""
import struct, sys, time
import zstandard as zstd
import websocket

FRAMES_TARGET = 3000
URL = "ws://127.0.0.1:7200"
for i, a in enumerate(sys.argv):
    if a == "--frames": FRAMES_TARGET = int(sys.argv[i + 1])
    if a == "--url":    URL = sys.argv[i + 1]

# ---- legacy ZCST chain state (mirror of maplecast_mirror.cpp:1605-1634) ----
zcst_dctx = zstd.ZstdDecompressor()
legacy_prev = None                 # last reconstructed TA buffer
legacy_by_fn = {}                  # frameNum -> TA bytes (bounded)

# ---- TDW state --------------------------------------------------------------
tdw_stream = None                  # streaming decompressobj
tdw_dict = []                      # id -> block bytes
tdw_epoch = None
tdw_seq_next = None
tdw_synced = False
tdw_by_fn = {}
have_snapshot = False              # a TDWS for tdw_epoch has been applied

compared = 0
mismatches = 0
tdw_msgs = 0
legacy_frames = 0
t_start = None
first_mismatch_detail = None

def parse_legacy(raw):
    """raw = decompressed ZCST inner. Returns (frameNum, ta) or None."""
    global legacy_prev
    if len(raw) < 80 or raw[:4] in (b"SYNC", b"STAF", b"CHRQ", b"MCSV", b"TDWS"):
        return None
    frameNum = struct.unpack_from("<I", raw, 4)[0]
    taSize, dps = struct.unpack_from("<II", raw, 72)
    if taSize > 8 << 20 or dps > 8 << 20:
        return None
    p = 80
    if dps == taSize:
        buf = bytearray(raw[p:p + taSize])
    else:
        if legacy_prev is None:
            return None
        buf = bytearray(taSize)
        n = min(taSize, len(legacy_prev)); buf[:n] = legacy_prev[:n]
        q, end = p, p + dps
        while q + 4 <= end:
            o = struct.unpack_from("<I", raw, q)[0]; q += 4
            if o == 0xFFFFFFFF: break
            rl = struct.unpack_from("<H", raw, q)[0]; q += 2
            if o + rl <= taSize: buf[o:o + rl] = raw[q:q + rl]
            q += rl
    legacy_prev = buf
    return frameNum, bytes(buf)

def apply_tdws(inner):
    global tdw_dict, tdw_epoch, have_snapshot, tdw_synced
    epoch = inner[4]
    nBlk, secB = struct.unpack_from("<II", inner, 8)
    p = 16
    d = []
    for _ in range(nBlk):
        ln = inner[p]; p += 1
        d.append(inner[p:p + ln]); p += ln
    if p != 16 + secB:
        print(f"[gate] TDWS section mismatch p={p} expect={16+secB}"); return
    tdw_dict = d
    tdw_epoch = epoch
    have_snapshot = True
    tdw_synced = False          # need a streamStart TDW1 to (re)enter the zstd stream
    print(f"[gate] TDWS applied: epoch={epoch} blocks={nBlk}")

def decode_tdw1(msg):
    global tdw_stream, tdw_seq_next, tdw_synced, tdw_epoch
    epoch, flags = msg[4], msg[5]
    seq = struct.unpack_from("<H", msg, 6)[0]
    innerSize = struct.unpack_from("<I", msg, 8)[0]
    if not have_snapshot or epoch != tdw_epoch:
        return None                       # dict unknown for this epoch — wait for TDWS
    if flags & 1:                         # streamStart: fresh zstd session
        tdw_stream = zstd.ZstdDecompressor().decompressobj()
        tdw_seq_next = seq
        tdw_synced = True
    if not tdw_synced:
        return None
    if seq != tdw_seq_next:
        print(f"[gate] TDW1 seq gap {seq} != {tdw_seq_next} — desync, waiting for restart")
        tdw_synced = False
        return None
    tdw_seq_next = (tdw_seq_next + 1) & 0xFFFF
    inner = tdw_stream.decompress(msg[12:])
    if len(inner) != innerSize:
        print(f"[gate] inner size {len(inner)} != {innerSize} — desync")
        tdw_synced = False
        return None
    frameNum, vframe, taSize, nBlocks, newSection = struct.unpack_from("<IIIII", inner, 0)
    p = 20 + (132 if (flags & 8) else 0)   # bit3: in-band camera block (stage_id+M2+M1)
    refs_end = p + 4 * nBlocks
    np_, news_end = refs_end, refs_end + newSection
    out = bytearray()
    for i in range(nBlocks):
        bid = struct.unpack_from("<I", inner, p + 4 * i)[0]
        if bid == len(tdw_dict):
            ln = inner[np_]; np_ += 1
            tdw_dict.append(inner[np_:np_ + ln]); np_ += ln
        elif bid > len(tdw_dict):
            raise AssertionError(f"ref {bid} > dict {len(tdw_dict)}")
        out += tdw_dict[bid]
    assert np_ == news_end, "newBlocks length mismatch"
    assert len(out) == taSize, f"taSize {taSize} != rebuilt {len(out)}"
    return frameNum, bytes(out)

def try_compare():
    global compared, mismatches, first_mismatch_detail
    for fn in list(tdw_by_fn.keys()):
        if fn in legacy_by_fn:
            a, b = legacy_by_fn.pop(fn), tdw_by_fn.pop(fn)
            compared += 1
            if a != b:
                mismatches += 1
                if first_mismatch_detail is None:
                    d = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]),
                             min(len(a), len(b)))
                    first_mismatch_detail = f"frame {fn}: len {len(a)} vs {len(b)}, first diff @ {d}"
                    print(f"[gate] MISMATCH {first_mismatch_detail}")
            if compared % 300 == 0:
                dt = time.time() - t_start
                print(f"[gate] {compared}/{FRAMES_TARGET} compared, {mismatches} mismatches "
                      f"({compared/dt:.0f} fps, legacy={legacy_frames} tdw_msgs={tdw_msgs})")
    # bound the match buffers (frames whose sibling never arrives)
    for m in (legacy_by_fn, tdw_by_fn):
        while len(m) > 600:
            m.pop(next(iter(m)))

def on_message(ws, msg):
    global tdw_msgs, legacy_frames, t_start
    if not isinstance(msg, (bytes, bytearray)) or len(msg) < 8:
        return
    if t_start is None:
        t_start = time.time()
    magic = bytes(msg[:4])
    if magic == b"TDWS":
        # Own outer envelope: 'TDWS'(4) + usize(4) + zstd blob (NOT ZCST — a new
        # inner type under the ZCST outer corrupts legacy clients; 2026-07-14).
        usize = struct.unpack_from("<I", msg, 4)[0]
        if usize > 256 << 20:
            return
        try:
            apply_tdws(zcst_dctx.decompress(bytes(msg[8:]), max_output_size=usize))
        except zstd.ZstdError:
            print("[gate] TDWS decompress failed")
        return
    if magic == b"ZCST":
        usize = struct.unpack_from("<I", msg, 4)[0]
        if usize > 64 << 20:
            return
        try:
            raw = zcst_dctx.decompress(bytes(msg[8:]), max_output_size=usize)
        except zstd.ZstdError:
            return
        r = parse_legacy(raw)
        if r:
            legacy_frames += 1
            legacy_by_fn[r[0]] = r[1]
            try_compare()
    elif magic == b"TDW1":
        tdw_msgs += 1
        r = decode_tdw1(bytes(msg))
        if r:
            tdw_by_fn[r[0]] = r[1]
            try_compare()
    # everything else (GSTA/PALF/OBJS/ZCS2/audio/...) is ignored
    if compared >= FRAMES_TARGET:
        ws.close()

def main():
    print(f"[gate] target={FRAMES_TARGET} frames  url={URL}  (start me BEFORE the server)")
    deadline = time.time() + 600
    while time.time() < deadline:
        try:
            ws = websocket.WebSocketApp(URL, on_message=on_message)
            ws.run_forever()
            if compared >= FRAMES_TARGET:
                break
            print("[gate] socket closed early — reconnecting in 2s "
                  f"(compared={compared})")
        except Exception as e:
            print(f"[gate] connect failed ({e}) — retry in 2s")
        time.sleep(2)
    ok = compared >= FRAMES_TARGET and mismatches == 0
    print(f"[gate] RESULT: {'PASS' if ok else 'FAIL'} — {compared} frames compared, "
          f"{mismatches} mismatches"
          + (f" ({first_mismatch_detail})" if first_mismatch_detail else ""))
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
