"""Python port of the native client's body_tex::decode_body (src/body_tex.rs) — decode one
MvC2 body-sprite tile straight from the 16 MB area-3 RAM image: GFX1 part table -> LZSS
(bank03) -> PAL4 detwiddle -> resident Dat_Pal (ARGB4444) -> straight-alpha RGBA8, palette
index 0 transparent. Lets the offline chain_drive geometry pipeline paint REAL sprites
(no VRAM needed — texels + palette both live in RAM). Line-for-line with the Rust.
"""

MASK = 0x00FFFFFF
CHAR_BASE = [0x268340, 0x2688E4, 0x268E88, 0x26942C, 0x2699D0, 0x269F74]
BASE_BANK = [16, 24, 32, 40, 48, 56]
PAL4_ORDER = [(0,0),(0,1),(1,0),(1,1),(0,2),(0,3),(1,2),(1,3),
              (2,0),(2,1),(3,0),(3,1),(2,2),(2,3),(3,2),(3,3)]

def r_u8(ram, a):
    a &= MASK
    return ram[a] if a < len(ram) else None
def r_u16(ram, a):
    a &= MASK
    return (ram[a] | ram[a+1]<<8) if a+2 <= len(ram) else None
def r_u32(ram, a):
    a &= MASK
    return (ram[a] | ram[a+1]<<8 | ram[a+2]<<16 | ram[a+3]<<24) if a+4 <= len(ram) else None

def twiddle_slow(x, y, xs, ys):
    rv = sh = 0
    xs >>= 1; ys >>= 1
    while xs or ys:
        if ys:
            rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh += 1
        if xs:
            rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh += 1
    return rv

def log2i(v):
    n = -1
    while v:
        v >>= 1; n += 1
    return max(0, n)

def lzss_decode(ram, sp, src_end, dest_len):
    out = bytearray(dest_len)
    end = min(src_end, len(ram))
    o = 0; bc = 0; flags = 0
    while o < dest_len and sp < end:
        if bc == 0:
            flags = ram[sp]; sp += 1; bc = 0x80
            if sp >= end: break
        if (flags & bc) == 0:
            if sp >= end: break
            out[o] = ram[sp]; o += 1; sp += 1
        else:
            if sp >= end: break
            b = ram[sp]; sp += 1
            s = o - (b >> 4) - 1
            cnt = (b & 0x0F) + 2
            k = 0
            while k < cnt and o < dest_len:
                out[o] = out[s] if 0 <= s < o else 0
                o += 1; s += 1; k += 1
        bc >>= 1
    return out

def detwiddle_pal4(data, w, h):
    bcx = log2i(w); bcy = log2i(h)
    idx = bytearray(w*h)
    y = 0
    while y < h:
        x = 0
        while x < w:
            d0 = twiddle_slow(x, 0, 1024, 1 << bcy)
            d1 = twiddle_slow(0, y, 1 << bcx, 1024)
            base = ((d0 + d1)//16) * 8
            for i in range(16):
                cx, cy = PAL4_ORDER[i]
                bi = base + (i >> 1)
                byte = data[bi] if bi < len(data) else 0
                nib = (byte >> 4) & 0xF if (i & 1) else byte & 0xF
                idx[(y+cy)*w + (x+cx)] = nib
            x += 4
        y += 4
    return idx

def resolve_palette(ram, gfx1, palsel):
    for s in range(6):
        base = CHAR_BASE[s]
        a = r_u8(ram, base)
        if a is None or a == 0:
            continue
        datpal = r_u32(ram, base + 0x164)
        if datpal is None or ((datpal >> 24) & 0x7F) != 0x0C:
            continue
        gfx1_s = r_u32(ram, base + 0x15C)
        if gfx1_s != gfx1:
            continue
        if palsel != BASE_BANK[s]:
            return None
        dp = datpal & MASK
        pal = []
        for i in range(16):
            w = r_u16(ram, dp + i*2)
            if w is None: return None
            pal.append(w)
        return pal
    return None

def decode_body(q, srcdesc, is_effect, ram, colrow):
    """q: dict(gfx1,sel,tcw,u1,tsp,mirror). srcdesc: [m,cx,ry,fl]. colrow: [col,row].
    Returns (rgba: bytes, m, m) or None."""
    if is_effect:
        return None
    gfx1 = q['gfx1']
    if (gfx1 & 0x0C000000) == 0 and (gfx1 & 0x8C000000) == 0:
        return None
    if 0x0CED0000 <= gfx1 < 0x0CEE0000:
        return None
    v = r_u32(ram, gfx1)
    if v is None: return None
    n = v >> 2
    if n > 0x40000: n = 0
    sel = q['sel']
    if sel >= n: return None
    this_off = r_u32(ram, (gfx1 + sel*4) & 0xFFFFFFFF)
    if this_off is None: return None
    end_off = None
    for i in range(n):
        o = r_u32(ram, (gfx1 + i*4) & 0xFFFFFFFF)
        if o is None: return None
        if o > this_off:
            end_off = o if end_off is None else min(end_off, o)
    if end_off is None:
        end_off = (this_off + 0x4000) & 0xFFFFFFFF
    pbase = (gfx1 + this_off) & 0xFFFFFFFF
    sw = r_u8(ram, (pbase + 2) & 0xFFFFFFFF); sh = r_u8(ram, (pbase + 3) & 0xFFFFFFFF)
    if sw is None or sh is None: return None
    w = sw*8; h = sh*8
    if w == 0 or h == 0 or w > 1024 or h > 1024: return None
    dest_len = (w*h) >> 1
    src_start = (pbase + 4) & MASK
    src_end = (gfx1 + end_off) & MASK
    raw = lzss_decode(ram, src_start, src_end, dest_len)
    lin = detwiddle_pal4(raw, w, h)
    # palette
    palsel = (q['tcw'] >> 21) & 0x3F
    pal_rgba = bytearray(64)
    dat = resolve_palette(ram, gfx1, palsel)
    if dat is not None:
        for i in range(16):
            word = dat[i]
            r = (word >> 8) & 0xF; g = (word >> 4) & 0xF; b = word & 0xF; a = (word >> 12) & 0xF
            pal_rgba[i*4]   = (r << 4) | r
            pal_rgba[i*4+1] = (g << 4) | g
            pal_rgba[i*4+2] = (b << 4) | b
            pal_rgba[i*4+3] = (a << 4) | a
    else:
        return None  # non-base palette needs the PVR bank we don't ship offline -> skip
    # tile geometry
    usz = 8 << ((q['tsp'] >> 3) & 7)
    mq = max(1, min(32, int(q['u1'] * usz + 0.5)))
    m = mq
    p_cols = max(1, w // m); p_rows = max(1, h // m)
    dm, dcx, dry, dfl = srcdesc
    if (dfl & 1) != 0 and dm == mq:
        col = dcx % p_cols
        rr = max(0, min(p_rows-1, p_rows - dry))
    else:
        col = colrow[0] % p_cols
        rr = max(0, min(p_rows-1, p_rows - colrow[1]))
    row = rr
    rgba = bytearray(m*m*4)
    ox = col*m; oy = row*m
    for yy in range(m):
        py = oy + yy
        if py >= h: break
        for xx in range(m):
            px = ox + xx
            if px >= w: break
            idx = lin[py*w + px] & 0xF
            if idx == 0: continue
            d = (yy*m + xx)*4
            rgba[d:d+4] = pal_rgba[idx*4:idx*4+4]
    return (bytes(rgba), m, m)
