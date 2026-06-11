#!/usr/bin/env python3
"""
rip_extras_hotspot.py — offline EXTRAS bounding-box hotspot ripper (projectile/object
anchor fix for the whole-sprite client).

THE PROBLEM IT FIXES
--------------------
The whole-sprite bake (web/webgpu/bake.mjs) anchors every sprite with
`dx = crop_left - BODY.screen_x`. That is correct for character BODIES (their own
origin == the body), but WRONG for satellite objects / projectiles whose own origin
is NOT the body. The TRUE per-sprite anchor is the bounding box (min dx, min dy) over
that sprite's EXTRAS / OAM assembly parts (marvelous2 quad emitter loc_8c033e90;
docs/MARVELOUS2-GFX-NOTES.md sec 3/6). For bodies the EXTRAS bbox == the body anchor
(unchanged); for objects it shifts them to the correct spot.

DATA SOURCES (all offline, ROM-derived — outputs are NOT committed)
-------------------------------------------------------------------
For each character PLxx we need two segments out of the disc's dev files:
  * EXTRAS_DATA  — the 8-byte OAM assembly records. Lives at the end of PLxxPAK.BIN,
                   pointed to by header +0x0C (runs from that pointer to EOF).
  * ANIMATION    — the sprite_id -> extras_slot map. This is the PLxx_TBL.BIN file
                   (a.k.a. the DAT's ANIMATION segment, header +0x14). 0x000-0x3FF is
                   the header; 0x400+ is a u32-LE table of per-group keyframe-list
                   offsets. Each keyframe is 20 bytes: sprite_id @+4 (u16),
                   ender @+3 (bit 0x80 ends the group), extras_slot @+0x12 (u16).

  (The repo's dasm_PLDAT/PLDATs/*.BIN are the SAME PAK files renamed *_DAT.BIN — they
   are STRIPPED: only GFX/PAL/EXTRAS are present, the ANIMATION/HITBOX/ATTACK header
   pointers are 0. That is why the old dasm_PLDAT decoder produced 0-byte EXTRAS /
   89-byte ANIMATION: its pairwise "walk pointers" logic mis-paired the EXTRAS start
   with a stray low pointer at header +0x20, yielding a negative length. The real
   ANIMATION for those files lives in the sibling _TBL.BIN. See rip_pldat_segments.py
   for the corrected DAT-header segmenter.)

EXTRAS record layout (8 bytes) — CONFIRMED byte-exact vs atlas/chars/PL2A_asm.json:
  [dx:s16 @+0][dy:s16 @+2][part:u8 @+4][b5:u8 @+5][mode:u8 @+6][flip:u8 @+7]
  mode == 0xFF  -> assembly terminator. all-zero record -> separator.
  An assembly = EXTRAS + slot*0x400, records start at +0x08 (the first 8 bytes are a
  0x00FF-terminated header).

HOTSPOT
-------
hotspot(sprite_id) = { dx: min(rec.dx), dy: min(rec.dy) } over that sprite's assembly.

USAGE
  # verify the PL2A rip matches the known-good runtime rip (correctness gate):
  python3 tools/rip_extras_hotspot.py --verify-pl2a \
      --dev "C:/Users/trist/Downloads/MVC2 Dev Files/MVC2 Dev Files"

  # rip every char -> JSON of {sprite_id: {dx,dy}} per char:
  python3 tools/rip_extras_hotspot.py --all --out /tmp/hotspots \
      --dev "C:/Users/trist/Downloads/MVC2 Dev Files/MVC2 Dev Files"
"""
import argparse, json, os, struct, sys

SLOT_STRIDE = 0x400
REC = 8


def load_char(devdir, name):
    """Return (extras_bytes, anim_bytes) for a char like 'PL2A'."""
    pak = os.path.join(devdir, name + "PAK.BIN")
    tbl = os.path.join(devdir, name + "_TBL.BIN")
    if not (os.path.exists(pak) and os.path.exists(tbl)):
        return None, None
    pd = open(pak, "rb").read()
    ext_ptr = struct.unpack_from("<I", pd, 0x0C)[0]
    extras = pd[ext_ptr:]                     # EXTRAS runs from its ptr to EOF in the PAK
    anim = open(tbl, "rb").read()
    return extras, anim


def slot_records(extras, slot):
    base = slot * SLOT_STRIDE
    if base + REC > len(extras):
        return []
    recs = []
    pos = base + REC                          # skip the header record
    end = base + SLOT_STRIDE
    while pos + REC <= len(extras) and pos < end:
        dx, dy = struct.unpack_from("<hh", extras, pos)
        part = extras[pos + 4]; b5 = extras[pos + 5]
        mode = extras[pos + 6]; flip = extras[pos + 7]
        if mode == 0xFF:
            break
        if dx == 0 and dy == 0 and part == 0 and b5 == 0 and mode == 0 and flip == 0:
            break
        recs.append({"dx": dx, "dy": dy, "part": part, "flip": flip & 0x80})
        pos += REC
    return recs


def slot_hotspot(extras, slot):
    recs = slot_records(extras, slot)
    if not recs:
        return None
    return {"dx": min(r["dx"] for r in recs), "dy": min(r["dy"] for r in recs),
            "nrecs": len(recs)}


def sid_to_slot(anim):
    """Walk the ANIMATION keyframe lists -> {sprite_id: extras_slot} (first occ wins)."""
    out = {}
    # u32-LE per-group offset table at 0x400 (entries until they run past the buffer)
    i = 0x400
    ptrs = []
    while i + 4 <= len(anim):
        ptrs.append(struct.unpack_from("<I", anim, i)[0]); i += 4
    for go in ptrs:
        if go == 0 or go >= len(anim):
            continue
        pos = go
        for _ in range(256):                  # generous keyframe cap per group
            if pos + 20 > len(anim):
                break
            sid = struct.unpack_from("<H", anim, pos + 4)[0]
            slot = struct.unpack_from("<H", anim, pos + 0x12)[0]
            ender = anim[pos + 3]
            if sid not in out:
                out[sid] = slot
            pos += 20
            if ender & 0x80:
                break
    return out


def rip_char(devdir, name):
    """Return {sprite_id(int): {dx,dy}} EXTRAS-bbox hotspots for a char."""
    extras, anim = load_char(devdir, name)
    if extras is None:
        return None
    s2s = sid_to_slot(anim)
    hot = {}
    for sid, slot in s2s.items():
        h = slot_hotspot(extras, slot)
        if h is not None:
            hot[sid] = {"dx": h["dx"], "dy": h["dy"]}
    return hot


def verify_pl2a(devdir, repo):
    """Gate: ripped PL2A sid hotspots must match atlas/chars/PL2A_asm.json."""
    known = json.load(open(os.path.join(repo, "atlas", "chars", "PL2A_asm.json")))
    asm = known["assemblies"]                 # sid-keyed live rip (the oracle)
    hot = rip_char(devdir, "PL2A")
    if hot is None:
        print("PL2A dev files not found", file=sys.stderr); return False
    ok = bad = miss = 0
    for sid_s, recs in asm.items():
        sid = int(sid_s)
        kdx = min(r["dx"] for r in recs); kdy = min(r["dy"] for r in recs)
        if sid not in hot:
            miss += 1; continue
        if hot[sid]["dx"] == kdx and hot[sid]["dy"] == kdy:
            ok += 1
        else:
            bad += 1
            print("  MISMATCH sid %d: ripped %s vs known {dx:%d,dy:%d}"
                  % (sid, hot[sid], kdx, kdy))
    print("PL2A verify: ok=%d bad=%d miss=%d (of %d known sids); %d total sids ripped"
          % (ok, bad, miss, len(asm), len(hot)))
    # PASS = zero mismatches and the great majority match (272/273 are live-only
    # effect sids absent from the animation keyframe table -> counted as miss, not bad).
    return bad == 0 and ok >= len(asm) - 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", required=True, help="MVC2 Dev Files dir (PLxxPAK.BIN + PLxx_TBL.BIN)")
    ap.add_argument("--out", help="output dir for per-char PLxx_hotspots.json")
    ap.add_argument("--all", action="store_true", help="rip all 59 chars")
    ap.add_argument("--char", help="single char, e.g. 2A")
    ap.add_argument("--verify-pl2a", action="store_true")
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = ap.parse_args()

    if args.verify_pl2a:
        sys.exit(0 if verify_pl2a(args.dev, args.repo) else 1)

    names = []
    if args.all:
        names = ["PL%02X" % i for i in range(0x3B)]
    elif args.char:
        names = ["PL" + args.char.upper()]
    else:
        ap.error("need --all or --char or --verify-pl2a")

    if args.out:
        os.makedirs(args.out, exist_ok=True)
    total = 0
    for name in names:
        hot = rip_char(args.dev, name)
        if hot is None:
            print("%s: MISSING dev files" % name); continue
        total += 1
        print("%s: %d sprite_id hotspots" % (name, len(hot)))
        if args.out:
            with open(os.path.join(args.out, name + "_hotspots.json"), "w") as f:
                json.dump({str(k): v for k, v in sorted(hot.items())}, f, indent=1)
    print("ripped %d chars" % total)


if __name__ == "__main__":
    main()
