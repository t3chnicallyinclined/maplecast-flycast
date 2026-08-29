#!/usr/bin/env python3
"""vh_slots.py <capture.v3> — per-slot behaviour profile + candidate visibility-rule scoring."""
import sys, struct, collections
SR = 25; PARK = 1386.7
raw = open(sys.argv[1], "rb").read(); assert raw[:8] == b"V3SYNC02"
n = (len(raw) - 8) // (4 + 6*SR); frames = []; off = 8
for _ in range(n):
    fc = struct.unpack_from("<I", raw, off)[0]; off += 4
    sl = []
    for i in range(6):
        a, cid, sid, hp, red, fac, wx, wy, sx, sy = struct.unpack_from("<BBHHHBffff", raw, off); off += SR
        sl.append(dict(act=a, cid=cid, sid=sid & 0x7fff, hp=hp, red=red, fac=fac, wx=wx, wy=wy, sx=sx, sy=sy))
    frames.append((fc, sl))
def park(s): return abs(abs(s['wx']) - PARK) < 5.0

print("slot cid   act1     parked   act1&parked  wx-range                 hp-range  sid-distinct")
for k in range(6):
    ss = [f[1][k] for f in frames]
    cids = collections.Counter(s['cid'] for s in ss)
    a1 = sum(1 for s in ss if s['act'] == 1); pk = sum(1 for s in ss if park(s))
    both = sum(1 for s in ss if s['act'] == 1 and park(s))
    wx = [s['wx'] for s in ss]; hp = [s['hp'] for s in ss]
    sids = len(set(s['sid'] for s in ss))
    print(f"  {k}  {dict(cids)!s:22s} {a1:5d} {pk:6d} {both:9d}   [{min(wx):9.1f},{max(wx):9.1f}]  [{min(hp)},{max(hp)}]  {sids}")

# transitions of act per slot (how often it flips) + sid movement while parked
print("\nact-flip counts per slot:", [sum(1 for i in range(len(frames)-1) if frames[i][1][k]['act'] != frames[i+1][1][k]['act']) for k in range(6)])
print("sid-change counts per slot:", [sum(1 for i in range(len(frames)-1) if frames[i][1][k]['sid'] != frames[i+1][1][k]['sid']) for k in range(6)])
print("wx-change counts per slot:", [sum(1 for i in range(len(frames)-1) if frames[i][1][k]['wx'] != frames[i+1][1][k]['wx']) for k in range(6)])

# candidate rule: pick per side the slot with the SMALLEST |wx| among alive, then check separation
def cand(sl, par, rule):
    return [i for i in range(par, 6, 2) if rule(sl[i])]
rules = {
  'act1':        lambda s: s['act'] == 1,
  'act1_nopark': lambda s: s['act'] == 1 and not park(s),
  'nopark_alive':lambda s: s['hp'] > 0 and not park(s),
  'moving':      None,
}
for nm, r in rules.items():
    if r is None: continue
    seps = []; ok1 = 0
    for _, sl in frames:
        A = cand(sl, 0, r); B = cand(sl, 1, r)
        if len(A) == 1 and len(B) == 1: ok1 += 1
        if A and B:
            a = min(A, key=lambda i: abs(sl[i]['wx'])); b = min(B, key=lambda i: abs(sl[i]['wx']))
            seps.append(abs(sl[a]['wx'] - sl[b]['wx']))
    if seps:
        bad = sum(1 for s in seps if s > 640)
        print(f"\n[{nm}] frames-with-exactly-1-per-side={ok1}/{len(frames)}  sep n={len(seps)} "
              f"mean={sum(seps)/len(seps):.1f} max={max(seps):.1f} >640px: {bad} ({bad/len(seps)*100:.1f}%)")
    else:
        print(f"\n[{nm}] no pairs")

# dworld_y modes (Y unit calibration) + apex
dy = collections.Counter(); apex = 0.0
for i in range(len(frames)-1):
    if frames[i+1][0]-frames[i][0] != 1: continue
    for k in range(6):
        a, b = frames[i][1][k], frames[i+1][1][k]
        if a['act'] == 1 and a['hp'] > 0 and not park(a):
            d = round(b['wy']-a['wy'], 3)
            if d: dy[d] += 1
            apex = max(apex, a['wy'])
print(f"\n[dworld_y modes] {dy.most_common(12)}\n[max world_y (jump apex)] {apex:.2f}")
