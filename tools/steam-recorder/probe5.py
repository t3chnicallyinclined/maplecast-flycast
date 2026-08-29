import sys, struct
import numpy as np
sys.path.insert(0, r"C:\Users\trist\AppData\Local\Temp\claude\c--Users-trist-projects\87af92a3-d210-44d2-94eb-ea46c3fdb39e\scratchpad")
from mvcmem import Mem, EXE, anchors
np.seterr(all='ignore')
m = Mem(); blk, arr = anchors(m)
BLKSZ = 0x33b18; d = m.read(blk, BLKSZ)
SLOT0 = 0x3f24; STRIDE = 0x738
sb = [SLOT0 + i*STRIDE for i in range(6)]
handles = {blk + sb[i] + 0x5CC: "s%d" % i for i in range(6)}
handles[blk + 0x3DB8] = "s6*"

POOL = 0x6D80          # candidate base (owner handle at node+0x80)
NSTRIDE = 0x280
print("=== object pool census: base=block+0x%X stride 0x%X, owner assumed at node+0x80 ===" % (POOL, NSTRIDE))
live = []
for i in range(300):
    n = POOL + i*NSTRIDE
    if n + NSTRIDE > BLKSZ:
        break
    own = struct.unpack_from('<Q', d, n + 0x80)[0]
    nz = any(d[n:n+NSTRIDE])
    if own in handles or nz:
        live.append((i, n, handles.get(own, "%016X" % own if own else "-"), nz))
print("   nodes with data: %d (first index %d, last %d)" %
      (len(live), live[0][0] if live else -1, live[-1][0] if live else -1))
for i, n, o, nz in live[:24]:
    print("   node%3d @block+0x%05X owner=%-6s nonzero=%s" % (i, n, o, nz))

print("\n=== full dump of the first node whose owner is a character handle ===")
tgt = None
for i, n, o, nz in live:
    if o.startswith('s'):
        tgt = (i, n, o); break
if tgt:
    i, n, o = tgt
    print("   node%d @block+0x%05X (arr%+#x) owner=%s" % (i, n, n - SLOT0, o))
    for off in range(0, 0x180, 16):
        a = n + off
        fs = struct.unpack_from('<4f', d, a)
        print("    +0x%03X %s | %s" % (off, d[a:a+16].hex(' '),
              " ".join(("%11.3f" % f) if 1e-9 < abs(f) < 1e9 else ("%11s" % ("0" if f == 0 else "-")) for f in fs)))

print("\n=== where exactly do the handles sit inside a 0x280 node? (mod-0x280 histogram) ===")
U = np.frombuffer(d[:BLKSZ//4*4], dtype=np.uint32)
hist = {}
for h in handles:
    for j in np.where(U == (h & 0xFFFFFFFF))[0]:
        off = int(j)*4
        if off < 0x6A74:
            continue
        hist.setdefault(off % NSTRIDE, []).append(off)
for k in sorted(hist):
    print("   offset %% 0x280 == 0x%03X : %d hits (first block+0x%05X)" % (k, len(hist[k]), hist[k][0]))

print("\n=== camera struct window, array-relative, annotated ===")
for a in range(0x68DC, 0x6A74, 4):
    v = struct.unpack_from('<f', d, a)[0]
    u = struct.unpack_from('<I', d, a)[0]
    if u == 0:
        continue
    note = ""
    if abs(v + 960) < .01: note = "  eyeX"
    if abs(v - 95) < .01: note = "  eyeY (DC clamp floor 95)"
    if abs(v - 812.3571) < .01: note = "  eyeZ (DC 812.29)"
    if abs(v - 433.4) < .01: note = "  ground line (DC 433.39)"
    if abs(abs(v) - 1280) < .01: note = "  stage bound"
    print("   arr+0x%05X (block+0x%05X) u32=%08X f32=%12.4f%s" % (a - SLOT0, a, u, v, note))
