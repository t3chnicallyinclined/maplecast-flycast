// pldat-codec.mjs — clean-room decoder for the MVC2 PLxx_DAT GFX "part" pixel codec.
//
// =============================================================================
// STATUS (2026-06-06): CODEC ALGORITHM CONFIRMED & VALIDATED on self-contained
// parts. FULL-CHARACTER decode is BLOCKED on the back-reference WINDOW SOURCE
// (see "DECOMPRESSION ARCHITECTURE" below) — documented from a complete
// disassembly trace; one piece of runtime state is not reconstructable from the
// statically-extracted GFX_DATA_00 file alone.
//   - Decoded self-contained sprite parts render as COHERENT 4bpp graphics
//     (Ryu flesh/gi/glove fragments, recognizable shapes — NOT noise) with this
//     exact decoder + the PALETTE_DATA palette. The 8x8 tile unit and ARGB4444
//     palette are confirmed.
//   - ~14% of PL00's 1533 parts are self-contained (their back-refs stay within
//     their own already-emitted output). The other ~86% (all multi-tile body
//     parts) contain back-references with byte-offsets (up to operand<<1 =
//     0x7FF<<1 = 4094 B) that exceed the part's own emitted output at that point
//     — i.e. they index a buffer region BEFORE the part's start.
//   - Also confirmed empirically: feeding those big parts through the asm-exact
//     decoder makes the output explode, caused by tokens with top-5 count == 0
//     triggering the extended-count path (next u16 read as a 14000+ word copy)
//     in the middle of otherwise-clean pixel literals. Those words are clearly
//     meant to be literals, which means the flag stream is being mis-applied to
//     them. Combined with the back-ref-underflow, this is the SAME root cause:
//     the data the asm decoder actually consumes at runtime is NOT byte-for-byte
//     what sits in the extracted GFX_DATA_00 blob — there is a staging step (or
//     a not-yet-identified per-part src/size that bounds output) that the static
//     file does not capture.
// =============================================================================
//
// The codec is a FLAG-BIT LZSS over 16-bit little-endian words. It was reverse
// engineered clean-room from the SH4 routine `loc_8c03552a` (MVC2 NTSC-U). The
// exact parameters below were each confirmed against the routine's constants
// (operand mask 0x07ff, mask seed 0x00008000) and validated empirically by
// rendering decoded parts against the community sprite-sheet oracle.
//
// CONFIRMED PARAMETERS
// --------------------
//   - Word size:        u16 little-endian throughout (flag words, literals,
//                       tokens, extended-count words).
//   - Flag bit order:   MSB-FIRST. The flag mask is seeded to 0x8000 and
//                       arithmetic-shifted right each op; refill a new flag
//                       word when it reaches 0 (i.e. after 16 ops).
//   - Flag polarity:    bit CLEAR (0) => LITERAL ; bit SET (1) => TOKEN.
//                       (asm: `tst mask, flagword; bt literal` — branch-if-zero
//                       goes to the literal path.)
//   - Token layout:     count = token >> 11      (top 5 bits)
//                       operand = token & 0x07ff  (low 11 bits)
//   - Extended count:   if (count == 0) the real count is the NEXT u16 word
//                       (read verbatim, up to 65535).
//   - Count off-by-one: NONE. The asm copy/fill loops are do-while (`dt`
//                       decrement-and-test), so exactly `count` words are
//                       emitted.
//   - operand == 0:     ZERO/transparent fill — emit `count` words of 0x0000.
//                       (Fill value is the constant 0, NOT 0xffff. Index 0 is
//                       the transparent palette entry.)
//   - operand != 0:     BACK-REFERENCE. offset = operand << 1 BYTES from the
//                       current destination pointer (== `operand` WORDS back),
//                       then copy `count` words forward, word-by-word, overlap-
//                       capable (LZ window copy from already-emitted output).
//                       NB: the doc handoff said "operand<<1 words"; the asm
//                       shifts the operand left by 1 and subtracts it from a
//                       BYTE pointer, so it is operand<<1 BYTES = operand words.
//   - Termination:      none internal — the routine runs until its input is
//                       exhausted (the caller knows the output size).
//
// PART-BLOB / TABLE LAYOUT (from PLxx_DAT_GFX_DATA_00.BIN, confirmed)
// ------------------------------------------------------------------
//   The GFX_DATA_00 block begins with a u32 LE offset table. table[0] equals
//   the table's own byte size, so the number of entries is table[0] / 4
//   (1533 for PL00 / Ryu). Each entry is the byte offset of one part blob
//   within the same block; the data region runs from table[0] to EOF.
//
//   Each part blob = a 4-BYTE HEADER followed by the LZSS stream:
//       header[0] = logical width  in 8px TILE units
//       header[1] = logical height in 8px TILE units
//       header[2] = storage width  in 8px TILE units (>= logical, padded)
//       header[3] = storage height in 8px TILE units (>= logical, padded)
//   Confirmed: a "1x1" part decodes to exactly 64 nibbles = one 8x8 tile
//   (part 326 of PL00 is a clean self-contained 8x8 tile). The LZSS stream
//   starts at blob offset 4; its first u16 is the initial flag word (e.g.
//   PL00 parts 1 and 7 both start `7f 00` => flag 0x007f at offset 4).
//
//   Decoded output is a sequence of 4bpp texels packed two-per-byte (low
//   nibble first). The texels are organised as 8x8 TILES in storage-row-major
//   order: tile (tx,ty) occupies texels [ (ty*sw + tx)*64 .. +64 ), and within
//   a tile pixel (x,y) is texel y*8 + x. tileToImage() below performs this
//   de-interleave into a linear indexed WxH image.
//
// PALETTE (PLxx_DAT_PALETTE_DATA.BIN)
//   16 colours, ARGB4444 little-endian, 2 bytes each. Index 0 is transparent.
//   Each nibble expands *17 to 8-bit (0xF -> 0xFF).
//
// DECOMPRESSION ARCHITECTURE (full disassembly trace, 2026-06-06)
//   The GFX-load path was traced end to end in marvelous2 (read-only, facts only):
//     - loc_8c0322c0 (bank03:5069): per-part fetch wrapper. r4 = part index;
//       r6+8 = a 16-byte-stride directory; entry+8 = that part's output dest.
//       Tail-calls the decoder (loc_8c03552a) with r4 = compressed src,
//       r5 = dest.
//     - loc_8c032696 (bank03:5685): the per-part DECODE LOOP. For each part it
//       loads the compressed src from the GFX offset table (src = table_base +
//       table[idx]) and calls the decoder with the dest pinned to the CONSTANT
//       scratch 0x0CE60000 (loc_8c032854). After decoding, it COPIES the result
//       OUT of 0x0CE60000 into the part's own texture slot (the two `mov.l @r6+`
//       copy loops). So: DECOMPRESSION IS PER-PART, every part decodes to the
//       SAME fixed scratch (overwriting the previous), then is copied away.
//     - loc_8c0323b2 (bank03:5221, file-load path) is the same decoder with
//       r4 = 0x0CC00000 (staged GFX), r5 = a single dest — the whole-block stage.
//
//   So the answer to "one call vs per-part" is PER-PART; the offset table holds
//   COMPRESSED-INPUT offsets (they match the blob boundaries in the file); the
//   4-byte (w,h,sw,sh) headers live at the front of each compressed blob.
//
//   THE WALL: because every part decodes into the same scratch 0x0CE60000 and
//   is then copied out, a part's back-references reach into whatever is at
//   0x0CE60000 BEFORE/AROUND its write pointer — i.e. RESIDUE of the previously
//   decoded part (or a pre-cleared region). That runtime scratch state is not
//   reconstructable from the static GFX_DATA_00 file, and naive reconstructions
//   (empty buffer, 0x0000-prefill, 0xffff-prefill, cumulative buffer in table
//   order, assembly-reference order, sliding ring window) all either explode on
//   the top-5==0 extended-count tokens or render as noise. The remaining work is
//   to identify the scratch's initial contents and the exact per-part output
//   SIZE bound (the decoder has no internal terminator; the caller must stop it
//   at the part's pixel count), most reliably by single-stepping the real
//   loc_8c032696 loop in an emulator against a known part, or by locating the
//   per-part output-size field the copy-out loops use. See
//   docs/MARVELOUS2-RE-HANDOFF.md §2.
//
//   API: decodePart() decodes one blob standalone (correct for self-contained
//   parts; multi-tile parts' underflowing back-refs resolve to transparent).
//   decodeSharedBuffer() runs all parts through one growing buffer in table
//   order — provided for experimentation with the window source; NOT yet a
//   correct full-character decode for the reasons above.

const OPERAND_MASK = 0x07ff; // low 11 bits  (asm constant)
const COUNT_SHIFT = 11;      // count = token >> 11  (top 5 bits)
const TILE = 8;              // 8x8 pixel storage tile

/**
 * Parse the u32 LE offset table at the front of a GFX_DATA_00 block.
 * @param {Uint8Array} gfx
 * @returns {number[]} part blob byte offsets, plus a trailing EOF sentinel.
 */
export function parseOffsetTable(gfx) {
  const dv = new DataView(gfx.buffer, gfx.byteOffset, gfx.byteLength);
  const tableBytes = dv.getUint32(0, true);
  const n = tableBytes >>> 2;
  const offs = new Array(n);
  for (let i = 0; i < n; i++) offs[i] = dv.getUint32(i * 4, true);
  offs.push(gfx.byteLength); // sentinel so blob i = [offs[i], offs[i+1])
  return offs;
}

/**
 * Read a part blob's 4-byte header.
 * @returns {{w:number,h:number,sw:number,sh:number}} dimensions in 8px tiles.
 */
export function readPartHeader(gfx, blobOffset) {
  return {
    w: gfx[blobOffset],
    h: gfx[blobOffset + 1],
    sw: gfx[blobOffset + 2],
    sh: gfx[blobOffset + 3],
  };
}

/**
 * Core flag-bit LZSS decode. Appends decoded bytes onto `out`.
 *
 * @param {Uint8Array} stream   the LZSS byte stream (blob without its 4B header)
 * @param {number[]|Uint8Array} out  growable output (use a normal Array of
 *        bytes, or pass an object via the bytearray-like helpers). We use a
 *        plain Array<number> for append simplicity; callers can Uint8Array it.
 * @param {number} priorLen     number of bytes already in `out` that belong to
 *        EARLIER parts (the shared window). Back-references may legally reach
 *        into [0, priorLen); references before 0 emit transparent (0) bytes.
 */
function decodeStream(stream, out) {
  const n = stream.length;
  const rd = (p) => (p + 1 < n ? stream[p] | (stream[p + 1] << 8) : (p < n ? stream[p] : 0));
  let pos = 0;
  let mask = 0;
  let flag = 0;

  while (pos + 1 < n) {
    if (mask === 0) {            // refill flag word (MSB-first, seed 0x8000)
      flag = rd(pos); pos += 2;
      mask = 0x8000;
      continue;
    }
    const bit = flag & mask;
    mask >>>= 1;

    if (bit === 0) {             // CLEAR => literal: copy one u16 verbatim
      out.push(stream[pos], stream[pos + 1]);
      pos += 2;
    } else {                     // SET => token
      const tok = rd(pos); pos += 2;
      let count = tok >>> COUNT_SHIFT;       // top 5 bits
      const operand = tok & OPERAND_MASK;    // low 11 bits
      if (count === 0) {                     // extended length: next u16
        count = rd(pos); pos += 2;
      }
      if (operand === 0) {                   // zero / transparent fill
        for (let i = 0; i < count; i++) out.push(0, 0);
      } else {                               // back-reference (overlap-capable)
        let src = out.length - (operand << 1); // operand<<1 BYTES = operand words
        for (let i = 0; i < count; i++) {
          if (src >= 0 && src + 1 < out.length) {
            out.push(out[src], out[src + 1]);
          } else {
            out.push(0, 0);                  // before window start => transparent
          }
          src += 2;
        }
      }
    }
  }
}

/**
 * Decode one part blob in isolation (no shared window).
 * Works for self-contained parts; multi-tile body parts whose back-references
 * reach before their own start will have those refs resolve to transparent.
 * For full-character decode use decodeSharedBuffer().
 *
 * @param {Uint8Array} gfx
 * @param {number} blobOffset  byte offset of the blob (4B header + stream)
 * @param {number} blobEnd     byte offset just past the blob
 * @returns {{header:{w,h,sw,sh}, texels:Uint8Array}} texels = 4bpp packed bytes
 */
export function decodePart(gfx, blobOffset, blobEnd) {
  const header = readPartHeader(gfx, blobOffset);
  const stream = gfx.subarray(blobOffset + 4, blobEnd);
  const out = [];
  decodeStream(stream, out);
  return { header, texels: Uint8Array.from(out) };
}

/**
 * Decode every part of a GFX_DATA_00 block sequentially into one shared buffer,
 * mirroring the game's single-destination decompress. Each part's decoded
 * region begins where the previous part ended; back-references reach into the
 * accumulated buffer (the LZ window) exactly as the hardware decoder does.
 *
 * @param {Uint8Array} gfx
 * @param {number} [count] number of parts to decode (default: all)
 * @returns {{buffer:Uint8Array, partStarts:number[], headers:object[]}}
 *   partStarts[i] = byte index in `buffer` where part i begins.
 */
export function decodeSharedBuffer(gfx, count) {
  const offs = parseOffsetTable(gfx);
  const nParts = count == null ? offs.length - 1 : Math.min(count, offs.length - 1);
  const out = [];
  const partStarts = new Array(nParts);
  const headers = new Array(nParts);
  for (let i = 0; i < nParts; i++) {
    partStarts[i] = out.length;
    headers[i] = readPartHeader(gfx, offs[i]);
    const stream = gfx.subarray(offs[i] + 4, offs[i + 1]);
    decodeStream(stream, out);
  }
  return { buffer: Uint8Array.from(out), partStarts, headers };
}

/**
 * De-interleave a part's packed 4bpp tile data into a linear indexed image.
 * Storage is `sw`x`sh` tiles of 8x8; logical content is the top-left `w`x`h`
 * tiles. Returns the full storage-sized indexed image (width sw*8, height
 * sh*8); crop to (w*8, h*8) for the logical sprite part.
 *
 * @param {Uint8Array} texels  packed 4bpp (two pixels per byte, low nibble first)
 * @param {{w,h,sw,sh}} header
 * @returns {{width:number,height:number,indices:Uint8Array}}
 */
export function tileToImage(texels, header) {
  const { sw, sh } = header;
  const width = sw * TILE;
  const height = sh * TILE;
  const indices = new Uint8Array(width * height);
  const tilesPerRow = sw;
  // texel t -> nibble: byte t>>1, low nibble if t even
  const nib = (t) => (t & 1 ? (texels[t >> 1] >> 4) & 0xf : texels[t >> 1] & 0xf);
  const tileTexels = TILE * TILE; // 64
  for (let ty = 0; ty < sh; ty++) {
    for (let tx = 0; tx < sw; tx++) {
      const base = (ty * tilesPerRow + tx) * tileTexels;
      for (let py = 0; py < TILE; py++) {
        for (let px = 0; px < TILE; px++) {
          const t = base + py * TILE + px;
          if (t >> 1 >= texels.length) continue;
          indices[(ty * TILE + py) * width + (tx * TILE + px)] = nib(t);
        }
      }
    }
  }
  return { width, height, indices };
}

/**
 * Parse a PALETTE_DATA block into RGBA8 entries. ARGB4444 LE, 16 colours,
 * index 0 transparent.
 * @param {Uint8Array} pal
 * @param {number} [bank] palette bank (16 colours each) — default 0
 * @returns {Uint8Array} length 16*4 RGBA8
 */
export function parsePalette(pal, bank = 0) {
  const out = new Uint8Array(16 * 4);
  const off = bank * 32;
  for (let i = 0; i < 16; i++) {
    const v = pal[off + i * 2] | (pal[off + i * 2 + 1] << 8);
    const a = (v >> 12) & 0xf, r = (v >> 8) & 0xf, g = (v >> 4) & 0xf, b = v & 0xf;
    out[i * 4 + 0] = r * 17;
    out[i * 4 + 1] = g * 17;
    out[i * 4 + 2] = b * 17;
    out[i * 4 + 3] = i === 0 ? 0 : a * 17 || 255; // index 0 transparent
  }
  return out;
}

/**
 * Apply a 16-colour RGBA palette to an indexed image -> RGBA8 pixels.
 */
export function indicesToRGBA(indices, palette) {
  const out = new Uint8Array(indices.length * 4);
  for (let i = 0; i < indices.length; i++) {
    const idx = indices[i] & 0xf;
    out[i * 4 + 0] = palette[idx * 4 + 0];
    out[i * 4 + 1] = palette[idx * 4 + 1];
    out[i * 4 + 2] = palette[idx * 4 + 2];
    out[i * 4 + 3] = palette[idx * 4 + 3];
  }
  return out;
}
