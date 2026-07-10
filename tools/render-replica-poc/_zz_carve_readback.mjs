// ACTUAL-carve readback gate with the PRODUCTION Y-first twiddle. Runs the real
// ensureBodyTextures (mirror-neutralized: qMir=0, no srcdesc -> col=Ax-ASC storage rank) and
// reads back each tile at its TCW with the Y-first detwiddle, comparing to the whole-part Y-first
// detwiddle slice at (col,row). Reports ALL multi-tile runs (native AND linear). Multi-instance
// single-tile repeats are harmless; multi-tile rank-merge runs may show false positives (noted).
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
import { ensureBodyTextures, decodeA } from '../../web/render-replica/body_decoder.mjs';
const CAP = process.argv[2], FILTER = parseInt(process.argv[3], 16), STEP = parseInt(process.argv[4] || '120', 10);
const GFX = fileURLToPath(new URL('../../web/render-replica/gfx/', import.meta.url));
const buf = new Uint8Array(readFileSync(fileURLToPath(new URL(CAP, import.meta.url))));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); u32(); const nS = u32(), nD = u32(), nF = u32(), vB = u32(), pB = u32(); u32();
const reg = () => { const a = u32(), l = u32(); let t = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) t += String.fromCharCode(c); } p += 8; return { addr: a >>> 0, len: l, tag: t }; };
const sR = Array.from({ length: nS }, reg), dR = Array.from({ length: nD }, reg); p += vB; p += pB;
const sD = sR.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const fStart = p; const G = a => (a >>> 0) & 0xFFFFFF;
const baseRam = new Uint8Array(16 * 1024 * 1024);
sR.forEach((r, i) => { if (r.tag === 'ram16') baseRam.set(sD[i], 0); else baseRam.set(sD[i], G(r.addr)); });
p = fStart; const frames = [];
for (let f = 0; f < nF; f++) { u32(); const vf = u32(); u32(); const dynOff = p; for (const r of dR) p += r.len; const gfxOff = p; const nG = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0; if (nG <= 64) { p += 4; for (let g = 0; g < nG && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } } frames.push({ vframe: vf, dynOff, gfxOff }); }
function aGfx(ram, off) { if (off + 4 > buf.length) return; const nG = dv.getUint32(off, true); off += 4; if (nG > 64) return; for (let i = 0; i < nG; i++) { if (off + 8 > buf.length) break; const base = dv.getUint32(off, true); off += 4; const len = dv.getUint32(off, true); off += 4; if (len > 0x800000 || off + len > buf.length) break; ram.set(buf.subarray(off, off + len), G(base)); off += len; } }
const SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
const u8r = (r, a) => r[G(a)], u32r = (r, a) => (r[G(a)] | (r[G(a) + 1] << 8) | (r[G(a) + 2] << 16) | (r[G(a) + 3] << 24)) >>> 0;
function tw(x, y, xs, ys) { let rv = 0, sh = 0; xs >>= 1; ys >>= 1; while (xs || ys) { if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; } if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; } } return rv; }
const DETW = [[], []]; for (let s = 0; s < 11; s++) { const ys = 1 << s; DETW[0][s] = new Int32Array(1024); DETW[1][s] = new Int32Array(1024); for (let i = 0; i < 1024; i++) { DETW[0][s][i] = tw(i, 0, 1024, ys); DETW[1][s][i] = tw(0, i, ys, 1024); } }
const ORD = [[0, 0], [0, 1], [1, 0], [1, 1], [0, 2], [0, 3], [1, 2], [1, 3], [2, 0], [2, 1], [3, 0], [3, 1], [2, 2], [2, 3], [3, 2], [3, 3]];
const l2 = v => { let n = -1; while (v) { v >>= 1; n++; } return n; };
function detwiddle(data, w, h) { const bcx = l2(w), bcy = l2(h); const idx = new Uint8Array(w * h); for (let y = 0; y < h; y += 4) for (let x = 0; x < w; x += 4) { const blk = ((DETW[0][bcy][x] + DETW[1][bcx][y]) / 16) | 0, base = blk * 8; for (let i = 0; i < 16; i++) { const cx = ORD[i][0], cy = ORD[i][1], b = (base + (i >> 1) < data.length) ? data[base + (i >> 1)] : 0; idx[(y + cy) * w + (x + cx)] = (i & 1) ? ((b >> 4) & 0xF) : (b & 0xF); } } return idx; }
function readTile32(vram, addr) { const t = new Uint8Array(1024); for (let Y = 0; Y < 32; Y += 4) for (let X = 0; X < 32; X += 4) { const blk = ((DETW[0][5][X] + DETW[1][5][Y]) / 16) | 0, base = blk * 8; for (let i = 0; i < 16; i++) { const cx = ORD[i][0], cy = ORD[i][1], b = vram[addr + base + (i >> 1)] || 0; t[(Y + cy) * 32 + (X + cx)] = (i & 1) ? (b >> 4) & 0xF : b & 0xF; } } return t; }
const M = await createRenderFrame({ locateFile: x => x });
const rep = new Map();
for (let fi = 0; fi < nF; fi += STEP) {
  const fr = frames[fi]; const ram = baseRam.slice();
  { let o = fr.dynOff; for (const r of dR) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }
  aGfx(ram, fr.gfxOff);
  const cids = new Set();
  for (const b of SLOTS) { if (u8r(ram, b) === 0) continue; const cid = u8r(ram, b + 1); const g1b = u32r(ram, b + 0x15C); if (!((g1b & 0x0C000000) || (g1b & 0x8C000000))) continue; if (cids.has(cid)) continue; cids.add(cid); const hex = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0'); let g1, g2; try { g1 = new Uint8Array(readFileSync(GFX + hex + '_gfx1.bin')); g2 = new Uint8Array(readFileSync(GFX + hex + '_gfx2.bin')); } catch { continue; } const g1d = G(g1b), g2d = G(u32r(ram, b + 0x160)); if (g1d + g1.length <= ram.length) ram.set(g1, g1d); if (g2d + g2.length <= ram.length) ram.set(g2, g2d); }
  if (!cids.has(FILTER)) continue;
  const g2c = new Map(); for (const b of SLOTS) { if (u8r(ram, b) === 0) continue; g2c.set(u32r(ram, b + 0x15C) & 0xFFFFFF, u8r(ram, b + 1)); }
  const rp = M._malloc(ram.length); M.HEAPU8.set(ram, rp); const op = M._malloc(262144);
  const len = M._render_frame_ta(rp, op, 262144); const quads = M._render_frame_quad_count();
  const ta = M.HEAPU8.slice(op, op + len); const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
  const sp = M._malloc(quads * 2), gp = M._malloc(quads * 4); M._render_frame_quad_sels(sp, quads); M._render_frame_quad_gfx1s(gp, quads);
  const sels = new Uint16Array(M.HEAPU8.buffer.slice(sp, sp + quads * 2)), gfxs = new Uint32Array(M.HEAPU8.buffer.slice(gp, gp + quads * 4));
  const crP = M._malloc(quads * 8); M._render_frame_quad_colrow(crP, quads); const cr = new Int32Array(M.HEAPU8.buffer.slice(crP, crP + quads * 8));
  const vram = new Uint8Array(8 * 1024 * 1024); const qMir = new Uint8Array(quads);
  ensureBodyTextures(ram, vram, ta, quads, {}, sels, gfxs, cr, undefined, qMir); // mirror-neutral, rank path
  // instance count per (gfx1,sel)
  const inst = new Map();
  for (let q = 0; q < quads; q++) { const g = gfxs[q] >>> 0; if (!(g & 0x0C000000) && !(g & 0x8C000000)) continue; const k = (g & 0xFFFFFF) + ':' + sels[q]; inst.set(k, (inst.get(k) || 0) + 1); }
  const runs = new Map();
  for (let q = 0; q < quads; q++) { const g = gfxs[q] >>> 0; if (!(g & 0x0C000000) && !(g & 0x8C000000)) continue; const key = (g & 0xFFFFFF).toString(16) + ':' + sels[q]; let r = runs.get(key); if (!r) { r = { g: g & 0xFFFFFF, sel: sels[q], t: [] }; runs.set(key, r); } const tcw = tdv.getUint32(q * 96 + 0x0C, true); r.t.push({ a: ((tcw & 0x1FFFFF) << 3) >>> 0, col: cr[2 * q] | 0, row: cr[2 * q + 1] | 0 }); }
  for (const r of runs.values()) {
    const cid = g2c.get(r.g & 0xFFFFFF) ?? -1; if (cid !== FILTER) continue;
    const off = u32r(ram, (r.g >= 0x0C000000 ? r.g : (r.g | 0x0C000000)) + r.sel * 4);
    const g1b = r.g >= 0x0C000000 ? r.g : (r.g | 0x0C000000);
    const poff = u32r(ram, g1b + r.sel * 4); const pb = G(g1b) + poff; const W = ram[pb + 2] * 8, H = ram[pb + 3] * 8;
    if (!(W > 0 && H > 0 && W <= 1024 && H <= 1024)) continue;
    let cols = 1, rows = 1; for (const t of r.t) { if (t.col + 1 > cols) cols = t.col + 1; if (t.row + 1 > rows) rows = t.row + 1; }
    let m = (W / cols) | 0; const mR = (H / rows) | 0; if (mR < m) m = mR; if (m <= 0) m = 32; if (m > 32) m = 32;
    const Tw = (W / 32) | 0, Th = (H / 32) | 0; const native = (m === 32 && cols > 1 && rows > 1);
    const multiInst = (inst.get((r.g & 0xFFFFFF) + ':' + r.sel) || 1) > (cols * rows);
    const srt = []; { const nn = u32r(ram, g1b) >> 2, s = new Set(); for (let i = 0; i < nn; i++) s.add(u32r(ram, g1b + i * 4)); for (const v of s) srt.push(v); srt.sort((a, b) => a - b); }
    let end = pb; for (const o of srt) { if ((G(g1b) + o) > pb) { end = G(g1b) + o; break; } } if (end <= pb) end = pb + 0x8000;
    const raw = decodeA(ram, pb + 4, end, (W * H) >> 1); const ref = detwiddle(raw, W, H);
    let bad = 0, tot = 0;
    for (const t of r.t) { const til = readTile32(vram, t.a); tot++; let tb = 0; for (let y = 0; y < m; y++) for (let x = 0; x < m; x++) { const dx = t.col * m + x, dy = t.row * m + y; if (dx >= W || dy >= H) continue; if (til[y * 32 + x] !== ref[dy * W + dx]) tb++; } if (tb > 0) bad++; }
    const key = `sel${r.sel} ${W}x${H} ${cols}x${rows} m${m} ${native ? 'NATIVE' : 'LINEAR'}${multiInst ? ' MULTI' : ''}`;
    let e = rep.get(key); if (!e) { e = { fr: 0, maxBad: 0, tot }; rep.set(key, e); } e.fr++; if (bad > e.maxBad) e.maxBad = bad;
  }
  M._free(rp); M._free(op); M._free(sp); M._free(gp); M._free(crP);
}
console.log(`\n=== ACTUAL-carve Y-first readback  cid=0x${FILTER.toString(16)} cap=${CAP} step=${STEP} (mirror-neutral rank path) ===`);
for (const [k, e] of [...rep.entries()].sort((a, b) => b[1].maxBad - a[1].maxBad)) {
  const flag = e.maxBad > 0 ? (k.includes('MULTI') ? 'multi?' : 'BAD   ') : 'clean ';
  console.log(`${flag} ${k.padEnd(38)} frames=${e.fr} maxBadTiles=${e.maxBad}/${e.tot}`);
}
