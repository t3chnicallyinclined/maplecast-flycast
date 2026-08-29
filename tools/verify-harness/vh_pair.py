#!/usr/bin/env python3
"""vh_pair.py <capture.v3> — which (even,odd) slot pair is ever within one screen?
Decisive offline test of the 'two point fighters share one 640px screen' invariant."""
import sys, struct, collections
SR = 25
raw = open(sys.argv[1], "rb").read(); assert raw[:8] == b"V3SYNC02"
n = (len(raw) - 8)//(4+6*SR); frames=[]; off=8
for _ in range(n):
    fc = struct.unpack_from("<I", raw, off)[0]; off += 4
    sl=[]
    for i in range(6):
        a,cid,sid,hp,red,fac,wx,wy,sx,sy = struct.unpack_from("<BBHHHBffff", raw, off); off+=SR
        sl.append(dict(act=a,cid=cid,sid=sid&0x7fff,hp=hp,red=red,fac=fac,wx=wx,wy=wy,sx=sx,sy=sy))
    frames.append((fc,sl))
print(f"{len(frames)} frames")
pairs = collections.Counter(); span = collections.defaultdict(list)
for _, sl in frames:
    for i in (0,2,4):
        for j in (1,3,5):
            d = abs(sl[i]['wx']-sl[j]['wx']); span[(i,j)].append(d)
            if d <= 640: pairs[(i,j)] += 1
print("\npair  frames_within_640   min_sep   max_sep   mean_sep")
for k in sorted(span):
    v = span[k]
    print(f"{k}   {pairs[k]:5d}/{len(v)}   {min(v):8.1f} {max(v):8.1f} {sum(v)/len(v):9.1f}")

# Per-frame: is there ANY pair within 640, and is it unique?
cnt = collections.Counter()
for _, sl in frames:
    c = sum(1 for i in (0,2,4) for j in (1,3,5) if abs(sl[i]['wx']-sl[j]['wx']) <= 640)
    cnt[c]+=1
print("\npairs-within-640 per frame:", dict(sorted(cnt.items())))

# The moving-slot test: per slot, is wx changing this frame window?
print("\nper-slot |wx| stats and 'moved in last 30 frames' fraction")
for k in range(6):
    xs=[f[1][k]['wx'] for f in frames]
    moved=sum(1 for i in range(30,len(xs)) if xs[i]!=xs[i-30])
    print(f"  slot{k} cid={frames[0][1][k]['cid']:3d} distinct_wx={len(set(xs)):5d} moved30={moved}/{max(0,len(xs)-30)}")

# screen coords per slot (the failed 0x6f0/0x6f4) — in-box fraction per slot
print("\nper-slot +0x6f0/+0x6f4 in-box fraction (-80<sx<720, -80<sy<560, not (0,0))")
for k in range(6):
    ss=[f[1][k] for f in frames]
    ib=sum(1 for s in ss if not(s['sx']==0 and s['sy']==0) and -80<s['sx']<720 and -80<s['sy']<560)
    nz=sum(1 for s in ss if not(s['sx']==0 and s['sy']==0))
    xr=[s['sx'] for s in ss]; yr=[s['sy'] for s in ss]
    print(f"  slot{k} inbox={ib:5d} nonnull={nz:5d} sx[{min(xr):9.1f},{max(xr):9.1f}] sy[{min(yr):8.1f},{max(yr):8.1f}]")
