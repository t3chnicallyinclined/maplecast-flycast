#!/usr/bin/env python3
"""Frame Oracle capture analyzer.

Reads a /dev/shm/mc_oracle.jsonl capture (per docs/FRAME-ORACLE-SPEC.md) and
reports the draw-structure breakdown that drives the per-object attribution
filter in core/network/maplecast_mirror.cpp (MAPLECAST_FRAME_ORACLE block):

  * quad size distribution (separates character parts from stage/backdrop)
  * texId recurrence (recurring opaque/large texIds == stage/bg layers)
  * blend-mode mix ([1,0] opaque stage/HUD vs [4,5] translucent char sprite
    vs [4,1] additive effect)
  * VRAM region (addr = (tcw & 0x1FFFFF) << 3)
  * the sprite-survivor count after the same filter the C++ applies, and how
    tightly survivor centers cluster around their object's screen_xy.

The .jsonl is ROM-derived (real MVC2 draw lists) and is gitignored. This
script is NOT — it is the reproducible tool that justified the filter
thresholds (textured & !tiled & size<=200 & !opaque[1,0]; radius 120px).

Usage:  python _oracle/analyze_oracle.py [path-to.jsonl]
"""
import json
import sys
import collections

PATH = sys.argv[1] if len(sys.argv) > 1 else "_oracle/mc_oracle.jsonl"

# --- filter thresholds (must match the C++ classifier) ---
SIZE_MAX = 200.0          # w or h above this -> stage backdrop / parallax
U_LO, U_HI = -0.05, 1.05  # outside -> page-tiled scrolling background
RADIUS = 120.0            # attribution proximity radius (px)


def classify(q):
    """Return 'untex' | 'tiled' | 'oversized' | 'opaque' | 'sprite'."""
    w, h, t = q["w"], q["h"], q["texId"]
    u, v, bl = q["u"], q["v"], tuple(q["blend"])
    if t == "00000000":
        return "untex"
    if u[0] < U_LO or u[1] > U_HI or v[0] < U_LO or v[1] > U_HI:
        return "tiled"
    if w > SIZE_MAX or h > SIZE_MAX:
        return "oversized"
    if bl == (1, 0):
        return "opaque"
    return "sprite"


def main():
    cat = collections.Counter()
    tex_frames = collections.defaultdict(set)
    tex_med = collections.defaultdict(list)
    blends = collections.Counter()
    survivor_dist = []
    frames = 0
    nq_tot = sprite_tot = 0
    seen_frames = set()

    for line in open(PATH):
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        frames += 1
        seen_frames.add(d["frame"])
        nq_tot += d.get("nq", 0)
        for o in d.get("objects", []):
            sx, sy = o["screen_xy"]
            hdx = o.get("asm_src", {}).get("hotspot_dx", 0)
            hdy = o.get("asm_src", {}).get("hotspot_dy", 0)
            for q in o["quads"]:
                c = classify(q)
                cat[c] += 1
                blends[tuple(q["blend"])] += 1
                if q["texId"] != "00000000":
                    tex_frames[q["texId"]].add(d["frame"])
                    tex_med[q["texId"]].append(max(q["w"], q["h"]))
                if c == "sprite":
                    sprite_tot += 1
                    cx = q["x"] + q["w"] / 2.0
                    cy = q["y"] + q["h"] / 2.0
                    dd = ((cx - (sx + hdx)) ** 2 + (cy - (sy + hdy)) ** 2) ** 0.5
                    survivor_dist.append(dd)

    nf = len(seen_frames)
    print("frames: %d  distinct: %d  sum nq: %d  avg nq/frame: %.1f"
          % (frames, nf, nq_tot, nq_tot / max(frames, 1)))
    print()
    print("--- quad classification (of attributed quads in capture) ---")
    for k, val in cat.most_common():
        print("  %-12s %d" % (k, val))
    print()
    print("--- blend mix [src,dst] ---")
    for k, val in blends.most_common():
        print("  %s : %d" % (k, val))
    print()
    print("--- texId recurrence (frames-seen %, median size) ---")
    print("  %-10s %7s %7s %s" % ("texId", "freq%", "medSz", "n"))
    rows = []
    for t, fs in tex_frames.items():
        szs = sorted(tex_med[t])
        rows.append((len(fs), t, szs[len(szs) // 2], len(szs)))
    for nfr, t, med, n in sorted(rows, reverse=True)[:20]:
        tag = " <- recurring/large = STAGE" if (nfr / nf > 0.10 or med > 200) else ""
        print("  %-10s %6.1f%% %7d %d%s" % (t, 100 * nfr / nf, med, n, tag))
    print()
    if survivor_dist:
        survivor_dist.sort()
        n = len(survivor_dist)
        print("--- sprite survivors (post-filter): %d (%.1f%% of attributed) ---"
              % (n, 100 * sprite_tot / max(sum(cat.values()), 1)))
        print("  center->screen_xy dist: med=%.0f p75=%.0f p90=%.0f max=%.0f"
              % (survivor_dist[n // 2], survivor_dist[int(n * .75)],
                 survivor_dist[int(n * .9)], survivor_dist[-1]))
        within = sum(1 for x in survivor_dist if x <= RADIUS)
        print("  within %.0fpx radius: %d (%.1f%%)"
              % (RADIUS, within, 100 * within / n))


if __name__ == "__main__":
    main()
