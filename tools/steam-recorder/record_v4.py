#!/usr/bin/env python3
"""record_v4.py <out.v4> [seconds] — Steam MvC2 frame recorder, plan S1 (V4BLOCK01).

ONE coherent whole-block read per frame:
  * poll the frame counter (blk+0x3CC8) with a 4-byte RPM (~2.5us)
  * on change, settle to edge + SETTLE_MS, then ONE ReadProcessMemory of the ENTIRE match block
  * take the frame number FROM THAT BUFFER (never a second call) -> mis-pairing is impossible
  * emit exactly one row per distinct fc, NEVER drop a row; a GAP flag marks any fc jump

Why the whole block and not just the slot span: the draw list (+0x300D0), the camera (+0x6914),
the object pool (+0x6DD8) and battle-globals (+0x32500) all live outside the slots, and reading
them in a second call re-creates flycast's S-vs-S-1 pairing bug. One RPM is ~3.4us for 11KB vs
2.5us for 4 bytes -- the syscall is the cost, the size is free.

DRAW GATE (the fix for every mis-render so far): a slot is drawn iff its OBJECT HANDLE
  H(i) = blk + 0x44F0 + i*0x738
appears somewhere in the block outside the fighter array (i.e. in a draw bucket). NEVER gate on
+0x004 (measured ANTI-correlated with being the point char) and never on coordinates.

File: b"V4BLOCK01", then per frame:
  [u32 fc][u8 drawn_mask][u8 gap][3f eyeX,eyeY,eyeZ][6 x SLOT]
  SLOT = <B cid, H sid, H hp, H red, B fac, f wx, f wy, f sx, f sy, I drawoff>
"""
import sys, os, time, struct
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mvcmem import Mem, anchors, EXE

EXE_PTR = EXE + 0xAC6EF0    # the match-block pointer; the block it names RELOCATES per match

BLKSZ    = 0x33B18
FC_OFF   = 0x3CC8          # frame counter, block-relative (== arr - 0x25C)
ARR_OFF  = 0x3F24
STRIDE   = 0x738
H_OFF    = 0x44F0          # object handle base: H(i) = blk + H_OFF + i*STRIDE
EYE_OFF  = 0x6914          # eyeX/eyeY/eyeZ f32 (verify per match via wx - sx + 320)
SETTLE_MS = 1.0

OFF_CID, OFF_SID, OFF_HP, OFF_RED = 0x554, 0x01C, 0x40C, 0x410
OFF_FAC, OFF_WX, OFF_WY, OFF_SX, OFF_SY = 0x720, 0x61C, 0x620, 0x6F0, 0x6F4
SLOT_FMT = "<BHHHBffffI"


def main():
    out_path = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    m = Mem()
    blk, arr = anchors(m)
    assert arr == blk + ARR_OFF, f"array {arr:#x} != blk+0x3F24 {blk + ARR_OFF:#x}"
    handles = np.array([(blk + H_OFF + i * STRIDE) & 0xFFFFFFFFFFFFFFFF for i in range(6)],
                       dtype=np.uint64)
    arr_lo, arr_hi = ARR_OFF, ARR_OFF + 6 * STRIDE          # exclude the array itself from the scan
    print(f"blk={blk:#x} arr={arr:#x} handles={[hex(int(h)) for h in handles]}")

    f = open(out_path, "wb")
    f.write(b"V4BLOCK01")
    n = gaps = 0
    last_fc = None
    t_end = time.time() + secs
    relocs = 0
    while time.time() < t_end:
        # ⚠ THE MATCH BLOCK RELOCATES (new match / rollback realloc). Re-read the pointer every
        # frame (8 bytes) and re-anchor if it moved — a stale blk silently yields incoherent rows
        # (slots that animate but never move, and vice versa).
        cur = m.u64(EXE_PTR)
        if cur and cur != blk and 0x10000 < cur < 0x7FFFFFFFFFFF:
            blk = cur
            handles = np.array([(blk + H_OFF + i * STRIDE) & 0xFFFFFFFFFFFFFFFF for i in range(6)],
                               dtype=np.uint64)
            relocs += 1
            last_fc = None
        fc = m.u32(blk + FC_OFF)
        if fc is None or fc == last_fc:
            continue                                          # tight poll; no sleep (~2.5us/read)
        edge = time.perf_counter()
        while (time.perf_counter() - edge) * 1000.0 < SETTLE_MS:
            pass                                              # settle: let the tick finish writing
        buf = m.read(blk, BLKSZ)                              # THE one read
        if buf is None or len(buf) < BLKSZ:
            continue
        bfc = struct.unpack_from("<I", buf, FC_OFF)[0]        # frame number FROM THE BUFFER

        gap = 0
        if last_fc is not None and bfc != last_fc + 1:
            gap = 1
            gaps += 1
        last_fc = bfc

        # draw list: where does each slot's handle appear, outside the fighter array?
        u64 = np.frombuffer(buf[: (BLKSZ // 8) * 8], dtype=np.uint64)
        drawoff = [0] * 6
        mask = 0
        for i in range(6):
            for j in np.flatnonzero(u64 == handles[i]):
                off = int(j) * 8
                if not (arr_lo <= off < arr_hi):              # a bucket, not the struct itself
                    drawoff[i] = off
                    mask |= (1 << i)
                    break

        eye = struct.unpack_from("<fff", buf, EYE_OFF)
        rec = struct.pack("<IBB", bfc, mask, gap) + struct.pack("<fff", *eye)
        for i in range(6):
            b = ARR_OFF + i * STRIDE
            rec += struct.pack(
                SLOT_FMT,
                buf[b + OFF_CID],
                struct.unpack_from("<H", buf, b + OFF_SID)[0],
                struct.unpack_from("<I", buf, b + OFF_HP)[0] & 0xFFFF,
                struct.unpack_from("<H", buf, b + OFF_RED)[0],
                buf[b + OFF_FAC],
                struct.unpack_from("<f", buf, b + OFF_WX)[0],
                struct.unpack_from("<f", buf, b + OFF_WY)[0],
                struct.unpack_from("<f", buf, b + OFF_SX)[0],
                struct.unpack_from("<f", buf, b + OFF_SY)[0],
                drawoff[i],
            )
        f.write(rec)
        n += 1
    f.close()
    print(f"recorded {n} frames ({gaps} gaps, {relocs} block relocations) -> {out_path}")


if __name__ == "__main__":
    main()
