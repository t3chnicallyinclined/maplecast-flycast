#!/usr/bin/env python3
"""Step 1 proof: keyframe/delta the TDW inner stream (the loss-tolerant TDW wire).

Reads the per-frame TDW inner payloads dumped by `maplecast-native gate <cap>
--dump-inner <file>` ([u32 len][bytes] per frame) and applies the SAME proven
keyframe/delta codec we use for the state wire:
  - keyframe every N frames: full inner (independently decodable)
  - delta: gap-merged runs of bytes differing from the last KEYFRAME
So a lost frame just skips one; the next delta rebases on the keyframe -> NO
permanent desync (unlike streaming zstd, which G0 proved dies on one loss).

Proves: (1) byte-exact reconstruction, (2) loss-tolerance (inject drops, no
cascade), (3) bandwidth vs the raw inner and vs the current streaming-zstd wire.

Usage: python statewire_tdw_kf.py <inner.bin> [--key N] [--drop PCT] [--merge M]
"""
import struct, sys, zlib
import numpy as np

path = sys.argv[1]
KEY = 60; DROP = 0.0; MERGE = 8; PREV = '--prev' in sys.argv
a = 2
while a < len(sys.argv):
    if sys.argv[a] == '--key':   KEY   = int(sys.argv[a+1]); a += 2
    elif sys.argv[a] == '--drop': DROP = float(sys.argv[a+1]); a += 2
    elif sys.argv[a] == '--merge': MERGE = int(sys.argv[a+1]); a += 2
    else: a += 1
try:
    import zstandard as _z; _c = _z.ZstdCompressor(level=3); zc = lambda b: len(_c.compress(b)); zl = "zstd-3"
except Exception:
    zc = lambda b: len(zlib.compress(b, 6)); zl = "zlib-6"

buf = open(path, 'rb').read()
frames = []
p = 0
while p + 4 <= len(buf):
    ln = struct.unpack_from('<I', buf, p)[0]; p += 4
    if p + ln > len(buf): break
    frames.append(np.frombuffer(buf, np.uint8, ln, p)); p += ln
print(f"== TDW keyframe/delta proof ==")
print(f"inner stream : {path}  ({len(frames)} frames)")
if not frames: sys.exit("no frames")

def runs_vs(cur, key):
    n = min(len(cur), len(key))
    # size change => the whole frame differs from here; treat tail as one run
    d = np.nonzero(cur[:n] != key[:n])[0]
    tail = [] if len(cur) == len(key) else [(n, len(cur) - n)]
    if d.size == 0: return tail
    brk = np.nonzero(np.diff(d) > MERGE)[0]
    st = np.concatenate(([d[0]], d[brk+1])); en = np.concatenate((d[brk], [d[-1]]))
    return [(int(s), int(e - s + 1)) for s, e in zip(st, en)] + tail

key = None; keyid = 0; dec_key = None; dec_prev = None
raw=[]; enc_sz=[]; encz=[]; rawz=[]
ok = True; corrupt = 0; ndelta = 0; nkey = 0
rng = np.random.default_rng(1234); dropped = 0
for fi, cur in enumerate(frames):
    is_key = (fi % KEY == 0)
    if is_key:
        enc = b'\x01' + struct.pack('<I', len(cur)) + cur.tobytes(); key = cur; keyid = fi; nkey += 1
    else:
        ref = (dec_prev if PREV else key)
        ref = ref if ref is not None else cur[:0]
        rs = runs_vs(cur, ref)
        # header carries the target length so decode handles grow/shrink of the inner
        body = bytearray(struct.pack('<III', keyid, len(cur), len(rs)))
        for off, ln in rs: body += struct.pack('<II', off, ln) + cur[off:off+ln].tobytes()
        enc = b'\x00' + bytes(body); ndelta += 1
        if PREV: key = cur
    raw.append(len(cur))
    if (not is_key) and DROP > 0 and rng.random() < DROP/100.0:
        dropped += 1; continue                  # dropped on the wire — decoder holds last frame
    rawz.append(zc(cur.tobytes())); enc_sz.append(len(enc)); encz.append(zc(enc))
    # decode with the decoder's own state (so loss propagates)
    if enc[0] == 1:
        rl = struct.unpack_from('<I', enc, 1)[0]; dec = np.frombuffer(enc, np.uint8, rl, 5).copy(); dec_key = dec
    else:
        _, target, nr = struct.unpack_from('<III', enc, 1); base = (dec_prev if PREV else dec_key)
        dec = np.zeros(target, np.uint8)                 # target length from the header
        m = min(target, len(base)); dec[:m] = base[:m]   # copy the keyframe prefix
        o = 13
        for _ in range(nr):
            off, ln = struct.unpack_from('<II', enc, o); o += 8
            dec[off:off+ln] = np.frombuffer(enc, np.uint8, ln, o); o += ln
    dec_prev = dec
    if not (len(dec) == len(cur) and np.array_equal(dec, cur)):
        corrupt += 1; ok = False

raw=np.array(raw); es=np.array(enc_sz); ez=np.array(encz); rz=np.array(rawz)
print(f"config       : keyframe/{KEY} | merge {MERGE} | drop {DROP}% | {'PREV' if PREV else 'keyframe-delta'} | comp {zl}")
print(f"encoded      : {nkey} keyframes + {ndelta} deltas ; dropped {dropped}")
print(f"CORRECTNESS  : {'PASS - decode == inner, byte-exact' if ok and corrupt==0 else f'FAIL - {corrupt} corrupt'}")
print(f"BANDWIDTH (bytes/frame):")
print(f"  raw inner            : {raw.mean():9.0f}")
print(f"  raw inner + {zl:<7}  : {rz.mean():9.0f}   (~ the current streaming-zstd wire, per-frame independent)")
print(f"  kf/delta             : {es.mean():9.0f}   (p50 {np.percentile(es,50):.0f})")
print(f"  kf/delta + {zl:<7}   : {ez.mean():9.0f}   (p50 {np.percentile(ez,50):.0f})  <- the loss-tolerant wire")
if DROP>0:
    print(f"LOSS-TOLERANCE: dropped {dropped} deltas -> {'PASS, zero cascade (every surviving frame byte-exact)' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
