#!/usr/bin/env python3
"""vh_anim.py <capture.v3> <anim_dir> [slot] — ANIM-CHAIN IDENTITY gate.
Compresses the captured per-slot sprite_id stream into (sid, run_len) runs and looks each run
up in the character's DC cell table (PLxx.json groups[*].subanims[*].cells). A run whose sid
exists AND whose length equals that cell's Duration is a match. If Steam's sid numbering were
NOT the DC/atlas numbering, sid membership would fail wholesale and durations would not line up.
This is the offline half of 'verify the sprite_id -> atlas mapping FIRST' (handoff NEXT STEP 1)."""
import sys, struct, json, os, collections
SR=25
raw=open(sys.argv[1],"rb").read(); assert raw[:8]==b"V3SYNC02"
adir=sys.argv[2]
n=(len(raw)-8)//(4+6*SR); F=[]; off=8
for _ in range(n):
    fc=struct.unpack_from("<I",raw,off)[0]; off+=4
    sl=[]
    for i in range(6):
        a,cid,sid,hp,red,fac,wx,wy,sx,sy=struct.unpack_from("<BBHHHBffff",raw,off); off+=SR
        sl.append(dict(act=a,cid=cid,sid=sid&0x7fff,hp=hp,red=red,fac=fac,wx=wx,wy=wy,sx=sx,sy=sy))
    F.append((fc,sl))

def load(cid):
    p=os.path.join(adir,f"PL{cid:02X}.json")
    if not os.path.exists(p): return None
    j=json.load(open(p))
    dur={}; chains=[]
    for g in j['groups'].values():
        for sa in g['subanims']:
            ch=[(c['sprite_id'],c['duration']) for c in sa['cells']]
            chains.append(ch)
            for sid,d in ch: dur.setdefault(sid,set()).add(d)
    return dur,chains

slots=[int(sys.argv[3])] if len(sys.argv)>3 else range(6)
for k in slots:
    ss=[f[1][k] for f in F]
    cid=ss[0]['cid']
    L=load(cid)
    if not L: print(f"slot{k} cid={cid}: no anim table"); continue
    dur,chains=L
    runs=[]; cur=ss[0]['sid']; c=1
    for s in ss[1:]:
        if s['sid']==cur: c+=1
        else: runs.append((cur,c)); cur=s['sid']; c=1
    runs.append((cur,c))
    if len(runs)<3: print(f"slot{k} cid={cid}: static sid={cur} (no animation)"); continue
    inset=sum(1 for sid,_ in runs if sid in dur)
    # interior runs only (first/last are truncated by the capture window)
    inner=runs[1:-1]
    dmatch=sum(1 for sid,ln in inner if sid in dur and ln in dur[sid])
    dur255=sum(1 for sid,ln in inner if sid in dur and 255 in dur[sid])
    print(f"slot{k} cid={cid} (0x{cid:02X}) runs={len(runs)}: sid-in-DC-table {inset}/{len(runs)} "
          f"({inset/len(runs)*100:.1f}%) | interior run-length==DC duration {dmatch}/{len(inner)} "
          f"({dmatch/max(1,len(inner))*100:.1f}%) | hold-cells(dur 0xFF) {dur255}")
    print(f"    first 12 runs: {runs[:12]}")
    # longest consecutive-chain match
    best=0
    sids=[s for s,_ in runs]
    for ch in chains:
        cs=[s for s,_ in ch]
        for i in range(len(sids)):
            j2=0
            while j2<len(cs) and i+j2<len(sids) and sids[i+j2]==cs[j2]: j2+=1
            best=max(best,j2)
    print(f"    longest exact DC subanim-chain match: {best} cells")
