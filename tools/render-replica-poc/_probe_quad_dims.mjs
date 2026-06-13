// _probe_quad_dims.mjs — for a given (gfx1,sel), dump per-quad TSP texU/texV (tile pixel dims),
// vaddr, screen XY, and UV rect, so we can see the ACTUAL per-tile texture size the walker uses.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
const path = process.argv[2] ?? '../../_sentinel_scramble.mcrr';
const wantF = +(process.argv[3] ?? 0);
const wantGfx = parseInt(process.argv[4] ?? 'c420040', 16) >>> 0;
const wantSel = +(process.argv[5] ?? 121);
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
// TSP texU/texV: bits in TSP word (offset 8 in 16B header? param layout). flycast: TSP is word2.
// 96B textured sprite: header 16B (PCW,ISP,TSP,TCW) + ... we know TCW at +0x0C. TSP at +0x08.
const texDims = tsp => { const u = (tsp >> 3) & 7, v = tsp & 7; return [8 << u, 8 << v]; };
console.log(`sel${wantSel} quads in run:`);
const list = [];
for (let q = 0; q < quads; q++) {
  if ((gfxs[q] & 0xFFFFFF) !== (wantGfx & 0xFFFFFF) || sels[q] !== wantSel) continue;
  const o = q * 96;
  const tsp = tdv.getUint32(o + 0x08, true);
  const tcw = tdv.getUint32(o + 0x0C, true);
  const v = ((tcw & 0x1FFFFF) << 3) >>> 0;
  const [tu, tv] = texDims(tsp);
  // UVs for the 4 verts of a sprite (paraType5 textured sprite): u/v often packed in last verts.
  const x0 = tdv.getFloat32(o + 36, true), y0 = tdv.getFloat32(o + 40, true);
  list.push({ q, v, tu, tv, tcw: tcw >>> 0, col: cr[2 * q] | 0, row: cr[2 * q + 1] | 0, x0, y0 });
}
list.sort((a, b) => a.v - b.v);
const base = list.length ? list[0].v : 0;
for (const t of list) {
  console.log(`  q${String(t.q).padStart(2)} +0x${(t.v - base).toString(16).padStart(4)} tcw=0x${t.tcw.toString(16)} texDim=${t.tu}x${t.tv} cell(${t.col},${t.row}) screen(${Math.round(t.x0)},${Math.round(t.y0)})`);
}
// part dims
const slot = SLOTS.find(b => (u32r(b + 0x15C) & 0xFFFFFF) === (wantGfx & 0xFFFFFF));
const g1abs = u32r(slot + 0x15C);
const off = u32r(g1abs + wantSel * 4); const pb = G(g1abs) + off;
console.log(`part sw=${ram[pb + 2]} sh=${ram[pb + 3]} => W=${ram[pb + 2] * 8} H=${ram[pb + 3] * 8}`);
