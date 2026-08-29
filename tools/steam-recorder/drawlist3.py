import sys, struct, collections
import numpy as np
sys.path.insert(0, r"C:\Users\trist\AppData\Local\Temp\claude\c--Users-trist-projects\87af92a3-d210-44d2-94eb-ea46c3fdb39e\scratchpad")
from mvcmem import Mem, EXE, anchors
np.seterr(all='ignore')
m = Mem(); blk, arr = anchors(m)
BLKSZ = 0x33b18; d = m.read(blk, BLKSZ)
POOL = blk + 0x6D80; NS = 0x280; NN = 256
HBASE = 0x58                       # node handle = node + 0x58
lo = (POOL + HBASE) & 0xFFFFFFFF
U = np.frombuffer(d[:BLKSZ//4*4], dtype=np.uint32)
ishandle = np.zeros(len(U), bool)
sel = np.where((U >= lo) & (U < lo + NN*NS))[0]
for j in sel:
    if (int(U[j]) - lo) % NS == 0:
        ishandle[j] = True
# also character handles count as draw-list entries
chars = {(blk + 0x3f24 + i*0x738 + 0x5CC) & 0xFFFFFFFF for i in range(6)}
chars.add((blk + 0x3DB8) & 0xFFFFFFFF)
for j in np.where(np.isin(U, list(chars)))[0]:
    ishandle[j] = True

print("=== stride-8 runs of OBJECT HANDLES outside the pool and outside the fighter array ===")
i = 0
runs = []
while i < len(U):
    if ishandle[i]:
        j = i
        while j + 2 < len(U) and ishandle[j+2]:
            j += 2
        off = i*4
        if not (0x6D80 <= off < 0x6D80 + NN*NS) and not (0x3f24 <= off < 0x3f24+6*0x738):
            runs.append((off, (j - i)//2 + 1))
        i = j + 2
    else:
        i += 1
for off, n in runs:
    if n >= 2:
        print("   block+0x%05X (arr%+#x)  %d entries" % (off, off - 0x3f24, n))

print("\n=== spacing between run starts (looking for 0x300 = 96 ptrs x 8 = DC 0x180 doubled) ===")
st = [o for o, n in runs if n >= 2]
for a, b in zip(st, st[1:]):
    print("   0x%05X -> 0x%05X   delta 0x%X" % (a, b, b - a))

print("\n=== candidate count arrays: 16 small bytes near the table ===")
for base in (0x2FDD0, 0x300D0, 0x330D0, 0x2FFD0, 0x30000):
    if base + 0x20 < BLKSZ:
        print("   block+0x%05X: %s" % (base, d[base:base+0x20].hex(' ')))

print("\n=== dump the three runs with their node indices ===")
for off, n in runs:
    if n < 2:
        continue
    print("  -- run @block+0x%05X" % off)
    for k in range(min(n, 24)):
        v = struct.unpack_from('<Q', d, off + k*8)[0]
        rel = v - blk
        if 0x6D80 <= rel < 0x6D80 + NN*NS:
            print("     [%2d] node%3d  (block+0x%05X)" % (k, (rel - 0x6D80 - HBASE)//NS, rel))
        else:
            print("     [%2d] %016X (block+0x%05X)" % (k, v, rel))
