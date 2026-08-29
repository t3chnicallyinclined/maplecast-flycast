#!/usr/bin/env python3
"""vh_atlas.py <capture.v3> <atlas_dir> — ATLAS COVERAGE gate.
Every (char_id, sprite_id) the tape will ask the renderer to draw must exist in PL{cid:02X}.json's
sprite map. Anything missing is silently masked by sprite-client.mjs's held-pose fallback
(sprite-client.mjs:951-957), so it must be measured, never eyeballed."""
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
# demand set: any slot with non-null screen coords (i.e. the engine rendered it at some point)
demand=collections.Counter()
for fc,sl in F:
    for s in sl:
        if not(s['sx']==0.0 and s['sy']==0.0):
            demand[(s['cid'],s['sid'])]+=1
atl={}
for cid in sorted(set(c for c,_ in demand)):
    p=os.path.join(adir,f"PL{cid:02X}.json")
    if not os.path.exists(p): atl[cid]=None; print(f"cid {cid} (0x{cid:02X}): NO ATLAS FILE {p}"); continue
    j=json.load(open(p))
    keys=j.get('sprites') or j.get('chars',{}).get(str(cid),{}).get('sprites') or {}
    atl[cid]=set(int(k) for k in keys.keys())
    print(f"cid {cid} (0x{cid:02X}): atlas has {len(atl[cid])} sprites, ids [{min(atl[cid])},{max(atl[cid])}]")
tot=0; miss=0; msamples=0; tsamples=0
per=collections.Counter()
for (cid,sid),c in demand.items():
    tot+=1; tsamples+=c
    if atl.get(cid) is None or sid not in atl[cid]:
        miss+=1; msamples+=c; per[cid]+=c
print(f"\n[ATLAS COVERAGE] distinct (cid,sid) demanded={tot} missing={miss} ({miss/tot*100:.1f}%)")
print(f"[ATLAS COVERAGE] slot-frame samples={tsamples} missing={msamples} ({msamples/tsamples*100:.2f}%)")
print(f"[per-cid missing samples] {dict(per)}")
ex=[f"{c}/0x{s:x}" for (c,s) in sorted(demand) if atl.get(c) is None or s not in atl[c]][:15]
print(f"[examples] {ex}")
