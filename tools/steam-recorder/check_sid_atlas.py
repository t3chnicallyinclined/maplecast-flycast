#!/usr/bin/env python3
"""check_sid_atlas.py — THE GATE: do Steam's live sprite_ids index the SAME numbering as our
baked DC atlases? Compares every sprite_id observed in live captures against the sprite_id set
in mvc2-skin-studio/web/anim/PLxx.json for that character. High coverage => same numbering."""
import struct, json, os, re

SR = 25
ANIM = "C:/Users/trist/projects/mvc2-skin-studio/web/anim"

obs = {}
for fn in ("real.v3", "match2.v3", "match3.v3", "match.v3"):
    if not os.path.exists(fn):
        continue
    raw = open(fn, "rb").read()
    if raw[:8] != b"V3SYNC02":
        continue
    off = 8
    while off + 4 + 6 * SR <= len(raw):
        off += 4
        for i in range(6):
            act, cid, sid, hp, red, fac, wx, wy, sx, sy = struct.unpack_from("<BBHHHBffff", raw, off + i * SR)
            if hp > 0 and sid:
                obs.setdefault(cid, set()).add(sid & 0x7FFF)
        off += 6 * SR

atlas = {}
for fn in os.listdir(ANIM):
    m = re.match(r"PL([0-9A-Fa-f]{2})\.json$", fn)
    if not m:
        continue
    d = json.load(open(os.path.join(ANIM, fn)))
    cid = d.get("char_id", int(m.group(1), 16))
    sids = set()
    for g in d["groups"].values():
        sa = g["subanims"]
        for s2 in (sa if isinstance(sa, list) else sa.values()):
            for c in s2.get("cells", []):
                if c.get("sprite_id") is not None:
                    sids.add(c["sprite_id"])
    atlas[cid] = (d.get("name", fn), sids)

print("SPRITE_ID <-> ATLAS NUMBERING GATE (live Steam sids vs baked DC atlas sids)")
if not obs:
    print("  no captures found in cwd")
for cid in sorted(obs):
    if cid not in atlas:
        print(f"  cid {cid}: no atlas file")
        continue
    nm, a = atlas[cid]
    o = obs[cid]
    hit = len(o & a)
    miss = sorted(o - a)[:6]
    print(f"  cid {cid:3d} {nm:<12} observed={len(o):4d}  in-atlas={hit:4d} ({100*hit/len(o):5.1f}%)"
          f"  obs_max={max(o)} atlas_max={max(a)}  misses={miss}")
