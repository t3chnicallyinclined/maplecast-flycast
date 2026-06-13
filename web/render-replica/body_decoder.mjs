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
//   * TILING (the fix, 2026-06-13): the body walker (loc_8c0344d4) expands ONE GFX2 cell record
//     into N TILES (N = desc[r13+1]+1 from the 0x8C1F9F9C tile-descriptor table), each tile a
//     separate emitted quad, ALL SHARING that cell's GFX1 sel (r11+6 is read once per cell, not
//     per tile). So quad-count > cell-count whenever any cell tiles, and the OLD 1:1 pairing
//     quad[i]<->cell-sel[i] SLIPPED after the first tiled cell -> right colors, wrong quad =
//     the scramble. PROVED on a live frame (maxq_86.mcrr): Cable sid 0x47 = 19 cells -> 71 quads,
//     slip onset quad 2. THE FIX: render_frame now exposes render_frame_quad_sels() = the SOURCE
//     sel per emitted quad (the sel the walker actually used, captured at submit time from r11+6).
//     We decode EACH quad's OWN sel to EACH quad's OWN TCW (a tiled cell's N tiles all decode the
//     same sel to N different TCWs). NO 1:1 sel walk, NO slip. (The single-char _ryu_capture passed
//     before only because that pose had no tiled cells; live tiled limbs expose it.)
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
    return { bytes: decodeA(ram, srcStart, srcEnd, destLen), destLen };
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
// TILING-SAFE PAIRING (2026-06-13 fix): we iterate the EMITTED QUADS (not the cell records) and
// decode each quad's OWN (gfx1,sel) to its OWN TCW. The body walker expands one GFX2 cell into N
// tiles, all carrying the same (gfx1,sel) but distinct TCWs — so this writes the SAME sprite to
// each of that cell's tiles, with NO slip. (The old quad[i]<->cell-sel[i] 1:1 walk slipped after
// the first tiled cell -> right colors, wrong quad = the scramble; proven on maxq_86.mcrr.)
//
// DECODE MEMO: keyed by (gfx1,sel) so a tiled cell decodes ONCE and re-writes to each tile; a
// static pose re-uses last frame's decode. Returns {decoded,bytes,written,quads}.
// ----------------------------------------------------------------------------
const QUAD = 96;                                  // textured-sprite TA block size (paraType 5)
const TCW_OFF = 0x0C;                             // TCW is word 3 of the 16B param header

export function ensureBodyTextures(ram, vram, ta, quadCount, cache, quadSels, quadGfx1s) {
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

    for (let q = 0; q < quadCount; q++) {
        const sel  = quadSels[q];
        const gfx1 = quadGfx1s[q] >>> 0;
        if (!(gfx1 & 0x0C000000) && !(gfx1 & 0x8C000000)) continue;  // no valid body art
        quads++;

        // decode-on-(gfx1,sel): a tiled cell's repeated sel hits this memo after the first tile.
        const key = (gfx1 >>> 0).toString(16) + ':' + sel;
        let p = cache._dec.get(key);
        if (p === undefined) {
            const G = gfx1Offsets(ram, gfx1, cache._gfx);
            p = decodePart(ram, gfx1, G, sel);    // {bytes,destLen} or null
            cache._dec.set(key, p);
            if (p) { decoded++; bytes += p.destLen; }
        }
        if (!p) continue;

        const addr = tcwAddrOf(q);                // THIS quad's own TCW (distinct per tile)
        if (addr < 0 || addr + p.destLen > vram.length) continue;
        vram.set(p.bytes, addr);                  // VERBATIM (twiddled storage) — render samples this
        written++;
    }
    return { decoded, bytes, written, quads };
}
