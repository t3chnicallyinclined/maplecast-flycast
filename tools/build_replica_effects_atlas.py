#!/usr/bin/env python3
"""Build the render-replica EFFECTS atlas, keyed by the Effect-Poly DIRECTORY INDEX
(idx 0..N) — the GROUNDED binding the client resolves live from each effect node:

    dirBase = *(0x0CED0008)                       (read from the client RAM image)
    idx     = (node+0x15C - dirBase) / 0x10        (node's GFX base IS a dir-entry ptr)

This is NOT the unproven sprite_id binding (fx_atlas.json keymap note). The directory
itself is in the 16MB area-3 RAM the replica ships, so the client reads dirBase + each
entry's {w,h,fmt,texelPtr} live and only needs the PIXELS from this local atlas (the
texels are static for the match — a ROM-derived asset, gitignored, scp/local-cached).

INPUT
  --dir   effects-capture/mc_effects.log  (the directory dump: idx, wxh, e4 fmt, e8)
  --png   effects-capture/               (the offline-decoded efx_NNN.png, y-bit-first
                                          Morton de-twiddle already applied by decode_effects.py)
OUTPUT (web/render-replica/effects/, gitignored)
  effects.png   — all entries packed into a grid, padded to avoid bleed
  effects.json  — { screenW, screenH, name, image, dir:[ {idx,w,h,fmt,x,y} ] }
                  the client looks up dir[idx] -> rect; quad = point-centered on the
                  node anchor (dx=-w/2, dy=-h/2), size = (w,h) (the directory e0).

Effect textures are square (128/256). We pack them into a fixed 6-wide grid with 2px
pad. The packing is deterministic so the JSON rects are reproducible.
"""
import argparse, json, os, re
from PIL import Image

def parse_dir_log(path):
    """Parse mc_effects.log -> list of {idx,w,h,fmt}. fmt = (e4>>8)&0xff PVR code
       mapped to our atlas fmt name (informational; pixels already decoded)."""
    FMT = {0x00: "ARGB1555", 0x01: "RGB565", 0x02: "ARGB4444", 0x03: "PAL8"}
    # The log appends one block per capture-fire, so each idx repeats; keep the FIRST
    # (all fires read the same static directory). Dedup by idx, preserve sorted order.
    seen = {}
    for line in open(path):
        m = re.match(r'\[EFX\]\s+(\d+)\s+(\d+)x(\d+)\s+\w+\s+(\w+)', line)
        if not m:
            continue
        idx, w, h, e4 = int(m[1]), int(m[2]), int(m[3]), int(m[4], 16)
        # only the real texture directory entries (square, <=512); skip the trailing junk
        if idx > 64 or w == 0 or h == 0 or w > 512 or h > 512:
            continue
        if idx in seen:
            continue
        seen[idx] = {"idx": idx, "w": w, "h": h, "fmt": (e4 >> 8) & 0xff,
                     "fmt_name": FMT.get((e4 >> 8) & 0xff, "?")}
    return [seen[k] for k in sorted(seen)]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="effects-capture/mc_effects.log")
    ap.add_argument("--png", default="effects-capture")
    ap.add_argument("--out", default="web/render-replica/effects")
    ap.add_argument("--cols", type=int, default=6)
    ap.add_argument("--pad", type=int, default=2)
    args = ap.parse_args()

    entries = parse_dir_log(args.dir)
    # attach decoded image (prefer the plain efx_NNN.png; decode_effects.py output)
    have = []
    for e in entries:
        fn = os.path.join(args.png, f"efx_{e['idx']:03d}.png")
        if not os.path.exists(fn):
            print(f"  skip idx {e['idx']}: no {fn}")
            continue
        e["img"] = Image.open(fn).convert("RGBA")
        have.append(e)

    if not have:
        raise SystemExit("no decoded effect PNGs found")

    # deterministic grid pack: cell = max texture size, cols fixed
    cell = max(max(e["w"], e["h"]) for e in have) + args.pad
    cols = args.cols
    rows = (len(have) + cols - 1) // cols
    W, H = cols * cell, rows * cell
    atlas = Image.new("RGBA", (W, H), (0, 0, 0, 0))

    dir_out = []
    for i, e in enumerate(have):
        cx, cy = (i % cols) * cell, (i // cols) * cell
        atlas.paste(e["img"], (cx, cy))
        dir_out.append({"idx": e["idx"], "w": e["w"], "h": e["h"],
                        "fmt": e["fmt"], "fmt_name": e["fmt_name"],
                        "x": cx, "y": cy})

    os.makedirs(args.out, exist_ok=True)
    atlas.save(os.path.join(args.out, "effects.png"))
    meta = {
        "screenW": 640, "screenH": 480, "name": "effects",
        "image": "effects.png", "atlasW": W, "atlasH": H,
        # client binding (read live from RAM, not baked): idx=(node+0x15C-*(0x0CED0008))/0x10
        "binding": "idx = (node+0x15C - *(0x0CED0008)) / 0x10",
        "dir": dir_out,
    }
    json.dump(meta, open(os.path.join(args.out, "effects.json"), "w"), indent=1)
    print(f"packed {len(have)} effect textures -> {args.out}/effects.png ({W}x{H}), effects.json")
    print("dir idxs:", ",".join(str(e['idx']) for e in have))

if __name__ == "__main__":
    main()
