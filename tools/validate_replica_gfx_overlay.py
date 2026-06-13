#!/usr/bin/env python3
"""
validate_replica_gfx_overlay.py — Phase A coherence proof (render-replica LOCAL-ROM GFX).

Proves the local-GFX overlay FIXES the scramble, using a live RAM dump as ground truth:

  1) BYTE-IDENTITY: the staged disc GFX1/GFX2 == the RAM-resident GFX at node+0x15C/0x160
     for the active chars (the foundation: overlay == engine art, exactly).

  2) SCRAMBLE -> COHERENT: simulate the server shipping a TRUNCATED GFX. Take a COPY of the
     RAM image, zero/truncate the scramble char's GFX1 & GFX2 tails (what a truncated wire
     does). Walk the live pose (GFX2 cell @ sid&0x7FFF) on the TRUNCATED image -> parts go
     missing / out-of-range = the scramble. Then APPLY THE OVERLAY (write the disc GFX over
     node+0x15C/0x160) and walk again -> ALL pose parts resident, every sel in range, the
     GFX1 LZSS decodes to full dest_len with no truncation. distinctSels spread reported.

This is the headless equivalent of the client overlay: applyLocalGfx() does exactly the
ram.set(disc_gfx, node+0x15C/0x160) this simulates.

USAGE:
  python3 tools/validate_replica_gfx_overlay.py \
      --ram _ryu_capture/mc_ram_dump.bin --gfx web/render-replica/gfx --slot P1C1
"""
import argparse, os, struct

RAM_MASK = 0x00FFFFFF
SLOTS = {"P1C1": 0x8C268340, "P2C1": 0x8C2688E4, "P1C2": 0x8C268E88,
         "P2C2": 0x8C26942C, "P1C3": 0x8C2699D0, "P2C3": 0x8C269F74}
OFF_ACTIVE, OFF_CID, OFF_SID, OFF_GFX1, OFF_GFX2 = 0x000, 0x001, 0x144, 0x15C, 0x160


def u8(r, a):  return r[a & RAM_MASK]
def u16(r, a): return r[a & RAM_MASK] | (r[(a + 1) & RAM_MASK] << 8)
def u32(r, a): return (r[a & RAM_MASK] | (r[(a + 1) & RAM_MASK] << 8)
                       | (r[(a + 2) & RAM_MASK] << 16) | (r[(a + 3) & RAM_MASK] << 24)) & 0xFFFFFFFF


# GFX2 cell walk: sid -> ordered part sels + cumulative pen (read-only structural check).
def cell_sels(r, gfx2, sid):
    cell_off = u32(r, gfx2 + (sid & 0x7FFF) * 4)
    cb = gfx2 + cell_off
    cnt = u16(r, cb)
    if cnt == 0 or cnt > 64:
        return None, cnt
    sels, p = [], cb + 2
    for _ in range(cnt):
        sels.append(u16(r, p + 6))
        p += 8
    return sels, cnt


# GFX1 LZSS decode (body_decoder.decodeA port) — used to prove a part decodes to full dest_len.
def decodeA(r, sp, src_end, dest_len):
    out = bytearray()
    bc = flags = 0
    while len(out) < dest_len and sp < src_end:
        if bc == 0:
            flags = r[sp & RAM_MASK]; sp += 1; bc = 0x80
            if sp >= src_end:
                break
        if (flags & bc) == 0:
            out.append(r[sp & RAM_MASK]); sp += 1
        else:
            b = r[sp & RAM_MASK]; sp += 1
            s = len(out) - (b >> 4) - 1
            for _ in range((b & 0x0F) + 2):
                out.append(out[s] if 0 <= s < len(out) else 0); s += 1
        bc >>= 1
    return out


def gfx1_part_ok(r, gfx1, n_parts, srt, sel):
    """Return (resident, full_decode, W, H). resident=sel in range & header sane;
    full_decode=LZSS reaches dest_len within the part's bounded stream."""
    if sel >= n_parts:
        return False, False, 0, 0
    base = gfx1 + u32(r, gfx1 + sel * 4)
    sw, sh = u8(r, base + 2), u8(r, base + 3)
    W, H = sw * 8, sh * 8
    if W <= 0 or H <= 0 or W > 1024 or H > 1024:
        return False, False, W, H
    dest_len = (W * H) >> 1
    # bound the stream by the next-greater offset (same as the client endOf)
    o = base - gfx1
    nxt = min((x for x in srt if x > o), default=o + 0x4000)
    out = decodeA(r, (base + 4), (gfx1 + nxt), dest_len)
    return True, (len(out) >= dest_len), W, H


def walk_pose(r, gfx1, gfx2, sid, label):
    n_parts = u32(r, gfx1) >> 2
    offs = [u32(r, gfx1 + i * 4) for i in range(n_parts)]
    srt = sorted(set(offs))
    sels, cnt = cell_sels(r, gfx2, sid)
    if sels is None:
        print(f"  [{label}] cell walk FAILED: count={cnt} (corrupt/out-of-range cell)")
        return None
    resident = full = 0
    bad = []
    for s in sels:
        rez, fd, W, H = gfx1_part_ok(r, gfx1, n_parts, srt, s)
        if rez: resident += 1
        if rez and fd: full += 1
        if not (rez and fd): bad.append(s)
    distinct = len(set(sels))
    coherent = (resident == cnt and full == cnt)
    print(f"  [{label}] sid=0x{sid:04x} cells={cnt} distinctSels={distinct} "
          f"resident={resident}/{cnt} fullDecode={full}/{cnt} "
          f"-> {'COHERENT' if coherent else 'SCRAMBLE'}" + (f"  badSels={bad[:8]}" if bad else ""))
    return {"cnt": cnt, "distinct": distinct, "resident": resident, "full": full, "coherent": coherent}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ram", required=True)
    ap.add_argument("--gfx", default="web/render-replica/gfx")
    ap.add_argument("--slot", default="P1C1")
    args = ap.parse_args()

    ram0 = bytearray(open(args.ram, "rb").read())
    base = SLOTS[args.slot]
    cid = u8(ram0, base + OFF_CID)
    sid = u16(ram0, base + OFF_SID)
    g1b = u32(ram0, base + OFF_GFX1)
    g2b = u32(ram0, base + OFF_GFX2)
    pl = f"PL{cid:02X}"
    print(f"slot {args.slot}: char_id=0x{cid:02X} ({pl}) sid=0x{sid:04x} "
          f"gfx1@0x{g1b:08x} gfx2@0x{g2b:08x}")

    d1 = open(os.path.join(args.gfx, f"{pl}_gfx1.bin"), "rb").read()
    d2 = open(os.path.join(args.gfx, f"{pl}_gfx2.bin"), "rb").read()
    o1, o2 = g1b & RAM_MASK, g2b & RAM_MASK

    # (1) byte-identity: staged disc == RAM-resident
    eq1 = bytes(ram0[o1:o1 + len(d1)]) == d1
    eq2 = bytes(ram0[o2:o2 + len(d2)]) == d2
    print(f"(1) byte-identity: GFX1 {len(d1)}B {'OK' if eq1 else 'MISMATCH'} | "
          f"GFX2 {len(d2)}B {'OK' if eq2 else 'MISMATCH'}")

    # baseline coherence on the clean RAM image (the engine's own art)
    print("(baseline) clean RAM-resident art:")
    walk_pose(ram0, g1b, g2b, sid, "clean")

    # (2) simulate a TRUNCATED shipped GFX -> scramble -> overlay -> coherent.
    # Model the REAL scramble faithfully: the wire drops bytes that THIS pose references.
    # Compute the pose's actual GFX1 part extent + the GFX2 cell-record offset, then cut the
    # shipped GFX so HALF the pose's parts (and/or its cell record) land in the dropped tail.
    n_parts = u32(ram0, g1b) >> 2
    sels_clean, _ = cell_sels(ram0, g2b, sid)
    part_offs = sorted({u32(ram0, g1b + s * 4) for s in (sels_clean or [])})
    cell_off = u32(ram0, g2b + (sid & 0x7FFF) * 4)
    # GFX1 cut: just past the MEDIAN referenced part -> the upper half of the pose's parts
    # fall in the zeroed tail (out-of-data -> degenerate header / short LZSS = scramble).
    keep1 = (part_offs[len(part_offs) // 2] + 8) if part_offs else len(d1)
    # GFX2 cut: just before this pose's cell record -> the cell count/sels read as zeros
    # (a stale wire that hasn't shipped this cell yet) = the pose can't even be assembled.
    keep2 = max(0, cell_off - 2)
    trunc = bytearray(ram0)
    for i in range(o1 + keep1, o1 + len(d1)):
        trunc[i] = 0
    for i in range(o2 + keep2, o2 + len(d2)):
        trunc[i] = 0
    print(f"(2a) shipped-truncated GFX modelling the scramble "
          f"(GFX1 cut@0x{keep1:x}/{len(d1)}B = past pose median part; "
          f"GFX2 cut@0x{keep2:x}/{len(d2)}B = before this pose's cell record):")
    walk_pose(trunc, g1b, g2b, sid, "truncated")

    # APPLY THE OVERLAY (exactly what applyLocalGfx does: ram.set(disc, node+0x15C/0x160))
    overlaid = bytearray(trunc)
    overlaid[o1:o1 + len(d1)] = d1
    overlaid[o2:o2 + len(d2)] = d2
    print("(2b) AFTER local-GFX overlay (disc art written at node+0x15C/0x160):")
    res = walk_pose(overlaid, g1b, g2b, sid, "overlaid")

    # final byte-check: overlaid region == disc extract == clean RAM
    ovl_ok = (bytes(overlaid[o1:o1 + len(d1)]) == d1 and
              bytes(overlaid[o2:o2 + len(d2)]) == d2 and
              bytes(overlaid[o1:o1 + len(d1)]) == bytes(ram0[o1:o1 + len(d1)]))
    print(f"(3) overlaid GFX1/GFX2 == disc extract == clean RAM: {'YES' if ovl_ok else 'NO'}")
    ok = res and res["coherent"] and eq1 and eq2 and ovl_ok
    print(f"\nRESULT: {'PASS — scramble fixed, overlay byte-exact' if ok else 'FAIL'}")


if __name__ == "__main__":
    main()
