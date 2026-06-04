#!/usr/bin/env python3
"""
analyze-statelog.py -- ROM-asset-client feasibility analysis.

Consumes the CSV produced by the MAPLECAST_STATELOG probe in
core/network/maplecast_mirror.cpp (serverPublish). Answers the two
go/no-go questions for the ROM-asset streaming client:

  1. WORKING SET  -- how many distinct sprites does a character actually
     use in a match? Is it a bounded, preloadable set that recurs?
  2. KEY DETERMINISM -- does the live game state (anim_state, anim_timer)
     consistently select the SAME sprite_id every time? If yes, the
     253-byte state is a sufficient key to pick a preloaded sprite.

Usage:
  python scripts/analyze-statelog.py <statelog.csv>
"""
import sys, csv
from collections import defaultdict, Counter

if len(sys.argv) < 2:
    print("usage: python scripts/analyze-statelog.py <statelog.csv>")
    sys.exit(1)

rows = []
with open(sys.argv[1], newline="") as f:
    for r in csv.DictReader(f):
        rows.append(r)

# Only analyze in-match frames; menus/loading produce junk sprite ids.
inmatch = [r for r in rows if r["in_match"] == "1"]
print(f"total rows: {len(rows)}   in-match rows: {len(inmatch)}")
if not inmatch:
    print("No in-match frames captured. Make sure you were IN A MATCH while logging.")
    sys.exit(0)

frames = sorted(set(int(r["frame"]) for r in inmatch))
span = frames[-1] - frames[0] + 1 if frames else 0
print(f"in-match frame span: {span} frames (~{span/60:.1f}s at 60fps)\n")

by_char = defaultdict(list)
for r in inmatch:
    by_char[int(r["char_id"])].append(r)

print("=" * 72)
print("(1) WORKING SET  -- unique sprites per character (preloadable?)")
print("=" * 72)
for cid in sorted(by_char):
    rs = by_char[cid]
    sprites = Counter(int(r["sprite_id"]) for r in rs)
    anims   = set(int(r["anim_state"]) for r in rs)
    pals    = set(int(r["palette"]) for r in rs)
    reuse = len(rs) / max(len(sprites), 1)
    print(f"char_id 0x{cid:02X} ({cid:3}): {len(rs):5} frames | "
          f"{len(sprites):4} unique sprite_ids | {len(anims):3} anim_states | "
          f"{len(pals)} palettes | avg reuse {reuse:.1f}x")
    top = sprites.most_common(5)
    print("      most-used sprites: " +
          ", ".join(f"0x{s:04X}({n})" for s, n in top))

print()
print("=" * 72)
print("(2) KEY DETERMINISM -- does (anim_state, anim_timer) -> ONE sprite_id?")
print("=" * 72)
# If the same (char, anim_state, anim_timer) ever maps to >1 sprite_id, the
# 253-byte state is NOT a sufficient key on its own.
for cid in sorted(by_char):
    key_to_sprites = defaultdict(set)
    for r in by_char[cid]:
        key = (int(r["anim_state"]), int(r["anim_timer"]))
        key_to_sprites[key].add(int(r["sprite_id"]))
    total = len(key_to_sprites)
    ambiguous = {k: v for k, v in key_to_sprites.items() if len(v) > 1}
    pct = 100.0 * (total - len(ambiguous)) / max(total, 1)
    verdict = "DETERMINISTIC" if not ambiguous else f"{len(ambiguous)} AMBIGUOUS keys"
    print(f"char_id 0x{cid:02X}: {total:5} distinct (anim_state,timer) keys -> "
          f"{pct:.1f}% map to a single sprite_id  [{verdict}]")
    for k, v in list(ambiguous.items())[:3]:
        print(f"      e.g. anim_state=0x{k[0]:04X} timer={k[1]} -> sprites "
              + ", ".join(f"0x{s:04X}" for s in sorted(v)))

print()
print("=" * 72)
print("VERDICT GUIDE")
print("=" * 72)
print("""  GO if:  unique sprite_ids per char is bounded (hundreds, not growing
          every frame) AND key determinism is ~100%.
          -> preload the per-char sprite set, stream the 253B state,
             look up sprite by (char_id, anim_state, anim_timer).
  RESHAPE if: sprite count keeps climbing (not a closed set) or many
          ambiguous keys (state alone can't pick the sprite -> need
          more state bytes, e.g. anim_ptr or sub_anim_phase).""")
