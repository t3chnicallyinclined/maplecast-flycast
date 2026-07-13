//! body_tex.rs — per-body-quad PAL4 texture decode, ported byte-exact from the C++
//! `gstaDecodeBodies` path in `core/network/maplecast_mirror.cpp`.
//!
//! For one emitted body `SceneQuad` this decodes its GFX1 part out of the resident
//! 16MB SH4 RAM (LZSS -> whole-part PAL4 detwiddle), carves the quad's own tile using
//! the emit-time source descriptor, resolves the 16-colour palette from the character's
//! resident `Dat_Pal` window (the Cable-all-blue fix), and expands to straight-alpha
//! RGBA8 with palette index 0 forced transparent.
//!
//! Ports (function names are durable; line ranges are the state at time of port):
//!   * LZSS decompressor            <- `gstaDecodeA`            (maplecast_mirror.cpp ~4884-4899)
//!   * PAL4 detwiddle               <- `gstaDetwiddlePal4`      (~4856-4868) + `gsta_twiddleSlow`
//!                                     (~4829-4836), `gsta_PAL4_ORDER` (~4838-4840), `gsta_log2i`
//!                                     (~4853). The precomputed `gsta_DETW` table (~4842-4851) is
//!                                     inlined as direct `twiddle_slow` calls.
//!   * GFX1 offset table + part hdr <- `gstaGfx1Offsets` (~4932-4942), `gstaEndOf` (~4943-4946),
//!                                     `gstaDecodeBodies` part-header read (~5056-5067)
//!   * tile geometry / carve        <- `gstaDecodeBodies` desc-keyed carve (~5149-5237)
//!   * palette (Dat_Pal / bank)     <- the private-bank scheme (~5704-5757)
//!
//! Simplification vs the C++ (behaviour-equivalent): the C++ produces a re-twiddled 512B
//! PAL4 VRAM tile via two branches — the `m==32` multi-tile NATIVE-CHUNK copy
//! (`gstaTwTileYFirst`, ~5199-5226) and the `gstaRetwiddle32` linear path (~5227-5246). Both
//! branches yield the SAME on-screen linear tile as carving directly from the whole-part
//! Y-first detwiddle buffer `lin`: the engine stores each part as one verbatim W×H twiddle
//! blob (re_kb/70), and `gstaDetwiddlePal4`'s separable Y-first interleave places tile
//! (col,row) at linear (col*m, row*m). We therefore carve linear indices straight from `lin`
//! (no re-twiddle round trip) and map through the palette. This is exactly the pixels the PVR2
//! path samples in the C++.

use crate::ffi::SceneQuad;

/// One decoded body tile: `w*h*4` straight-alpha RGBA8, palette index 0 transparent.
pub struct BodyTex {
    pub rgba: Vec<u8>,
    pub w: u32,
    pub h: u32,
}

/// PAL4 4×4-block sub-pixel order (`gsta_PAL4_ORDER`, maplecast_mirror.cpp ~4838-4840).
const PAL4_ORDER: [(usize, usize); 16] = [
    (0, 0), (0, 1), (1, 0), (1, 1), (0, 2), (0, 3), (1, 2), (1, 3),
    (2, 0), (2, 1), (3, 0), (3, 1), (2, 2), (2, 3), (3, 2), (3, 3),
];

/// The 6 character-struct bases (area-3 low 24 bits), `GFIX_CHAR_BASE` (~5705-5706).
const CHAR_BASE: [u32; 6] = [0x268340, 0x2688E4, 0x268E88, 0x26942C, 0x2699D0, 0x269F74];
/// Per-slot base PVR palette bank (skin formula), `GFIX_BASE_BANK` (~5742).
const BASE_BANK: [u32; 6] = [16, 24, 32, 40, 48, 56];

// ---- area-3 RAM readers (mask to low 24 bits, bounds-checked -> None on OOB) ----
// Mirror the `gramU8/gramU16/gramU32` LE helpers (~4901-4903).
#[inline]
fn r_u8(ram: &[u8], a: u32) -> Option<u8> {
    ram.get((a & 0x00FF_FFFF) as usize).copied()
}
#[inline]
fn r_u16(ram: &[u8], a: u32) -> Option<u16> {
    let a = (a & 0x00FF_FFFF) as usize;
    if a + 2 > ram.len() {
        return None;
    }
    Some(ram[a] as u16 | (ram[a + 1] as u16) << 8)
}
#[inline]
fn r_u32(ram: &[u8], a: u32) -> Option<u32> {
    let a = (a & 0x00FF_FFFF) as usize;
    if a + 4 > ram.len() {
        return None;
    }
    Some(ram[a] as u32
        | (ram[a + 1] as u32) << 8
        | (ram[a + 2] as u32) << 16
        | (ram[a + 3] as u32) << 24)
}

/// `gsta_twiddleSlow` (maplecast_mirror.cpp ~4829-4836) — Y-first (y-bit before x-bit) bit
/// interleave. `xs`/`ys` are span sizes (halved on entry, matching the C++).
#[inline]
fn twiddle_slow(mut x: u32, mut y: u32, mut xs: u32, mut ys: u32) -> u32 {
    let (mut rv, mut sh) = (0u32, 0u32);
    xs >>= 1;
    ys >>= 1;
    while xs != 0 || ys != 0 {
        if ys != 0 {
            rv |= (y & 1) << sh;
            ys >>= 1;
            y >>= 1;
            sh += 1;
        }
        if xs != 0 {
            rv |= (x & 1) << sh;
            xs >>= 1;
            x >>= 1;
            sh += 1;
        }
    }
    rv
}

/// `gsta_log2i` (maplecast_mirror.cpp ~4853): floor(log2), -1 for 0.
#[inline]
fn log2i(mut v: u32) -> u32 {
    // Bodies always have v>0 (W/H validated), so the -1 case never reaches the table index.
    let mut n: i32 = -1;
    while v != 0 {
        v >>= 1;
        n += 1;
    }
    n.max(0) as u32
}

/// `gstaDecodeA` (maplecast_mirror.cpp ~4884-4899): GFX1 LZSS (bank03 `loc_8c0354c0`).
/// Flag byte MSB-first from 0x80; clear=literal, set=back-ref (b=*src++, dist=b>>4,
/// count=(b&0x0F)+2 copied from out-(dist+1)). `sp`/`src_end` are area-3 byte indices.
fn lzss_decode(ram: &[u8], mut sp: usize, src_end: usize, dest_len: usize) -> Vec<u8> {
    let mut out = vec![0u8; dest_len];
    let end = src_end.min(ram.len());
    let mut o = 0usize;
    let mut bc: u32 = 0;
    let mut flags: u32 = 0;
    while o < dest_len && sp < end {
        if bc == 0 {
            flags = ram[sp] as u32;
            sp += 1;
            bc = 0x80;
            if sp >= end {
                break;
            }
        }
        if (flags & bc) == 0 {
            if sp >= end {
                break;
            }
            out[o] = ram[sp];
            o += 1;
            sp += 1;
        } else {
            if sp >= end {
                break;
            }
            let b = ram[sp] as usize;
            sp += 1;
            let mut s = o as isize - (b >> 4) as isize - 1;
            let cnt = (b & 0x0F) + 2;
            let mut k = 0;
            while k < cnt && o < dest_len {
                out[o] = if s >= 0 && (s as usize) < o {
                    out[s as usize]
                } else {
                    0
                };
                o += 1;
                s += 1;
                k += 1;
            }
        }
        bc >>= 1;
    }
    out
}

/// `gstaDetwiddlePal4` (maplecast_mirror.cpp ~4856-4868): twiddled W×H PAL4 bytes -> linear
/// W×H index buffer (1 byte/px, low nibble). The `gsta_DETW` lookup (~4842-4851) is inlined
/// as direct `twiddle_slow` calls: DETW[0][log2 H][x] and DETW[1][log2 W][y].
fn detwiddle_pal4(data: &[u8], w: usize, h: usize) -> Vec<u8> {
    let bcx = log2i(w as u32);
    let bcy = log2i(h as u32);
    let mut idx = vec![0u8; w * h];
    let mut y = 0;
    while y < h {
        let mut x = 0;
        while x < w {
            let d0 = twiddle_slow(x as u32, 0, 1024, 1u32 << bcy);
            let d1 = twiddle_slow(0, y as u32, 1u32 << bcx, 1024);
            let base = ((d0 + d1) / 16) as usize * 8;
            for i in 0..16 {
                let (cx, cy) = PAL4_ORDER[i];
                let bi = base + (i >> 1);
                let byte = if bi < data.len() { data[bi] } else { 0 };
                let nib = if i & 1 == 1 { (byte >> 4) & 0xF } else { byte & 0xF };
                idx[(y + cy) * w + (x + cx)] = nib;
            }
            x += 4;
        }
        y += 4;
    }
    idx
}

/// Resolve the 16-colour ARGB4444 palette for a body quad, ported from the private-bank
/// scheme in `gstaBuildBodies` (maplecast_mirror.cpp ~5704-5757).
///
/// A quad is a base-palette body iff its `gfx1` matches an active slot's node+0x15C GFX1
/// base AND its TCW pal-select equals that slot's base bank (16/24/.../56). Those get the
/// stable resident `Dat_Pal` at char+0x164 — the Cable-all-blue fix (the shared PVR bank
/// time-multiplexes to an effect palette mid-frame). Any quad the engine deliberately points
/// elsewhere (pal17 projectiles, pal25 hit-flash, effect banks) is NOT a Dat_Pal body: the
/// C++ preserves its engine pal-select and colours it from the per-frame PVR palette. Since
/// that palette is not in `ram`, such quads return None here (the caller colours them via the
/// baked PVR palette + the quad's TCW pal-select).
fn resolve_palette(ram: &[u8], gfx1: u32, palsel: u32) -> Option<[u16; 16]> {
    for s in 0..6 {
        let base = CHAR_BASE[s];
        if r_u8(ram, base)? == 0 {
            continue; // +0x000 active == 0 -> slot inactive
        }
        let datpal = r_u32(ram, base + 0x164)?; // node+0x164 Dat_Pal ptr
        if ((datpal >> 24) & 0x7F) != 0x0C {
            continue; // not an area-3 pointer -> no private bank for this slot
        }
        let gfx1_s = r_u32(ram, base + 0x15C)?; // node+0x15C GFX1 base
        if gfx1_s != gfx1 {
            continue;
        }
        // First slot whose GFX1 matches decides (matches the C++ `break` semantics).
        if palsel != BASE_BANK[s] {
            return None; // engine non-base palette: not a Dat_Pal body
        }
        let dp = datpal & 0x00FF_FFFF;
        let mut pal = [0u16; 16];
        for (i, e) in pal.iter_mut().enumerate() {
            *e = r_u16(ram, dp + (i as u32) * 2)?; // 16 ARGB4444 LE entries
        }
        return Some(pal);
    }
    None
}

/// Decode one body part's texture from RAM. `q` is the SceneQuad, `srcdesc` is its emit-time
/// `[m, cx, ry, flags]` snapshot, `is_effect` from `gsta_quad_is_effect`. Returns None for
/// effect quads, non-body GFX1, or any out-of-bounds / unresolvable input. Colours come from
/// the resident Dat_Pal window (see `resolve_palette`).
pub fn decode_body(
    q: &SceneQuad,
    srcdesc: [u8; 4],
    is_effect: bool,
    ram: &[u8],
    pvr_pal: &[u8],
    colrow: [i32; 2],
) -> Option<BodyTex> {
    // BIT15 effect quads are resident-backed; their sels are not GFX1 indices (~5004).
    if is_effect {
        return None;
    }
    let gfx1 = q.gfx1;
    // No body art unless gfx1 is an area-3 GFX1 pointer (~4996).
    if (gfx1 & 0x0C00_0000) == 0 && (gfx1 & 0x8C00_0000) == 0 {
        return None;
    }
    // Effect-Poly bank is an absolute-pointer texture directory, not a GFX1 LZSS table (~5045).
    if (0x0CED_0000..0x0CEE_0000).contains(&gfx1) {
        return None;
    }

    // ---- GFX1 offset table (`gstaGfx1Offsets` ~4932-4942) ----
    // n = u32[gfx1] >> 2 (= offs[0]/4 = part count); sanity-clamp garbage.
    let mut n = (r_u32(ram, gfx1)? >> 2) as usize;
    if n > 0x40000 {
        n = 0;
    }
    let sel = q.sel as usize;
    if sel >= n {
        return None; // sel out of the part table
    }
    let this_off = r_u32(ram, gfx1.wrapping_add((sel as u32) * 4))?;

    // ---- part end = smallest offset strictly greater than this_off, else this_off+0x4000
    //      (`gstaEndOf` ~4943-4946) ----
    let mut end_off: Option<u32> = None;
    for i in 0..n {
        let o = r_u32(ram, gfx1.wrapping_add((i as u32) * 4))?;
        if o > this_off {
            end_off = Some(end_off.map_or(o, |e| e.min(o)));
        }
    }
    let end_off = end_off.unwrap_or(this_off.wrapping_add(0x4000));

    // ---- part header + LZSS + detwiddle (`gstaDecodeBodies` ~5056-5067) ----
    let pbase = gfx1.wrapping_add(this_off); // unmasked; readers mask each access
    let sw = r_u8(ram, pbase.wrapping_add(2))? as usize; // width  / 8
    let sh = r_u8(ram, pbase.wrapping_add(3))? as usize; // height / 8
    let w = sw * 8;
    let h = sh * 8;
    if w == 0 || h == 0 || w > 1024 || h > 1024 {
        return None;
    }
    let dest_len = (w * h) >> 1;
    let src_start = (pbase.wrapping_add(4) & 0x00FF_FFFF) as usize;
    let src_end = (gfx1.wrapping_add(end_off) & 0x00FF_FFFF) as usize;

    let raw = lzss_decode(ram, src_start, src_end, dest_len);
    let lin = detwiddle_pal4(&raw, w, h);

    // ---- palette -> unified RGBA8 [u8;64] ----
    // Base-bank bodies keep the resident Dat_Pal (the Cable-all-blue fix, unchanged).
    // Non-base banks (pal17 projectile / pal25 hit-flash / super-tint on move frames)
    // are coloured from the live PVR palette bank instead of bailing to magenta.
    let palsel = (q.tcw >> 21) & 0x3F;
    let mut pal_rgba = [0u8; 64];
    match resolve_palette(ram, gfx1, palsel) {
        Some(dat) => {
            for i in 0..16 {
                let word = dat[i] as u32; // ARGB4444
                let r = ((word >> 8) & 0xF) as u8;
                let g = ((word >> 4) & 0xF) as u8;
                let b = (word & 0xF) as u8;
                let a = ((word >> 12) & 0xF) as u8;
                pal_rgba[i * 4] = (r << 4) | r;
                pal_rgba[i * 4 + 1] = (g << 4) | g;
                pal_rgba[i * 4 + 2] = (b << 4) | b;
                pal_rgba[i * 4 + 3] = (a << 4) | a;
            }
        }
        None => {
            let base = (palsel as usize) * 16 * 4; // 16 RGBA8 entries per bank
            if base + 64 <= pvr_pal.len() {
                pal_rgba.copy_from_slice(&pvr_pal[base..base + 64]);
            }
        }
    }

    // ---- tile geometry: engine tile size mq = u1 * (8<<TSP.texU); desc-keyed (col,row)
    //      (`gstaDecodeBodies` ~5155-5188) ----
    let usz = 8u32 << ((q.tsp >> 3) & 7);
    let mq = ((q.u1 * usz as f32 + 0.5) as i32).clamp(1, 32);
    let m = mq as usize;
    let p_cols = (w / m).max(1) as i32;
    let p_rows = (h / m).max(1) as i32;

    let dm = srcdesc[0] as i32;
    let dcx = srcdesc[1] as i32;
    let dry = srcdesc[2] as i32;
    let dfl = srcdesc[3] as i32;
    // Desc-keyed carve: cx = storage column (facing-independent), ry -> row = pRows - dry.
    // The C++ rank/runExt fallback needs the whole-frame walker ranks (unavailable per quad);
    // a coherent native frame always carries a valid desc with dm == mq for real body quads,
    // so the fallback below (single top-left tile) is only reached on torn/degenerate input.
    let (col, row) = if (dfl & 1) != 0 && dm == mq {
        let col = (dcx.rem_euclid(p_cols)) as usize;
        let rr = (p_rows - dry).clamp(0, p_rows - 1);
        (col, rr as usize)
    } else {
        // Desc snapshot torn: fall back to the LIVE walker rank (the same rank the desc
        // snapshots — render_frame_quad_colrow_impl). This carves each tile of a
        // multi-tile part from its own cell instead of the (0,0) corner (which made the
        // whole part render as one flat block on torn move frames).
        //   col = facing-INDEPENDENT storage column (ASC for both facings; the L/R flip
        //         is the texU mirror alone at UV time — render_frame.c:880-892). No flip.
        //   row = pRows - rowRank (matches the desc branch's `p_rows - dry`).
        let col = (colrow[0].rem_euclid(p_cols)) as usize;
        let rr = (p_rows - colrow[1]).clamp(0, p_rows - 1);
        (col, rr as usize)
    };

    // ---- carve the m×m tile from the linear whole-part buffer, map through the palette ----
    let mut rgba = vec![0u8; m * m * 4];
    let ox = col * m;
    let oy = row * m;
    for yy in 0..m {
        let py = oy + yy;
        if py >= h {
            break;
        }
        for xx in 0..m {
            let px = ox + xx;
            if px >= w {
                break;
            }
            let idx = (lin[py * w + px] as usize) & 0xF;
            if idx == 0 {
                continue; // palette index 0 = transparent (rgba already zero)
            }
            let d = (yy * m + xx) * 4;
            rgba[d..d + 4].copy_from_slice(&pal_rgba[idx * 4..idx * 4 + 4]);
        }
    }

    Some(BodyTex {
        rgba,
        w: m as u32,
        h: m as u32,
    })
}
