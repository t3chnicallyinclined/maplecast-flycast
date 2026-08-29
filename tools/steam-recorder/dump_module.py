#!/usr/bin/env python3
"""dump_module.py <out.bin> — dump the UNPACKED main-module image of the running Steam MvC2
process (the on-disk exe is packed/DRM'd; only the runtime image has real code).
Writes a FLAT image: file offset N == virtual address (base + N), gaps zero-filled, so it
loads into Ghidra as Raw Binary at base 0x140000000 with every address matching live memory.
"""
import sys, struct, ctypes, subprocess
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p),
                ("SizeOfImage", wintypes.DWORD),
                ("EntryPoint", ctypes.c_void_p)]

class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_ulonglong),
                ("AllocationBase", ctypes.c_ulonglong),
                ("AllocationProtect", wintypes.DWORD),
                ("__alignment1", wintypes.DWORD),
                ("RegionSize", ctypes.c_ulonglong),
                ("State", wintypes.DWORD),
                ("Protect", wintypes.DWORD),
                ("Type", wintypes.DWORD),
                ("__alignment2", wintypes.DWORD)]

def find_pid():
    for ln in subprocess.run(["tasklist", "/FO", "CSV"], capture_output=True, text=True).stdout.splitlines():
        if ln.startswith('"MarvelVsCapcom'):
            return int(ln.split('","')[1])
    return None

pid = find_pid()
if not pid:
    sys.exit("MarvelVsCapcom not running — launch the game first")
h = k32.OpenProcess(0x0410, False, pid)   # VM_READ | QUERY_INFORMATION
if not h:
    sys.exit(f"OpenProcess failed {ctypes.get_last_error()}")

psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_void_p),
                                       wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
psapi.GetModuleInformation.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                       ctypes.POINTER(MODULEINFO), wintypes.DWORD]
k32.VirtualQueryEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                               ctypes.POINTER(MEMORY_BASIC_INFORMATION64), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
mods = (ctypes.c_void_p * 1)()
needed = wintypes.DWORD()
psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(needed), 0x03)
mi = MODULEINFO()
psapi.GetModuleInformation(h, ctypes.c_void_p(mods[0]), ctypes.byref(mi), ctypes.sizeof(mi))
base = mi.lpBaseOfDll
size = mi.SizeOfImage
print(f"pid={pid} module base={base:#x} SizeOfImage={size:#x} ({size/1024/1024:.1f} MB)")

out = open(sys.argv[1], "wb")
CHUNK = 0x10000
buf = ctypes.create_string_buffer(CHUNK)
got = ctypes.c_size_t()
mbi = MEMORY_BASIC_INFORMATION64()
written = 0
readable = 0
addr = base
end = base + size
while addr < end:
    # query the region to skip unreadable pages fast
    if k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0:
        span = CHUNK
        ok = False
    else:
        span = min(int(mbi.RegionSize - (addr - mbi.BaseAddress)), end - addr)
        if span <= 0:
            span = CHUNK
        # MEM_COMMIT and not PAGE_NOACCESS/GUARD
        ok = (mbi.State == 0x1000) and not (mbi.Protect & 0x101) and mbi.Protect != 0
    pos = 0
    while pos < span:
        n = min(CHUNK, span - pos)
        data = b"\x00" * n
        if ok:
            if k32.ReadProcessMemory(h, ctypes.c_void_p(addr + pos), buf, n, ctypes.byref(got)) and got.value:
                data = buf.raw[:got.value] + b"\x00" * (n - got.value)
                readable += got.value
        out.write(data)
        written += n
        pos += n
    addr += span
out.close()
print(f"wrote {written} bytes ({written/1024/1024:.1f} MB), {readable/1024/1024:.1f} MB readable -> {sys.argv[1]}")
print(f"LOAD IN GHIDRA: Raw Binary, x86:LE:64:default, base address {base:#x}")
