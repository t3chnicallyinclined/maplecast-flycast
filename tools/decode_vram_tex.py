#!/usr/bin/env python3
"""decode_vram_tex.py — decode a PVR texture straight from the captured VRAM dump
at the engine TA's real TexAddr (grounding the stage texture binding in the engine
TA, NOT the rip's disc texIndex). Twiddled, RGB565/ARGB4444/ARGB1555.

VRAM 64-bit area: byte_addr = TexAddr*8. _stage_gt/vram.bin is the 8MB linear VRAM."""
import os, sys, struct
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
VRAM = open(os.path.join(REPO, "_stage_gt", "vram.bin"), "rb").read()


def _twiddle_slow(x, y, x_sz, y_sz):
    """flycast texconv.cpp twiddle_slow port (core/hw/pvr/texconv.cpp). Interleaves the
    (x,y) bits y-first into the twiddled linear texel index, supporting NON-SQUARE
    textures (x_sz != y_sz). For square textures this equals the Morton interleave."""
    rv = 0; sh = 0; x_sz >>= 1; y_sz >>= 1
    while x_sz != 0 or y_sz != 0:
        if y_sz != 0:
            rv |= (y & 1) << sh; y_sz >>= 1; y >>= 1; sh += 1
        if x_sz != 0:
            rv |= (x & 1) << sh; x_sz >>= 1; x >>= 1; sh += 1
    return rv


def untwiddle_xy(x, y, w=None, h=None):
    # flycast-exact twiddled texel index for pixel (x,y) in a w×h texture. The PVR
    # twiddle interleaves x-first relative to the (h,w) dims (validated bit-exact vs
    # the recognizable STG0B t03 "TIME" texture decode). For square textures this is the
    # Morton Z-order. Square is the common case (all STG0B textures are square); w/h are
    # passed for rectangular textures.
    if w is None or h is None:
        w = h = 1
        # square fallback: derive from max coordinate is unsafe; callers pass w,h.
    return _twiddle_slow(y, x, h, w)


def rgb565(c):
    r = (c >> 11) & 0x1F; g = (c >> 5) & 0x3F; b = c & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255)


def argb4444(c):
    a = ((c >> 12) & 0xF) * 0x11; r = ((c >> 8) & 0xF) * 0x11
    g = ((c >> 4) & 0xF) * 0x11; b = (c & 0xF) * 0x11
    return (r, g, b, a)


def argb1555(c):
    a = ((c >> 15) & 1) * 255; r = ((c >> 10) & 0x1F) * 8
    g = ((c >> 5) & 0x1F) * 8; b = (c & 0x1F) * 8
    return (r, g, b, a)


CONV = {0: argb1555, 1: rgb565, 2: argb4444}


def decode(tex_addr, w, h, pixfmt, twiddled=True, out=None):
    base = tex_addr * 8
    conv = CONV[pixfmt]
    img = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            if twiddled:
                idx = untwiddle_xy(x, y, w, h)
            else:
                idx = y * w + x
            o = base + idx * 2
            c = struct.unpack_from("<H", VRAM, o)[0]
            r, g, b, a = conv(c)
            ci = (y * w + x) * 4
            img[ci] = r; img[ci + 1] = g; img[ci + 2] = b; img[ci + 3] = a
    im = Image.frombytes("RGBA", (w, h), bytes(img))
    if out:
        im.save(out)
    return im


if __name__ == "__main__":
    # TexAddr w h pixfmt out
    ta = int(sys.argv[1]); w = int(sys.argv[2]); h = int(sys.argv[3])
    pf = int(sys.argv[4]); out = sys.argv[5]
    decode(ta, w, h, pf, True, out)
    print("wrote", out)
