#!/usr/bin/env python3
"""vh_audit.py <capture.v3> — verification-harness audit of a V3SYNC02 capture.
Prints the objective numbers every gate in VERIFICATION-HARNESS.md thresholds on.
Read-only, offline. No game required."""
import sys, struct, collections

SR = 25
PARK = 1386.7

raw = open(sys.argv[1], "rb").read()
assert raw[:8] == b"V3SYNC02", raw[:8]
n = (len(raw) - 8) // (4 + 6 * SR)
frames = []
off = 8
for _ in range(n):
    fc = struct.unpack_from("<I", raw, off)[0]; off += 4
    slots = []
    for i in range(6):
        act, cid, sid, hp, red, fac, wx, wy, sx, sy = struct.unpack_from("<BBHHHBffff", raw, off)
        off += SR
        slots.append(dict(act=act, cid=cid, sid=sid & 0x7fff, raw_sid=sid, hp=hp, red=red,
                          fac=fac, wx=wx, wy=wy, sx=sx, sy=sy))
    frames.append((fc, slots))

print(f"file={sys.argv[1]} frames={len(frames)} fc[{frames[0][0]}..{frames[-1][0]}]")

# --- G1 frame continuity ---
d = [frames[i+1][0] - frames[i][0] for i in range(len(frames)-1)]
h = collections.Counter(d)
span = frames[-1][0] - frames[0][0] + 1
print(f"\n[G1 continuity] fc-delta histogram: {dict(sorted(h.items()))}")
print(f"   captured {len(frames)} of span {span}  -> coverage {len(frames)/span*100:.2f}%")

# --- G2 who is on screen: rival rules ---
def park(s): return abs(abs(s['wx']) - PARK) < 5.0
rules = {
    'act==1':            lambda s: s['act'] == 1,
    'act==1 & hp>0':     lambda s: s['act'] == 1 and s['hp'] > 0,
    '|wx|<1300 & hp>0':  lambda s: abs(s['wx']) < 1300 and s['hp'] > 0,
    'screenbox(6f0)':    lambda s: (not (s['sx'] == 0 and s['sy'] == 0)) and -80 < s['sx'] < 720 and -80 < s['sy'] < 560 and s['hp'] > 0,
}
for name, f in rules.items():
    cnt = collections.Counter(sum(1 for s in sl if f(s)) for _, sl in frames)
    per_side = collections.Counter(
        (sum(1 for i in (0,2,4) if f(sl[i])), sum(1 for i in (1,3,5) if f(sl[i]))) for _, sl in frames)
    print(f"\n[G2 {name}] total-per-frame {dict(sorted(cnt.items()))}")
    print(f"     (P1,P2) counts {dict(sorted(per_side.items()))}")

# --- G3 act flag vs park spot cross-tab ---
xt = collections.Counter()
for _, sl in frames:
    for s in sl:
        xt[(s['act'], park(s), s['hp'] > 0)] += 1
print("\n[G3 act x parked x alive] (act, parked, alive) -> slot-samples")
for k in sorted(xt): print(f"     {k} -> {xt[k]}")

# --- G4 ground line ---
gy = collections.Counter()
for _, sl in frames:
    for s in sl:
        if s['act'] == 1 and s['hp'] > 0 and not park(s):
            gy[round(s['wy'], 1)] += 1
top = gy.most_common(6)
print(f"\n[G4 world_y modes for live non-parked] {top}")

# --- G5 screen coords (the FAILED +0x6f0/+0x6f4) ---
inbox = collections.Counter()
for _, sl in frames:
    c = sum(1 for s in sl if -80 < s['sx'] < 720 and -80 < s['sy'] < 560 and not (s['sx'] == 0 and s['sy'] == 0))
    inbox[c] += 1
print(f"\n[G5 slots inside 640x480 box per frame @+0x6f0/+0x6f4] {dict(sorted(inbox.items()))}")
# shared-camera test: sx - wx spread across slots that are non-null
sp = []
for _, sl in frames:
    v = [s['sx'] - s['wx'] for s in sl if not (s['sx'] == 0 and s['sy'] == 0)]
    if len(v) >= 2: sp.append(max(v) - min(v))
if sp: print(f"   shared-camera (sx-wx) spread: max={max(sp):.3f} mean={sum(sp)/len(sp):.3f} over {len(sp)} frames")

# --- G6 walk-speed calibration: |dwx| modes for the point chars ---
dh = collections.Counter()
for i in range(len(frames)-1):
    if frames[i+1][0] - frames[i][0] != 1: continue
    for k in range(6):
        a, b = frames[i][1][k], frames[i+1][1][k]
        if a['act'] == 1 and b['act'] == 1 and a['hp'] > 0 and not park(a):
            dx = round(abs(b['wx'] - a['wx']), 3)
            if dx > 0: dh[dx] += 1
print(f"\n[G6 |dworld_x|/frame modes, live non-parked] {dh.most_common(10)}")

# --- G7 sprite_id sanity ---
sids = collections.Counter()
cids = collections.Counter()
bit15 = 0
for _, sl in frames:
    for s in sl:
        if s['act'] == 1 and s['hp'] > 0:
            sids[s['sid']] += 1; cids[s['cid']] += 1
            if s['raw_sid'] & 0x8000: bit15 += 1
print(f"\n[G7 sprite_id] distinct={len(sids)} range=[{min(sids) if sids else '-'},{max(sids) if sids else '-'}] bit15set={bit15}")
print(f"   char_ids seen on live slots: {dict(cids)}")

# --- G8 inter-fighter separation from world coords ---
seps = []
for _, sl in frames:
    p1 = [s for i, s in enumerate(sl) if i % 2 == 0 and s['act'] == 1 and s['hp'] > 0 and not park(s)]
    p2 = [s for i, s in enumerate(sl) if i % 2 == 1 and s['act'] == 1 and s['hp'] > 0 and not park(s)]
    if len(p1) == 1 and len(p2) == 1: seps.append(abs(p1[0]['wx'] - p2[0]['wx']))
if seps:
    print(f"\n[G8 separation |wx1-wx2|] frames={len(seps)} min={min(seps):.1f} max={max(seps):.1f} mean={sum(seps)/len(seps):.1f}")
else:
    print("\n[G8 separation] NO frame had exactly one live non-parked fighter per side")
