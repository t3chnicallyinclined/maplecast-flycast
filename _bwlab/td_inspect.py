#!/usr/bin/env python3
"""TDW2 floor study — step 0: inventory the actual per-frame data.
Prints (a) char-struct fields from the 4 true full-RAM dumps, (b) MCRR region tables."""
import struct, sys, os

RAM = 16*1024*1024
BASE = 0x8C268340
STRIDE = 0x5A4
SLOTS = {'P1C1':0x8C268340,'P2C1':0x8C2688E4,'P1C2':0x8C268E88,
         'P2C2':0x8C26942C,'P1C3':0x8C2699D0,'P2C3':0x8C269F74}

def f32(b,o): return struct.unpack_from('<f',b,o)[0]
def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def u8(b,o):  return b[o]

def show_ram(path):
    b = open(path,'rb').read()
    print(f"\n=== {os.path.basename(path)} ({len(b)} bytes) ===")
    for name,addr in SLOTS.items():
        o = addr & 0xFFFFFF
        act=u8(b,o+0); cid=u8(b,o+1); px=f32(b,o+0x34); py=f32(b,o+0x38)
        sx=f32(b,o+0xE0); sy=f32(b,o+0xE4); fac=u8(b,o+0x110)
        at=u16(b,o+0x142); sid=u16(b,o+0x144); anim=u16(b,o+0x1D0)
        hp=u8(b,o+0x420)
        print(f"  {name} act={act} cid={cid:3d} pos=({px:8.2f},{py:8.2f}) scr=({sx:7.1f},{sy:7.1f}) fac={fac} at={at} sid={sid:5d} anim={anim} hp={hp}")

def show_mcrr(path):
    b = open(path,'rb').read(4096)
    p=[0]
    def u32():
        v=struct.unpack_from('<I',b,p[0])[0]; p[0]+=4; return v
    assert u32()==0x5252434D,'bad magic'
    ver=u32();nS=u32();nD=u32();nF=u32();vram=u32();pvr=u32();u32()
    print(f"\n=== {os.path.basename(path)} MCRR ver={ver} nStatic={nS} nDynamic={nD} nFrames={nF} vram={vram:#x} pvr={pvr:#x} ===")
    def region():
        a=u32();l=u32();tag=b[p[0]:p[0]+8].split(b'\0')[0].decode('latin1');p[0]+=8;return(a,l,tag)
    S=[region() for _ in range(nS)]
    D=[region() for _ in range(nD)]
    print(" STATIC regions:")
    for a,l,t in S: print(f"   {a:#010x} len={l:#x}({l}) tag={t}")
    print(" DYNAMIC regions:")
    tot=0
    for a,l,t in D:
        tot+=l; print(f"   {a:#010x} len={l:#x}({l}) tag={t}")
    print(f" total dynamic bytes/frame = {tot} ({tot/1024:.1f} KB)")

if __name__=='__main__':
    for f in ['tools/render-replica-poc/_ram_f90.bin','tools/render-replica-poc/_ram_f91.bin',
              'tools/render-replica-poc/_ram_f92.bin','tools/render-replica-poc/_ram_f93.bin']:
        show_ram(f)
    show_mcrr('_bwlab/cap.gsta.mcrr')
    show_mcrr('tools/render-replica-poc/_live_fx5.gsta.mcrr')
