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
//     "TCW-BITEXACT"), and the walker emits ONE quad per cell record IN ORDER -> TA quad order ==
//     cell-record order per char (verified vs mc_render_rec_gt.bin). So we read TCWs straight from
//     the emitted TA and pair quad[i] <-> the char's cell-sel[i] with NO search and NO heuristic.
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
// PUBLIC: ensureBodyTextures(ram, vram, ta, quadCount, cache)
//   ram       : seeded 16MB RAM image (Uint8Array) — GFX1/GFX2/char structs.
//   vram      : pane.vram (Uint8Array, 8MB) — where TextureManager re-decodes textures from.
//   ta        : the TA render_frame just emitted (Uint8Array) — authoritative per-quad TCW.
//   quadCount : render_frame_quad_count() — number of 96B textured-sprite quads in `ta`.
//   cache     : a persistent object {} the caller keeps across frames (decode-on-change memo).
// For each active char (slot order), pairs its cell sels with the matching run of TA quad TCWs
// (quad order == cell order per char) and writes each decoded part VERBATIM into vram at the
// TCW addr. Decode runs only when a slot's sprite_id changes; the write (cheap memcpy) runs each
// call so a freshly-applied VRAM frame stays correct. Returns {decoded,bytes,slots,written}.
// ----------------------------------------------------------------------------
const QUAD = 96;                                  // textured-sprite TA block size (paraType 5)
const TCW_OFF = 0x0C;                             // TCW is word 3 of the 16B param header

export function ensureBodyTextures(ram, vram, ta, quadCount, cache) {
    if (!cache._sid)  cache._sid = new Int32Array(SLOTS.length).fill(-1);
    if (!cache._gfx)  cache._gfx = new Map();     // gfx1 base -> {n,offs,srt}
    if (!cache._pose) cache._pose = new Array(SLOTS.length).fill(null); // cached decoded parts/slot
    let decoded = 0, bytes = 0, slots = 0, written = 0;

    const dv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
    const tcwAddrOf = (q) => {
        const off = q * QUAD + TCW_OFF;
        if (off + 4 > ta.byteLength) return -1;
        const tcw = dv.getUint32(off, true);
        return ((tcw & 0x1FFFFF) << 3) >>> 0;
    };

    // Walk active chars in slot order; the TA quads are emitted in the same slot order, one run
    // of `cnt` quads per char (cnt == that char's cell record count). qcur advances across runs.
    let qcur = 0;
    for (let s = 0; s < SLOTS.length && qcur < quadCount; s++) {
        const base = SLOTS[s];
        if (u8(ram, base + OFF_ACTIVE) === 0) continue;
        const sid = u16(ram, base + OFF_SID);
        const gfx1 = u32(ram, base + OFF_GFX1), gfx2 = u32(ram, base + OFF_GFX2);
        if (!(gfx1 & 0x0C000000) && !(gfx1 & 0x8C000000)) continue;
        const sels = cellSels(ram, gfx2, sid);
        if (!sels) continue;

        // Decode-on-change: rebuild this slot's decoded part list only when sprite_id changed.
        let pose = cache._pose[s];
        if (cache._sid[s] !== sid || !pose) {
            const G = gfx1Offsets(ram, gfx1, cache._gfx);
            pose = [];
            for (const sel of sels) pose.push(decodePart(ram, gfx1, G, sel));
            cache._pose[s] = pose;
            cache._sid[s] = sid;
            slots++;
            for (const p of pose) if (p) { decoded++; bytes += p.destLen; }
        }

        // Pair this char's cell sels with its run of TA quad TCWs (quad order == cell order) and
        // write each decoded part VERBATIM to the TCW VRAM addr. We consume `sels.length` quads.
        const run = sels.length;
        for (let i = 0; i < run && qcur < quadCount; i++, qcur++) {
            const p = pose[i];
            if (!p) continue;
            const addr = tcwAddrOf(qcur);
            if (addr < 0 || addr + p.destLen > vram.length) continue;
            vram.set(p.bytes, addr);              // VERBATIM (twiddled storage) — render samples this
            written++;
        }
    }
    return { decoded, bytes, slots, written };
}
