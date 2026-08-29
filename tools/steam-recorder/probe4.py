import sys, struct
import numpy as np
sys.path.insert(0, r"C:\Users\trist\AppData\Local\Temp\claude\c--Users-trist-projects\87af92a3-d210-44d2-94eb-ea46c3fdb39e\scratchpad")
from mvcmem import Mem, EXE, anchors
np.seterr(all='ignore')
m = Mem(); blk, arr = anchors(m)
BLKSZ = 0x33b18; d = m.read(blk, BLKSZ)
SLOT0 = 0x3f24; STRIDE = 0x738
sb = [SLOT0 + i*STRIDE for i in range(6)]
U = np.frombuffer(d[:BLKSZ//4*4], dtype=np.uint32)

print("=== every u32 in the block that equals a character HANDLE (slot+0x5CC or 0x3DB8) ===")
targets = {blk + sb[i] + 0x5CC: "char%d(slot%d)" % (i, i) for i in range(6)}
targets[blk + 0x3DB8] = "char6(block+0x3DB8)"
for v, nm in targets.items():
    for j in np.where(U == (v & 0xFFFFFFFF))[0]:
        off = int(j)*4
        owner = ""
        for k in range(6):
            if sb[k] <= off < sb[k] + STRIDE:
                owner = " [in slot%d at +0x%03X]" % (k, off - sb[k])
        if off >= 0x32500 and off < 0x32600:
            owner = " [battle_globals+0x%02X]" % (off - 0x32500)
        print("   block+0x%05X (arr%+#x) -> %s%s" % (off, off - SLOT0, nm, owner))

print("\n=== EnemyPointer probe: slot+0x5DC in every slot ===")
for i in range(6):
    v = struct.unpack_from('<Q', d, sb[i]+0x5DC)[0]
    rel = v - blk if blk <= v < blk+BLKSZ else None
    tag = "NULL" if v == 0 else ("block+0x%05X" % rel if rel is not None else "%016X" % v)
    if rel is not None:
        for k in range(6):
            if sb[k] <= rel < sb[k]+STRIDE:
                tag += " == slot%d+0x%03X" % (k, rel - sb[k])
    print("   slot%d (cid 0x%02X, side %d) -> %s" % (i, d[sb[i]+0x554], d[sb[i]+8], tag))

print("\n=== full dump slot5+0x580 .. slot5+0x738 (the camera-shaped region) ===")
for o in range(sb[5]+0x580, sb[5]+0x738, 16):
    fs = struct.unpack_from('<4f', d, o)
    print("  s5+0x%03X blk+0x%05X %s | %s" % (o - sb[5], o, d[o:o+16].hex(' '),
          " ".join(("%11.3f" % f) if 1e-9 < abs(f) < 1e9 else ("%11s" % ("0" if f == 0 else "-")) for f in fs)))

print("\n=== same window in slot0 for contrast ===")
for o in range(sb[0]+0x580, sb[0]+0x738, 16):
    fs = struct.unpack_from('<4f', d, o)
    print("  s0+0x%03X blk+0x%05X %s | %s" % (o - sb[0], o, d[o:o+16].hex(' '),
          " ".join(("%11.3f" % f) if 1e-9 < abs(f) < 1e9 else ("%11s" % ("0" if f == 0 else "-")) for f in fs)))

print("\n=== camera cross-check with the 7th object (wx=-1155, sx=125, wy=2.14286, sy=431.26) ===")
c = 0x37EC
wx = struct.unpack_from('<f', d, c+0x61c)[0]; wy = struct.unpack_from('<f', d, c+0x620)[0]
sx = struct.unpack_from('<f', d, c+0x6f0)[0]; sy = struct.unpack_from('<f', d, c+0x6f4)[0]
print("   eyeX = wx - sx + 320   = %.4f" % (wx - sx + 320.0))
print("   eyeY = sy - 240 + wy - 98.394 = %.4f" % (sy - 240.0 + wy - 98.394))
print("   stored triple @block+0x6914: (%.4f, %.4f, %.4f)" % struct.unpack_from('<3f', d, 0x6914))
print("   stored triple @block+0x6920: (%.4f, %.4f, %.4f)" % struct.unpack_from('<3f', d, 0x6920))
