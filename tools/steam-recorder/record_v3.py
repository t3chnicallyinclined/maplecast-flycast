#!/usr/bin/env python3
"""record_v3.py <out.v3> [seconds] — FRAME-SYNCED tape-v2 recorder (goldmine coherency law).
Locks to the game frame counter (arr-0x25c, +1/frame @60fps): one coherent sample per game
frame, tagged with the real frame number. Coherence guard: read fc, read all slots, read fc
again — if it changed mid-read the sample straddled a frame boundary, so retry (no shattered
bodies). Proven Steam offsets: active+0x004 sid+0x1c hp+0x40c red+0x410 cid+0x554
world+0x61c/+0x620 facing+0x720.
File: V3SYNC02, then per frame [u32 fc][6 x <B active,B cid,H sid,H hp,H red,B fac,f wx,f wy,f sx,f sy>]."""
import sys, time, struct, ctypes, subprocess
from ctypes import wintypes
STRIDE=0x738; PTR_OFF=0xAC6EF0; ARR_ADD=0x3F24; FC_REL=-0x25c
OFF_ACT=0x004; OFF_CID=0x554; OFF_SID=0x1c; OFF_HP=0x40c; OFF_RED=0x410; OFF_FAC=0x720; OFF_WX=0x61c; OFF_WY=0x620
OFF_SX=0x6f0; OFF_SY=0x6f4  # GAME-COMPUTED screen x/y (render-authoritative; DC +0xE0/+0xE4 analogue)
k32=ctypes.WinDLL("kernel32",use_last_error=True); psapi=ctypes.WinDLL("psapi",use_last_error=True)
def find_pid():
    for ln in subprocess.run(["tasklist","/FO","CSV"],capture_output=True,text=True).stdout.splitlines():
        if ln.startswith('"MarvelVsCapcom'): return int(ln.split('","')[1])
def rpm(h,a,n):
    b=ctypes.create_string_buffer(n); g=ctypes.c_size_t()
    return b.raw if k32.ReadProcessMemory(h,ctypes.c_void_p(a),b,n,ctypes.byref(g)) and g.value==n else None
pid=find_pid()
if not pid: sys.exit("MarvelVsCapcom not running")
h=k32.OpenProcess(0x0410,False,pid)
mods=(wintypes.HMODULE*1)(); need=wintypes.DWORD()
psapi.EnumProcessModulesEx(h,mods,ctypes.sizeof(mods),ctypes.byref(need),0x03); base=mods[0]
blk=struct.unpack("<Q",rpm(h,base+PTR_OFF,8))[0]
if not (0x10000<blk<0x7FFFFFFFFFFF): sys.exit(f"match ptr invalid {blk:#x}")
arr=blk+ARR_ADD; fca=arr+FC_REL
def read_fc():
    b=rpm(h,fca,4); return struct.unpack("<I",b)[0] if b else None
out=sys.argv[1]; secs=float(sys.argv[2]) if len(sys.argv)>2 else 30.0
f=open(out,"wb"); f.write(b"V3SYNC02")
last_fc=None; n=0; straddle=0; t_end=time.time()+secs
while time.time()<t_end:
    fc0=read_fc()
    if fc0 is None: continue
    if fc0==last_fc:
        time.sleep(0.002); continue          # same frame — wait for advance
    slots=[rpm(h,arr+i*STRIDE,STRIDE) for i in range(6)]
    fc1=read_fc()
    if any(s is None for s in slots) or fc1!=fc0:
        straddle+=1; continue                 # straddled a frame boundary — discard, retry
    rec=struct.pack("<I",fc0)
    for s in slots:
        rec+=struct.pack("<BBHHHBffff", s[OFF_ACT], s[OFF_CID],
            struct.unpack_from("<H",s,OFF_SID)[0],
            struct.unpack_from("<I",s,OFF_HP)[0]&0xFFFF,
            struct.unpack_from("<I",s,OFF_RED)[0]&0xFFFF,
            s[OFF_FAC],
            struct.unpack_from("<f",s,OFF_WX)[0],
            struct.unpack_from("<f",s,OFF_WY)[0],
            struct.unpack_from("<f",s,OFF_SX)[0],
            struct.unpack_from("<f",s,OFF_SY)[0])
    f.write(rec); last_fc=fc0; n+=1
f.close()
print(f"recorded {n} frame-synced samples ({straddle} straddles discarded) -> {out}")
