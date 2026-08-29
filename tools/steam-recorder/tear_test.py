#!/usr/bin/env python3
"""tear_test.py [seconds] — does a NAIVE "fc edge -> read immediately" recorder tear?

Per frame: tight-poll fc; the instant it increments, read the 6-slot array (A).  Then wait
2 ms (well past the measured 0.13 ms write burst) and read it again (B), plus re-read fc to
prove we are still in the SAME game frame.  A != B  =>  A was captured mid-update: a torn /
half-old snapshot (flycast's "shattered bodies" class).  Prints tear rate for the immediate
read and for a settled read (edge + 0.5 ms).
"""
import sys, time, struct, ctypes, subprocess
from ctypes import wintypes
STRIDE=0x738; PTR_OFF=0xAC6EF0; ARR_ADD=0x3F24; FC_REL=-0x25c; NS=6
k32=ctypes.WinDLL("kernel32",use_last_error=True); psapi=ctypes.WinDLL("psapi",use_last_error=True)
def find_pid():
    for ln in subprocess.run(["tasklist","/FO","CSV"],capture_output=True,text=True).stdout.splitlines():
        if ln.startswith('"MarvelVsCapcom'): return int(ln.split('","')[1])
pid=find_pid(); h=k32.OpenProcess(0x0410,False,pid)
mods=(wintypes.HMODULE*1)(); need=wintypes.DWORD()
psapi.EnumProcessModulesEx(h,mods,ctypes.sizeof(mods),ctypes.byref(need),0x03); base=mods[0]
g=ctypes.c_size_t()
tmp=ctypes.create_string_buffer(8)
k32.ReadProcessMemory(h,ctypes.c_void_p(base+PTR_OFF),tmp,8,ctypes.byref(g))
blk=struct.unpack("<Q",tmp.raw)[0]; arr=blk+ARR_ADD; fca=arr+FC_REL
A=ctypes.create_string_buffer(NS*STRIDE); Bb=ctypes.create_string_buffer(NS*STRIDE)
S=ctypes.create_string_buffer(NS*STRIDE)
f4=ctypes.create_string_buffer(4)
def fc():
    k32.ReadProcessMemory(h,ctypes.c_void_p(fca),f4,4,ctypes.byref(g)); return struct.unpack("<I",f4.raw)[0]
def slots(buf): return k32.ReadProcessMemory(h,ctypes.c_void_p(arr),buf,NS*STRIDE,ctypes.byref(g))
secs=float(sys.argv[1]) if len(sys.argv)>1 else 10.0
tend=time.perf_counter()+secs
n=0; tear_imm=0; tear_settled=0; sameframe=0
last=fc()
while time.perf_counter()<tend:
    v=fc()
    if v==last: continue
    last=v
    t_edge=time.perf_counter()
    slots(A)                                    # NAIVE: read the instant fc ticks
    while time.perf_counter()-t_edge < 0.0005: pass
    slots(S)                                    # SETTLED: edge + 0.5 ms
    while time.perf_counter()-t_edge < 0.002: pass
    slots(Bb)                                   # REFERENCE: edge + 2 ms
    if fc()!=v:                                 # frame advanced under us -> not a valid comparison
        continue
    sameframe+=1; n+=1
    if A.raw!=Bb.raw: tear_imm+=1
    if S.raw!=Bb.raw: tear_settled+=1
print(f"frames compared (same-frame verified): {n}")
print(f"  IMMEDIATE read (edge+~5us) differed from settled truth: {tear_imm}/{n} = {100.0*tear_imm/max(n,1):.1f}%")
print(f"  SETTLED   read (edge+500us) differed from settled truth: {tear_settled}/{n} = {100.0*tear_settled/max(n,1):.1f}%")
