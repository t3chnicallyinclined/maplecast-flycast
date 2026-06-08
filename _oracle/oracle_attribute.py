#!/usr/bin/env python3
"""oracle_attribute.py — OFFLINE attribution + analysis for the Frame Oracle.

PIVOT 2026-06-08 (docs/FRAME-ORACLE-SPEC.md + HANDOFF-2026-06-08): the server-side
nearest-anchor attribution inside the MAPLECAST_FRAME_ORACLE block matched ~0
character quads and emitted only the matched ones, so we debugged blind. The
server now DUMPS RAW: each /dev/shm/mc_oracle.jsonl line is

  {"frame":N,"in_match":1,"nq":..,"sprite":..,"filtered":..,"rtt":..,
   "objects":[{slot,owner_cid,category,sprite_id,screen_xy:[x,y],scale,flip,
               node_addr,
               tex_src:{vram_addr,region,gfx1_ptr,pal_ptr,part_idx},
               asm_src:{extras_ptr,file_ptr,fac_ptr,
                        hotspot_dx,hotspot_dy,        # legacy +0x178 walk (body-constant)
                        hot_cell_dx,hot_cell_dy,hot_cell_slot}}],   # cell-slot walk
   "sprite_quads":[{x,y,w,h,u:[u0,u1],v:[v0,v1],texId,tcw,blend:[s,d]}]}  # UNATTRIBUTED

This tool does the attribution the server no longer does, and ITERATES on it
cheaply (no rebuild/redeploy):

  * For each frame, attribute every sprite_quad to the nearest object by quad-center
    vs object anchor. Three anchor variants are evaluated head-to-head so we can
    see which (if any) hotspot reading helps:
        screen      = screen_xy
        hot178      = screen_xy + (hotspot_dx, hotspot_dy)        [legacy +0x178]
        hotcell     = screen_xy + (hot_cell_dx, hot_cell_dy)      [cell-slot]
  * Report the nearest-object distance distribution, the match rate at radii
    60/96/120/160 px, and per-object how many parts get assigned.
  * Multi-object aware: a character is usually several slot entries (body + parts)
    at different y; sprite_ids carry the 0x8000 hflip bit, masked with & 0x7FFF.

Output is a human summary + the verdict: are character parts cleanly attributable
now, and at what radius?  The .jsonl is ROM-derived (gitignored); this .py is the
committable reproducible tool.

Usage:  python _oracle/oracle_attribute.py [path-to.jsonl] [--radius R] [--per-frame N]
"""
import argparse
import collections
import json
import math
import sys

RADII = [60.0, 96.0, 120.0, 160.0]
ANCHORS = ["screen", "hot178", "hotcell"]


def obj_anchor(o, variant):
    sx, sy = o["screen_xy"]
    a = o.get("asm_src", {})
    if variant == "hot178":
        return sx + a.get("hotspot_dx", 0), sy + a.get("hotspot_dy", 0)
    if variant == "hotcell":
        return sx + a.get("hot_cell_dx", 0), sy + a.get("hot_cell_dy", 0)
    return float(sx), float(sy)


def quad_center(q):
    return q["x"] + q["w"] / 2.0, q["y"] + q["h"] / 2.0


def percentile(sorted_vals, p):
    if not sorted_vals:
        return float("nan")
    i = min(len(sorted_vals) - 1, int(len(sorted_vals) * p))
    return sorted_vals[i]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="?", default="_oracle/mc_oracle.jsonl")
    ap.add_argument("--radius", type=float, default=120.0,
                    help="primary radius for per-object assignment report")
    ap.add_argument("--per-frame", type=int, default=0,
                    help="print the first N frames' per-object quad counts (debug)")
    args = ap.parse_args()

    try:
        fh = open(args.path)
    except OSError as e:
        print("cannot open %s: %s" % (args.path, e), file=sys.stderr)
        print("\n>>> No capture present. The Oracle writes /dev/shm/mc_oracle.jsonl "
              "only while in_match. Re-capture by playing after the redeploy, then "
              "scp it to _oracle/ and re-run.", file=sys.stderr)
        sys.exit(2)

    # Detect schema: NEW = top-level "sprite_quads"; OLD = per-object "quads".
    schema = None
    raw_lines = []
    for line in fh:
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        raw_lines.append(d)
        if schema is None:
            if "sprite_quads" in d:
                schema = "new"
            elif d.get("objects") and "quads" in d["objects"][0]:
                schema = "old"
    fh.close()

    if not raw_lines:
        print("empty / unparseable capture: %s" % args.path)
        sys.exit(2)

    if schema == "old":
        print("=" * 72)
        print("DETECTED OLD-SCHEMA capture (per-object 'quads', no flat 'sprite_quads').")
        print("This file predates the raw-dump server change; it only contains the")
        print("server's already-attributed quads (matched~3/frame), NOT the full")
        print("sprite list, so offline re-attribution is not meaningful on it.")
        print("Re-capture with the redeployed binary to get 'sprite_quads'.")
        print("=" * 72)
        # Still print a couple of useful facts so the run isn't empty.
        nf = len(raw_lines)
        sp = sum(d.get("sprite", 0) for d in raw_lines)
        ma = sum(d.get("matched", 0) for d in raw_lines)
        hot_nonzero = 0
        hot_total = 0
        for d in raw_lines:
            for o in d.get("objects", []):
                a = o.get("asm_src", {})
                hot_total += 1
                if a.get("hotspot_dx", 0) or a.get("hotspot_dy", 0):
                    hot_nonzero += 1
        print("frames=%d  (old schema has no 'sprite'/'filtered' header in early "
              "captures)" % nf)
        if hot_total:
            print("hotspot_dx/dy nonzero: %d / %d objects (%.1f%%)  <- 0%% confirms the "
                  "always-0 bug in this capture" % (hot_nonzero, hot_total,
                                                    100.0 * hot_nonzero / hot_total))
        sys.exit(0)

    # ---- NEW SCHEMA: the real attribution analysis. ----
    frames = len(raw_lines)
    nq_sum = sum(d.get("nq", 0) for d in raw_lines)
    sprite_sum = sum(d.get("sprite", 0) for d in raw_lines)
    filt_sum = sum(d.get("filtered", 0) for d in raw_lines)

    # nearest-distance per quad, per anchor variant
    near = {v: [] for v in ANCHORS}
    # match counts at each radius, per variant
    hit = {v: {r: 0 for r in RADII} for v in ANCHORS}
    total_quads = 0
    # per-object (by masked sprite_id) assigned-part counts at the primary radius+best variant
    per_obj_parts = collections.Counter()
    per_obj_frames = collections.defaultdict(set)
    multi_obj_frames = 0
    obj_count_hist = collections.Counter()
    # hotspot health
    hot178_nz = hotcell_nz = hot_total = 0
    cell_slot_valid = 0

    for d in raw_lines:
        objs = d.get("objects", [])
        quads = d.get("sprite_quads", [])
        obj_count_hist[len(objs)] += 1
        if len(objs) > 1:
            multi_obj_frames += 1
        for o in objs:
            a = o.get("asm_src", {})
            hot_total += 1
            if a.get("hotspot_dx", 0) or a.get("hotspot_dy", 0):
                hot178_nz += 1
            if a.get("hot_cell_dx", 0) or a.get("hot_cell_dy", 0):
                hotcell_nz += 1
            if a.get("hot_cell_slot", -1) not in (-1, None):
                cell_slot_valid += 1

        if not objs:
            continue
        # precompute anchors per variant
        anchors = {v: [obj_anchor(o, v) for o in objs] for v in ANCHORS}
        for q in quads:
            total_quads += 1
            cx, cy = quad_center(q)
            for v in ANCHORS:
                best = 1e18
                bi = -1
                for i, (ax, ay) in enumerate(anchors[v]):
                    dd = (cx - ax) ** 2 + (cy - ay) ** 2
                    if dd < best:
                        best = dd
                        bi = i
                dist = math.sqrt(best)
                near[v].append(dist)
                for r in RADII:
                    if dist <= r:
                        hit[v][r] += 1
                # per-object assignment, primary radius, on the 'screen' anchor
                # (the baseline; hotspots are degenerate per HANDOFF). Assign at
                # args.radius.
                if v == "screen" and bi >= 0 and dist <= args.radius:
                    sid = objs[bi]["sprite_id"] & 0x7FFF
                    cid = objs[bi].get("owner_cid", 0)
                    key = (cid, sid)
                    per_obj_parts[key] += 1
                    per_obj_frames[key].add(d["frame"])

    # ---- report ----
    print("=" * 72)
    print("FRAME ORACLE -- offline attribution (NEW raw-dump schema)")
    print("file: %s" % args.path)
    print("=" * 72)
    print("frames=%d  sum nq=%d  sum sprite=%d  sum filtered=%d" %
          (frames, nq_sum, sprite_sum, filt_sum))
    print("avg/frame: nq=%.1f sprite=%.1f filtered=%.1f  | total sprite_quads attributed=%d" %
          (nq_sum / max(frames, 1), sprite_sum / max(frames, 1),
           filt_sum / max(frames, 1), total_quads))
    print()
    print("--- objects/frame histogram (multi-object = several parts/slots) ---")
    for n, c in sorted(obj_count_hist.items()):
        print("  %2d objects : %5d frames%s" % (n, c, "" if n != 1 else "  (single)"))
    print("  multi-object frames: %d / %d (%.1f%%)" %
          (multi_obj_frames, frames, 100.0 * multi_obj_frames / max(frames, 1)))
    print()
    print("--- hotspot health (per object instance, n=%d) ---" % hot_total)
    if hot_total:
        print("  hotspot_dx/dy (+0x178)  nonzero: %d (%.1f%%)" %
              (hot178_nz, 100.0 * hot178_nz / hot_total))
        print("  hot_cell_dx/dy (cell)   nonzero: %d (%.1f%%)" %
              (hotcell_nz, 100.0 * hotcell_nz / hot_total))
        print("  hot_cell_slot valid(>=0): %d (%.1f%%)" %
              (cell_slot_valid, 100.0 * cell_slot_valid / hot_total))
    print()
    print("--- nearest-object distance (quad-center -> anchor), per anchor variant ---")
    print("  %-8s %6s %6s %6s %6s %6s" % ("anchor", "med", "p75", "p90", "p95", "max"))
    for v in ANCHORS:
        s = sorted(near[v])
        if not s:
            print("  %-8s  (no quads)" % v)
            continue
        print("  %-8s %6.0f %6.0f %6.0f %6.0f %6.0f" %
              (v, percentile(s, .5), percentile(s, .75), percentile(s, .90),
               percentile(s, .95), s[-1]))
    print()
    print("--- match rate at radius (quads within R of SOME object) ---")
    print("  %-8s %s" % ("anchor", "  ".join("R<=%d" % int(r) for r in RADII)))
    for v in ANCHORS:
        n = max(len(near[v]), 1)
        cells = "  ".join("%5.1f%%" % (100.0 * hit[v][r] / n) for r in RADII)
        print("  %-8s %s" % (v, cells))
    print()
    print("--- per-object parts assigned (anchor=screen, R<=%.0f) ---" % args.radius)
    print("  %-14s %7s %7s %8s" % ("(cid,sid)", "parts", "frames", "parts/fr"))
    rows = sorted(per_obj_parts.items(), key=lambda kv: kv[1], reverse=True)
    for (cid, sid), parts in rows[:25]:
        fr = len(per_obj_frames[(cid, sid)])
        print("  cid%-3d sid%-5d %7d %7d %8.2f" %
              (cid, sid, parts, fr, parts / max(fr, 1)))
    if len(rows) > 25:
        print("  ... %d more (cid,sid) groups" % (len(rows) - 25))
    print()

    # ---- verdict ----
    print("=" * 72)
    print("VERDICT")
    print("=" * 72)
    # best variant = highest R<=96 hit rate
    best_v = max(ANCHORS, key=lambda v: hit[v][96.0] / max(len(near[v]), 1))
    for v in ANCHORS:
        n = max(len(near[v]), 1)
        print("  %-8s: med=%.0fpx  R<=96 %.1f%%  R<=120 %.1f%%" %
              (v, percentile(sorted(near[v]), .5),
               100.0 * hit[v][96.0] / n, 100.0 * hit[v][120.0] / n))
    n = max(len(near[best_v]), 1)
    r96 = 100.0 * hit[best_v][96.0] / n
    r120 = 100.0 * hit[best_v][120.0] / n
    print()
    if r120 >= 85.0:
        verd = ("CLEAN: raw-dump + offline attribution groups character parts well "
                "(best anchor=%s, %.0f%% within 120px). No PC-hook needed; pick the "
                "smallest radius that holds (see match-rate table)." % (best_v, r120))
    elif r120 >= 60.0:
        verd = ("PARTIAL: %.0f%% of sprite_quads land within 120px of an object "
                "(best=%s). Overlap/ambiguity in the remainder -- try tightening the "
                "isSprite filter or a per-category anchor before reaching for the "
                "exact PC-hook." % (r120, best_v))
    else:
        verd = ("POOR: only %.0f%% within 120px (best=%s). Either the quad coords are "
                "in a different space than screen_xy (check rtt/native), the objects[] "
                "list is missing the owners of these quads, or the exact 0x8C03093C/"
                "0x8C033E90 PC-hook is required for ground-truth attribution." %
                (r120, best_v))
    print(verd)
    print("  (R<=96 best=%.1f%%, R<=120 best=%.1f%%)" % (r96, r120))


if __name__ == "__main__":
    main()
