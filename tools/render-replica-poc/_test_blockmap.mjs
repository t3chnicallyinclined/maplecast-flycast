// Validate the "square-block twiddle storage" hypothesis for EVERY multi-tile run in
// frame 0 of a capture, byte-exact vs flycast-twop VRAM.
//
// HYPOTHESIS (measured on sel267 64x128 2x4): a part W x H tiled cols x rows (tile=32)
// is stored as a sequence of square sq x sq tile-blocks where sq = min(cols,rows).
// Block grid: Bc = cols/sq, Br = rows/sq, blocks raster-ordered (br*Bc+bc). Within a
// block, the sq x sq tiles follow the SQUARE twiddle twop(lc,lr,log2sq,log2sq).
//   chunkK(col,row) = (br*Bc+bc)*sq*sq + twop(col%sq, row%sq, log2sq, log2sq)
//   where bc=col/sq, br=row/sq.
// The tile's true pixels = the (chunkK)-th 512B raw chunk read as a standalone 32x32 twop.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
import { ensureBodyTextures, decodeA } from '../../web/render-replica/body_decoder.mjs';
const CAP = process.argv[2] || '../../_camcap2.mcrr';
const GFX_DIR = fileURLToPath(new URL('../../web/render-replica/gfx/', import.meta.url));
const buf = new Uint8Array(readFileSync(fileURLToPath(new URL(CAP, import.meta.url))));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); u32(); const nS = u32(), nD = u32(), nF = u32(), vB = u32(), pB = u32(); u32();
const reg = () => { const a = u32(), l = u32(); let t = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) t += String.fromCharCode(c); } p += 8; return { addr: a >>> 0, len: l, tag: t }; };
const sR = Array.from({ length: nS }, reg), dR = Array.from({ length: nD }, reg); p += vB; p += pB;
const sD = sR.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const fStart = p; const G = a => (a >>> 0) & 0xFFFFFF; const ram = new Uint8Array(16 * 1024 * 1024);
sR.forEach((r, i) => { if (r.tag === 'ram16') ram.set(sD[i], 0); else ram.set(sD[i], G(r.addr)); });
p = fStart; u32(); u32(); u32(); let o = p; for (const r of dR) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; }
{ let off = o; const nG = dv.getUint32(off, true); off += 4; if (nG <= 64) for (let i = 0; i < nG; i++) { const base = dv.getUint32(off, true); off += 4; const len = dv.getUint32(off, true); off += 4; if (len > 0x800000) break; ram.set(buf.subarray(off, off + len), G(base)); off += len; } }
const SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
const u8r = a => ram[G(a)]; const u32rOf = a => (ram[G(a)] | (ram[G(a) + 1] << 8) | (ram[G(a) + 2] << 16) | (ram[G(a) + 3] << 24)) >>> 0;
for (const b of SLOTS) { if (u8r(b) === 0) continue; const cid = u8r(b + 1); const g1b = u32rOf(b + 0x15C); if (!((g1b & 0x0C000000) || (g1b & 0x8C000000))) continue; const hex = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0'); let g1, g2; try { g1 = new Uint8Array(readFileSync(GFX_DIR + hex + '_gfx1.bin')); g2 = new Uint8Array(readFileSync(GFX_DIR + hex + '_gfx2.bin')); } catch { continue; } const g1d = G(g1b), g2d = G(u32rOf(b + 0x160)); if (g1d + g1.length <= ram.length) ram.set(g1, g1d); if (g2d + g2.length <= ram.length) ram.set(g2, g2d); }
function twop(x, y, bx, by) { let r = 0, b = 0; const sq = Math.min(bx, by); for (let i = 0; i < sq; i++) { r |= ((x >> i) & 1) << b; b++; r |= ((y >> i) & 1) << b; b++; } if (bx > by) r |= (x >> sq) << b; else if (by > bx) r |= (y >> sq) << b; return r; }
const ORD = [[0, 0], [0, 1], [1, 0], [1, 1], [0, 2], [0, 3], [1, 2], [1, 3], [2, 0], [2, 1], [3, 0], [3, 1], [2, 2], [2, 3], [3, 2], [3, 3]];
function readTile32(src, addr) { const til = new Uint8Array(1024); for (let Y = 0; Y < 32; Y += 4) for (let X = 0; X < 32; X += 4) { const blk = (twop(X, 0, 5, 5) + twop(0, Y, 5, 5)) / 16 | 0; const base = blk * 8; for (let i = 0; i < 16; i++) { const cx = ORD[i][0], cy = ORD[i][1]; const b = src[addr + base + (i >> 1)] || 0; til[(Y + cy) * 32 + (X + cx)] = (i & 1) ? (b >> 4) & 0xF : b & 0xF; } } return til; }
const log2i = n => Math.log2(n) | 0;
// x-first twiddle (the current NATIVE square path) — correct for SQUARE tile grids.
function twTileX(col, row, Tw, Th) { let r = 0, b = 0; const sq = Math.min(Tw, Th); for (let i = 0; i < log2i(sq); i++) { r |= ((col >> i) & 1) << b; b++; r |= ((row >> i) & 1) << b; b++; } if (Tw > Th) r |= (col >> log2i(sq)) << b; else if (Th > Tw) r |= (row >> log2i(sq)) << b; return r; }
// y-first twiddle (flycast _twiddleSlow convention) — correct for NON-SQUARE tile grids.
function twTileY(col, row, Tw, Th) { let rv = 0, sh = 0; let xs = Tw >> 1, ys = Th >> 1, x = col, y = row; while (xs || ys) { if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; } if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; } } return rv; }
function chunkK(col, row, cols, rows) { return (cols === rows) ? twTileX(col, row, cols, rows) : twTileY(col, row, cols, rows); }
const M = await createRenderFrame({ locateFile: x => x });
const rp = M._malloc(ram.length); M.HEAPU8.set(ram, rp); const op = M._malloc(262144);
const len = M._render_frame_ta(rp, op, 262144); const quads = M._render_frame_quad_count();
const ta = M.HEAPU8.slice(op, op + len); const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
const sp = M._malloc(quads * 2), gp = M._malloc(quads * 4); M._render_frame_quad_sels(sp, quads); M._render_frame_quad_gfx1s(gp, quads);
const sels = new Uint16Array(M.HEAPU8.buffer.slice(sp, sp + quads * 2)); const gfxs = new Uint32Array(M.HEAPU8.buffer.slice(gp, gp + quads * 4));
const crP = M._malloc(quads * 8); M._render_frame_quad_colrow(crP, quads); const cr = new Int32Array(M.HEAPU8.buffer.slice(crP, crP + quads * 8));
const vram = new Uint8Array(8 * 1024 * 1024); ensureBodyTextures(ram, vram, ta, quads, {}, sels, gfxs, cr);
const runs = new Map();
for (let q = 0; q < quads; q++) { const g = gfxs[q] >>> 0; if (!(g & 0x0C000000) && !(g & 0x8C000000)) continue; const sel = sels[q]; const key = (g & 0xFFFFFF).toString(16) + ':' + sel; let r = runs.get(key); if (!r) { r = { g, sel, t: [] }; runs.set(key, r); } const tcw = tdv.getUint32(q * 96 + 0x0C, true); r.t.push({ a: ((tcw & 0x1FFFFF) << 3) >>> 0, col: cr[2 * q] | 0, row: cr[2 * q + 1] | 0 }); }
let okRuns = 0, badRuns = 0;
for (const r of runs.values()) {
  if (r.t.length <= 1) continue;
  const off = u32rOf(r.g + r.sel * 4); const pb = G(r.g) + off; const W = ram[pb + 2] * 8, H = ram[pb + 3] * 8;
  if (!(W > 0 && H > 0 && W <= 1024 && H <= 1024)) continue;
  const srt = []; { const nn = u32rOf(r.g) >> 2; const s = new Set(); for (let i = 0; i < nn; i++) s.add(u32rOf(r.g + i * 4)); for (const v of s) srt.push(v); srt.sort((a, b) => a - b); }
  let end = pb; for (const oo of srt) { if ((G(r.g) + oo) > pb) { end = G(r.g) + oo; break; } } if (end <= pb) end = pb + 0x8000;
  const raw = decodeA(ram, pb + 4, end, (W * H) >> 1);
  let cols = 1, rows = 1; for (const t of r.t) { if (t.col + 1 > cols) cols = t.col + 1; if (t.row + 1 > rows) rows = t.row + 1; }
  let m = (W / cols) | 0; const mR = (H / rows) | 0; if (mR < m) m = mR; if (m <= 0) m = 32; if (m > 32) m = 32;
  // The 512B-chunk-per-tile storage model ONLY applies to FULL 32x32 tiles (m==32,
  // i.e. W>32 AND H>32). Sub-32 tiles (m<32) are sub-divisions of a single twiddle
  // chunk and are handled by the linear carve. SKIP those here.
  if (!(m === 32 && cols > 1 && rows > 1)) continue;
  const sq = Math.min(cols, rows);
  const pow2 = (sq & (sq - 1)) === 0;
  let bad = 0, tot = 0;
  for (const t of r.t) {
    const vt = readTile32(vram, t.a);
    const k = chunkK(t.col, t.row, cols, rows); const o2 = k * 512;
    const ct = (o2 + 512 <= raw.length) ? readTile32(raw, o2) : new Uint8Array(1024);
    let b = 0; for (let i = 0; i < 1024; i++) if (vt[i] !== ct[i]) b++;
    bad += b; tot += 1024;
  }
  const status = bad === 0 ? 'OK  ' : 'BAD ';
  if (bad === 0) okRuns++; else badRuns++;
  console.log(`${status}sel${String(r.sel).padStart(4)} ${W}x${H} ${cols}x${rows} sq=${sq}${pow2 ? '' : '(np2)'} badPx=${bad}/${tot}`);
}
console.log(`\nblock-map storage: ${okRuns} OK, ${badRuns} BAD multi-tile runs`);
process.exit(badRuns ? 1 : 0);
