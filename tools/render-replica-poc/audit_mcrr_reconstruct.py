#!/usr/bin/env python3
"""audit_mcrr_reconstruct.py — read-set coverage audit, step 1.

Reconstruct, from a captured live MCRR stream, the two 16MB RAM images the CLIENT
render_frame actually sees:
  SEED  = the static prefix's ram16 (frozen at connect; overlaid by static GFX regions)
  LIVE@N = SEED with frame N's dynamic regions overlaid (== exactly client RAM at frame N)

Writes both as flat 16MB dumps so the instrumented audit harness can run render_frame
over LIVE@N and we can diff SEED vs LIVE per read region.

Also reports, for EVERY dynamic region, how many bytes changed seed->frameN (proof the
region is live), and dumps the whole-RAM seed-vs-liveN diff coalesced into runs so we can
see what STATE moves OUTSIDE the shipped dyn regions (the stale-seed candidates).

Usage: python audit_mcrr_reconstruct.py <capture.mcrr> <frameIndex> [outprefix]
NOT a fix; a diagnostic. Does not touch any shared transpiled source.
"""
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else 'side_cap.mcrr'
frame_idx = int(sys.argv[2]) if len(sys.argv) > 2 else 120
outpref = sys.argv[3] if len(sys.argv) > 3 else '_audit'

buf = open(path, 'rb').read()
p = 0
def u32():
    global p
    v = struct.unpack_from('<I', buf, p)[0]; p += 4; return v
assert u32() == 0x5252434D, 'bad MCRR magic'
ver = u32(); nS = u32(); nD = u32(); nF = u32(); vram = u32(); pvr = u32(); u32()
def region():
    global p
    a = u32(); l = u32(); tag = buf[p:p+8].split(b'\0')[0].decode('latin1'); p += 8
    return (a, l, tag)
S = [region() for _ in range(nS)]
D = [region() for _ in range(nD)]
vramOff = p; p += vram; pvrOff = p; p += pvr
staticData = []
for (a, l, t) in S:
    staticData.append(buf[p:p+l]); p += l
frameStart = p

RAM = 16*1024*1024
seed = bytearray(RAM)
for (a, l, t), data in zip(S, staticData):
    off = 0 if t == 'ram16' else (a & 0xFFFFFF)
    seed[off:off+len(data)] = data

# index frames
frames = []
p = frameStart
dynbytes = sum(l for _, l, _ in D)
HDR = 12  # FRMx + vframe + taSize (the dyn payload follows immediately)
TRAILER = 4  # per-frame trailer (checksum/pad) after the dyn payload
STRIDE = dynbytes + HDR + TRAILER  # steady-state frame stride (taSize=0 state-only stream)
# Locate the true frame stream by the longest evenly-spaced FRMx chain (frame 0 may
# carry an extra one-shot payload; the steady chain is the reliable anchor).
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
print(f"  (frame chain: {chain_n} frames @ stride {STRIDE:#x}, starts {chain_start:#x})")
p = chain_start
for f in range(chain_n):
    fm = struct.unpack_from('<I', buf, p)[0]; p += 4
    assert fm == 0x784D5246, f'frame {f} bad FRMx @ {p-4:#x}'
    vf = struct.unpack_from('<I', buf, p)[0]; p += 4
    taSize = struct.unpack_from('<I', buf, p)[0]; p += 4
    dynOff = p
    p += dynbytes
    taOff = p; p += taSize + TRAILER
    frames.append((vf, taSize, dynOff, taOff))
nF = chain_n
print(f"MCRR {path}: nStatic={nS} nDynamic={nD} nFrames={nF}")
if frame_idx >= nF:
    frame_idx = nF // 2
    print(f"  (frame index clamped to {frame_idx})")

# build LIVE@frame_idx
live = bytearray(seed)
vf, taSize, dynOff, taOff = frames[frame_idx]
off = dynOff
dynspans = []  # (lo,hi,tag) guest-normalized idx space
print(f"\nframe {frame_idx} vframe={vf} taSize={taSize}")
print("per-dyn-region change vs seed:")
for (a, l, t) in D:
    chunk = buf[off:off+l]
    gi = a & 0xFFFFFF
    live[gi:gi+l] = chunk
    dynspans.append((gi, gi+l, t))
    # count changed bytes seed->live for this region
    nch = sum(1 for i in range(l) if seed[gi+i] != chunk[i])
    print(f"  {a:#010x} {t:<8} len={l:<7} changed={nch}")
    off += l

open(f'{outpref}_seed.bin', 'wb').write(seed)
open(f'{outpref}_live.bin', 'wb').write(live)
print(f"\nwrote {outpref}_seed.bin and {outpref}_live.bin (16MB each)")

# whole-RAM seed-vs-live diff, coalesced into runs, EXCLUDING the shipped dyn regions.
# These runs are STATE that changed between connect and frame N but is NOT in any dyn
# region => served stale from the seed. (Not all are read by render_frame; the C harness
# intersects with the actual read-set. This is the universe of stale candidates.)
def in_dyn(gi):
    for lo, hi, t in dynspans:
        if lo <= gi < hi: return t
    return None

print("\nseed-vs-live RAM runs OUTSIDE shipped dyn regions (stale-seed candidates, >=4B):")
runs = []
i = 0
while i < RAM:
    if seed[i] != live[i] and in_dyn(i) is None:
        j = i
        while j < RAM and seed[j] != live[j] and in_dyn(j) is None:
            j += 1
        runs.append((i, j))
        i = j
    else:
        i += 1
# merge runs with small gaps (<=64) for readability
merged = []
for lo, hi in runs:
    if merged and lo - merged[-1][1] <= 64:
        merged[-1] = (merged[-1][0], hi)
    else:
        merged.append([lo, hi])
total = sum(hi-lo for lo, hi in merged)
print(f"  {len(merged)} runs, {total} bytes changed outside dyn regions")
for lo, hi in merged[:80]:
    print(f"    0x8C{lo:06X}..0x8C{hi:06X}  ({hi-lo}B)")
if len(merged) > 80:
    print(f"    ... +{len(merged)-80} more runs")
