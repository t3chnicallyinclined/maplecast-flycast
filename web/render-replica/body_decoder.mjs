// body_decoder.mjs — Phase 5 client-side body-sprite decoder (PURE-STATE TEXTURES).
//
// THESIS: keep the live render-replica wire at GSTA-size (state only, ~7KB/frame). The
// CLIENT reconstructs each active body part's sprite TEXTURE from the shipped-once compressed
// GFX (PLxx_DAT GFX1/GFX2, already in the 16MB RAM image) + the per-frame sprite_id, and
// writes the decoded pixels into pane.vram at the TCW address render_frame carries — instead
// of streaming decoded VRAM pixels (the 320KB/frame "bodytex" shortcut, now removed).
//
// GROUND TRUTH (all CONFIRMED against marvelous2 + the _ryu_capture byte-exact gate):
//   * GFX1 LZSS decoder == bank03 loc_8c0354c0 (KB routine:loc_8c0354c0). decodeA() below is a
//     line-for-line port; its LINEAR output is BYTE-EXACT vs the engine decode buffer 0x0CE60000
//     and, written VERBATIM (no detwiddle) to VRAM, matches the engine's decoded VRAM byte-exact
//     (PVR PAL4_TW twiddled STORAGE order == the LZSS output; flycast texPAL4_TW reads it back).
//     Proof (tools-verified vs _ryu_capture/mc_{ram,vram}_dump.bin, frame-coherent):
//       Cable char23 sid 0xd4 sels [1264,1267,1265,1266] decoded from shipped GFX1 ==
//       VRAM @ [0x415400,0x415c00,0x416000,0x416200] -> 4/4 byte-exact; and 110 rectab-TCW-
//       referenced resident parts -> 110 byte-exact, 0 mismatch.
//   * The load-time decoder loc_8c033d78 (driver) calls loc_8c0354c0 per part and DMAs the
//     LINEAR decode output to VRAM as-is. So the client writes decodeA() output VERBATIM.
//   * GFX1 part header [base..+4] = [lw][lh][sw][sh] (logical vs STORAGE tiles). The decoded
//     texture is the FULL STORAGE span sw*8 x sh*8; PAL4 dest_len = (sw*8)*(sh*8)/2.
//   * GFX2 cell: cell = GFX2 + u32_le(GFX2 + (sid&0x7FFF)*4); u16 count, then count 8-byte
//     records [dx s16][dy s16][flags u16][sel u16 @+6]. sel @+6 = the GFX1 part selector.
//   * Per-part VRAM addr = the render param TCW: byteAddr = (TCW & 0x1FFFFF) << 3 (fmt5 PAL4).
//     render_frame's emitted TA carries that TCW per quad, BYTE-EXACT vs engine (test_ta_emit.c
//     "TCW-BITEXACT").
//   * TILING (corrected 2026-06-13, PROVEN vs resident VRAM): the body walker (loc_8c0344d4)
//     expands ONE GFX2 cell record into N TILES (N = desc[r13+1]+1 from the 0x8C1F9F9C tile-
//     descriptor table), each tile a separate emitted quad, ALL SHARING that cell's GFX1 sel
//     (r11+6 is read once per cell, not per tile). Each tile's TSP is a FIXED 32x32 PAL4_TW and
//     its TCW steps +0x200 (= 512B = one 32x32 PAL4 tile) ACROSS THE SAME CONTIGUOUS WHOLE-PART
//     VRAM BLOB. i.e. the engine stores the part ONCE, contiguously (sw*8 x sh*8, decodeA output
//     VERBATIM, byte-exact), at the part-base VRAM addr = the run's FIRST (minimum) tile TCW; the
//     subsequent tile TCWs point INTO that same blob at +512B offsets (the renderer reads each
//     32x32 tile from its own offset). Load-time decoder loc_8c033d78 confirms this: it decodes
//     each whole part to 0x0CE60000 contiguously and DMAs it VERBATIM, NO per-tile twiddle.
//     PROVED vs _scramble_actual.mcrr RESIDENT VRAM (the resident pose sid 0xd4, which IS in VRAM):
//       sel1264 64x64 2048B -> 4 tiles @ 0x415400/600/800/a00 (the part spans exactly 0x415400..
//       0x415c00); decodeA(1264) == VRAM[0x415400..+2048] 2048/2048 EXACT. sel1267/1265/1266 same.
//     THE SCRAMBLE BUG (the OLD code): it wrote the WHOLE-PART blob at EACH quad's OWN TCW, so a
//     2048B part was re-written starting at 0x415400 AND 0x415600 AND 0x415800 AND 0x415a00 ->
//     self-overwrite smear (each later write shifts the part +512B over the prior) = the scramble.
//     THE FIX: write each (gfx1,sel) part's decoded bytes ONCE, at the run's PART-BASE VRAM addr =
//     the MINIMUM tile TCW vaddr among that run's quads. The other tiles' TCWs already index into
//     that blob; do NOT write them again. (The single-char _ryu_capture passed before only because
//     those poses' parts were single-tile m=32: one 32x32 tile == the whole part, so write-each ==
//     write-once. Multi-tile limbs expose the self-overwrite.)
//
// COST: decode runs ONLY when a char slot's sprite_id changes (cache by slot|sprite_id). A pose
// is 4-19 parts, ~64..8192 decode bytes each; a full pose decode is sub-millisecond in JS. Static
// poses (the common case) cost ZERO (cache hit). No per-frame texture bytes on the wire.

const RAM = 0x00FFFFFF;            // area-3 / main-RAM low-24-bit mask (0x8C.. and 0x0C.. alias)

// ---- the validated GFX1 LZSS decoder (bank03 loc_8c0354c0) ------------------
// Flag byte consumed MSB-first from 0x80. bit CLEAR -> literal (*dst++ = *src++);
// bit SET -> back-ref b=*src++, dist=b>>4, count=(b&0x0F)+2 copied from dst-(dist+1).
// The output buffer IS the back-ref window (self-contained per part). VERBATIM == VRAM.
export function decodeA(src, sp, srcEnd, destLen) {
    const out = new Uint8Array(destLen);
    let o = 0, bc = 0, flags = 0;
    while (o < destLen && sp < srcEnd) {
        if (bc === 0) { flags = src[sp++]; bc = 0x80; if (sp >= srcEnd) break; }
        if ((flags & bc) === 0) {                 // literal
            out[o++] = src[sp++];
        } else {                                  // back-ref
            const b = src[sp++];
            let s = o - (b >> 4) - 1;
            const cnt = (b & 0x0F) + 2;
            for (let k = 0; k < cnt && o < destLen; k++, s++) out[o++] = (s >= 0 && s < o) ? out[s] : 0;
        }
        bc >>= 1;
    }
    return out;                                   // o < destLen leaves a zero tail (never seen)
}

// ---- little-endian RAM readers over the seeded 16MB image -------------------
const u8  = (ram, a) => ram[a & RAM];
const u16 = (ram, a) => ram[a & RAM] | (ram[(a + 1) & RAM] << 8);
const u32 = (ram, a) => (ram[a & RAM] | (ram[(a + 1) & RAM] << 8) | (ram[(a + 2) & RAM] << 16) | (ram[(a + 3) & RAM] << 24)) >>> 0;

// Char-struct layout (re_kb struct:char_struct / CLAUDE.md). Slot order == render slot order.
const SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
const OFF_ACTIVE = 0x000, OFF_SID = 0x144, OFF_GFX1 = 0x15C, OFF_GFX2 = 0x160;

// Build the sorted GFX1 offset table once per (gfx1 base) so we can bound each part's stream.
function gfx1Offsets(ram, gfx1, cache) {
    let e = cache.get(gfx1);
    if (e) return e;
    const n = u32(ram, gfx1) >>> 2;
    const offs = new Uint32Array(n);
    for (let i = 0; i < n; i++) offs[i] = u32(ram, gfx1 + i * 4);
    const srt = Uint32Array.from(new Set(offs)).sort((a, b) => a - b);
    e = { n, offs, srt };
    cache.set(gfx1, e);
    return e;
}
function endOf(srt, off) {                        // next-greater offset, bounds the LZSS stream
    let lo = 0, hi = srt.length;
    while (lo < hi) { const m = (lo + hi) >> 1; if (srt[m] <= off) lo = m + 1; else hi = m; }
    return lo < srt.length ? srt[lo] : off + 0x4000;
}

// Resolve a sprite_id -> ordered list of GFX1 part sels (GFX2 cell walk).
function cellSels(ram, gfx2, sid) {
    const cellOff = u32(ram, gfx2 + (sid & 0x7FFF) * 4);
    const cb = gfx2 + cellOff;
    const cnt = u16(ram, cb);
    if (cnt === 0 || cnt > 64) return null;
    const sels = new Array(cnt);
    let p = cb + 2;
    for (let i = 0; i < cnt; i++) { sels[i] = u16(ram, p + 6); p += 8; }
    return sels;
}

// Decode one GFX1 part to its VERBATIM (twiddled-storage) PAL4 bytes — exactly the bytes the
// engine puts in VRAM. Returns {bytes, destLen} or null if the part is degenerate.
function decodePart(ram, gfx1, G, sel) {
    if (sel >= G.n) return null;
    const pbase = gfx1 + G.offs[sel];
    const sw = u8(ram, pbase + 2), sh = u8(ram, pbase + 3);
    const W = sw * 8, H = sh * 8;
    if (W <= 0 || H <= 0 || W > 1024 || H > 1024) return null;
    const destLen = (W * H) >> 1;                 // PAL4: 2 px/byte
    const srcStart = (pbase + 4) & RAM;
    const srcEnd = (gfx1 + endOf(G.srt, G.offs[sel])) & RAM;
    return { bytes: decodeA(ram, srcStart, srcEnd, destLen), destLen, W, H };
}

// ----------------------------------------------------------------------------
// PUBLIC: ensureBodyTextures(ram, vram, ta, quadCount, cache, quadSels, quadGfx1s)
//   ram       : seeded 16MB RAM image (Uint8Array) — GFX1 part streams.
//   vram      : pane.vram (Uint8Array, 8MB) — where TextureManager re-decodes textures from.
//   ta        : the TA render_frame just emitted (Uint8Array) — authoritative per-quad TCW.
//   quadCount : render_frame_quad_count() — number of 96B textured-sprite quads in `ta`.
//   cache     : a persistent object {} the caller keeps across frames (decode memo).
//   quadSels  : Uint16Array[quadCount] from render_frame_quad_sels() — the SOURCE GFX1 sel the
//               walker actually used for quad k (TILING-SAFE: a tiled cell's N tiles share one sel).
//   quadGfx1s : Uint32Array[quadCount] from render_frame_quad_gfx1s() — the owning body's GFX1 base
//               for quad k (so each sel decodes against the RIGHT character's art).
//
// PER-TILE PLACEMENT (2026-06-13, corrected & PROVEN vs resident VRAM): the body walker expands
// one GFX2 cell into N tiles, all carrying the same (gfx1,sel) but TCWs that step +0x200 (=512B)
// across the SAME CONTIGUOUS WHOLE-PART blob. So the part is written ONCE at its base VRAM addr =
// the MINIMUM tile TCW among that (gfx1,sel) run's quads; the other tiles index into that blob.
// (The OLD code wrote the whole part at EACH quad's own TCW -> self-overwrite smear = the scramble.)
//
// We therefore (1) iterate the EMITTED QUADS to group them by (gfx1,sel) and find each run's base
// (min) TCW vaddr, then (2) decode each distinct part ONCE and write it ONCE at that base. A static
// pose re-uses last frame's decode via the memo. Returns {decoded,bytes,written,quads,parts}.
// ----------------------------------------------------------------------------
const QUAD = 96;                                  // textured-sprite TA block size (paraType 5)
const TCW_OFF = 0x0C;                             // TCW is word 3 of the 16B param header

// ---- WIDE-PART TILE STORAGE ORDER (re_kb finding:wide_part_tile_storage_order) -------
// CANONICAL (traced from loc_8c033d78 storage + loc_8c0344d4 walker, PROVEN on
// _sentinel_scramble.mcrr PL34 sel124 128x128 / PL17 sel285 64x128):
//   * Load-time decoder stores a WxH part as ONE CONTIGUOUS FULL-WxH PVR rect-twiddle blob.
//   * The per-frame walker emits N=(W/32)*(H/32) FIXED 32x32 tiles whose TCW vaddrs step
//     +0x200 contiguously across that blob: tile k at base+k*0x200, k = vaddr-chunk index.
//   * The renderer (_pal4) reads each tile as an INDEPENDENT 32x32 LOCAL twiddle at its TCW.
//   * For W>32 AND H>32 the full-WxH-twiddle chunk order (storage) and the walker per-tile
//     SCREEN cell order DIVERGE (a tile-grid permutation). So a contiguous full-blob write
//     puts the WRONG 32x32 chunk under each wide tile -> the near-empty Sentinel.
// THE FIX (2026-06-13, GENERAL — square, wide, tall, AND sub-32px tiles): the OLD chunk-permutation
//   model (carve the full-WxH-twiddle blob's 512B chunk at twTile(col,row,Tw,Th)) assumed every tile
//   is 32x32 and derived the grid from sw/sh (Tw=W>>5,Th=H>>5). That is WRONG for parts whose tile
//   size is 16 or 8: e.g. Sentinel sel121 is 128x16 painted as an 8x1 grid of 16x16 tiles (m=16),
//   and sel125 64x16 as 4x1 of m=16. sw/sh>>5 gave Th=0 -> the gate fell through to the contiguous
//   whole-blob write -> tiles 2..7 read past the 1024B decode -> scatter. The universal truth
//   (probed across every multi-tile sel): the walker lays a (cols x rows) grid of m x m tiles where
//   m = W/cols = H/rows (always 8/16/32, square). THE GENERAL CARVE: decode the part ONCE to its
//   full-WxH-twiddle IMAGE (indices), then for each emitted tile (col,row) build a fresh 32x32
//   PAL4 LOCAL-twiddle tile whose top-left m x m holds image[(row*m..)+(col*m..)] and write it to
//   that tile's OWN TCW vaddr. The renderer (_pal4) reads each tile as a 32x32 local twiddle and the
//   walker's u1 UV-clamp samples only the top-left m x m -> byte-exact for ALL shapes. VERIFIED 0px:
//   sel121 (8x1 m16), sel124/sel112 (4x4 m32), sel285 (2x4 m32), sel125 (4x1 m16), + every probed sel.
//   (Single-tile parts keep the proven direct whole-blob write at the run base.)
const TILE_BYTES = 0x200;                         // one 32x32 PAL4 tile = 512B (1024 px / 2)

// flycast PVR rectangular twiddle index for a w x h region (bx=log2 w, by=log2 h): interleave
// x/y bits up to min(w,h), then run the longer axis linearly. Used to (a) de-twiddle the stored
// full-WxH blob into a row-major image and (b) re-twiddle each carved m x m cell into a 32x32 tile.
function twop(x, y, bx, by) {
    let r = 0, b = 0; const sq = Math.min(bx, by);
    for (let i = 0; i < sq; i++) { r |= ((x >> i) & 1) << b; b++; r |= ((y >> i) & 1) << b; b++; }
    if (bx > by) r |= (x >> sq) << b; else if (by > bx) r |= (y >> sq) << b;
    return r;
}
// De-twiddle a full-WxH PAL4 storage blob into a row-major nibble image (Uint8Array, W*H, one
// 4-bit index per byte). bytes = decodePart output (the verbatim VRAM blob).
function detwiddleImage(bytes, W, H) {
    const img = new Uint8Array(W * H);
    const bx = Math.log2(W), by = Math.log2(H);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        const ti = twop(x, y, bx, by);
        const b = bytes[ti >> 1] | 0;
        img[y * W + x] = (ti & 1) ? (b >> 4) & 0xF : b & 0xF;
    }
    return img;
}
// Build one 32x32 PAL4 LOCAL-twiddle tile (512B) holding image cell (col,row) of pixel size m in
// its top-left m x m. Returns a Uint8Array(0x200). Matches exactly what the renderer's _pal4 reads.
function carveTile(img, W, H, col, row, m) {
    const tile = new Uint8Array(TILE_BYTES);
    for (let y = 0; y < m; y++) for (let x = 0; x < m; x++) {
        const sx = col * m + x, sy = row * m + y;
        if (sx >= W || sy >= H) continue;
        const v = img[sy * W + sx];
        const ti = twop(x, y, 5, 5);                 // 32x32 local twiddle
        if (ti & 1) tile[ti >> 1] = (tile[ti >> 1] & 0x0F) | (v << 4);
        else        tile[ti >> 1] = (tile[ti >> 1] & 0xF0) | v;
    }
    return tile;
}

export function ensureBodyTextures(ram, vram, ta, quadCount, cache, quadSels, quadGfx1s, quadColRow) {
    if (!cache._gfx)  cache._gfx = new Map();     // gfx1 base -> {n,offs,srt}
    if (!cache._dec)  cache._dec = new Map();     // "gfx1:sel" -> {bytes,destLen} (decode memo)
    if (cache._dec.size > 4096) cache._dec.clear();  // bound the memo across many distinct poses
    let decoded = 0, bytes = 0, written = 0, quads = 0;

    const dv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
    const tcwAddrOf = (q) => {
        const off = q * QUAD + TCW_OFF;
        if (off + 4 > ta.byteLength) return -1;
        const tcw = dv.getUint32(off, true);
        return ((tcw & 0x1FFFFF) << 3) >>> 0;
    };

    // ---- pass 1: group emitted quads into (gfx1,sel) part-runs. Record each run's BASE (min)
    // vaddr AND, per tile, its vaddr + intra-part (col,row) (for the wide-part carve). ----
    const runs = new Map();                       // "gfx1:sel" -> { gfx1, sel, base, tiles:[{addr,col,row}] }
    for (let q = 0; q < quadCount; q++) {
        const gfx1 = quadGfx1s[q] >>> 0;
        if (!(gfx1 & 0x0C000000) && !(gfx1 & 0x8C000000)) continue;  // no valid body art
        quads++;
        const sel  = quadSels[q];
        const addr = tcwAddrOf(q);
        if (addr < 0) continue;
        const key  = gfx1.toString(16) + ':' + sel;
        let r = runs.get(key);
        if (r === undefined) { r = { gfx1, sel, base: addr, tiles: [] }; runs.set(key, r); }
        const col = quadColRow ? (quadColRow[2 * q] | 0) : 0;
        const row = quadColRow ? (quadColRow[2 * q + 1] | 0) : 0;
        r.tiles.push({ addr, col, row });
        if (addr < r.base) r.base = addr;         // PART BASE = minimum tile TCW vaddr of the run
    }

    // ---- pass 2: decode each distinct part ONCE; write it at the correct VRAM bytes ----
    for (const r of runs.values()) {
        const { gfx1, sel, base, tiles } = r;
        const key = gfx1.toString(16) + ':' + sel;
        let p = cache._dec.get(key);
        if (p === undefined) {
            const G = gfx1Offsets(ram, gfx1, cache._gfx);
            p = decodePart(ram, gfx1, G, sel);    // {bytes,destLen,W,H} or null
            cache._dec.set(key, p);
            if (p) { decoded++; bytes += p.destLen; }
        }
        if (!p) continue;

        // GRID FROM THE EMITTED TILES (authoritative — NOT from sw/sh>>5): cols/rows = the
        // walker's actual per-tile screen cells; the tile pixel size m = W/cols = H/rows (always
        // 8/16/32, square). This is correct for every shape incl sub-32px tiles (sel121 8x1 m16).
        let cols = 1, rows = 1;
        for (const t of tiles) { if (t.col + 1 > cols) cols = t.col + 1; if (t.row + 1 > rows) rows = t.row + 1; }
        const multi = (tiles.length > 1) && quadColRow && cols >= 1 && rows >= 1 &&
                      (p.W % cols === 0) && (p.H % rows === 0) && (cols > 1 || rows > 1);
        if (multi) {
            // GENERAL CARVE: de-twiddle the part once, then carve each emitted tile's m x m cell
            // into a fresh 32x32 local-twiddle tile at its OWN TCW vaddr (re_kb finding:
            // wide_part_tile_storage_order, corrected 2026-06-13 to derive m from the emitted grid).
            const m = (p.W / cols) | 0;               // == p.H/rows (square tiles)
            const img = detwiddleImage(p.bytes, p.W, p.H);
            for (const { addr, col, row } of tiles) {
                if (col < 0 || col >= cols || row < 0 || row >= rows) continue;
                if (addr + TILE_BYTES > vram.length) continue;
                vram.set(carveTile(img, p.W, p.H, col, row, m), addr);
                written++;
            }
        } else {
            // SINGLE-TILE part (or no col/row available): write the VERBATIM whole-part blob ONCE
            // at the run base. (Proven byte-exact path; gfx1_decode_equals_vram.)
            if (base + p.destLen > vram.length) continue;
            vram.set(p.bytes, base);
            written++;
        }
    }
    return { decoded, bytes, written, quads, parts: runs.size };
}
