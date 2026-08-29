"""Minimal ReadProcessMemory helper for Steam MvC2 (MarvelVsCapcomFightingCollection.exe).
Image base is fixed 0x140000000 (DYNAMIC_BASE off, no .reloc)."""
import ctypes, ctypes.wintypes as w, struct, sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

k32.OpenProcess.restype = w.HANDLE
k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.ReadProcessMemory.argtypes = [w.HANDLE, w.LPCVOID, w.LPVOID, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]

EXE = 0x140000000

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", w.DWORD), ("__a", w.DWORD),
                ("RegionSize", ctypes.c_size_t), ("State", w.DWORD),
                ("Protect", w.DWORD), ("Type", w.DWORD), ("__b", w.DWORD)]

def find_pid():
    import subprocess
    out = subprocess.check_output(
        ['powershell.exe', '-NoProfile', '-Command',
         "(Get-Process MarvelVsCapcomFightingCollection).Id"], text=True)
    return int(out.strip().splitlines()[0])

class Mem:
    def __init__(self, pid=None):
        self.pid = pid or find_pid()
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, self.pid)
        if not self.h:
            raise OSError("OpenProcess failed %d" % ctypes.get_last_error())

    def read(self, addr, n):
        buf = (ctypes.c_char * n)()
        got = ctypes.c_size_t(0)
        ok = k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
        if not ok or got.value != n:
            return None
        return bytes(buf)

    def u8(self, a):
        b = self.read(a, 1);  return None if b is None else b[0]
    def u16(self, a):
        b = self.read(a, 2);  return None if b is None else struct.unpack('<H', b)[0]
    def u32(self, a):
        b = self.read(a, 4);  return None if b is None else struct.unpack('<I', b)[0]
    def i32(self, a):
        b = self.read(a, 4);  return None if b is None else struct.unpack('<i', b)[0]
    def u64(self, a):
        b = self.read(a, 8);  return None if b is None else struct.unpack('<Q', b)[0]
    def f32(self, a):
        b = self.read(a, 4);  return None if b is None else struct.unpack('<f', b)[0]

    def regions(self, lo=0x10000, hi=0x7FFFFFFFFFFF):
        mbi = MBI(); a = lo
        while a < hi:
            r = k32.VirtualQueryEx(self.h, ctypes.c_void_p(a), ctypes.byref(mbi),
                                   ctypes.sizeof(mbi))
            if not r:
                break
            base = mbi.BaseAddress or 0
            size = mbi.RegionSize or 0
            if size == 0:
                break
            yield base, size, mbi.State, mbi.Protect, mbi.Type
            a = base + size

def anchors(m):
    """Return (block_base, array_base) or (None, None)."""
    blk = m.u64(EXE + 0xAC6EF0)
    if not blk:
        return None, None
    return blk, blk + 0x3F24
