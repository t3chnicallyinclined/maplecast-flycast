// _probe_wide_sels.mjs — enumerate every (gfx1,sel) run emitted, with sw/sh, tile grid,
// the emitted (col,row) cells, and whether it's a WIDE part (Tw>=2 && Th>=2).
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
const path = process.argv[2] ?? '../../_sentinel_scramble.mcrr';
const wantF = +(process.argv[3] ?? 0);
const GFX_DIR = fileURLToPath(new URL('../../web/render-replica/gfx/', import.meta.url));
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); u32(); const nS = u32(), nD = u32(), nF = u32(), vB = u32(), pB = u32(); u32();
const reg = () => { const a = u32(), l = u32(); let t = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) t += String.fromCharCode(c); } p += 8; return { addr: a >>> 0, len: l, tag: t }; };
const sR = Array.from({ length: nS }, reg), dR = Array.from({ length: nD }, reg);
p += vB; p += pB;
const sD = sR.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const fStart = p; const G = a => (a >>> 0) & 0xFFFFFF; const ram = new Uint8Array(16 * 1024 * 1024);
sR.forEach((r, i) => { if (r.tag === 'ram16') ram.set(sD[i], 0); else ram.set(sD[i], G(r.addr)); });
p = fStart; const frames = [];
for (let f = 0; f < nF; f++) { u32(); const vf = u32(); const ts = u32(); const dynOff = p; for (const r of dR) p += r.len; const gfxOff = p; const nG = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0; if (nG <= 64) { p += 4; for (let g = 0; g < nG && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } } const taOff = p; p += ts; frames.push({ vframe: vf, taSize: ts, dynOff, gfxOff, taOff }); }
function aGfx(off) { if (off + 4 > buf.length) return; const nG = dv.getUint32(off, true); off += 4; if (nG > 64) return; for (let i = 0; i < nG; i++) { if (off + 8 > buf.length) break; const base = dv.getUint32(off, true); off += 4; const len = dv.getUint32(off, true); off += 4; if (len > 0x800000 || off + len > buf.length) break; ram.set(buf.subarray(off, off + len), G(base)); off += len; } }
const SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
const u8r = a => ram[G(a)]; const u32r = a => (ram[G(a)] | (ram[G(a) + 1] << 8) | (ram[G(a) + 2] << 16) | (ram[G(a) + 3] << 24)) >>> 0;
function aLocal() { const done = new Set(); for (const b of SLOTS) { if (u8r(b) === 0) continue; const cid = u8r(b + 1); const g1b = u32r(b + 0x15C); if (!((g1b & 0x0C000000) || (g1b & 0x8C000000))) continue; const hex = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0'); let g1, g2; try { g1 = new Uint8Array(readFileSync(GFX_DIR + hex + '_gfx1.bin')); g2 = new Uint8Array(readFileSync(GFX_DIR + hex + '_gfx2.bin')); } catch { continue; } if (done.has(cid)) continue; done.add(cid); ram.set(g1, G(g1b)); ram.set(g2, G(u32r(b + 0x160))); } }
const fr = frames[wantF];
{ let o = fr.dynOff; for (const r of dR) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }
aGfx(fr.gfxOff); aLocal();
// slot -> gfx1 / cid for labelling
const slotInfo = SLOTS.map(b => ({ b, active: u8r(b), cid: u8r(b + 1), g1: u32r(b + 0x15C) >>> 0 }));
const M = await createRenderFrame({ locateFile: x => x });
const rp = M._malloc(ram.length); M.HEAPU8.set(ram, rp); const op = M._malloc(262144);
const len = M._render_frame_ta(rp, op, 262144); const quads = M._render_frame_quad_count();
const ta = M.HEAPU8.slice(op, op + len);
const sp = M._malloc(quads * 2), gp = M._malloc(quads * 4), crP = M._malloc(quads * 8);
M._render_frame_quad_sels(sp, quads); M._render_frame_quad_gfx1s(gp, quads); M._render_frame_quad_colrow(crP, quads);
const sels = new Uint16Array(M.HEAPU8.buffer.slice(sp, sp + quads * 2));
const gfxs = new Uint32Array(M.HEAPU8.buffer.slice(gp, gp + quads * 4));
const cr = new Int32Array(M.HEAPU8.buffer.slice(crP, crP + quads * 8));
const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
// group runs
const runs = new Map();
for (let q = 0; q < quads; q++) {
  const g = gfxs[q] >>> 0; if (!(g & 0x0C000000) && !(g & 0x8C000000)) continue;
  const sel = sels[q]; const key = (g & 0xFFFFFF).toString(16) + ':' + sel;
  let r = runs.get(key); if (!r) { r = { g, sel, tiles: [] }; runs.set(key, r); }
  const o = q * 96; const tcw = tdv.getUint32(o + 0x0C, true); const v = ((tcw & 0x1FFFFF) << 3) >>> 0;
  r.tiles.push({ v, col: cr[2 * q] | 0, row: cr[2 * q + 1] | 0, x: tdv.getFloat32(o + 36, true), y: tdv.getFloat32(o + 40, true) });
}
function partDims(g1abs, sel) { const off = u32r(g1abs + sel * 4); const pb = G(g1abs) + off; return { sw: ram[pb + 2], sh: ram[pb + 3] }; }
console.log(`frame vf=${fr.vframe} quads=${quads} runs=${runs.size}`);
for (const [k, r] of [...runs.entries()].sort((a,b)=>b[1].tiles.length - a[1].tiles.length)) {
  const slot = slotInfo.find(s => (s.g1 & 0xFFFFFF) === (r.g & 0xFFFFFF));
  const { sw, sh } = partDims(r.g, r.sel);
  const W = sw * 8, H = sh * 8, Tw = W >> 5, Th = H >> 5;
  const wide = (Tw >= 2 && Th >= 2);
  const cells = r.tiles.map(t => `(${t.col},${t.row})`).join(' ');
  const tag = wide ? 'WIDE' : '    ';
  const cidStr = slot ? `cid${slot.cid}` : '?';
  console.log(`${tag} ${cidStr} sel${String(r.sel).padStart(3)} ${W}x${H} grid ${Tw}x${Th} tiles=${r.tiles.length}  cells: ${cells}`);
}
