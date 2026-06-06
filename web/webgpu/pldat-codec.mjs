// pldat-codec.mjs — clean-room decoder for the MVC2 PLxx_DAT GFX "part" pixel codec.
//
// =============================================================================
// STATUS (2026-06-06): CODEC ALGORITHM CONFIRMED & VALIDATED.
//   - Decoded self-contained sprite parts render as COHERENT 4bpp graphics
//     (Ryu flesh/gi/glove fragments, recognizable shapes — NOT noise) when
//     fed through this exact decoder + the PALETTE_DATA palette.
//   - The 8x8 tile unit and ARGB4444 palette are confirmed.
//   - REMAINING OPEN ITEM: large (multi-tile) body parts use LZSS back-
//     references that reach BEFORE the start of their own blob — i.e. into a
//     SHARED decode buffer that accumulates earlier parts. Decoding a single
//     part in isolation only works for the ~14% of parts that are self-
//     contained. Full-character decode requires running decodeSharedBuffer()
//     over the whole part table in the game's decode order (see notes below).
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
// SHARED-BUFFER NOTE (open item)
//   The SH4 caller stages the whole PLxx_DAT GFX into one work buffer and calls
//   loc_8c03552a to decompress into one destination texture buffer; parts are
//   emitted sequentially and later parts back-reference earlier parts' pixels.
//   `decodePart` therefore takes an optional `priorOutput` (a Uint8Array of all
//   bytes already emitted for this character) and resolves back-references that
//   reach before the part's own start into it. To decode a full character,
//   call decodeSharedBuffer() which feeds every part in table order through one
//   growing buffer. The exact decode ORDER the game uses (table order vs
//   assembly-driven on-demand) is not yet pinned; table order reproduces the
//   self-contained parts but the cross-part window for full body parts needs
//   the game's real ordering to line up. See docs/MARVELOUS2-RE-HANDOFF.md §2.

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
