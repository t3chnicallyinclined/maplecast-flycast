#!/usr/bin/env python3
"""phase_burst.py [seconds] — per-frame WRITE-BURST ENVELOPE of the Steam fighter array.

For every game frame: t=0 is the sample where the frame counter (arr-0x25c) was first seen
incremented.  Reports when the LAST byte anywhere in [arr .. arr+6*0x738) changed, i.e. how
long after the fc tick the array is still being mutated.  That interval is the window in which
a naive "fc changed -> read all 6 slots" recorder SHATTERS the snapshot.

Also reports: how many distinct write bursts per frame, and the phase of the last write to the
render-key fields (sid +0x1c / anim_timer +0x1a / cell_ptr +0x2c) vs the kinematics fields
(world +0x61c/+0x620, vel +0x644/+0x648, hp +0x40c).
"""
import sys, time, struct, ctypes, subprocess, json
from ctypes import wintypes
import numpy as np

STRIDE=0x738; PTR_OFF=0xAC6EF0; ARR_ADD=0x3F24; FC_REL=-0x25c
NSLOT=6; REG_LO=FC_REL; REG_HI=NSLOT*STRIDE; REG_LEN=REG_HI-REG_LO
ARR_IDX = -REG_LO            # index of arr+0 inside the buffer

k32=ctypes.WinDLL("kernel32",use_last_error=True); psapi=ctypes.WinDLL("psapi",use_last_error=True)
def find_pid():
    for ln in subprocess.run(["tasklist","/FO","CSV"],capture_output=True,text=True).stdout.splitlines():
        if ln.startswith('"MarvelVsCapcom'): return int(ln.split('","')[1])
pid=find_pid()
if not pid: sys.exit("not running")
h=k32.OpenProcess(0x0410,False,pid)
mods=(wintypes.HMODULE*1)(); need=wintypes.DWORD()
psapi.EnumProcessModulesEx(h,mods,ctypes.sizeof(mods),ctypes.byref(need),0x03); base=mods[0]
b=ctypes.create_string_buffer(8); g=ctypes.c_size_t()
k32.ReadProcessMemory(h,ctypes.c_void_p(base+PTR_OFF),b,8,ctypes.byref(g))
blk=struct.unpack("<Q",b.raw)[0]
if not (0x10000<blk<0x7FFFFFFFFFFF): sys.exit("not in a match")
arr=blk+ARR_ADD; start=arr+REG_LO
secs=float(sys.argv[1]) if len(sys.argv)>1 else 8.0

RENDERKEY=set()
KINEM=set()
for s in range(NSLOT):
    b0=ARR_IDX+s*STRIDE
    for o in (0x1a,0x1b,0x1c,0x1d,0x2c,0x2d,0x2e,0x2f,0x30,0x31,0x32,0x33): RENDERKEY.add(b0+o)
    for o in list(range(0x61c,0x624))+list(range(0x644,0x64c))+list(range(0x40c,0x414)): KINEM.add(b0+o)
RK=np.zeros(REG_LEN,bool); KN=np.zeros(REG_LEN,bool)
for i in RENDERKEY: RK[i]=True
for i in KINEM: KN[i]=True
ARRMASK=np.zeros(REG_LEN,bool); ARRMASK[ARR_IDX:]=True     # the 6-slot array only (exclude PRE globals)

cbuf=ctypes.create_string_buffer(REG_LEN); prev=None
frames=[]   # dict per frame
cur_f=None; t_edge=None; last_fc=None
t0=time.perf_counter(); tend=t0+secs; nsamp=0
while time.perf_counter()<tend:
    t=time.perf_counter()
    if not k32.ReadProcessMemory(h,ctypes.c_void_p(start),cbuf,REG_LEN,ctypes.byref(g)) or g.value!=REG_LEN:
        continue
    nsamp+=1
    cur=np.frombuffer(cbuf.raw,dtype=np.uint8)
    fc=int(struct.unpack_from("<I",cbuf.raw,0)[0])
    if prev is None:
        prev=cur.copy(); last_fc=fc; t_edge=t; continue
    diff = cur^prev
    changed = diff!=0
    if fc!=last_fc:
        if cur_f is not None: frames.append(cur_f)
        last_fc=fc; t_edge=t
        cur_f=dict(fc=fc, first=None, last=None, rk=None, kn=None, bursts=0, lastgap=None, n=0)
    if cur_f is not None and changed.any():
        ph=(t-t_edge)*1000.0
        anyarr = bool((changed&ARRMASK).any())
        if anyarr:
            if cur_f["first"] is None: cur_f["first"]=ph
            if cur_f["last"] is not None and ph-cur_f["last"]>0.5: cur_f["bursts"]+=1
            cur_f["last"]=ph; cur_f["n"]+=1
        if (changed&RK).any(): cur_f["rk"]=ph
        if (changed&KN).any(): cur_f["kn"]=ph
    prev=cur.copy()
if cur_f is not None: frames.append(cur_f)
dt=time.perf_counter()-t0
print(f"samples={nsamp} rate={nsamp/dt:.0f}/s frames={len(frames)}")

def stat(key,label):
    v=np.array([f[key] for f in frames if f.get(key) is not None])
    if v.size==0: print(f"{label}: (never changed)"); return
    print(f"{label}: n={v.size} min={v.min():.3f} p50={np.median(v):.3f} p90={np.percentile(v,90):.3f} "
          f"p99={np.percentile(v,99):.3f} max={v.max():.3f} ms")
stat("first","array FIRST write after fc tick ")
stat("last" ,"array LAST  write after fc tick ")
stat("rk"   ,"render-key (sid/timer/cellptr)  ")
stat("kn"   ,"kinematics (pos/vel/hp)         ")
bl=np.array([f["bursts"] for f in frames])
print(f"extra bursts (>0.5ms gap) per frame: mean={bl.mean():.2f} max={bl.max()}")
lat=[f for f in frames if f.get("last") is not None and f["last"]>1.0]
print(f"frames whose array was still being written >1.0 ms after the fc tick: {len(lat)}/{len(frames)}"
      + (f"  e.g. {[round(f['last'],2) for f in lat[:8]]}" if lat else ""))
json.dump(frames,open("phase_burst.json","w"))
