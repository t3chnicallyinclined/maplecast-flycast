// body_decoder.mjs — Phase 5 client-side body-sprite decoder (PURE-STATE TEXTURES).
//
// THESIS: keep the live render-replica wire at GSTA-size (state only, ~7KB/frame). The
// CLIENT reconstructs each active body part's sprite TEXTURE from the shipped-once compressed
// GFX (PLxx_DAT GFX1/GFX2, already in the 16MB RAM image) + the per-frame sprite_id, and
// writes the decoded pixels into pane.vram at the TCW address render_frame carries — instead
// of streaming decoded VRAM pixels (the 320KB/frame "bodytex" shortcut, now removed).
//
// ============================================================================
// FAITHFUL TRANSPILE (2026-06-14) — the texture decode is now a 1:1 port of the ENGINE's
// load-time decode path, NOT a hand model. CITE: loc_8c033d78 (part-decode driver) +
// loc_8c0354c0 (LZSS) read instruction-by-instruction from marvelous2 bank03.
//
//   * loc_8c0354c0 LZSS: decodeA() below is the faithful transpile (register contract r4=src=
//     part_base+4, r5=dest_len, r6=dest_base, r9=r5+r6=end; flag MSB-first from 0x80; bit CLEAR=
//     literal *dest++=*src++; bit SET=back-ref b=*src++, dist=b>>4, count=(b&0x0F)+2 from
//     dest-(dist+1); output buffer IS the back-ref window, self-contained per part). PROVEN
//     BYTE-EXACT vs the ENGINE's own decode output (_ryu_capture/PL00_raw_{0001,0013,0024,0034,
//     0061,0127}.bin, captured live) for square, WIDE (sel24 128x32), and TALL (sel1/13/34/127
//     32..64 x128) parts: 6/6 diff=0. Same decoder validated across all 1533 Ryu sels in
//     tools/extract_gfx1_atlas.py (1.93M px, 0 fail).
//
//   * loc_8c033d78 storage: the driver LZSS-decodes each part CONTIGUOUSLY into scratch
//     0x0CE60000 (advancing dest by destLen=(sw*8)*(sh*8)/2 per part, NO twiddle, NO tile
//     reorder), then DMAs the scratch to VRAM VERBATIM (loc_8c1240a0/loc_8c123e00 assign each
//     part ONE base texaddr and copy the bytes as-is). So the part lives in VRAM as ONE
//     CONTIGUOUS full-W×H PVR-twiddle blob = exactly decodeA()'s output.
//
//   * THE FAITHFUL CLIENT OP — write decodeA() output VERBATIM at the part-base VRAM addr
//     (= min TCW of the part's emitted tiles). NO carve, NO detwiddle, NO re-twiddle. The
//     per-frame walker (loc_8c0344d4, render_frame 0.00px) emits N tiles whose TCW vaddrs step
//     +0x200 (=one 32×32 PAL4 chunk) CONTIGUOUSLY into that one blob; the client renderer
//     (texture-manager _pal4) reads each tile's declared w×h twiddle (w=8<<texU,h=8<<texV from
//     the TSP; twop over w×h) from its own TCW — byte-for-byte flycast texture_TW (texconv.cpp).
//     PROVEN (offline, disc): every 512B chunk of the contiguous blob, read as a standalone
//     32×32 twiddle, EXACTLY equals the screen 32×32 cell the full-W×H twiddle places it at —
//     sel13 64×128 8/8, sel24 128×32 4/4, sel1 32×128 4/4 chunks. The chunk→cell PERMUTATION is
//     the ENGINE's (the walker's per-tile TCW pairing), already reproduced 0.00px by render_frame;
//     the client does NOT model it — verbatim-write makes every tile read the right chunk for free.
//
//   * SUPERSEDES the hand-MODELED carve (detwiddle whole part with min(W,H) square blocks →
//     re-twiddle each tile into a fresh 32×32 at a SCREEN-RANK-derived (col,row)). That model was
//     validated vs a RECONSTRUCTED twiddle MODEL, never the engine, and mis-ordered tiles under
//     opposite facing / repeated tiles (the "carve-off is closer for Cable-on-P2" symptom). A
//     faithful transpile is correct BY CONSTRUCTION, the same way render_frame's geometry is.
//     (re_kb finding:faithful_texture_decode_transpile supersedes wide_part_tile_storage_order*.)
// ============================================================================
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

// ============================================================================
// PER-TILE RE-TILE (THE 2026-06-14 FIX — supersedes the verbatim-blob write).
//
// WHY VERBATIM WAS WRONG (proven vs the engine, not a model): the engine stores each part
// as ONE W×H PVR-TWIDDLED blob (decodeA output, byte-exact), BUT the per-frame walker
// (loc_8c0344d4) emits N tiles for that part, EACH with its OWN rectab-resident TCW, and the
// pvr2 renderer reads each tile as a STANDALONE 32×32 PAL4_TW texture (flycast texPAL4_TW).
// A 32×32 twiddle read at part_base+k*0x200 of a W×H-twiddled blob only coincides with the
// k-th screen cell when W==H==32 (single tile) — for WIDE (128×16 sel121/113), TALL, or any
// non-32-square part the bytes at +k*0x200 are the MIDDLE of a twiddled row, so the tile reads
// garbage (the live-Sentinel scatter the user saw). The "every 512B chunk reconstructs its cell"
// claim held only for the square _fidcap poses that validated it.
//
// THE FIX (occupancy-validated 100% vs the PROVEN emitter atlas web/test-atlas/chars/PLxx_parts,
// tools/render-replica-poc/_validate_retile.mjs — sels 121/112/113/118 all 100%): the engine's
// per-tile VRAM IS a sequence of standalone 32×32 PAL4_TW chunks. So the faithful client op is:
//   1. decodeA(part)           -> raw bytes (W×H PVR twiddle) — byte-exact vs engine, unchanged.
//   2. detwiddlePal4(raw,W,H)  -> linear W×H index buffer (flycast texconv port, atlas-validated).
//   3. per emitted tile (col,row from render_frame_quad_colrow = STORAGE col Ax-ASC / row Ay-DESC):
//      extract the 32×32 linear region at (col*32,row*32) (clamped to W×H, zero-pad), re-twiddle
//      it as a standalone 32×32 PAL4_TW, and write those 512B at THIS tile's own TCW vaddr.
// The texU mirror (draw-time) does the facing L/R flip; col is facing-independent so the part is
// re-tiled once in storage order. (re_kb finding:faithful_texture_decode_transpile is RETRACTED;
// new finding:per_tile_retile_decode.)
// ============================================================================

// flycast PAL4 twiddle (port of tools/extract_gfx1_atlas.py / texconv.cpp, CHARQ+atlas validated)
function _twiddleSlow(x, y, xs, ys) {
    let rv = 0, sh = 0; xs >>= 1; ys >>= 1;
    while (xs || ys) {
        if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; }
        if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; }
    }
    return rv;
}
const _DETW = [[], []];
for (let s = 0; s < 11; s++) {
    const ys = 1 << s; _DETW[0][s] = new Int32Array(1024); _DETW[1][s] = new Int32Array(1024);
    for (let i = 0; i < 1024; i++) { _DETW[0][s][i] = _twiddleSlow(i, 0, 1024, ys); _DETW[1][s][i] = _twiddleSlow(0, i, ys, 1024); }
}
const _PAL4_ORDER = [[0,0],[0,1],[1,0],[1,1],[0,2],[0,3],[1,2],[1,3],[2,0],[2,1],[3,0],[3,1],[2,2],[2,3],[3,2],[3,3]];
function _log2i(v) { let n = -1; while (v) { v >>= 1; n++; } return n; }

// TWIDDLED W×H PAL4 bytes -> linear W×H index buffer (1 byte/px, low nibble = palette index).
function detwiddlePal4(data, w, h) {
    const bcx = _log2i(w), bcy = _log2i(h);
    const idx = new Uint8Array(w * h);
    for (let y = 0; y < h; y += 4) for (let x = 0; x < w; x += 4) {
        const blk = ((_DETW[0][bcy][x] + _DETW[1][bcx][y]) / 16) | 0;
        const base = blk * 8;
        for (let i = 0; i < 16; i++) {
            const cx = _PAL4_ORDER[i][0], cy = _PAL4_ORDER[i][1];
            const b = (base + (i >> 1) < data.length) ? data[base + (i >> 1)] : 0;
            idx[(y + cy) * w + (x + cx)] = (i & 1) ? ((b >> 4) & 0xF) : (b & 0xF);
        }
    }
    return idx;
}
// PVR rectangular-twiddle of TILE coordinates (col,row) within a Tw×Th tile grid -> the storage
// chunk index k. For a part stored as ONE full-W×H PVR rect-twiddle blob, the k-th contiguous
// 512B (=one 32×32 PAL4 tile) chunk is the grid cell at twTile(col,row,Tw,Th). CONFIRMED-BY-
// MEASUREMENT (tools/render-replica-poc/_measure_chunk.mjs): for sel124 128×128 4×4 the chunk at
// twTile(col,row,4,4) read as a standalone 32×32 twiddle == the reference detwiddled cell 0/16384;
// sel285 64×128 2×4 == 0/8192. (re_kb finding:wide_part_tile_storage_order twTile rule.)
function twTile(x, y, bx, by) {
    let r = 0, b = 0; const sq = Math.min(bx, by);
    for (let i = 0; i < sq; i++) { r |= ((x >> i) & 1) << b; b++; r |= ((y >> i) & 1) << b; b++; }
    if (bx > by) r |= (x >> sq) << b; else if (by > bx) r |= (y >> sq) << b;
    return r;
}
// Y-FIRST rectangular twiddle of TILE coords (col,row) in a Tw×Th tile grid -> chunk index.
// This is flycast's REAL _twiddleSlow interleave (y-bit before x-bit), the correct storage
// order for a NON-SQUARE multi-32×32-tile part (Tw != Th). twTile() above (x-first) is the
// SQUARE case. Args are TILE counts (Tw,Th = W/32,H/32), NOT log2. CONFIRMED-BY-MEASUREMENT
// 2026-06-15: y-first reproduces the engine VRAM chunk byte-exact for sel267/285/273 64×128
// 2×4, while x-first / linear scatter it (the Storm-cape / Sentinel garble). See
// docs/GSTA-FINDINGS-FOR-BROWSER.md and tools/render-replica-poc/_test_blockmap.mjs.
function twTileYFirst(col, row, Tw, Th) {
    let rv = 0, sh = 0, xs = Tw >> 1, ys = Th >> 1, x = col, y = row;
    while (xs || ys) {
        if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; }
        if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; }
    }
    return rv;
}
// COLUMN-PAIR-MAJOR storage chunk index for a Tw×Th tile grid (re_kb/68). A LINE-FOR-LINE
// port of render_frame.c rebuild_tile_grid's engine tile-emit loop: a 2-column macro-column
// walks ALL 2-row bands, THEN the next pair -> [col-pair][2-row band][col-in-pair][row]. This
// IS the engine's per-tile TCW-slot order (walker steps +0x200 per emit slot) AND therefore the
// verbatim-DMA VRAM chunk order at the part base. The client's native-chunk carve must index the
// blob by THIS (== render_frame's byte-exact TCW addr slot) so blob[k*512] lands at the SAME
// relative offset the engine DMAed it to. IDENTICAL to the old row-band-major carve for Tw<=2 OR
// Th<=2 (proven: every satellite/body run bit-unchanged); for 4x4+ parts (Sentinel sel124/112)
// the old carve 2x2-block-SWAPPED the off-diagonal tiles = re_kb/68's "swapped diagonally"
// residual on the TEXEL side. render_frame widecarve1 fixed the geometry twin; this fixes the carve.
function colPairChunk(col, row, Tw, Th) {
    let t = 0;
    for (let cp = 0; cp < Tw; cp += 2) {
        const cw = (Tw - cp < 2) ? (Tw - cp) : 2;
        for (let by = 0; by < Th; by += 2) {
            const bh = (Th - by < 2) ? (Th - by) : 2;
            for (let cx2 = 0; cx2 < cw; cx2++) {
                for (let ry = 0; ry < bh; ry++) {
                    if (cp + cx2 === col && by + ry === row) return t;
                    t++;
                }
            }
        }
    }
    return -1;
}

// Re-twiddle one 32×32 linear index region (1024 entries) into 512 bytes of PAL4_TW (the engine's
// per-tile VRAM storage). Inverse of detwiddlePal4 for w=h=32 (bcx=bcy=5).
function retwiddle32(lin) {
    const out = new Uint8Array(512);
    for (let y = 0; y < 32; y += 4) for (let x = 0; x < 32; x += 4) {
        const blk = ((_DETW[0][5][x] + _DETW[1][5][y]) / 16) | 0;
        const base = blk * 8;
        for (let i = 0; i < 16; i++) {
            const cx = _PAL4_ORDER[i][0], cy = _PAL4_ORDER[i][1];
            const nib = lin[(y + cy) * 32 + (x + cx)] & 0xF;
            if (i & 1) out[base + (i >> 1)] |= nib << 4; else out[base + (i >> 1)] |= nib;
        }
    }
    return out;
}

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
const OFF_ACTIVE = 0x000, OFF_SID = 0x144, OFF_FACING = 0x110, OFF_GFX1 = 0x15C, OFF_GFX2 = 0x160;

// facingForGfx1 — the owning body's facing (node+0x110 != 0 <=> faces RIGHT, the MIRRORED side).
// The carve reverses the STORAGE column for the mirrored facing (see ensureBodyTextures). We read
// it straight from the resident char struct by matching the body's GFX1 base (node+0x15C).
function facingForGfx1(ram, gfx1) {
    const g = gfx1 & 0x00FFFFFF;
    for (const s of SLOTS) {
        if (!u8(ram, s + OFF_ACTIVE)) continue;
        if ((u32(ram, s + OFF_GFX1) & 0x00FFFFFF) === g) return u8(ram, s + OFF_FACING) ? 1 : 0;
    }
    return 0;
}

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

// Decode one GFX1 part and detwiddle it to a LINEAR W×H index buffer (1 byte/px). The raw
// decodeA output is W×H PVR-twiddled (byte-exact vs the engine); detwiddlePal4 makes it linear
// so per-tile 32×32 chunks can be sliced + re-twiddled into the walker's tile TCWs. Returns
// {lin, W, H, cols, rows} (cols/rows = part dims in 32-px tiles) or null if degenerate.
function decodePart(ram, gfx1, G, sel) {
    if (sel >= G.n) return null;
    const pbase = gfx1 + G.offs[sel];
    const sw = u8(ram, pbase + 2), sh = u8(ram, pbase + 3);
    const W = sw * 8, H = sh * 8;
    if (W <= 0 || H <= 0 || W > 1024 || H > 1024) return null;
    const destLen = (W * H) >> 1;                 // PAL4: 2 px/byte
    const srcStart = (pbase + 4) & RAM;
    const srcEnd = (gfx1 + endOf(G.srt, G.offs[sel])) & RAM;
    const raw = decodeA(ram, srcStart, srcEnd, destLen);   // W×H PVR twiddle (engine byte-exact)
    const lin = detwiddlePal4(raw, W, H);                   // -> linear W×H index buffer
    // `raw` is kept for the W>32 && H>32 SQUARE-part native-chunk carve (see ensureBodyTextures):
    // for those the engine's VRAM 512B chunk is the full-W×H-twiddle chunk at twTile(col,row), which
    // a linear-slice→retwiddle32 roundtrip does NOT reproduce. Copy the native chunk verbatim.
    return { lin, raw, W, H, destLen, cols: Math.ceil(W / 32), rows: Math.ceil(H / 32) };
}

// ----------------------------------------------------------------------------
// PUBLIC: ensureBodyTextures(ram, vram, ta, quadCount, cache, quadSels, quadGfx1s, quadColRow)
//   ram        : seeded 16MB RAM image (Uint8Array) — GFX1 part streams.
//   vram       : pane.vram (Uint8Array, 8MB) — where TextureManager re-decodes textures from.
//   ta         : the TA render_frame just emitted (Uint8Array) — authoritative per-quad TCW.
//   quadCount  : render_frame_quad_count() — number of 96B textured-sprite quads in `ta`.
//   cache      : a persistent object {} the caller keeps across frames (decode memo).
//   quadSels   : Uint16Array[quadCount] from render_frame_quad_sels() — the SOURCE GFX1 sel the
//                walker used for quad k (TILING-SAFE: a tiled cell's N tiles share one sel).
//   quadGfx1s  : Uint32Array[quadCount] from render_frame_quad_gfx1s() — owning body's GFX1 base.
//   quadColRow : Int32Array[quadCount*2] from render_frame_quad_colrow() — per-tile STORAGE
//                (col,row) (col = Ax-ASC rank, row = Ay-DESC rank). This is the tile's position
//                in the part's 32-px tile grid (REQUIRED — the re-tile key).
//
// PER-TILE RE-TILE (THE 2026-06-14 FIX, see header): the engine stores each part as ONE W×H
// PVR-twiddled blob, but the walker emits N tiles each read by pvr2 as a STANDALONE 32×32 PAL4_TW
// at its OWN rectab TCW. So for each emitted quad we decode+detwiddle its part to a linear W×H
// buffer (memoized), extract the 32×32 region at (col*32,row*32), re-twiddle it as a standalone
// 32×32, and write those 512B at THIS quad's own TCW. (The verbatim-blob write was wrong: a 32×32
// twiddle read of a W×H-twiddled blob only coincides for 32-square parts; wide/tall parts read
// garbage = the live-Sentinel scatter.) Returns {decoded,bytes,written,quads,parts}.
// ----------------------------------------------------------------------------
const QUAD = 96;                                  // textured-sprite TA block size (paraType 5)
const TCW_OFF = 0x0C;                             // TCW is word 3 of the 16B param header

// quadSrcDesc (optional, Uint8Array[quadCount*4] from render_frame_quad_srcdesc): the
// EMIT-TIME per-quad source descriptor [m, cx, ry, flags] — the walker's OWN DESC_TABLE
// entry (flags bit0=valid, bit1=per-record flip4000). THE CARVE KEY that supersedes the
// rank-based quadColRow (2026-07-05 satellite-fragmentation fix): global Ax/Ay ranks
// merge/interleave when 2+ satellite nodes draw the same (gfx1,sel) per frame (typhoon),
// collapsing the rank grid (sel 0xDEC: 16 merged ranks on an 8-col part -> m 16->8, tail
// cols carved out of range -> the pink tile-debris / Cable fragmentation). Byte-gate
// certified offline (tools/render-replica-poc/texel_gate.cpp vs engine mirror VRAM):
// coherent-frame pal17 satellites 0 -> 98 EXACT + 36 both-zero of 134; bodies/hit-flash
// unchanged-perfect. Lockstep with maplecast_mirror.cpp gstaDecodeBodies.
export function ensureBodyTextures(ram, vram, ta, quadCount, cache, quadSels, quadGfx1s, quadColRow, opts, quadMirror, quadSrcDesc) {
    if (!cache._gfx)  cache._gfx = new Map();     // gfx1 base -> {n,offs,srt}
    if (!cache._dec)  cache._dec = new Map();     // "gfx1:sel" -> {lin,W,H,...} (decode memo)
    // opts.mask (OUTPUT-ONLY, optional): Uint8Array[quadCount]; set to 1 for every quad whose
    // tile bytes this call actually wrote into `vram` (the ?bodytex=local direct-texture path
    // keys per-quad texObj attach on it — quads skipped here MUST fall back to wire VRAM).
    const mask = (opts && opts.mask) || null;
    if (cache._dec.size > 4096) cache._dec.clear();  // bound the memo across many distinct poses
    let decoded = 0, bytes = 0, written = 0, quads = 0;
    const parts = new Set();

    const dv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
    const tcwAddrOf = (q) => {
        const off = q * QUAD + TCW_OFF;
        if (off + 4 > ta.byteLength) return -1;
        const tcw = dv.getUint32(off, true);
        return ((tcw & 0x1FFFFF) << 3) >>> 0;       // fmt5 PAL4: byteAddr = (TCW & 0x1FFFFF) << 3
    };

    // PRE-PASS — per-(gfx1,sel) RUN tile-grid extent: cols = maxcol+1, rows = maxrow+1.
    // The carve step is the ENGINE TILE SIZE m = W/cols = H/rows (8/16/32, square per
    // re_kb finding:wide_part_tile_storage_order_v2), NOT a hardcoded 32. For a multi-ROW
    // part tiled at m<32 (e.g. sel285 W=64 H=16 -> 4×1 m16 or 8×2 m8) the old `row*32`
    // over-stepped past H so every row>=1 sampled past-end/zero -> the flat grey blocks the
    // user saw on Storm's cape. The walker's per-tile (col,row) ranks are correct (geometry
    // 0.00px, finding:tiling_geometry_byte_faithful); only the carve PITCH was wrong.
    // MEASURED root cause: ASMTRACE PC 0x8C034864 steps screenY per row (the walker advances
    // the Y-pen); render_frame reproduces it byte-exact — so the degenerate-Ay symptom was
    // NOT a dropped Y-pen but this carve over-step. (CONFIRMED-BY-MEASUREMENT 2026-06-15.)
    const runExtent = new Map();   // "gfx1:sel" -> {mc:maxcol, mr:maxrow}
    if (quadColRow) {
        for (let q = 0; q < quadCount; q++) {
            const g = quadGfx1s[q] >>> 0;
            if (!(g & 0x0C000000) && !(g & 0x8C000000)) continue;
            const k = g.toString(16) + ':' + quadSels[q];
            const c = quadColRow[2 * q] | 0, r = quadColRow[2 * q + 1] | 0;
            let e = runExtent.get(k);
            if (e === undefined) { e = { mc: c, mr: r }; runExtent.set(k, e); }
            else { if (c > e.mc) e.mc = c; if (r > e.mr) e.mr = r; }
        }
    }

    // Re-tile EACH emitted quad: slice its (col,row) 32×32 from the part's linear buffer, re-twiddle,
    // write 512B at the quad's OWN TCW. The walker assigns each tile a distinct +0x200 rectab slot;
    // we fill exactly that slot with the standalone 32×32 PAL4_TW the pvr2 renderer expects.
    const _f16dv = new DataView(new ArrayBuffer(4));
    const _f16 = (bits) => { _f16dv.setUint32(0, (bits << 16) >>> 0, true); return _f16dv.getFloat32(0, true); };
    for (let q = 0; q < quadCount; q++) {
        const gfx1 = quadGfx1s[q] >>> 0;
        if (!(gfx1 & 0x0C000000) && !(gfx1 & 0x8C000000)) continue;  // no valid body art
        // EFFECT-POLY GUARD (GAP 1; C++ twin maplecast_mirror.cpp gstaDecodeBodies): a gfx1
        // in the shared Effect-Poly bank [0x0CED0000,0x0CEE0000) is an ABSOLUTE-POINTER texture
        // DIRECTORY (already-decoded PVR texels), NOT a GFX1 LZSS offset table. Feeding it to
        // the LZSS path decodes a corrupt blob over the quad's TCW (the pink-streak garble).
        // SKIP: the quad keeps the engine-resident VRAM texels at its TCW.
        if ((gfx1 & 0x0FFFFFFF) >= 0x0CED0000 && (gfx1 & 0x0FFFFFFF) < 0x0CEE0000) continue;
        // BIT15 EFFECT QUADS ARE RESIDENT-BACKED -- NEVER STAGE (2026-07-05 _live4 byte-gate,
        // lockstep with maplecast_mirror.cpp gstaDecodeBodies). Their textures are engine-
        // uploaded (effect slots 0x475xxx/0x60xxxx/0x400xxx, shipped byte-exact in the GSTA
        // prefix VRAM); their sels are 0xC000-class sentinels, not GFX1 indices -- decoding
        // them staged garbage OVER the good resident texels. srcdesc flags bit0==0 marks them.
        if (quadSrcDesc && !(quadSrcDesc[4 * q + 3] & 1)) continue;
        quads++;
        const sel  = quadSels[q];
        const addr = tcwAddrOf(q);
        if (addr < 0 || addr + 512 > vram.length) continue;
        const key = gfx1.toString(16) + ':' + sel;
        let p = cache._dec.get(key);
        if (p === undefined) {
            const G = gfx1Offsets(ram, gfx1, cache._gfx);
            p = decodePart(ram, gfx1, G, sel);    // {lin,W,H,cols,rows} — detwiddled linear part
            cache._dec.set(key, p);
            if (p) { decoded++; bytes += p.destLen; }
        }
        if (!p) continue;
        parts.add(key);
        // STORAGE (col,row) for this tile. quadColRow is the authoritative grid index; fall back
        // to 0,0 (single-tile parts) if it's absent.
        const colRaw = quadColRow ? (quadColRow[2 * q] | 0) : 0;   // SCREEN col (Ax-ASC rank); -> storage col below
        const row = quadColRow ? (quadColRow[2 * q + 1] | 0) : 0;
        // TILE SIZE m = W/cols = H/rows (the engine's square per-tile pitch, finding 22-v2).
        // Derive cols/rows from this run's emitted grid extent (maxcol+1 / maxrow+1). m is
        // clamped to the 32×32 tile the pvr2 renderer reads (the engine declares a 32×32
        // texture and UV-clamps to the top-left m×m via u1=m/tile). Fallback m=32 when the
        // extent is absent or degenerate (single 32-square tile = the prior validated path).
        const lin = p.lin, W = p.W, H = p.H;
        const e = quadColRow ? runExtent.get(key) : undefined;
        const cols = e ? (e.mc + 1) : 1, rows = e ? (e.mr + 1) : 1;
        // STORAGE COLUMN vs the EFFECTIVE MIRROR (the per-side multi-column fix, 2026-07-03).
        // render_frame's quadColRow gives col = Ax-ASC rank = the SCREEN column (left→right). The
        // storage↔screen column direction REVERSES whenever the tile is horizontally mirrored on
        // screen — mir = facing XOR per-part flip4000 (the texU mirror, loc_8c0346c4 neg-r8 gate):
        // the leftmost screen tile must sample the RIGHTMOST storage column so that, once the
        // per-tile texU mirror flips each tile, the WHOLE part reads as the correctly L/R-mirrored
        // image. The old `col ASC for both` (render_frame 2026-06-14) flipped each column IN PLACE
        // but never reordered them -> multi-column parts scrambled on the mirrored side (facing=1
        // sel252/261/265: CURRENT 264/222/3714px wrong vs baker-mirrored; reversed col = 0px).
        // Single-column parts (cols==1) and un-mirrored tiles are a no-op. MEASURED vs the byte-
        // exact baker-mirrored (_screengate.mjs: facing=1 15/15, facing=0 clean). We key on the
        // per-quad texU mirror (render_frame_quad_mirror) when the caller supplies it (folds in
        // per-part flip4000); else fall back to the owning body's facing read from RAM (node+0x110)
        // — correct for the flip4000==0 common case. (re_kb finding:per_side_storage_col_reverses
        // — restored, keyed on the effective texU mirror.)
        let flip;
        if (quadMirror) { flip = quadMirror[q] & 1; }
        else {
            flip = cache._fac ? cache._fac.get(gfx1) : undefined;
            if (flip === undefined) { if (!cache._fac) cache._fac = new Map(); flip = facingForGfx1(ram, gfx1); cache._fac.set(gfx1, flip); }
        }
        let col = flip ? (cols - 1 - colRaw) : colRaw;
        let row2 = row;
        let m = cols > 0 ? (W / cols) : W;
        const mR = rows > 0 ? (H / rows) : H;
        if (mR < m) m = mR;                       // square tile = min (guards non-integer runs)
        m = m | 0; if (m <= 0) m = 32; if (m > 32) m = 32;   // clamp to the 32×32 carve window
        // ---- DESC-KEYED CARVE (2026-07-05; see the header note on quadSrcDesc). Uses the
        // walker's OWN emit-time descriptor: m = desc byte0 (engine tile size), storage
        // col = cx (facing-INDEPENDENT; per-record flip4000 pairs columns DESCENDING —
        // MEASURED sel 0xD4C engine slot0=col1; facing alone does NOT reorder storage,
        // MEASURED 98/98 facing-mirrored pal17). Part grid from FULL-SPAN dims / m. The
        // rank path above remains the fallback for quads without a valid desc (bit15) or
        // when the desc m disagrees with the part grid (torn input). Invariants untouched:
        // Y-first twiddle chunk order, the texU draw-time mirror (single source of the
        // visual flip), BTCW override, parity pin. Lockstep: maplecast_mirror.cpp.
        let pCols = cols, pRows = rows;
        if (quadSrcDesc) {
            const dm  = quadSrcDesc[4*q+0], dcx = quadSrcDesc[4*q+1];
            const dry = quadSrcDesc[4*q+2], dfl = quadSrcDesc[4*q+3];
            // ENGINE TILE SIZE mq = u1 * (8<<TSP.texU) — the quad's OWN u1 encoding (u1 =
            // m/tile, render_frame's body path), the same source as the walker's desc byte0.
            // GUARD dm==mq: on a torn desc (+0xDC shared scratch clobbered by a later node —
            // MEASURED idx-464 overlap Cable 0xD4C m32 vs satellite 0xDE6 m16) the entry
            // belongs to ANOTHER node; fall back to part-grid + screen-rank wrap. LOCKSTEP
            // with the byte-gated C++ twin (maplecast_mirror.cpp gstaDecodeBodies, 2026-07-05
            // texel_gate certification) — the old JS trusted dm with only an {8,16,32} check.
            const tspq = dv.getUint32(q * QUAD + 8, true);
            const usz = 8 << ((tspq >> 3) & 7);
            const u1q = Math.max(_f16(dv.getUint16(q * QUAD + 86, true)),
                                 _f16(dv.getUint16(q * QUAD + 90, true)),
                                 _f16(dv.getUint16(q * QUAD + 94, true)));
            let mq = Math.round(u1q * usz); if (mq < 1) mq = 1; if (mq > 32) mq = 32;
            pCols = (W / mq) | 0; if (pCols < 1) pCols = 1;
            pRows = (H / mq) | 0; if (pRows < 1) pRows = 1;
            if ((dfl & 1) && dm === mq) {
                // STORAGE COLUMN = the descriptor's RAW cx, WITHOUT the 0x4000 reversal (scene11,
                // 2026-07-10). The engine DMAs each part VERBATIM (loc_8c033d78), so storage is
                // facing-INDEPENDENT (re_kb/24 per_side_storage_col_reverses, ROM loc_8c0346c4) and
                // the per-record 0x4000 is a DRAW-TIME texU mirror ONLY. The old
                // `col = (dfl&2) ? (pCols-1-cc) : cc` DOUBLE-APPLIED 0x4000 here (once as the texU
                // mirror at draw time, again as a storage re-store) for the LINEAR path — the exact
                // twin of the re_kb/71 NATIVE double-apply that scene10 already removed. The native
                // path below overrode `col` with a raw ncol, so it was immune; the linear (m<32 or
                // Nx1 32-strip) path used `col` DIRECTLY and so column-reversed every flip4000
                // horizontal multi-column part. BYTE-GATED spurious: tools/render-replica-poc/
                // _zz_catalog_carve_gate.mjs (whole 59-char GFX2 catalog, whole-part Y-first
                // detwiddle GT — the same standard that engine-VRAM-validated the native fix):
                // reversal ON = 2618 BAD parts / 7664 BAD tiles, ALL flip4000 horizontal (strip|1 +
                // sub32|1); reversal OFF (this) = 0 bad, 0 regression (non-flip + vertical pCols==1
                // + native paths bit-unchanged by construction). C++ twins maplecast_mirror.cpp
                // gstaDecodeBodies + texel_gate.cpp need the same source-only change + native rebuild
                // for the definitive engine-mirror-VRAM cross-check (the seed[EX] proof _storm_native
                // did for native). re_kb finding:per_side_storage_col_reverses (linear twin).
                const cc = dcx % pCols;
                col = cc;
                let rr = pRows - dry;                       // desc[3] = rows - row
                if (rr < 0) rr = 0; if (rr >= pRows) rr = pRows - 1;
                row2 = rr;
            } else {
                // fallback: part grid from the quad's rank wrap (multi-instance safe)
                col = ((col % pCols) + pCols) % pCols;
                row2 = ((row % pRows) + pRows) % pRows;
            }
            m = mq;
        }
        // --- W>32 AND H>32 MULTI-TILE PART (m==32, cols>1, rows>1): copy the NATIVE storage chunk.
        // The engine stores such a part as ONE full-W×H PVR-twiddle blob whose 32×32 chunks follow
        // the PVR twiddle of the TILE grid; tile (col,row)'s VRAM is the chunk at twiddle(col,row).
        //   - Y-FIRST twiddle (twTileYFirst) for BOTH square AND non-square grids — flycast's real
        //     _twiddleSlow interleave (y-bit before x-bit). e.g. sel285 64×128 2×4, sel197 64×64 2×2,
        //     sel124 128×128 4×4. (The prior `Tw==Th ? twTile(x-first)` split was WRONG; see below.)
        // CONFIRMED-BY-MEASUREMENT 2026-07-03: reassembling the carved tiles reproduces the byte-exact
        // baker (extract_gfx1_atlas full-span detwiddled lin) 0px for all parts, both chars, 4 frames.
        // SUPERSEDES the broken non-square LINEAR fall-through (re_kb/44): linear scattered the off-
        // diagonal tiles -> the POSE-DEPENDENT Storm-cape grey-block garble (frame _gsta_nobg_360).
        const Tw = (W / 32) | 0, Th = (H / 32) | 0;
        if (m === 32 && pCols > 1 && pRows > 1 && p.raw) {
            // NATIVE-CHUNK ORDER = WHOLE-PART Y-FIRST TWIDDLE (twTileYFirst). CORRECTED
            // 2026-07-10 (re_kb/70): the engine stores each >2x2 part as ONE verbatim WxH
            // PVR-twiddle blob (DMA path loc_8c033d78, shape-agnostic), so tile (col,row)'s
            // 512B VRAM chunk is at the Y-FIRST tile-twiddle position twTileYFirst(col,row).
            // colPairChunk (the descriptor/screen-position emit order, re_kb/68) COINCIDES with
            // twTileYFirst ONLY for 4x4 & 8x4 — the roster byte-gate (_zz_roster_carve_gate.mjs,
            // whole 59-char GFX1 set) MEASURED colPairChunk WRONG on every 4x8 (128x256, 16
            // tiles) and the 8x8 (256x256, 32 tiles): 608 bad tiles / 37 parts / 13 chars
            // (Juggernaut, Abyss1, Sentinel sel570/790/1151/1161/1167 = the ROCKET-PUNCH arm,
            // Venom, Felicia, ...). twTileYFirst = 0 bad across all 21312 tiles, every shape,
            // and is byte-IDENTICAL to colPairChunk for Tw<=4&&Th<=4 (a zero-regression
            // superset: only 4x8/8x8 change, exactly the broken cases). Lockstep with
            // maplecast_mirror.cpp gstaDecodeBodies + texel_gate.cpp.
            void colPairChunk;   // kept for reference (descriptor emit order, re_kb/68; NOT the texel key)
            // NATIVE STORAGE COLUMN = the descriptor's raw STORAGE column (cx), WITHOUT the
            // per-record 0x4000 reversal applied to `col` above. The engine DMAs each part
            // VERBATIM (loc_8c033d78), so VRAM off k == decodeA(part)[k] unconditionally; the
            // 0x4000 flag is the DRAW-TIME texU mirror ONLY (re_kb/24 per_side_storage_col_reverses,
            // ROM-traced loc_8c0346c4), NEVER a storage re-store. The (dfl&2) reversal double-applies
            // 0x4000 for m32 NATIVE flip parts -> the Storm Lightning-Strike sel0x35d COLUMN-PAIR
            // SWAP (52/52 & 48/48 tiles wrong vs engine VRAM; this fix -> byte-EXACT, 0 regression
            // across 100+ non-flip native parts + rocket.mcrr sel0xdf2/0xdfc). Byte-gated:
            // tools/render-replica-poc/_storm_native_gate.mjs (CUR==FIX==engine-VRAM). The linear
            // (sub-32 satellite) path below keeps `col` untouched (byte-gated pal17 satellite cert).
            let ncol = col;
            if (quadSrcDesc && (quadSrcDesc[4 * q + 3] & 1) && quadSrcDesc[4 * q + 0] === m)
                ncol = ((quadSrcDesc[4 * q + 1] % pCols) + pCols) % pCols;
            const k = twTileYFirst(ncol, row2, Tw, Th);
            const o = k * 512;
            if (k >= 0 && o + 512 <= p.raw.length) { vram.set(p.raw.subarray(o, o + 512), addr); written++; if (mask) mask[q] = 1; continue; }
        }
        // extract the m×m linear region at (col*m, row*m), clamped to W×H, into the tile's
        // top-left (zero-pad the rest of the 32×32 = the engine's UV-clamped sample area).
        const tile = new Uint8Array(1024);
        const ox = col * m, oy = row2 * m;
        for (let yy = 0; yy < m; yy++) {
            const py = oy + yy; if (py >= H) break;
            const rowBase = py * W, dstBase = yy * 32;
            for (let xx = 0; xx < m; xx++) {
                const px = ox + xx; if (px >= W) break;
                tile[dstBase + xx] = lin[rowBase + px];
            }
        }
        vram.set(retwiddle32(tile), addr);        // standalone 32×32 PAL4_TW at this tile's own TCW
        written++; if (mask) mask[q] = 1;
    }
    return { decoded, bytes, written, quads, parts: parts.size };
}
