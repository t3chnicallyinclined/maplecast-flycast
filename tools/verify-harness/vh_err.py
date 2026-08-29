#!/usr/bin/env python3
"""vh_err.py <capture.v3> — quantify the CURRENT converter's placement error against the
engine's own screen coords (+0x6f0/+0x6f4), on the frames where the two rendered fighters
provably share one camera (cluster test). This is the end-to-end placement gate."""
import sys, struct, statistics
SR=25; TOL=1.0; GROUND=434.0
raw=open(sys.argv[1],"rb").read(); assert raw[:8]==b"V3SYNC02"
n=(len(raw)-8)//(4+6*SR); F=[]; off=8
for _ in range(n):
    fc=struct.unpack_from("<I",raw,off)[0]; off+=4
    sl=[]
    for i in range(6):
        a,cid,sid,hp,red,fac,wx,wy,sx,sy=struct.unpack_from("<BBHHHBffff",raw,off); off+=SR
        sl.append(dict(act=a,cid=cid,sid=sid&0x7fff,hp=hp,red=red,fac=fac,wx=wx,wy=wy,sx=sx,sy=sy))
    F.append((fc,sl))
ex=[];ey=[];n_used=0
for fc,sl in F:
    cams=[(i,sl[i]['sx']-sl[i]['wx']) for i in range(6) if not(sl[i]['sx']==0.0 and sl[i]['sy']==0.0)]
    grp=None
    for i,c in cams:
        g=[j for j,c2 in cams if abs(c2-c)<=TOL]
        if len(g)>=2 and (grp is None or len(g)>len(grp)): grp=g
    if not grp: continue
    e=[i for i in grp if i%2==0]; o=[i for i in grp if i%2==1]
    if len(e)!=1 or len(o)!=1: continue
    A,B=sl[e[0]],sl[o[0]]; n_used+=1
    cam=(A['wx']+B['wx'])/2.0                    # v4_to_gstarec.py:49 camera
    for s in (A,B):
        ex.append(abs((320.0+(s['wx']-cam))-s['sx']))
        ey.append(abs((GROUND-s['wy'])-s['sy']))
def rep(nm,v):
    v=sorted(v); print(f"{nm}: n={len(v)} mean={statistics.mean(v):.1f} median={v[len(v)//2]:.1f} "
                       f"p95={v[int(.95*len(v))]:.1f} max={max(v):.1f} <=8px:{sum(1 for x in v if x<=8)}")
print(f"frames with a provable 1-per-side camera cluster: {n_used}/{len(F)}")
rep("screen_x error (converter vs engine)",ex)
rep("screen_y error (converter vs engine)",ey)
