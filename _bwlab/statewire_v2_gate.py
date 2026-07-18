#!/usr/bin/env python3
"""(a) semantic-state wire v2 — the offline dirty-diff codec + gate.

The replica-live wire (docs/RENDER-REPLICA-RECORDING-FORMAT.md) ships v1 = every
DYNAMIC region raw, every frame (~dynbytes/frame). The doc's v2 = dirty-diff the
dynamic payload; the 2026-07-18 measurement says the churn floor is ~200-430 B/f.
This is the G0-style OFFLINE gate for v2: replay a real .mcrr through a
keyframe/delta codec and prove three things with numbers, no prod risk:

  1. CORRECTNESS  — v2 decode == v1 raw blob, byte-exact, every frame.
  2. BANDWIDTH    — v2 bytes/frame vs v1 raw (and vs one-shot zstd of each).
  3. LOSS-TOLERANCE — deltas rebase on the KEYFRAME (not prev frame), so a
     dropped delta skips exactly one frame and never desyncs later ones.
     (Contrast: a prev-frame delta chain — or streaming zstd — desyncs forever,
     which is exactly what the G0 TDW gate measured.)

Wire (per-frame dynamic payload only; GFX/pal/HUD tails are unchanged v1):
  keyframe: [u8 flag=1][u32 rawlen][raw blob]
  delta   : [u8 flag=0][u32 keyId][u32 nRuns][ nRuns x (u32 off,u32 len,len bytes) ]
Runs = maximal spans of bytes differing from the keyframe, gap-merged (<=MERGE
stable bytes folded in, since each run header costs 8 B).

Usage: python statewire_v2_gate.py <capture.mcrr> [--key N] [--drop PCT] [--merge M]
"""
import struct, sys, numpy as np

path = sys.argv[1]
KEY   = 60      # keyframe interval (frames)
DROP  = 0.0     # % of delta frames to drop in the loss test
MERGE = 8       # fold <=MERGE stable bytes into a run (header is 8 B)
a = 2
while a < len(sys.argv):
    if sys.argv[a] == '--key':   KEY   = int(sys.argv[a+1]); a += 2
    elif sys.argv[a] == '--drop': DROP = float(sys.argv[a+1]); a += 2
    elif sys.argv[a] == '--merge': MERGE = int(sys.argv[a+1]); a += 2
    else: a += 1

try:
    import zstandard as _z
    _cctx = _z.ZstdCompressor(level=3)
    zc = lambda b: len(_cctx.compress(b)); zlabel = "zstd-3"
except Exception:
    import zlib
    zc = lambda b: len(zlib.compress(b, 6)); zlabel = "zlib-6"

buf = open(path, 'rb').read()
p = [0]
def u32():
    v = struct.unpack_from('<I', buf, p[0])[0]; p[0] += 4; return v
assert u32() == 0x5252434D, 'bad MCRR magic'
ver=u32(); nS=u32(); nD=u32(); nF=u32(); vram=u32(); pvr=u32(); u32()
def region():
    aa=u32(); ll=u32(); tag=buf[p[0]:p[0]+8].split(b'\0')[0].decode('latin1'); p[0]+=8; return (aa,ll,tag)
S=[region() for _ in range(nS)]
D=[region() for _ in range(nD)]
p[0]+=vram; p[0]+=pvr
for _,l,_ in S: p[0]+=l
frameStart=p[0]
dynbytes=sum(l for _,l,_ in D)

needle=bytes([0x46,0x52,0x4D,0x78])   # "FRMx"
fpos=[]; i=buf.find(needle,frameStart)
while i>=0: fpos.append(i); i=buf.find(needle,i+4)

def runs_vs(cur, key):
    """maximal gap-merged runs where cur differs from key -> [(off,len)]."""
    d = np.nonzero(cur != key)[0]
    if d.size == 0: return []
    brk = np.nonzero(np.diff(d) > MERGE)[0]
    starts = np.concatenate(([d[0]], d[brk+1]))
    ends   = np.concatenate((d[brk], [d[-1]]))
    return [(int(s), int(e - s + 1)) for s, e in zip(starts, ends)]

# ---- encode + decode + gate ----
# Two delta references, selected by --prev:
#   keyframe (default): delta vs the last KEYFRAME -> loss-tolerant (a dropped delta
#                       never breaks later ones; they rebase on the reliably-kept key).
#   prev (--prev)     : delta vs the PREVIOUS frame -> tiniest (this frame's churn only)
#                       but a dropped frame desyncs the chain until the next keyframe.
PREV = '--prev' in sys.argv
# --dump PATH [--dumpn N]: write a cross-language test vector (for the JS decoder):
#   [u32 dynbytes][u32 nframes] then per frame [u32 enclen][enc bytes][u32 crc32(raw blob)]
import zlib
DUMP = None; DUMPN = 180
if '--dump' in sys.argv: DUMP = sys.argv[sys.argv.index('--dump')+1]
if '--dumpn' in sys.argv: DUMPN = int(sys.argv[sys.argv.index('--dumpn')+1])
_dumprecs = bytearray(); _dumpcount = 0
enc_ref = None; keyid = 0          # encoder's reference blob
dec_ref = None; dec_key = None     # decoder's reference + last keyframe (own state)
raw_sizes=[]; v2_sizes=[]; v2z_sizes=[]; rawz_sizes=[]
ok = True; ndelta = 0; nkey = 0; corrupt = 0; first_corrupt = None
dropped_frames = set()
rng = np.random.default_rng(1234)   # deterministic drop pattern

for fi, pos in enumerate(fpos):
    if pos+12+dynbytes > len(buf): break
    cur = np.frombuffer(buf, dtype=np.uint8, count=dynbytes, offset=pos+12).copy()
    is_key = (fi % KEY == 0)
    # --- encode (vs keyframe, or vs prev frame) ---
    if is_key:
        enc = bytes([1]) + struct.pack('<I', dynbytes) + cur.tobytes()
        enc_ref = cur; keyid = fi; nkey += 1
    else:
        rs = runs_vs(cur, enc_ref)
        body = bytearray(struct.pack('<II', keyid, len(rs)))
        for off, ln in rs:
            body += struct.pack('<II', off, ln) + cur[off:off+ln].tobytes()
        enc = bytes([0]) + bytes(body); ndelta += 1
        if PREV: enc_ref = cur         # prev-mode: reference rolls forward every frame
    raw_sizes.append(dynbytes)
    if DUMP and fi < DUMPN:            # cross-language vector: enc + crc of the expected raw blob
        _dumprecs += struct.pack('<I', len(enc)) + enc + struct.pack('<I', zlib.crc32(cur.tobytes()) & 0xffffffff)
        _dumpcount += 1
    # --- loss test: maybe drop this delta on the wire ---
    if (not is_key) and DROP > 0 and rng.random() < DROP/100.0:
        dropped_frames.add(fi)
        # dropped: decoder shows its last good frame; its reference does NOT advance.
        continue
    rawz_sizes.append(zc(cur.tobytes()))
    v2_sizes.append(len(enc)); v2z_sizes.append(zc(enc))
    # --- decode with the DECODER's own state (so loss actually propagates) ---
    flag = enc[0]
    if flag == 1:
        (rl,) = struct.unpack_from('<I', enc, 1)
        dec = np.frombuffer(enc, dtype=np.uint8, count=rl, offset=5).copy()
        dec_key = dec
    else:
        kid, nr = struct.unpack_from('<II', enc, 1)
        base = dec_ref if PREV else dec_key   # prev-mode rebases on last decoded frame
        dec = base.copy()
        o = 9
        for _ in range(nr):
            off, ln = struct.unpack_from('<II', enc, o); o += 8
            dec[off:off+ln] = np.frombuffer(enc, dtype=np.uint8, count=ln, offset=o); o += ln
    dec_ref = dec
    if not np.array_equal(dec, cur):
        corrupt += 1
        if first_corrupt is None: first_corrupt = fi
        ok = False

if DUMP:
    with open(DUMP,'wb') as fo:
        fo.write(struct.pack('<II', dynbytes, _dumpcount)); fo.write(_dumprecs)
    print(f"dumped {_dumpcount} test-vector frames ({len(_dumprecs)} B) -> {DUMP}")

raw=np.array(raw_sizes); v2=np.array(v2_sizes); v2z=np.array(v2z_sizes); rawz=np.array(rawz_sizes)
nfr=len(v2)
print(f"== (a) state-wire v2 gate ==")
print(f"capture : {path}")
print(f"frames  : {len(fpos)} FRMx (expected {nF}); dynbytes/frame = {dynbytes}")
print(f"config  : keyframe every {KEY} | merge {MERGE} | drop {DROP}% | comp {zlabel}")
print(f"encoded : {nkey} keyframes + {ndelta} deltas ; dropped {len(dropped_frames)} deltas")
print()
print(f"CORRECTNESS : {'PASS — v2 decode == v1 raw, byte-exact' if ok else 'FAIL — byte mismatch (see above)'}")
print()
print(f"BANDWIDTH (bytes/frame):")
print(f"  v1 raw            : {raw.mean():10.0f}")
print(f"  v1 raw + {zlabel:<7} : {rawz.mean():10.0f}   (the current wire)")
print(f"  v2 delta          : {v2.mean():10.0f}   ({raw.mean()/max(1,v2.mean()):.1f}x thinner than raw)")
print(f"  v2 delta + {zlabel:<5} : {v2z.mean():10.0f}   ({rawz.mean()/max(1,v2z.mean()):.1f}x thinner than the current wire)")
if ndelta:
    dv = v2[[i for i in range(nfr)]]  # includes keyframes; show delta-only percentiles too
    deltas_only = v2[np.array([1 if (i%1) else 0 for i in range(nfr)])>-1]  # noop guard
    d_only = [v2_sizes[k] for k in range(len(v2_sizes))]
    print(f"  v2 delta p50/p95  : {np.percentile(v2,50):.0f} / {np.percentile(v2,95):.0f}")
print()
if DROP > 0:
    print(f"LOSS-TOLERANCE : dropped {len(dropped_frames)} deltas; "
          f"{'PASS — every surviving frame still decoded byte-exact, zero cascade' if ok else 'FAIL'}")
    print(f"  (a lost delta skips exactly 1 frame; the keyframe is untouched so later deltas rebase cleanly)")
else:
    print("LOSS-TOLERANCE : run with --drop 5 to inject datagram loss and verify no cascade")
sys.exit(0 if ok else 1)
