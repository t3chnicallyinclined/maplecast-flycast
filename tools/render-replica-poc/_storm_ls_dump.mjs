// _storm_ls_dump.mjs — dump per-tile (col,row,mirror,srcdesc,addr) for Storm Lightning-Strike
// parts at a chosen frame, plus the addr->storage relationship, to see the real carve structure.
// Usage: node _storm_ls_dump.mjs <frame> [sel_hex]
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
import { decodeA } from '../../web/render-replica/body_decoder.mjs';

const CAP = 'torture.mcrr';
const FRAME = parseInt(process.argv[2] || '90', 10);
const SEL = process.argv[3] !== undefined ? parseInt(process.argv[3], 16) : null;
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

const fr = frames[FRAME]; const ram = baseRam.slice();
{ let o = fr.dynOff; for (const r of dR) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }
aGfx(ram, fr.gfxOff);
for (const b of SLOTS) { if (u8r(ram, b) === 0) continue; const cid = u8r(ram, b + 1); const g1b = u32r(ram, b + 0x15C); if (!((g1b & 0x0C000000) || (g1b & 0x8C000000))) continue; const hex = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0'); let g1, g2; try { g1 = new Uint8Array(readFileSync(GFX + hex + '_gfx1.bin')); g2 = new Uint8Array(readFileSync(GFX + hex + '_gfx2.bin')); } catch { continue; } const g1d = G(g1b), g2d = G(u32r(ram, b + 0x160)); if (g1d + g1.length <= ram.length) ram.set(g1, g1d); if (g2d + g2.length <= ram.length) ram.set(g2, g2d); }
const slot0 = SLOTS[0];
const sid = ram[G(slot0 + 0x144)] | (ram[G(slot0 + 0x144) + 1] << 8);
const facing = u8r(ram, slot0 + 0x110) ? 1 : 0;
const gfx1Storm = u32r(ram, slot0 + 0x15C) >>> 0;
console.log(`frame ${FRAME} vframe=${fr.vframe} Storm sid=0x${sid.toString(16)} facing=${facing} gfx1=0x${gfx1Storm.toString(16)}`);

const rp = M._malloc(ram.length); M.HEAPU8.set(ram, rp); const op = M._malloc(262144);
const len = M._render_frame_ta(rp, op, 262144); const quads = M._render_frame_quad_count();
const ta = M.HEAPU8.slice(op, op + len); const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
const sp = M._malloc(quads * 2), gp = M._malloc(quads * 4); M._render_frame_quad_sels(sp, quads); M._render_frame_quad_gfx1s(gp, quads);
const sels = new Uint16Array(M.HEAPU8.buffer.slice(sp, sp + quads * 2)), gfxs = new Uint32Array(M.HEAPU8.buffer.slice(gp, gp + quads * 4));
const crP = M._malloc(quads * 8); M._render_frame_quad_colrow(crP, quads); const cr = new Int32Array(M.HEAPU8.buffer.slice(crP, crP + quads * 8));
const miP = M._malloc(quads); M._render_frame_quad_mirror(miP, quads); const qMir = new Uint8Array(M.HEAPU8.buffer.slice(miP, miP + quads));
const sdP = M._malloc(quads * 4); M._render_frame_quad_srcdesc(sdP, quads); const qSD = new Uint8Array(M.HEAPU8.buffer.slice(sdP, sdP + quads * 4));

// group Storm body runs
const runs = new Map();
for (let q = 0; q < quads; q++) {
  const g = gfxs[q] >>> 0; if ((g & 0xFFFFFF) !== (gfx1Storm & 0xFFFFFF)) continue;
  const tcw = tdv.getUint32(q * 96 + 0x0C, true); const a = ((tcw & 0x1FFFFF) << 3) >>> 0;
  const tsp = tdv.getUint32(q * 96 + 8, true); const texU = (tsp >> 3) & 7, texV = tsp & 7;
  const key = sels[q];
  let r = runs.get(key); if (!r) { r = { sel: sels[q], t: [] }; runs.set(key, r); }
  r.t.push({ q, a, col: cr[2 * q] | 0, row: cr[2 * q + 1] | 0, mir: qMir[q] & 1, sd: [qSD[4*q], qSD[4*q+1], qSD[4*q+2], qSD[4*q+3]], texU, texV, palSel: (tcw>>21)&0x3f });
}
for (const r of [...runs.values()].sort((a,b)=>a.sel-b.sel)) {
  if (SEL !== null && r.sel !== SEL) continue;
  const gfx1 = gfx1Storm >>> 0; const nOff = u32r(ram, gfx1) >> 2; if (r.sel >= nOff) continue;
  const poff = u32r(ram, gfx1 + r.sel * 4); const pb = G(gfx1) + poff;
  const W = ram[pb + 2] * 8, H = ram[pb + 3] * 8;
  const dmaBase = Math.min(...r.t.map(t => t.a));
  const addrs = [...new Set(r.t.map(t=>t.a))].sort((a,b)=>a-b);
  const span = addrs[addrs.length-1] + 512 - dmaBase;
  console.log(`\nsel0x${r.sel.toString(16)} W=${W} H=${H} (${(W/32)|0}x${(H/32)|0} tiles) part=${W*H/2}B  tiles=${r.t.length} distinctAddr=${addrs.length} dmaBase=0x${dmaBase.toString(16)} span=${span}B partBytes=${W*H/2}`);
  const seen=new Set();
  for (const t of r.t.sort((a,b)=>a.a-b.a)) {
    const kk=t.a; if(seen.has(kk))continue; seen.add(kk);
    const off=t.a-dmaBase; const k=off/512;
    console.log(`  col=${t.col} row=${t.row} mir=${t.mir} texUV=${t.texU},${t.texV} pal=${t.palSel} srcdesc=[m${t.sd[0]},cx${t.sd[1]},ry${t.sd[2]},fl${t.sd[3]}] addr=0x${t.a.toString(16)} off=${off}(=${k}*512)`);
  }
}
M._free(rp); M._free(op); M._free(sp); M._free(gp); M._free(crP); M._free(miP); M._free(sdP);
