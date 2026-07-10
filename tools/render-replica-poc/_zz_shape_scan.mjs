// _zz_shape_scan.mjs — catalog, across a live capture, every DENSE-grid (m==32, cols>1, rows>1)
// run render_frame emits, per (cid,sel) with WxH and colsxrows. Tells us whether the 4x8/8x8
// shapes (where colPairChunk != twTileYFirst) are ever REACHED live. Reuses the _zz_realblob_gate
// mcrr parser + render_frame walk. Usage: node _zz_shape_scan.mjs band4.mcrr 30
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
const CAP = process.argv[2], STEP = parseInt(process.argv[3] || '120', 10);
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
const M = await createRenderFrame({ locateFile: x => x });
const cat = new Map();   // "cid:WxH:colsxrows" -> {cid,W,H,cols,rows,sels:Set,frames:count}
for (let fi = 0; fi < nF; fi += STEP) {
  const fr = frames[fi]; const ram = baseRam.slice();
  { let o = fr.dynOff; for (const r of dR) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }
  aGfx(ram, fr.gfxOff);
  const g2c = new Map();
  for (const b of SLOTS) { if (u8r(ram, b) === 0) continue; const cid = u8r(ram, b + 1); const g1b = u32r(ram, b + 0x15C); if (!((g1b & 0x0C000000) || (g1b & 0x8C000000))) continue; g2c.set(g1b & 0xFFFFFF, cid); const hex = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0'); let g1, g2; try { g1 = new Uint8Array(readFileSync(GFX + hex + '_gfx1.bin')); g2 = new Uint8Array(readFileSync(GFX + hex + '_gfx2.bin')); } catch { continue; } const g1d = G(g1b), g2d = G(u32r(ram, b + 0x160)); if (g1d + g1.length <= ram.length) ram.set(g1, g1d); if (g2d + g2.length <= ram.length) ram.set(g2, g2d); }
  const rp = M._malloc(ram.length); M.HEAPU8.set(ram, rp); const op = M._malloc(262144);
  M._render_frame_ta(rp, op, 262144); const quads = M._render_frame_quad_count();
  const sp = M._malloc(quads * 2), gp = M._malloc(quads * 4); M._render_frame_quad_sels(sp, quads); M._render_frame_quad_gfx1s(gp, quads);
  const sels = new Uint16Array(M.HEAPU8.buffer.slice(sp, sp + quads * 2)), gfxs = new Uint32Array(M.HEAPU8.buffer.slice(gp, gp + quads * 4));
  const crP = M._malloc(quads * 8); M._render_frame_quad_colrow(crP, quads); const cr = new Int32Array(M.HEAPU8.buffer.slice(crP, crP + quads * 8));
  const runs = new Map();
  for (let q = 0; q < quads; q++) { const g = gfxs[q] >>> 0; if (!(g & 0x0C000000) && !(g & 0x8C000000)) continue; const sel = sels[q]; const key = (g & 0xFFFFFF).toString(16) + ':' + sel; let r = runs.get(key); if (!r) { r = { g, sel, t: [] }; runs.set(key, r); } r.t.push({ col: cr[2 * q] | 0, row: cr[2 * q + 1] | 0 }); }
  for (const r of runs.values()) {
    if (r.t.length <= 1) continue; const cid = g2c.get(r.g & 0xFFFFFF) ?? -1; if (cid < 0) continue;
    const off = u32r(ram, r.g + r.sel * 4), pb = G(r.g) + off, W = ram[pb + 2] * 8, H = ram[pb + 3] * 8; if (!(W > 0 && H > 0 && W <= 1024 && H <= 1024)) continue;
    let cols = 1, rows = 1; for (const t of r.t) { if (t.col + 1 > cols) cols = t.col + 1; if (t.row + 1 > rows) rows = t.row + 1; }
    let m = (W / cols) | 0; const mR = (H / rows) | 0; if (mR < m) m = mR; if (m <= 0) m = 32; if (m > 32) m = 32;
    if (!(m === 32 && cols > 1 && rows > 1)) continue;
    const k = `${cid.toString(16)}:${W}x${H}:${cols}x${rows}`;
    let e = cat.get(k); if (!e) { e = { cid, W, H, cols, rows, sels: new Set(), frames: 0 }; cat.set(k, e); }
    e.sels.add(r.sel); e.frames++;
  }
  M._free(rp); M._free(op); M._free(sp); M._free(gp); M._free(crP);
}
console.log(`\n=== ${CAP} step=${STEP}: emitted DENSE (m32,cols>1,rows>1) shapes ===`);
const arr = [...cat.values()].sort((a, b) => (b.cols * b.rows) - (a.cols * a.rows));
for (const e of arr) {
  const nonSq = e.cols !== e.rows, tall = e.rows > 4 || e.cols > 4;
  const flag = (nonSq || (e.cols > 2 && e.rows > 2 && (e.cols > 4 || e.rows > 4))) ? '  <== colPair!=yFirst DISCRIMINATOR' : '';
  console.log(`  cid0x${e.cid.toString(16).padStart(2, '0')}  ${e.W}x${e.H}  ${e.cols}x${e.rows}  sels[${[...e.sels].join(',')}]  seen ${e.frames}f${flag}`);
}
if (!arr.length) console.log('  (no dense multi-tile runs emitted)');
