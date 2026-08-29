#!/usr/bin/env python3
"""vh_screen.py <capture.v3> — is +0x6f0/+0x6f4 a real screen transform of +0x61c/+0x620?
Tests: (a) sy + wy == const (ground-line law), (b) sx - wx equal across the two MOVING slots
(shared camera), (c) per-slot regression of sx on wx."""
import sys, struct, collections, statistics
SR=25
raw=open(sys.argv[1],"rb").read(); assert raw[:8]==b"V3SYNC02"
n=(len(raw)-8)//(4+6*SR); F=[]; off=8
for _ in range(n):
    fc=struct.unpack_from("<I",raw,off)[0]; off+=4
    sl=[]
    for i in range(6):
        a,cid,sid,hp,red,fac,wx,wy,sx,sy=struct.unpack_from("<BBHHHBffff",raw,off); off+=SR
        sl.append(dict(act=a,cid=cid,sid=sid&0x7fff,hp=hp,red=red,fac=fac,wx=wx,wy=wy,sx=sx,sy=sy))
    F.append((fc,sl))

MOV=[k for k in range(6) if len(set(f[1][k]['wx'] for f in F))>10]
print("moving slots:", MOV)

for k in range(6):
    ss=[f[1][k] for f in F]
    if all(s['sx']==0 and s['sy']==0 for s in ss): print(f"slot{k}: null (sx,sy)==(0,0) all frames"); continue
    sumy=[round(s['sy']+s['wy'],2) for s in ss]
    c=collections.Counter(sumy)
    dx=[round(s['sx']-s['wx'],2) for s in ss]
    cd=collections.Counter(dx)
    print(f"slot{k} cid={ss[0]['cid']:3d}: sy+wy modes {c.most_common(3)}  | sx-wx: distinct={len(cd)} "
          f"range[{min(dx):.1f},{max(dx):.1f}] top {cd.most_common(2)}")

if len(MOV)>=2:
    a,b=MOV[0],MOV[1]
    d=[abs((F[i][1][a]['sx']-F[i][1][a]['wx'])-(F[i][1][b]['sx']-F[i][1][b]['wx'])) for i in range(len(F))]
    print(f"\n[shared-camera, moving slots {a} vs {b}] |Δ(sx-wx)| mean={statistics.mean(d):.3f} "
          f"max={max(d):.3f} p95={sorted(d)[int(.95*len(d))]:.3f}  frames<=1px: {sum(1 for x in d if x<=1)}/{len(d)}")
    sep_s=[abs(F[i][1][a]['sx']-F[i][1][b]['sx']) for i in range(len(F))]
    sep_w=[abs(F[i][1][a]['wx']-F[i][1][b]['wx']) for i in range(len(F))]
    print(f"[screen sep between moving slots] min={min(sep_s):.1f} max={max(sep_s):.1f} mean={statistics.mean(sep_s):.1f}")
    print(f"[world  sep between moving slots] min={min(sep_w):.1f} max={max(sep_w):.1f} mean={statistics.mean(sep_w):.1f}")
    r=[sep_s[i]/sep_w[i] for i in range(len(F)) if sep_w[i]>20]
    if r: print(f"[screen/world sep ratio] n={len(r)} mean={statistics.mean(r):.4f} min={min(r):.4f} max={max(r):.4f}")
    both=sum(1 for i in range(len(F)) if all(-80<F[i][1][s]['sx']<720 and -80<F[i][1][s]['sy']<560 for s in (a,b)))
    print(f"[both moving slots in 640x480 box] {both}/{len(F)} frames")
    # ground-line: sy when wy==0
    g=[F[i][1][s]['sy'] for i in range(len(F)) for s in (a,b) if F[i][1][s]['wy']==0.0]
    if g: print(f"[sy at wy==0] n={len(g)} modes {collections.Counter(round(v,1) for v in g).most_common(4)}")
