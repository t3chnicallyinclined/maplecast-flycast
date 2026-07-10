// _storm_native_gate.mjs — DEFINITIVE native-part engine-VRAM gate.
//
// For native parts (W>=64 && H>=64, m=32, tiles at contiguous dmaBase+k*512), the engine's
// verbatim-DMA VRAM at off k == decodeA(part)[k*512..] (CONFIRMED: re_kb faithful decode +
// texel_gate CERTIFY; DMA driver loc_8c033d78 copies raw verbatim). render_frame's TCW addrs
// are byte-exact vs engine. So:
//   engineTile(off k) = raw[k*512 .. +512]           (== bodytex=wire VRAM, ground truth)
//   currentTile       = production ensureBodyTextures output at addr   (?bodytex=local now)
//   fixedTile         = raw[ twTileYFirst(cc, pRows-ry)*512 ]  with cc = srcdesc cx (NO 0x4000 reversal)
// Also cross-validates the DMA-verbatim model vs the REAL 8MB seed VRAM at frames where the
// part is resident (seed nonzero at addr) — proving engineTile == real engine VRAM.
//
// Usage: node _storm_native_gate.mjs [cap=torture.mcrr] [cid=42] [f0] [f1] [step]
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
import { ensureBodyTextures, decodeA } from '../../web/render-replica/body_decoder.mjs';

const CAP = process.argv[2] || 'torture.mcrr';
const FILTER = process.argv[3] !== undefined ? parseInt(process.argv[3], 10) : 42;
const F0 = process.argv[4] !== undefined ? parseInt(process.argv[4], 10) : 0;
const F1 = process.argv[5] !== undefined ? parseInt(process.argv[5], 10) : 1e9;
const STEP = process.argv[6] !== undefined ? parseInt(process.argv[6], 10) : 1;
const GFX = fileURLToPath(new URL('../../web/render-replica/gfx/', import.meta.url));
const buf = new Uint8Array(readFileSync(fileURLToPath(new URL(CAP, import.meta.url))));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); u32(); const nS = u32(), nD = u32(), nF = u32(), vB = u32(), pB = u32(); u32();
const reg = () => { const a = u32(), l = u32(); let t = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) t += String.fromCharCode(c); } p += 8; return { addr: a >>> 0, len: l, tag: t }; };
const sR = Array.from({ length: nS }, reg), dR = Array.from({ length: nD }, reg);
const vramSeedOff = p; p += vB; p += pB;
const sD = sR.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const fStart = p; const G = a => (a >>> 0) & 0xFFFFFF;
const baseRam = new Uint8Array(16 * 1024 * 1024);
sR.forEach((r, i) => { if (r.tag === 'ram16') baseRam.set(sD[i], 0); else baseRam.set(sD[i], G(r.addr)); });
const seedVram = buf.subarray(vramSeedOff, vramSeedOff + vB);
p = fStart; const frames = [];
for (let f = 0; f < nF; f++) { u32(); const vf = u32(); u32(); const dynOff = p; for (const r of dR) p += r.len; const gfxOff = p; const nG = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0; if (nG <= 64) { p += 4; for (let g = 0; g < nG && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } } frames.push({ vframe: vf, dynOff, gfxOff }); }
function aGfx(ram, off) { if (off + 4 > buf.length) return; const nG = dv.getUint32(off, true); off += 4; if (nG > 64) return; for (let i = 0; i < nG; i++) { if (off + 8 > buf.length) break; const base = dv.getUint32(off, true); off += 4; const len = dv.getUint32(off, true); off += 4; if (len > 0x800000 || off + len > buf.length) break; ram.set(buf.subarray(off, off + len), G(base)); off += len; } }
const SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
const u8r = (r, a) => r[G(a)], u32r = (r, a) => (r[G(a)] | (r[G(a) + 1] << 8) | (r[G(a) + 2] << 16) | (r[G(a) + 3] << 24)) >>> 0;
function twTileYFirst(col, row, Tw, Th) { let rv = 0, sh = 0, xs = Tw >> 1, ys = Th >> 1, x = col, y = row; while (xs || ys) { if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; } if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; } } return rv; }
const M = await createRenderFrame({ locateFile: x => x });

const rep = new Map();
let framesSeen = 0, seedValidated = 0, seedContra = 0;
const _f16dv = new DataView(new ArrayBuffer(4));
const _f16 = (bits) => { _f16dv.setUint32(0, (bits << 16) >>> 0, true); return _f16dv.getFloat32(0, true); };

for (let fi = F0; fi < nF && fi <= F1; fi += STEP) {
  const fr = frames[fi]; const ram = baseRam.slice();
  { let o = fr.dynOff; for (const r of dR) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }
  aGfx(ram, fr.gfxOff);
  const cids = new Set();
  for (const b of SLOTS) { if (u8r(ram, b) === 0) continue; const cid = u8r(ram, b + 1); const g1b = u32r(ram, b + 0x15C); if (!((g1b & 0x0C000000) || (g1b & 0x8C000000))) continue; if (cids.has(cid)) continue; cids.add(cid); const hex = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0'); let g1, g2; try { g1 = new Uint8Array(readFileSync(GFX + hex + '_gfx1.bin')); g2 = new Uint8Array(readFileSync(GFX + hex + '_gfx2.bin')); } catch { continue; } const g1d = G(g1b), g2d = G(u32r(ram, b + 0x160)); if (g1d + g1.length <= ram.length) ram.set(g1, g1d); if (g2d + g2.length <= ram.length) ram.set(g2, g2d); }
  if (FILTER >= 0 && !cids.has(FILTER)) continue;
  framesSeen++;
  const gfxInfo = new Map();
  for (const b of SLOTS) { if (u8r(ram, b) === 0) continue; const g1b = u32r(ram, b + 0x15C) >>> 0; gfxInfo.set(g1b & 0xFFFFFF, { cid: u8r(ram, b + 1), sid: (ram[G(b + 0x144)] | (ram[G(b + 0x144) + 1] << 8)), facing: u8r(ram, b + 0x110) ? 1 : 0 }); }

  const rp = M._malloc(ram.length); M.HEAPU8.set(ram, rp); const op = M._malloc(262144);
  const len = M._render_frame_ta(rp, op, 262144); const quads = M._render_frame_quad_count();
  const ta = M.HEAPU8.slice(op, op + len); const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
  const sp = M._malloc(quads * 2), gp = M._malloc(quads * 4); M._render_frame_quad_sels(sp, quads); M._render_frame_quad_gfx1s(gp, quads);
  const sels = new Uint16Array(M.HEAPU8.buffer.slice(sp, sp + quads * 2)), gfxs = new Uint32Array(M.HEAPU8.buffer.slice(gp, gp + quads * 4));
  const crP = M._malloc(quads * 8); M._render_frame_quad_colrow(crP, quads); const cr = new Int32Array(M.HEAPU8.buffer.slice(crP, crP + quads * 8));
  const miP = M._malloc(quads); M._render_frame_quad_mirror(miP, quads); const qMir = new Uint8Array(M.HEAPU8.buffer.slice(miP, miP + quads));
  const sdP = M._malloc(quads * 4); M._render_frame_quad_srcdesc(sdP, quads); const qSD = new Uint8Array(M.HEAPU8.buffer.slice(sdP, sdP + quads * 4));

  const vram = new Uint8Array(8 * 1024 * 1024);
  ensureBodyTextures(ram, vram, ta, quads, {}, sels, gfxs, cr, null, qMir, qSD);

  const runs = new Map();
  for (let q = 0; q < quads; q++) {
    const g = gfxs[q] >>> 0; if (!(g & 0x0C000000) && !(g & 0x8C000000)) continue;
    if ((g & 0x0FFFFFFF) >= 0x0CED0000 && (g & 0x0FFFFFFF) < 0x0CEE0000) continue;
    if (qSD && !(qSD[4 * q + 3] & 1)) continue;
    const info = gfxInfo.get(g & 0xFFFFFF); if (!info || (FILTER >= 0 && info.cid !== FILTER)) continue;
    const tcw = tdv.getUint32(q * 96 + 0x0C, true); const a = ((tcw & 0x1FFFFF) << 3) >>> 0;
    const tsp = tdv.getUint32(q * 96 + 8, true);
    const u1 = Math.max(_f16(tdv.getUint16(q*96+86,true)), _f16(tdv.getUint16(q*96+90,true)), _f16(tdv.getUint16(q*96+94,true)));
    const key = (g & 0xFFFFFF).toString(16) + ':' + sels[q];
    let r = runs.get(key); if (!r) { r = { g: g >>> 0, sel: sels[q], info, t: [] }; runs.set(key, r); }
    r.t.push({ a, col: cr[2 * q] | 0, row: cr[2 * q + 1] | 0, mir: qMir[q] & 1, sd: [qSD[4*q],qSD[4*q+1],qSD[4*q+2],qSD[4*q+3]], tsp, u1 });
  }

  for (const r of runs.values()) {
    if (r.t.length < 2) continue;
    const gfx1 = r.g >>> 0; const nOff = u32r(ram, gfx1) >> 2; if (r.sel >= nOff) continue;
    const poff = u32r(ram, gfx1 + r.sel * 4); const pb = G(gfx1) + poff;
    const W = ram[pb + 2] * 8, H = ram[pb + 3] * 8; if (!(W >= 64 && H >= 64 && W <= 1024 && H <= 1024)) continue; // NATIVE only
    const destLen = (W * H) >> 1;
    const srt = []; { const s = new Set(); for (let i = 0; i < nOff; i++) s.add(u32r(ram, gfx1 + i * 4)); for (const v of s) srt.push(v); srt.sort((a, b) => a - b); }
    let end = pb; for (const o of srt) { if ((G(gfx1) + o) > pb) { end = G(gfx1) + o; break; } } if (end <= pb) end = pb + 0x8000;
    const raw = decodeA(ram, pb + 4, end, destLen);
    const dmaBase = Math.min(...r.t.map(t => t.a));
    const Tw = (W / 32) | 0, Th = (H / 32) | 0, pCols = Tw, pRows = Th;
    // require contiguous native tiling: distinct offs 0..Tw*Th-1
    const offs = [...new Set(r.t.map(t => (t.a - dmaBase)/512))].sort((a,b)=>a-b);
    const contiguous = offs.length === Tw*Th && offs[0]===0 && offs[offs.length-1]===Tw*Th-1;
    if (!contiguous) continue; // skip multi-instance / non-native

    const k2 = `sid0x${r.info.sid.toString(16)} fac${r.info.facing} sel0x${r.sel.toString(16)} ${W}x${H}`;
    let e = rep.get(k2); if (!e) { e = { frames: new Set(), tiles:0, curEx:0, curWr:0, fixEx:0, fixWr:0, hasFlip:false, seedEx:0, seedWr:0, seedZ:0 }; rep.set(k2, e); }
    e.frames.add(fi);
    for (const t of r.t) {
      const off = (t.a - dmaBase); const k = off/512;
      if (off + 512 > raw.length) continue;
      const engine = raw.subarray(off, off + 512);
      const cur = vram.subarray(t.a, t.a + 512);
      // FIXED: no dfl&2 reversal; cc = srcdesc cx, storage col = cc directly
      const dm=t.sd[0], dcx=t.sd[1], dry=t.sd[2], dfl=t.sd[3];
      if (dfl & 2) e.hasFlip = true;
      const usz = 8 << ((t.tsp >> 3) & 7); let mq = Math.round(t.u1 * usz); if (mq<1)mq=1; if (mq>32)mq=32;
      const fCols = Math.max(1,(W/mq)|0), fRows = Math.max(1,(H/mq)|0);
      let fcol, frow;
      if ((dfl & 1) && dm === mq) { fcol = dcx % fCols; let rr = fRows - dry; if(rr<0)rr=0; if(rr>=fRows)rr=fRows-1; frow=rr; }
      else { fcol = t.col; frow = t.row; }
      const fk = twTileYFirst(fcol, frow, Tw, Th);
      const fixed = (fk*512+512<=raw.length) ? raw.subarray(fk*512, fk*512+512) : null;
      e.tiles++;
      let cbad=0; for(let i=0;i<512;i++) if(cur[i]!==engine[i]){cbad++;} if(cbad===0)e.curEx++; else e.curWr++;
      if (fixed){ let fbad=0; for(let i=0;i<512;i++) if(fixed[i]!==engine[i]){fbad++;} if(fbad===0)e.fixEx++; else e.fixWr++; }
      // seed-VRAM cross validation (real engine VRAM where resident)
      if (t.a+512 <= seedVram.length) {
        let sz=true, seq=true; for(let i=0;i<512;i++){ const sv=seedVram[t.a+i]; if(sv)sz=false; if(sv!==engine[i])seq=false; }
        if (sz) e.seedZ++; else if (seq){ e.seedEx++; seedValidated++; } else { e.seedWr++; seedContra++; }
      }
    }
  }
  M._free(rp); M._free(op); M._free(sp); M._free(gp); M._free(crP); M._free(miP); M._free(sdP);
}

console.log(`\n=== NATIVE engine-VRAM gate  cid=${FILTER} cap=${CAP} f[${F0}..${F1}] step=${STEP} framesSeen=${framesSeen} ===`);
console.log(`seed-VRAM model validation: raw==seedVRAM ${seedValidated} tiles, CONTRADICTIONS ${seedContra}\n`);
console.log(`col1=CURRENT(prod) col2=FIXED(no 0x4000 reversal) vs engine-VRAM ground truth:\n`);
const rows = [...rep.entries()].sort((a,b)=> b[1].curWr - a[1].curWr);
for (const [k,e] of rows) {
  const curFlag = e.curWr>0 ? 'CUR-BAD ' : 'cur-ok  ';
  const fixFlag = e.fixWr>0 ? 'FIX-BAD' : 'fix-ok ';
  console.log(`${curFlag}${fixFlag} ${k.padEnd(40)} flip4000=${e.hasFlip?'Y':'n'} tiles=${e.tiles} | CUR[EX=${e.curEx} WR=${e.curWr}] FIX[EX=${e.fixEx} WR=${e.fixWr}] | seed[EX=${e.seedEx} WR=${e.seedWr} Z=${e.seedZ}]`);
}
