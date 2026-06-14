#!/usr/bin/env python3
"""audit_seed_stale_scan.py — read-set coverage audit, step 2 (full-stream stale scan).

For a captured live MCRR, reconstruct the SEED (static prefix ram16) and then, for EVERY
frame, overlay that frame's dynamic regions and compute the set of RAM bytes that differ
from the SEED but lie OUTSIDE all shipped dynamic regions. Union across all frames =
the COMPLETE universe of state that moves during play yet is served stale from the seed.

If that union is empty, the dyn region set fully covers every byte that ever changes ->
no stale-seed field can drive a stray (any residual stray is a CLIENT/decode issue, not a
missing wire field). If non-empty, each run is a candidate missing wire region; we then
intersect with the render_frame read-set (audit_readset harness) to find the ones that
actually feed geometry.

Usage: python audit_seed_stale_scan.py <capture.mcrr>
"""
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else 'side_cap.mcrr'
buf = open(path, 'rb').read()
p = 0
def u32():
    global p
    v = struct.unpack_from('<I', buf, p)[0]; p += 4; return v
assert u32() == 0x5252434D
ver = u32(); nS = u32(); nD = u32(); nF = u32(); vram = u32(); pvr = u32(); u32()
def region():
    global p
    a = u32(); l = u32(); tag = buf[p:p+8].split(b'\0')[0].decode('latin1'); p += 8
    return (a, l, tag)
S = [region() for _ in range(nS)]
D = [region() for _ in range(nD)]
p += vram + pvr
staticData = []
for (a, l, t) in S:
    staticData.append(buf[p:p+l]); p += l
frameStart = p
RAM = 16*1024*1024
seed = bytearray(RAM)
for (a, l, t), data in zip(S, staticData):
    off = 0 if t == 'ram16' else (a & 0xFFFFFF)
    seed[off:off+len(data)] = data

dynbytes = sum(l for _, l, _ in D)
STRIDE = dynbytes + 16
needle = bytes([0x46, 0x52, 0x4D, 0x78])
allpos = []
i = buf.find(needle)
while i >= 0:
    allpos.append(i); i = buf.find(needle, i+4)
posset = set(allpos)
best = (0, frameStart)
for x in allpos:
    n = 0; c = x
    while c in posset and struct.unpack_from('<I', buf, c)[0] == 0x784D5246:
        n += 1; c += STRIDE
    if n > best[0]: best = (n, x)
chain_n, chain_start = best

dynspans = [(a & 0xFFFFFF, (a & 0xFFFFFF)+l, t) for (a, l, t) in D]
def in_dyn(gi):
    for lo, hi, t in dynspans:
        if lo <= gi < hi: return True
    return False

# Build a covered-mask once (dyn regions are constant addrs across frames)
import numpy as np
seed_np = np.frombuffer(bytes(seed), dtype=np.uint8)
covered = np.zeros(RAM, dtype=bool)
for lo, hi, t in dynspans:
    covered[lo:hi] = True

union_changed = np.zeros(RAM, dtype=bool)
print(f"MCRR {path}: {chain_n} frames @ {STRIDE:#x} from {chain_start:#x}")
maxframes = chain_n
for f in range(maxframes):
    fp = chain_start + f*STRIDE + 12  # skip 16-byte header to dyn payload
    live = bytearray(seed)
    off = fp
    for (a, l, t) in D:
        gi = a & 0xFFFFFF
        live[gi:gi+l] = buf[off:off+l]; off += l
    live_np = np.frombuffer(bytes(live), dtype=np.uint8)
    diff = (live_np != seed_np) & (~covered)
    union_changed |= diff

n_outside = int(union_changed.sum())
print(f"\nUNION over {maxframes} frames: {n_outside} bytes ever change OUTSIDE shipped dyn regions")
if n_outside == 0:
    print(">>> SEED COVERAGE COMPLETE: no byte read-or-not ever moves outside the dyn set.")
    print(">>> Any residual stray is NOT a missing wire field for THIS capture's object set.")
else:
    # coalesce
    idx = np.flatnonzero(union_changed)
    runs = []
    s = idx[0]; prev = idx[0]
    for x in idx[1:]:
        if x - prev <= 64:
            prev = x
        else:
            runs.append((s, prev+1)); s = x; prev = x
    runs.append((s, prev+1))
    print(f"{len(runs)} runs:")
    for lo, hi in runs[:120]:
        print(f"  0x8C{lo:06X}..0x8C{hi:06X}  ({hi-lo}B)")
