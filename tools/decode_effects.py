# Decode the captured Effect Poly (DM00 shared-effects) textures efx_NNN.raw -> PNG,
# using dims + format from mc_effects.log.
#
# ============================ THE DECODE FIX ============================
# The original decoder treated these as PLAIN 16bpp TWIDDLED (W*H*2 bytes) and used a
# hand-rolled x-bit-first morton(). That produced NOISE. TWO things were wrong:
#
#   (1) WRONG STORAGE CLASS. These are **VQ-COMPRESSED**, not plain 16bpp. Proof: the
#       e8 texel addresses in mc_effects.log are spaced 0x1800 (6144 B) apart for the
#       128x128 entries and 0x4800 (18432 B) for 256x256 — NOT 0x8000 / 0x20000. The
#       dumper over-read W*H*2 from e8, so each efx_NNN.raw is mostly the NEXT entries'
#       bytes. The REAL per-texture size is exactly the PVR VQ size:
#           VQ = 2048 B codebook (256 codes x 8 B = 256 codes x 4 texels x u16)
#                + (W/2)*(H/2) index bytes (1 byte per 2x2 block).
#           128x128 -> 2048 + 4096  = 6144   == 0x1800  (MATCH)
#           256x256 -> 2048 + 16384 = 18432  == 0x4800  (MATCH)
#       (The prompt's "32768 == 128*128*2 so NOT VQ" was fooled by the over-read.)
#
#   (2) WRONG TWIDDLE ORDER for the index plane. The PVR (flycast texconv.cpp
#       twiddle_slow/twop) interleaves **y-bit FIRST, then x** per pair. The old morton
#       was x-first (a transpose). We now use the flycast-canonical twop().
#
# VQ DECODE (exact port of flycast core/rend/texconv.cpp `texture_VQ` +
# `ConvertTwiddle`, the same path mcfx uses for real VRAM VQ textures):
#   codebook = first 2048 B; code p -> 4 u16 texels {p0,p1,p2,p3}.
#   index plane starts at offset 2048; for output 2x2 block at (x,y) (x,y step 2):
#       p = idxplane[ twop(x,y, bcx=log2(W), bcy=log2(H)) / 4 ]
#   the code's 4 texels fill the block COLUMN-MAJOR (ConvertTwiddle::Convert, prel):
#       p0 -> (x+0,y+0)  p1 -> (x+0,y+1)  p2 -> (x+1,y+0)  p3 -> (x+1,y+1)
#
# FORMAT: e4 = 0x000003ZZ; byte0 (ZZ) is the PVR pixel format
#   0 = ARGB1555, 1 = RGB565, 2 = ARGB4444  (byte1 0x03 is a constant "present" flag).
#   (This is the inverse of the GFX *part* directory where byte1 is the format; for the
#   effect directory every real entry is 16bpp-VQ and byte0 is the format. Validated by
#   eyeballing: byte0=0 entries decode as cyan electric-arc sheets, byte0=1 as smooth
#   RGB energy fields, byte0=2 as flash bursts — coherent, not noise.)
import re, os
from PIL import Image

# ---- flycast-canonical PVR de-twiddle (texconv.cpp twiddle_slow + twop) ----------
def _twiddle_slow(x, y, x_sz, y_sz):
    rv = 0; sh = 0; x_sz >>= 1; y_sz >>= 1
    while x_sz != 0 or y_sz != 0:
        if y_sz != 0:
            rv |= (y & 1) << sh; y_sz >>= 1; y >>= 1; sh += 1
        if x_sz != 0:
            rv |= (x & 1) << sh; x_sz >>= 1; x >>= 1; sh += 1
    return rv

_DETW = [[[0] * 1024 for _ in range(11)] for _ in range(2)]
for _s in range(11):
    _ysz = 1 << _s
    for _i in range(1024):
        _DETW[0][_s][_i] = _twiddle_slow(_i, 0, 1024, _ysz)
        _DETW[1][_s][_i] = _twiddle_slow(0, _i, _ysz, 1024)

def twop(x, y, w, h):
    return _DETW[0][h.bit_length() - 1][x] + _DETW[1][w.bit_length() - 1][y]

def dec(v, f):
    if f == 0:  # ARGB1555
        return (((v>>10)&31)*255//31, ((v>>5)&31)*255//31, (v&31)*255//31, 255 if v&0x8000 else 0)
    if f == 1:  # RGB565
        return (((v>>11)&31)*255//31, ((v>>5)&63)*255//63, (v&31)*255//31, 255)
    return (((v>>8)&15)*17, ((v>>4)&15)*17, (v&15)*17, ((v>>12)&15)*17)  # ARGB4444

def vq_size(w, h):
    return 2048 + (w // 2) * (h // 2)

def decode_vq(data, w, h, fmt):
    """Exact flycast texture_VQ + ConvertTwiddle port. Returns an RGBA Image."""
    cb = data[:2048]
    idxp = data[2048:vq_size(w, h)]
    img = Image.new('RGBA', (w, h))
    px = img.load()
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            ti = twop(x, y, w, h) // 4
            if ti >= len(idxp):
                continue
            base = idxp[ti] * 8
            if base + 7 >= len(cb):
                continue
            t = [cb[base + i*2] | (cb[base + i*2 + 1] << 8) for i in range(4)]
            px[x,     y    ] = dec(t[0], fmt)
            px[x,     y + 1] = dec(t[1], fmt)
            px[x + 1, y    ] = dec(t[2], fmt)
            px[x + 1, y + 1] = dec(t[3], fmt)
    return img

D = "_effects_capture"

def parse_dir():
    """Return (idx, w, h, e4, e8) for the 25 real effect entries, deduped by idx
    (the dump fires repeatedly and APPENDS identical [EFX] blocks to the log)."""
    seen = {}
    for line in open(f"{D}/mc_effects.log"):
        m = re.match(r'\[EFX\]\s+(\d+)\s+(\d+)x(\d+)\s+(\w+)\s+(\w+)\s+(\w+)', line)
        if not m:
            continue
        idx, w, h, e0, e4, e8 = int(m[1]), int(m[2]), int(m[3]), int(m[4],16), int(m[5],16), int(m[6],16)
        if idx > 24 or w == 0 or h == 0 or w > 512 or h > 512:
            continue
        if w & (w-1) or h & (h-1):   # power-of-two square dims only
            continue
        seen[idx] = (idx, w, h, e4, e8)   # last wins (all blocks identical)
    return [seen[i] for i in sorted(seen)]

def main():
    decoded = []
    for idx, w, h, e4, e8 in parse_dir():
        fn = f"{D}/efx_{idx:03d}.raw"
        if not os.path.exists(fn):
            continue
        data = open(fn, 'rb').read()
        fmt = e4 & 0xff
        img = decode_vq(data, w, h, fmt)
        out = f"{D}/efx_{idx:03d}.png"
        img.save(out)
        decoded.append((idx, w, h, fmt, e8))
        print(f"idx {idx:2d}: {w}x{h} fmt{fmt} VQ({vq_size(w,h)}B) -> efx_{idx:03d}.png")
    return decoded

if __name__ == "__main__":
    main()
