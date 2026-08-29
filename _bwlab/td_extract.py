#!/usr/bin/env python3
"""TDW2 floor study — step 1: walk a .gsta.mcrr and extract the per-frame state series.

Saves:
  <out>_charst.npy   (nFrames x 8664 uint8)  the 6 char structs @0x8C268340
  <out>_gstate.npy   (nFrames x 96   uint8)  global game-state page @0x8C289620
  <out>_meta.npz     vframes[], region table, per-byte change counts over FULL dyn payload,
                     per-region changed-byte counts (streaming, no full store)
"""
import struct, sys, numpy as np

path = sys.argv[1]
out  = sys.argv[2]

buf = open(path,'rb').read()
p=[0]
def u32():
    v=struct.unpack_from('<I',buf,p[0])[0]; p[0]+=4; return v
assert u32()==0x5252434D,'bad MCRR magic'
ver=u32();nS=u32();nD=u32();nF=u32();vram=u32();pvr=u32();u32()
def region():
    a=u32();l=u32();tag=buf[p[0]:p[0]+8].split(b'\0')[0].decode('latin1');p[0]+=8;return(a,l,tag)
S=[region() for _ in range(nS)]
D=[region() for _ in range(nD)]
# skip static payload
p[0]+=vram; p[0]+=pvr
for a,l,t in S: p[0]+=l
frameStart=p[0]

dynbytes=sum(l for _,l,_ in D)
# offsets of the regions I want inside the per-frame dyn payload
off=0; region_off={}
for a,l,t in D:
    region_off[(a,t)]=(off,l); off+=l
CHAR=(0x8c268340,'char_st'); GST=(0x8c289620,'gstate')
coff,clen=region_off[CHAR]; goff,glen=region_off[GST]

# Frame record layout (from captureFrame writer, maplecast_replica_live.cpp ~820-950):
#   [12B hdr: FRMx+vframe+taSize=0] [dynbytes dyn regions, table order] [variable GFX/pal/HUD tails]
# So index each frame by its scanned FRMx position; dyn payload = pos+12 .. +dynbytes.
needle=bytes([0x46,0x52,0x4D,0x78])
fpos=[]; i=buf.find(needle,frameStart)
while i>=0: fpos.append(i); i=buf.find(needle,i+4)
charst=[]; gstate=[]; vframes=[]; tasizes=[]
prev_dyn=None
change_count=np.zeros(dynbytes,dtype=np.int32)
reg_changed_frames=np.zeros(nD,dtype=np.int64)
nframes_walked=0
for pos in fpos:
    if pos+12+dynbytes>len(buf): break
    vf=struct.unpack_from('<I',buf,pos+4)[0]
    dynOff=pos+12
    dyn=np.frombuffer(buf,dtype=np.uint8,count=dynbytes,offset=dynOff)
    charst.append(dyn[coff:coff+clen].copy())
    gstate.append(dyn[goff:goff+glen].copy())
    vframes.append(vf); tasizes.append(0)
    if prev_dyn is not None:
        diff=(dyn!=prev_dyn)
        change_count+=diff.astype(np.int32)
        o2=0
        for ri,(a,l,t) in enumerate(D):
            reg_changed_frames[ri]+=int(diff[o2:o2+l].sum()); o2+=l
    prev_dyn=dyn.copy()
    nframes_walked+=1

charst=np.array(charst); gstate=np.array(gstate)
vframes=np.array(vframes,dtype=np.uint32); tasizes=np.array(tasizes,dtype=np.uint32)
np.save(out+'_charst.npy',charst)
np.save(out+'_gstate.npy',gstate)
np.savez(out+'_meta.npz',
         vframes=vframes, tasizes=tasizes,
         dynbytes=dynbytes, change_count=change_count,
         reg_changed_frames=reg_changed_frames,
         reg_addr=np.array([a for a,_,_ in D],dtype=np.uint32),
         reg_len=np.array([l for _,l,_ in D],dtype=np.uint32),
         reg_tag=np.array([t for _,_,t in D]))
ntrans=nframes_walked-1
print(f"walked {nframes_walked} frames (expected {nF}); vframe range {vframes.min()}..{vframes.max()}")
print(f"taSize: min={tasizes.min()} max={tasizes.max()} mean={tasizes.mean():.0f}")
print(f"transitions={ntrans}  dynbytes/frame={dynbytes}")
# global read-set churn: bytes that changed in >=1 transition, and mean changed/frame
ever=(change_count>0).sum()
mean_changed=change_count.sum()/max(1,ntrans)
print(f"\nDYN READ-SET CHURN over {ntrans} transitions:")
print(f"  bytes ever changing = {ever}/{dynbytes} ({100*ever/dynbytes:.2f}%)")
print(f"  mean changed bytes / frame = {mean_changed:.0f} ({100*mean_changed/dynbytes:.3f}% of shipped payload)")
print(f"\nPER-REGION mean changed bytes/frame:")
tags=[t for _,_,t in D]; lens=[l for _,_,l in [(a,t,l) for a,l,t in D]]
for ri,(a,l,t) in enumerate(D):
    mc=reg_changed_frames[ri]/max(1,ntrans)
    print(f"  {a:#010x} {t:<8} len={l:<7} mean_changed/frame={mc:8.1f} ({100*mc/l:6.2f}%)")
