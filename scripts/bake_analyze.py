#!/usr/bin/env python3
"""
bake_analyze.py -- P0 verification for the sprite bake harness.

Consumes bake.csv produced by the MAPLECAST_BAKE probe in Renderer_if.cpp.
Verifies "Layer B": the same (char_id, sprite_id) renders IDENTICAL pixels
every time it occurs at the same on-screen position + camera.

Method: group rows by (slot, char_id, sprite_id, screen_x, screen_y,
camera_x, camera_y). Within each group every frame is the same character in
the same pose at the same place over the same background, so the crop hash
MUST be constant. Groups that recur (>=2 frames) and stay single-hash are
PASS; recurring groups with multiple hashes FALSIFY the stability claim.

Usage:
  python scripts/bake_analyze.py <bakedir>           # analyze bake.csv
  python scripts/bake_analyze.py <bakedir> --png     # also convert crop_*.bin -> png
"""
import sys, os, csv, struct, zlib
from collections import defaultdict

if len(sys.argv) < 2:
    print("usage: python scripts/bake_analyze.py <bakedir> [--png]")
    sys.exit(1)
bakedir = sys.argv[1]
do_png = "--png" in sys.argv[2:]
csv_path = os.path.join(bakedir, "bake.csv")

rows = []
with open(csv_path, newline="") as f:
    for r in csv.DictReader(f):
        if r.get("crop_hash"):
            rows.append(r)
print(f"bake rows: {len(rows)}")
if not rows:
    print("No rows. Was MAPLECAST_BAKE set and a match on screen?")
    sys.exit(0)

# --- the verification ---
# key = same char, same sprite, same position + camera -> pixels must match.
groups = defaultdict(lambda: defaultdict(int))   # key -> {hash: count}
per_char = defaultdict(lambda: defaultdict(int))  # char -> {sprite: count}
for r in rows:
    key = (r["slot"], r["char_id"], r["sprite_id"],
           r["screen_x"], r["screen_y"], r["camera_x"], r["camera_y"])
    groups[key][r["crop_hash"]] += 1
    per_char[r["char_id"]][r["sprite_id"]] += 1

# Only groups that actually recurred test anything.
recurring = {k: h for k, h in groups.items() if sum(h.values()) >= 2}
stable    = {k: h for k, h in recurring.items() if len(h) == 1}
unstable  = {k: h for k, h in recurring.items() if len(h) > 1}

print()
print("=" * 72)
print("PER-CHARACTER WORKING SET")
print("=" * 72)
for cid in sorted(per_char, key=lambda x: int(x)):
    sprites = per_char[cid]
    print(f"char_id {int(cid):3} : {len(sprites):4} unique sprite_ids, "
          f"{sum(sprites.values())} frames")

print()
print("=" * 72)
print("LAYER B VERIFICATION  -- same (char,sprite,pos,camera) -> same pixels?")
print("=" * 72)
print(f"distinct (char,sprite,pos,camera) groups : {len(groups)}")
print(f"  ...that recurred (>=2 frames)          : {len(recurring)}")
print(f"  ...STABLE (single pixel hash)          : {len(stable)}")
print(f"  ...UNSTABLE (multiple hashes!)         : {len(unstable)}")
if recurring:
    pct = 100.0 * len(stable) / len(recurring)
    print(f"\n  PIXEL-STABILITY: {pct:.2f}% of recurring groups are byte-identical")
    if pct >= 99.5:
        print("  ==> GO. (char_id, sprite_id) is a stable pixel key. Theory confirmed.")
    elif pct >= 90:
        print("  ==> MOSTLY. Inspect the unstable groups below -- likely a confound")
        print("      (animated stage bg, super flash, shadow) not the sprite itself.")
    else:
        print("  ==> RESHAPE. sprite_id alone does not determine pixels; the unstable")
        print("      groups tell us what else drives the image.")
else:
    print("\n  No group recurred -- the character never held the same sprite at the")
    print("  same position twice. Re-capture standing IDLE in training so a sprite")
    print("  loop repeats in place.")

if unstable:
    print()
    print("--- sample UNSTABLE groups (same state, different pixels) ---")
    for k, h in list(unstable.items())[:8]:
        slot, cid, sp, sx, sy, cx, cy = k
        print(f"  char {cid} sprite 0x{int(sp):04X} @ pos({sx},{sy}) cam({cx},{cy}): "
              f"{len(h)} distinct hashes over {sum(h.values())} frames")

# --- optional: convert saved crops to PNG for eyeballing ---
if do_png:
    def write_png(path, W, H, rgb):
        def chunk(t, d):
            c = t + d
            return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
        raw = bytearray()
        for y in range(H):
            raw.append(0); raw += rgb[y*W*3:(y+1)*W*3]
        open(path, "wb").write(
            b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b""))
    n = 0
    for fn in os.listdir(bakedir):
        if fn.startswith("crop_") and fn.endswith(".bin"):
            data = open(os.path.join(bakedir, fn), "rb").read()
            w, h = struct.unpack_from("<II", data, 0)
            rgb = data[8:8 + w*h*3]
            if len(rgb) >= w*h*3:
                write_png(os.path.join(bakedir, fn[:-4] + ".png"), w, h, rgb)
                n += 1
    print(f"\nwrote {n} crop PNG(s) into {bakedir}")
