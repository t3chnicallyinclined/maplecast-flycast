#!/usr/bin/env python3
"""audit_pointer_targets.py — read-set coverage audit, step 3 (chased-pointer stability).

The seed-vs-live byte diff proves the FIXED-ADDRESS dyn regions cover all moving bytes.
But render_frame chases POINTERS (idxtab=*0x8C2DAD3C, rectab=*0x8C2DAD4C, per-node
GFX1/GFX2 at node+0x15C/+0x160, descriptor base 0x8C1F9F9C indexed by node+0xDC). If a
chased pointer's VALUE moves to a target the shipped fixed-address region does not cover,
render_frame reads stale even though every shipped byte is fresh. This checks each chased
pointer is (a) stable across frames and (b) lands inside a shipped dyn/static region.

Usage: python audit_pointer_targets.py <capture.mcrr>
"""
import struct, sys
path = sys.argv[1] if len(sys.argv) > 1 else 'side_cap.mcrr'
buf = open(path, 'rb').read()
p = 0
def u32():
    global p
    v = struct.unpack_from('<I', buf, p)[0]; p += 4; return v
assert u32() == 0x5252434D
ver=u32(); nS=u32(); nD=u32(); nF=u32(); vram=u32(); pvr=u32(); u32()
def region():
    global p
    a=u32(); l=u32(); tag=buf[p:p+8].split(b'\0')[0].decode('latin1'); p+=8; return (a,l,tag)
S=[region() for _ in range(nS)]; D=[region() for _ in range(nD)]
p+=vram+pvr
staticData=[]
for (a,l,t) in S: staticData.append(buf[p:p+l]); p+=l
frameStart=p
RAM=16*1024*1024
seed=bytearray(RAM)
for (a,l,t),data in zip(S,staticData):
    off=0 if t=='ram16' else (a&0xFFFFFF); seed[off:off+len(data)]=data
dynbytes=sum(l for _,l,_ in D); STRIDE=dynbytes+16
needle=bytes([0x46,0x52,0x4D,0x78])
allpos=[]; i=buf.find(needle)
while i>=0: allpos.append(i); i=buf.find(needle,i+4)
posset=set(allpos); best=(0,frameStart)
for x in allpos:
    n=0;c=x
    while c in posset and struct.unpack_from('<I',buf,c)[0]==0x784D5246: n+=1;c+=STRIDE
    if n>best[0]: best=(n,x)
chain_n,chain_start=best

# coverage = union of static + dyn region spans (guest idx)
spans=[]
for (a,l,t) in S:
    gi=0 if t=='ram16' else (a&0xFFFFFF); spans.append((gi,gi+l,t))
for (a,l,t) in D: gi=a&0xFFFFFF; spans.append((gi,gi+l,t))
def cover(gi):
    for lo,hi,t in spans:
        if lo<=gi<hi: return t
    return None
def isram(g): return ((g>>24)&0x7F)==0x0C and g!=0

def ru32(ram,g): i=g&0xFFFFFF; return ram[i]|ram[i+1]<<8|ram[i+2]<<16|ram[i+3]<<24
def ru8(ram,g): return ram[g&0xFFFFFF]

def buildlive(f):
    live=bytearray(seed); off=chain_start+f*STRIDE+12
    for (a,l,t) in D:
        gi=a&0xFFFFFF; live[gi:gi+l]=buf[off:off+l]; off+=l
    return live

print(f"MCRR {path}: {chain_n} frames")
# Per-frame, gather the chased pointers and the slot-walk's per-node GFX targets +
# the descriptor index range, and verify every chased target lands in a covered region.
PTR_IDX=0x8C2DAD3C; PTR_REC=0x8C2DAD4C
sample_frames=[0,1,chain_n//4,chain_n//2,(3*chain_n)//4,chain_n-1]
idxvals=set(); recvals=set()
gfx_uncov=[]; node_count_hist={}
desc_overflow=[]
for f in range(chain_n):
    live=buildlive(f)
    idx=ru32(live,PTR_IDX); rec=ru32(live,PTR_REC)
    idxvals.add(idx); recvals.add(rec)
    # slot-walk: for each layer, each node, check GFX1/GFX2 + descriptor index
    for L in range(16):
        cnt=ru8(live,0x8C2895E0+L)
        if cnt==0 or cnt>0x60: continue
        base=0x8C287DE0+L*0x180
        for k in range(cnt):
            node=ru32(live,base+k*4)
            if not isram(node): continue
            cat=ru8(live,(node+0x3)&0xFFFFFF)
            # signed byte
            if cat>=128: cat-=256
            gfx2=ru32(live,node+0x160); gfx1=ru32(live,node+0x15C)
            for g,nm in ((gfx2,'GFX2'),(gfx1,'GFX1')):
                if isram(g) and cover(g&0xFFFFFF) is None:
                    gfx_uncov.append((f,L,k,node,nm,g,cat))
            # descriptor index: node+0xDC (u16) indexes 0x8C1F9F9C, 4 bytes/entry
            dc=ru32(live,node+0xDC)&0xFFFF
            descend=0x8C1F9F9C+(dc)*4
            if cover(descend&0xFFFFFF) is None:
                desc_overflow.append((f,node,dc,descend))
print("idxtab ptr distinct values:",[hex(v) for v in idxvals])
print("rectab ptr distinct values:",[hex(v) for v in recvals])
print("  shipped idxtab region addr:",[hex(a) for a,_,t in D if t=='idxtab'])
print("  shipped rectab region addr:",[hex(a) for a,_,t in D if t=='rectab'])
print(f"\nGFX targets OUTSIDE any covered region: {len(gfx_uncov)}")
for rec in gfx_uncov[:20]:
    f,L,k,node,nm,g,cat=rec
    print(f"  f{f} L{L} k{k} node={node:#x} cat={cat} {nm}={g:#x} (UNCOVERED)")
print(f"\ndescriptor-index OUTSIDE tiledesc region: {len(desc_overflow)}")
for rec in desc_overflow[:20]:
    f,node,dc,de=rec
    print(f"  f{f} node={node:#x} node+0xDC={dc} -> {de:#x} (OUT of 0x8C1F9F9C+0x1800)")
