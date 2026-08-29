#!/usr/bin/env python3
"""vh_cam.py <capture.v3> — SHARED-CAMERA CLUSTER test.
For each frame compute cam_i = sx_i - wx_i for every slot with non-null screen coords.
Slots the engine actually rendered this frame must agree on cam (one camera). Stale/parked
slots keep an old cam and fall out of the cluster. Reports how often exactly 2 slots (one per
side) agree, and whether the resulting screen positions are on-canvas + ground-consistent."""
import sys, struct, collections, statistics
SR=25; TOL=1.0
raw=open(sys.argv[1],"rb").read(); assert raw[:8]==b"V3SYNC02"
n=(len(raw)-8)//(4+6*SR); F=[]; off=8
for _ in range(n):
    fc=struct.unpack_from("<I",raw,off)[0]; off+=4
    sl=[]
    for i in range(6):
        a,cid,sid,hp,red,fac,wx,wy,sx,sy=struct.unpack_from("<BBHHHBffff",raw,off); off+=SR
        sl.append(dict(act=a,cid=cid,sid=sid&0x7fff,hp=hp,red=red,fac=fac,wx=wx,wy=wy,sx=sx,sy=sy))
    F.append((fc,sl))

prev_cam=None
stat=collections.Counter(); sides=collections.Counter(); slotsel=collections.Counter()
onscreen=0; groundok=0; nground=0; seps=[]
for fc,sl in F:
    cams=[(i, sl[i]['sx']-sl[i]['wx']) for i in range(6)
          if not (sl[i]['sx']==0.0 and sl[i]['sy']==0.0)]
    # cluster by cam value
    best=None
    for i,c in cams:
        grp=[j for j,c2 in cams if abs(c2-c)<=TOL]
        if best is None or len(grp)>len(best[1]): best=(c,grp)
    if best is None: stat['no-screen-coords']+=1; continue
    cam,grp=best
    if prev_cam is not None and abs(cam-prev_cam)>200:
        stat['cam-jump>200']+=1
    prev_cam=cam
    stat[f'cluster{len(grp)}']+=1
    e=[i for i in grp if i%2==0]; o=[i for i in grp if i%2==1]
    sides[(len(e),len(o))]+=1
    slotsel[tuple(sorted(grp))]+=1
    if len(e)==1 and len(o)==1:
        A,B=sl[e[0]],sl[o[0]]
        seps.append(abs(A['sx']-B['sx']))
        if all(-80<s['sx']<720 and -80<s['sy']<560 for s in (A,B)): onscreen+=1
        for s in (A,B):
            if s['wy']==0.0:
                nground+=1
                if abs((s['sy']+s['wy'])-433.5)<3.0: groundok+=1
print(f"frames={len(F)}")
print("cluster sizes:", dict(sorted(stat.items())))
print("(even,odd) in cluster:", dict(sorted(sides.items())))
print("top selected slot sets:", slotsel.most_common(6))
if seps:
    print(f"pairs with exactly 1/side: {len(seps)}  screen-sep mean={statistics.mean(seps):.1f} "
          f"max={max(seps):.1f}  >640px: {sum(1 for s in seps if s>640)}")
    print(f"both on canvas: {onscreen}/{len(seps)}")
    print(f"ground-line (sy+wy within 3px of 433.5) when wy==0: {groundok}/{nground}")
