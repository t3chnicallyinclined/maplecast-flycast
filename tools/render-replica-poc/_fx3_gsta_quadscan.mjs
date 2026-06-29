// _fx3_gsta_quadscan.mjs — run render_frame (the GSTA client's EXACT body path) on each
// frame of an MCRR capture and report, per frame: total quads, body count, sat count, and
// the gfx1 histogram (how many quads share each gfx1 base). The "~6 tiled Storm bodies"
// garble would show as a gfx1 with ~6x the normal per-part quad count, or a runaway quad total.
//
//   node _fx3_gsta_quadscan.mjs <file.mcrr> [stepFrames]
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';

const CAP = process.argv[2];
const STEP = parseInt(process.argv[3] || '1', 10);
const buf = new Uint8Array(readFileSync(CAP));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
u32(); const nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr: addr >>> 0, len, tag }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
p += vramBytes + pvrBytes;
const G = a => (a >>> 0) & 0xFFFFFF;
const baseRam = new Uint8Array(16 * 1024 * 1024);
const sd = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
staticRegs.forEach((r, i) => { if (r.tag === 'ram16') baseRam.set(sd[i], 0); else baseRam.set(sd[i], G(r.addr)); });
const frames = [];
for (let f = 0; f < nFrames; f++) {
  if (u32() !== 0x784D5246) throw new Error(`frame ${f}: bad FRMx`);
  const vframe = u32(); const taSize = u32(); const dynOff = p;
  for (const r of dynamicRegs) p += r.len;
  const nGfx = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0;
  if (nGfx <= 64) { p += 4; for (let g = 0; g < nGfx && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } }
  // palette tail
  if (p + 4 <= buf.length) { const palLen = dv.getUint32(p, true); p += 4; if (palLen && p + palLen <= buf.length) p += palLen; }
  // HUDQ tail
  if (p + 8 <= buf.length && dv.getUint32(p, true) === 0x48554451) { p += 4; const nHud = dv.getUint32(p, true); p += 4; p += nHud * 96; }
  p += taSize; frames.push({ vframe, dynOff });
}
function applyFrame(fr) { const ram = baseRam.slice(); let o = fr.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } return ram; }

const M = await createRenderFrame({ locateFile: x => x });
const cap = 1024 * 1024, outPtr = M._malloc(cap);
let maxQuads = 0, maxQuadsVf = 0;
console.log(`CAP=${CAP} frames=${nFrames} step=${STEP}`);
for (let fi = 0; fi < nFrames; fi += STEP) {
  const fr = frames[fi];
  const ram = applyFrame(fr);
  const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
  M._render_frame_ta(ramPtr, outPtr, cap);
  const quads = M._render_frame_quad_count();
  const bodies = M._render_frame_body_count();
  const sats = M._render_frame_sat_count();
  const gfxPtr = M._malloc(quads * 4); M._render_frame_quad_gfx1s(gfxPtr, quads);
  const gdv = new DataView(M.HEAPU8.buffer, gfxPtr, quads * 4);
  const hist = new Map();
  for (let i = 0; i < quads; i++) { const g = gdv.getUint32(i * 4, true) >>> 0; hist.set(g, (hist.get(g) || 0) + 1); }
  if (quads > maxQuads) { maxQuads = quads; maxQuadsVf = fr.vframe; }
  // print frames with unusually high quad count or many distinct gfx bases
  if (quads > 120 || hist.size > 6) {
    const top = [...hist.entries()].sort((a, b) => b[1] - a[1]).slice(0, 8)
      .map(([g, c]) => `${g.toString(16)}:${c}`).join(' ');
    console.log(`vf=${fr.vframe} fi=${fi} quads=${quads} bodies=${bodies} sats=${sats} distinctGfx=${hist.size}  top=[${top}]`);
  }
  M._free(ramPtr); M._free(gfxPtr);
}
console.log(`MAX quads=${maxQuads} at vframe=${maxQuadsVf}`);
