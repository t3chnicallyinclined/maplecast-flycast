//! MVC2 / Dreamcast (PowerVR2) texture + palette decode.
//!
//! Faithful port of the decode in web/webgpu/texture-manager.mjs — the twiddle
//! table build, the 16-bit / paletted / VQ unpackers, and the palette bake — with
//! the browser-only cache, dirty-page tracking, and WebGPU upload stripped out.
//!
//! Twiddle / detwiddle is byte-identical to flycast core/rend/texconv.cpp
//! twiddle_slow + the detwiddle table build + twop. Little-endian everything.

use std::sync::OnceLock;

#[derive(Clone, Copy)]
pub enum Wrap {
    Repeat,
    Clamp,
    Mirror,
}

pub struct Tex {
    pub rgba: Vec<u8>, // w*h*4, RGBA8 straight alpha
    pub w: u32,
    pub h: u32,
    pub filter_linear: bool,
    pub wrap_u: Wrap,
    pub wrap_v: Wrap,
}

// --- Twiddle / detwiddle table -------------------------------------------------
//
// detwiddle[plane][s][i], flattened to plane*11*1024 + s*1024 + i.
//   plane 0: tw(i, 0, 1024, 1<<s)   plane 1: tw(0, i, 1<<s, 1024)
// (texture-manager.mjs:18-28)

const DW_S: usize = 11;
const DW_N: usize = 1024;

struct Detwiddle {
    t: Vec<u32>, // 2 * DW_S * DW_N
}

impl Detwiddle {
    #[inline(always)]
    fn at(&self, plane: usize, s: usize, i: usize) -> u32 {
        self.t[(plane * DW_S + s) * DW_N + i]
    }
}

fn detwiddle() -> &'static Detwiddle {
    static DW: OnceLock<Detwiddle> = OnceLock::new();
    DW.get_or_init(|| {
        // tw() emits the Y bit then the X bit each loop iteration (texture-manager.mjs:20-24).
        fn tw(mut x: u32, mut y: u32, mut xs: u32, mut ys: u32) -> u32 {
            let mut r = 0u32;
            let mut s = 0u32;
            xs >>= 1;
            ys >>= 1;
            while xs != 0 || ys != 0 {
                if ys != 0 {
                    r |= (y & 1) << s;
                    ys >>= 1;
                    y >>= 1;
                    s += 1;
                }
                if xs != 0 {
                    r |= (x & 1) << s;
                    xs >>= 1;
                    x >>= 1;
                    s += 1;
                }
            }
            r
        }
        let mut t = vec![0u32; 2 * DW_S * DW_N];
        for s in 0..DW_S {
            let ys = 1u32 << s;
            for i in 0..DW_N {
                t[(0 * DW_S + s) * DW_N + i] = tw(i as u32, 0, 1024, ys);
                t[(1 * DW_S + s) * DW_N + i] = tw(0, i as u32, ys, 1024);
            }
        }
        Detwiddle { t }
    })
}

#[inline(always)]
fn twop(dw: &Detwiddle, x: usize, y: usize, bx: usize, by: usize) -> usize {
    (dw.at(0, by, x) + dw.at(1, bx, y)) as usize
}

/// Smallest r such that (1<<r) >= v. For power-of-two dims this is log2(v).
/// (texture-manager.mjs:29)
#[inline(always)]
fn bsr(v: u32) -> usize {
    let mut r = 0usize;
    while (1u32 << r) < v {
        r += 1;
    }
    r
}

// --- Color expansion (texture-manager.mjs:30-35) -------------------------------

#[inline(always)]
fn e5(v: u32) -> u8 {
    ((v << 3) | (v >> 2)) as u8
}
#[inline(always)]
fn e6(v: u32) -> u8 {
    ((v << 2) | (v >> 4)) as u8
}
#[inline(always)]
fn e4(v: u32) -> u8 {
    ((v << 4) | v) as u8
}

#[inline(always)]
fn u1555(c: u32) -> [u8; 4] {
    [
        e5((c >> 10) & 31),
        e5((c >> 5) & 31),
        e5(c & 31),
        if (c >> 15) != 0 { 255 } else { 0 },
    ]
}
#[inline(always)]
fn u565(c: u32) -> [u8; 4] {
    [e5((c >> 11) & 31), e6((c >> 5) & 63), e5(c & 31), 255]
}
#[inline(always)]
fn u4444(c: u32) -> [u8; 4] {
    [
        e4((c >> 8) & 15),
        e4((c >> 4) & 15),
        e4(c & 15),
        e4((c >> 12) & 15),
    ]
}

/// Unpack a 16-bit texel for a non-paletted format (0/1/2). None for others.
#[inline(always)]
fn unp16(fmt: u32, c: u32) -> Option<[u8; 4]> {
    match fmt {
        0 => Some(u1555(c)),
        1 => Some(u565(c)),
        2 => Some(u4444(c)),
        _ => None,
    }
}

// --- Mip offset tables (texture-manager.mjs:37-48) -----------------------------

const VQ_CODEBOOK_SIZE: usize = 2048;

const VQ_MIP_POINT: [usize; 11] = [
    VQ_CODEBOOK_SIZE + 0x00000,
    VQ_CODEBOOK_SIZE + 0x00001,
    VQ_CODEBOOK_SIZE + 0x00002,
    VQ_CODEBOOK_SIZE + 0x00006,
    VQ_CODEBOOK_SIZE + 0x00016,
    VQ_CODEBOOK_SIZE + 0x00056,
    VQ_CODEBOOK_SIZE + 0x00156,
    VQ_CODEBOOK_SIZE + 0x00556,
    VQ_CODEBOOK_SIZE + 0x01556,
    VQ_CODEBOOK_SIZE + 0x05556,
    VQ_CODEBOOK_SIZE + 0x15556,
];

const OTHER_MIP_POINT: [usize; 11] = [
    0x00003, 0x00004, 0x00008, 0x00018, 0x00058, 0x00158, 0x00558, 0x01558, 0x05558, 0x15558,
    0x55558,
];

// --- Palette bake (texture-manager.mjs updatePalette, :92-103) -----------------

/// Bake the 1024-entry palette (RGBA8, 4096 bytes) from the PVR register mirror.
/// Format from pvr_regs[0x108]&3 (0=1555, 1=565, 2=4444, 3=8888); entries at
/// pvr_regs[0x1000..0x2000] as u32 LE.
pub fn bake_palette(pvr_regs: &[u8]) -> Vec<u8> {
    let mut pal = vec![0u8; 4096];
    // Matches updatePalette's guard (regs.length < 0x1000+4096): leave zeros.
    if pvr_regs.len() < 0x1000 + 4096 {
        return pal;
    }
    let ctrl = u32::from_le_bytes([
        pvr_regs[0x108],
        pvr_regs[0x109],
        pvr_regs[0x10A],
        pvr_regs[0x10B],
    ]) & 3;
    for i in 0..1024 {
        let o = 0x1000 + i * 4;
        let raw = u32::from_le_bytes([pvr_regs[o], pvr_regs[o + 1], pvr_regs[o + 2], pvr_regs[o + 3]]);
        let c = if ctrl == 3 {
            // ARGB8888 direct.
            [
                ((raw >> 16) & 0xFF) as u8,
                ((raw >> 8) & 0xFF) as u8,
                (raw & 0xFF) as u8,
                ((raw >> 24) & 0xFF) as u8,
            ]
        } else {
            let v = raw & 0xFFFF;
            match ctrl {
                0 => u1555(v),
                1 => u565(v),
                _ => u4444(v), // ctrl == 2
            }
        };
        let d = i * 4;
        pal[d] = c[0];
        pal[d + 1] = c[1];
        pal[d + 2] = c[2];
        pal[d + 3] = c[3];
    }
    pal
}

// --- Texture decode ------------------------------------------------------------

/// Decode a texture referenced by (tcw, tsp) from the 8MB VRAM + baked palette.
/// Returns None for unsupported formats (YUV/bump/reserved) — caller uses a white
/// fallback.
pub fn decode(tcw: u32, tsp: u32, vram: &[u8], palette: &[u8]) -> Option<Tex> {
    // TCW / TSP field decode (texture-manager.mjs getTexture, :146-149).
    // Base addr wraps at 8MB like the PVR2 VRAM bus (& 0x7FFFFF); per-texel reads
    // are still bounds-checked below.
    let addr = (((tcw & 0x1FFFFF) << 3) & 0x7FFFFF) as usize;
    let fmt = (tcw >> 27) & 7;
    let tex_u = ((tsp >> 3) & 7) as usize;
    let tex_v = (tsp & 7) as usize;
    let w = (8u32 << tex_u) as usize;
    let h = (8u32 << tex_v) as usize;
    let pal_sel = ((tcw >> 21) & 0x3F) as usize;
    let scan = (tcw >> 26) & 1;
    let vq = (tcw >> 30) & 1;
    let mip = (tcw >> 31) & 1;

    let bx = bsr(w as u32);
    let by = bsr(h as u32);
    let dw = detwiddle();

    // Output buffer: zero-cleared so gap / OOB texels stay transparent.
    let mut rgba = vec![0u8; w * h * 4];

    let ok = if vq == 1 {
        decode_vq(&mut rgba, vram, addr, fmt, w, h, bx, by, mip == 1, tex_u, dw)
    } else {
        // Mip-map: skip past the smaller mip levels to the base level.
        let mut tex_addr = addr;
        if mip == 1 {
            let mip_idx = tex_u + 3;
            tex_addr = match fmt {
                5 => addr + (OTHER_MIP_POINT[mip_idx] >> 1),
                6 => addr + OTHER_MIP_POINT[mip_idx],
                _ => addr + OTHER_MIP_POINT[mip_idx] * 2,
            };
        }
        match fmt {
            5 => decode_pal4(&mut rgba, vram, tex_addr, w, h, bx, by, pal_sel, palette, dw),
            6 => decode_pal8(&mut rgba, vram, tex_addr, w, h, bx, by, pal_sel, palette, dw),
            0 | 1 | 2 => {
                decode_16(&mut rgba, vram, tex_addr, fmt, w, h, bx, by, scan == 1, dw)
            }
            _ => false, // 3=YUV, 4=bump, 7=reserved
        }
    };

    if !ok {
        return None;
    }

    // Sampler bits (texture-manager.mjs :183-194, engine/TSP-bit path).
    let fm = (tsp >> 13) & 3;
    let cu = (tsp >> 16) & 1;
    let cv = (tsp >> 15) & 1;
    let fu = (tsp >> 18) & 1;
    let fv = (tsp >> 17) & 1;
    let filter_linear = fm != 0;
    let wrap_of = |clamp: u32, mirror: u32| {
        if clamp == 1 {
            Wrap::Clamp
        } else if mirror == 1 {
            Wrap::Mirror
        } else {
            Wrap::Repeat
        }
    };

    Some(Tex {
        rgba,
        w: w as u32,
        h: h as u32,
        filter_linear,
        wrap_u: wrap_of(cu, fu),
        wrap_v: wrap_of(cv, fv),
    })
}

/// Content fingerprint of a texture's SOURCE bytes — its (tcw,tsp) header + the VRAM
/// region + (for paletted formats) the palette bank it samples. This is the cross-frame
/// texture-cache key: if every byte `decode` would read is unchanged the fingerprint is
/// unchanged and the already-decoded GPU texture is reused; if ANY source byte changes
/// (stage animation, texture streaming, or a skin/palette swap) the fingerprint changes
/// and the texture is re-decoded.
///
/// SAFETY: the hashed VRAM range [start,end) is a SUPERSET of what `decode` reads (start
/// = the base addr ≤ the mip-adjusted tex_addr; end covers the largest texel offset), and
/// the palette range is the exact bank. So a stale (false) cache HIT is impossible — the
/// only failure mode is an unrelated byte in the superset changing, which forces a
/// harmless re-decode (a false MISS). Field decode mirrors `decode` exactly (same fixed
/// PowerVR2 TCW/TSP bit layout + the shared mip tables).
pub fn source_fingerprint(tcw: u32, tsp: u32, vram: &[u8], palette: &[u8]) -> u64 {
    const P: u64 = 0x0000_0100_0000_01b3;
    let mut h = 0xcbf2_9ce4_8422_2325u64;
    for &b in &tcw.to_le_bytes() {
        h ^= b as u64;
        h = h.wrapping_mul(P);
    }
    for &b in &tsp.to_le_bytes() {
        h ^= b as u64;
        h = h.wrapping_mul(P);
    }

    let addr = (((tcw & 0x1FFFFF) << 3) & 0x7FFFFF) as usize;
    let fmt = (tcw >> 27) & 7;
    let tex_u = ((tsp >> 3) & 7) as usize;
    let tex_v = (tsp & 7) as usize;
    let w = 8usize << tex_u;
    let hpx = 8usize << tex_v;
    let pal_sel = ((tcw >> 21) & 0x3F) as usize;
    let vq = (tcw >> 30) & 1;
    let mip = (tcw >> 31) & 1;

    // VRAM source range [start,end) — a SUPERSET of decode's reads (start = base addr).
    let (start, end) = if vq == 1 {
        let idx_addr = if mip == 1 { addr + VQ_MIP_POINT[tex_u + 3] } else { addr + VQ_CODEBOOK_SIZE };
        (addr, idx_addr + (w >> 1) * (hpx >> 1)) // codebook (2048B) + index stream
    } else {
        let mut tex_addr = addr;
        if mip == 1 {
            let mip_idx = tex_u + 3;
            tex_addr = match fmt {
                5 => addr + (OTHER_MIP_POINT[mip_idx] >> 1),
                6 => addr + OTHER_MIP_POINT[mip_idx],
                _ => addr + OTHER_MIP_POINT[mip_idx] * 2,
            };
        }
        let len = match fmt {
            5 => (w * hpx + 1) / 2,   // 4bpp
            6 => w * hpx,             // 8bpp
            0 | 1 | 2 => w * hpx * 2, // 16bpp
            _ => 0,                   // 3/4/7 unsupported -> decode returns None anyway
        };
        (addr, tex_addr + len)
    };
    let end = end.min(vram.len());
    if start < end {
        for &b in &vram[start..end] {
            h ^= b as u64;
            h = h.wrapping_mul(P);
        }
    }

    // Palette bank (paletted only) — a skin/palette swap MUST invalidate the cache.
    let (pstart, plen) = match fmt {
        5 => ((pal_sel << 4) * 4, 16 * 4),         // pal4: 16 entries
        6 => (((pal_sel >> 4) << 8) * 4, 256 * 4), // pal8: 256 entries
        _ => (0, 0),
    };
    if plen > 0 {
        let pend = (pstart + plen).min(palette.len());
        if pstart < pend {
            for &b in &palette[pstart..pend] {
                h ^= b as u64;
                h = h.wrapping_mul(P);
            }
        }
    }
    h
}

/// 16-bit non-paletted (ARGB1555 / RGB565 / ARGB4444). (texture-manager.mjs :226-232)
fn decode_16(
    rgba: &mut [u8],
    vram: &[u8],
    tex_addr: usize,
    fmt: u32,
    w: usize,
    h: usize,
    bx: usize,
    by: usize,
    linear_scan: bool,
    dw: &Detwiddle,
) -> bool {
    for y in 0..h {
        for x in 0..w {
            let idx = if linear_scan { y * w + x } else { twop(dw, x, y, bx, by) };
            let so = tex_addr + idx * 2;
            if so + 1 >= vram.len() {
                continue;
            }
            let c = unp16(fmt, (vram[so] as u32) | ((vram[so + 1] as u32) << 8));
            let c = match c {
                Some(c) => c,
                None => return false,
            };
            let d = (y * w + x) * 4;
            rgba[d] = c[0];
            rgba[d + 1] = c[1];
            rgba[d + 2] = c[2];
            rgba[d + 3] = c[3];
        }
    }
    true
}

/// 4bpp paletted. (texture-manager.mjs _pal4, :277-284)
#[allow(clippy::too_many_arguments)]
fn decode_pal4(
    rgba: &mut [u8],
    vram: &[u8],
    addr: usize,
    w: usize,
    h: usize,
    bx: usize,
    by: usize,
    pal_sel: usize,
    palette: &[u8],
    dw: &Detwiddle,
) -> bool {
    if palette.len() < 4096 {
        return false; // mirrors "no _pal -> null"
    }
    let pb = pal_sel << 4;
    for y in 0..h {
        for x in 0..w {
            let ti = twop(dw, x, y, bx, by);
            let bo = addr + (ti >> 1);
            if bo >= vram.len() {
                continue;
            }
            let ni = if ti & 1 != 0 {
                (vram[bo] >> 4) & 0xF
            } else {
                vram[bo] & 0xF
            } as usize;
            let pi = (pb + ni) * 4;
            let d = (y * w + x) * 4;
            rgba[d] = palette[pi];
            rgba[d + 1] = palette[pi + 1];
            rgba[d + 2] = palette[pi + 2];
            rgba[d + 3] = palette[pi + 3];
        }
    }
    true
}

/// 8bpp paletted. (texture-manager.mjs _pal8, :286-293)
#[allow(clippy::too_many_arguments)]
fn decode_pal8(
    rgba: &mut [u8],
    vram: &[u8],
    addr: usize,
    w: usize,
    h: usize,
    bx: usize,
    by: usize,
    pal_sel: usize,
    palette: &[u8],
    dw: &Detwiddle,
) -> bool {
    if palette.len() < 4096 {
        return false;
    }
    let pb = (pal_sel >> 4) << 8;
    for y in 0..h {
        for x in 0..w {
            let ti = twop(dw, x, y, bx, by);
            let bo = addr + ti;
            if bo >= vram.len() {
                continue;
            }
            let pi = (pb + vram[bo] as usize) * 4;
            let d = (y * w + x) * 4;
            rgba[d] = palette[pi];
            rgba[d + 1] = palette[pi + 1];
            rgba[d + 2] = palette[pi + 2];
            rgba[d + 3] = palette[pi + 3];
        }
    }
    true
}

/// VQ (vector-quantized) over a 16-bit format. (texture-manager.mjs _decodeVQ, :235-268)
/// 256-entry 2x2-block codebook at `addr` (2048B) + an index stream after it.
#[allow(clippy::too_many_arguments)]
fn decode_vq(
    rgba: &mut [u8],
    vram: &[u8],
    addr: usize,
    fmt: u32,
    w: usize,
    h: usize,
    bx: usize,
    by: usize,
    mip: bool,
    tex_u: usize,
    dw: &Detwiddle,
) -> bool {
    // Only 16-bit formats have a VQ unpacker; pal/YUV/bump/reserved -> None.
    if !matches!(fmt, 0 | 1 | 2) {
        return false;
    }
    let cb_addr = addr;
    let idx_addr = if mip {
        addr + VQ_MIP_POINT[tex_u + 3]
    } else {
        addr + VQ_CODEBOOK_SIZE
    };

    // Decode the 256 codebook entries (4 texels each) once.
    let mut cb = [0u8; 256 * 16];
    for i in 0..256 {
        let co = cb_addr + i * 8;
        for p in 0..4 {
            let so = co + p * 2;
            if so + 1 >= vram.len() {
                continue;
            }
            let c = match unp16(fmt, (vram[so] as u32) | ((vram[so + 1] as u32) << 8)) {
                Some(c) => c,
                None => return false,
            };
            let d = i * 16 + p * 4;
            cb[d] = c[0];
            cb[d + 1] = c[1];
            cb[d + 2] = c[2];
            cb[d + 3] = c[3];
        }
    }

    let hw = w >> 1;
    let hh = h >> 1;
    let bcx = bx - 1;
    let bcy = by - 1;
    for y in 0..hh {
        for x in 0..hw {
            let ti = twop(dw, x, y, bcx, bcy);
            let io = idx_addr + ti;
            if io >= vram.len() {
                continue;
            }
            let ci = vram[io] as usize * 16;
            let px = x * 2;
            let py = y * 2;
            // p0=TL, p1=BL, p2=TR, p3=BR
            let d = (py * w + px) * 4;
            rgba[d] = cb[ci];
            rgba[d + 1] = cb[ci + 1];
            rgba[d + 2] = cb[ci + 2];
            rgba[d + 3] = cb[ci + 3];
            let d = ((py + 1) * w + px) * 4;
            rgba[d] = cb[ci + 4];
            rgba[d + 1] = cb[ci + 5];
            rgba[d + 2] = cb[ci + 6];
            rgba[d + 3] = cb[ci + 7];
            let d = (py * w + px + 1) * 4;
            rgba[d] = cb[ci + 8];
            rgba[d + 1] = cb[ci + 9];
            rgba[d + 2] = cb[ci + 10];
            rgba[d + 3] = cb[ci + 11];
            let d = ((py + 1) * w + px + 1) * 4;
            rgba[d] = cb[ci + 12];
            rgba[d + 1] = cb[ci + 13];
            rgba[d + 2] = cb[ci + 14];
            rgba[d + 3] = cb[ci + 15];
        }
    }
    true
}
