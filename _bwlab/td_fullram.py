#!/usr/bin/env python3
"""TDW2 floor study — Q1: TRUE full-16MB-RAM change structure across the 4 consecutive dumps.
Tests the "99.9% of RAM is static or derivable" claim at full granularity, and localizes the
changing bytes (do they fall inside the shipped render read-set, or leak outside it?)."""
import numpy as np

files=['tools/render-replica-poc/_ram_f90.bin','tools/render-replica-poc/_ram_f91.bin',
       'tools/render-replica-poc/_ram_f92.bin','tools/render-replica-poc/_ram_f93.bin']
R=[np.fromfile(f,dtype=np.uint8) for f in files]
N=len(R[0]); assert all(len(r)==N for r in R)
print(f"4 dumps, {N} bytes each ({N/1024/1024:.0f} MB). 3 consecutive transitions.")

# per-byte change count across the 3 transitions
cc=np.zeros(N,dtype=np.uint8)
for a,b in zip(R,R[1:]):
    cc+=(a!=b).astype(np.uint8)
ever=(cc>0).sum()
print(f"\nQ1 CHANGE-FREQUENCY HISTOGRAM (full 16MB, 3 transitions):")
for k in range(4):
    n=int((cc==k).sum())
    print(f"  changed in {k}/3 transitions: {n:>12,} bytes ({100*n/N:7.4f}%)")
print(f"  --> bytes NEVER changing (static)     : {int((cc==0).sum()):>12,} ({100*(cc==0).sum()/N:.4f}%)")
print(f"  --> bytes changing at least once      : {int(ever):>12,} ({100*ever/N:.4f}%)")
# per-transition raw counts
for i,(a,b) in enumerate(zip(R,R[1:])):
    d=int((a!=b).sum()); print(f"  transition f9{i}->f9{i+1}: {d:,} bytes changed ({100*d/N:.5f}%)")

# localize the changing bytes into coalesced runs (merge gaps<=64)
chg=np.nonzero(cc>0)[0]
runs=[]
if len(chg):
    s=chg[0]; p=chg[0]
    for x in chg[1:]:
        if x-p<=64: p=x
        else: runs.append((s,p)); s=x; p=x
    runs.append((s,p))
print(f"\n{len(runs)} coalesced changing runs (gap<=64). Top 25 by size:")
runs_sz=sorted(runs,key=lambda r:-(r[1]-r[0]+1))
# label known regions
labels=[(0x268340,0x2400*3,'char_struct_page(616)'),(0x289000,0x1000,'globals_page(649)'),
        (0x26AA54,0x1D000,'objpool'),(0x1F9D00,0x400,'arena/tiledesc'),(0x24B000,0x30000,'idx/rectab'),
        (0x2D6A00,0x400,'cam'),(0x26A400,0x800,'camZ/rparam'),(0x349000,0x2000,'frame_ctr/misc')]
def lab(off):
    for a,l,nm in labels:
        if a<=off<a+l: return nm
    return '?'
for s,e in runs_sz[:25]:
    print(f"  0x8C{s:06X}..0x8C{e:06X}  {e-s+1:>7}B  [{lab(s)}]")

# how much of the change is INSIDE the shipped render read-set regions?
# (dyn region table from the MCRR, guest-normalized)
dyn=[(0x2895e0,0x10),(0x287de0,0x1800),(0x289620,0x60),(0x2895c0,0x60),(0x268340,0x21d8),
     (0x26aa54,0x1d000),(0x1f9d80,0x20),(0x1f9f9c,0x1800),(0x2d6ad8,0xc0),(0x26a510,0x40),
     (0x26823c,0x4),(0x268240,0x60),(0x26a974,0x100),(0x2dad30,0x40),(0x2aa4c0,0x10),
     (0x24b65c,0x2000),(0x24d7dc,0x10000),
     (0x565000,0x3000),(0x955000,0x3000),(0x6b5000,0x3000),(0xaa5000,0x3000),
     (0x805000,0x3000),(0xbf5000,0x3000),(0xd45000,0x3000)]
inset=np.zeros(N,dtype=bool)
for a,l in dyn: inset[a:a+l]=True
chg_mask=cc>0
in_rs=int((chg_mask&inset).sum()); out_rs=int((chg_mask&~inset).sum())
print(f"\nREAD-SET COMPLETENESS (changing full-RAM bytes vs shipped render read-set):")
print(f"  changing bytes INSIDE shipped read-set : {in_rs:,}")
print(f"  changing bytes OUTSIDE shipped read-set: {out_rs:,}  <- served stale by MCRR (garble candidates)")
# list outside runs
out=np.nonzero(chg_mask&~inset)[0]
oruns=[]
if len(out):
    s=out[0];p=out[0]
    for x in out[1:]:
        if x-p<=64:p=x
        else:oruns.append((s,p));s=x;p=x
    oruns.append((s,p))
print(f"  {len(oruns)} runs outside read-set; top 15:")
for s,e in sorted(oruns,key=lambda r:-(r[1]-r[0]+1))[:15]:
    print(f"    0x8C{s:06X}..0x8C{e:06X}  {e-s+1}B  [{lab(s)}]")
