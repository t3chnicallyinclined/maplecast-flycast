// _diff_vs_asm.mjs — per-part numeric diff: render_frame vs ASMTRACE ground truth.
//   node _diff_vs_asm.mjs <file.mcrr> <frameIndex> <asm_frame.txt>
// render_frame quad: corner A (top-left, +36/+40), corner C (bottom-right, +60/+64).
//   bottomLeft = (Ax, Cy). ASMTRACE screenX/Y = engine bottom anchor (r15+0x30/0x34).
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';

const path = process.argv[2], wantF = +process.argv[3], asmPath = process.argv[4];

// ---- parse MCRR + apply frame (verbatim subset of repro_scramble) ----
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
u32(); const nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr: addr >>> 0, len, tag }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
const vramOff = p; p += vramBytes; const pvrOff = p; p += pvrBytes;
const staticData = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const frameStart = p;
const G = a => (a >>> 0) & 0xFFFFFF;
const ram = new Uint8Array(16 * 1024 * 1024);
staticRegs.forEach((r, i) => { if (r.tag === 'ram16') ram.set(staticData[i], 0); else ram.set(staticData[i], G(r.addr)); });
p = frameStart; const frames = [];
for (let f = 0; f < nFrames; f++) {
  if (u32() !== 0x784D5246) throw new Error(`frame ${f}: bad FRMx`);
  const vframe = u32(); const taSize = u32();
  const dynOff = p; for (const r of dynamicRegs) p += r.len;
  const gfxOff = p; const nGfx = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0;
  if (nGfx <= 64) { p += 4; for (let g = 0; g < nGfx && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } }
  const taOff = p; p += taSize;
  frames.push({ vframe, taSize, dynOff, gfxOff, taOff });
}
const fr = frames[wantF];
{ let o = fr.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }

// ---- run render_frame ----
const M = await createRenderFrame({ locateFile: x => x });
const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
const cap = 256 * 1024, outPtr = M._malloc(cap);
const len = M._render_frame_ta(ramPtr, outPtr, cap);
const quads = M._render_frame_quad_count();
const ta = M.HEAPU8.slice(outPtr, outPtr + len);
const selPtr = M._malloc(quads * 2 || 2), gfxPtr = M._malloc(quads * 4 || 4);
M._render_frame_quad_sels(selPtr, quads); M._render_frame_quad_gfx1s(gfxPtr, quads);
const sels = new Uint16Array(M.HEAPU8.buffer.slice(selPtr, selPtr + quads * 2));
const gfxs = new Uint32Array(M.HEAPU8.buffer.slice(gfxPtr, gfxPtr + quads * 4));
const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);

// render_frame per-quad bottom-left anchor + owning node (by gfx1 -> char base)
const GFX1_TO_NODE = { 0xc420040: '0c268340', 0xc810040: '0c2688e4' }; // sentinel,cable from overlay log
const rfQuads = [];
for (let i = 0; i < quads; i++) {
  const o = i * 96;
  const ax = tdv.getFloat32(o + 36, true), cy = tdv.getFloat32(o + 64, true);
  rfQuads.push({ idx: i, sel: sels[i], gfx1: gfxs[i] >>> 0, bx: ax, by: cy });
}

// ---- parse ASMTRACE frame ----
const asmLines = readFileSync(asmPath, 'utf8').trim().split('\n').filter(Boolean);
// cols: frame sid slot cid sel dx dy accX accY screenX screenY pal row flip flags r11 r13 node
const asm = asmLines.map(l => { const t = l.split(/\s+/); return { sel: +t[4], sx: +t[9], sy: +t[10], node: t[17] }; });

// ---- match by node-owner then preserve emission order (both walk r13/r11 in order) ----
function diffForNode(nodeHex, gfx1) {
  const a = asm.filter(x => x.node === nodeHex);
  const r = rfQuads.filter(x => x.gfx1 === gfx1);
  const n = Math.min(a.length, r.length);
  let maxDX = 0, maxDY = 0, sumDX = 0, sumDY = 0, worst = null;
  for (let i = 0; i < n; i++) {
    const dX = Math.abs(r[i].bx - a[i].sx), dY = Math.abs(r[i].by - a[i].sy);
    sumDX += dX; sumDY += dY;
    if (dX > maxDX) maxDX = dX; if (dY > maxDY) maxDY = dY;
    if (!worst || dX + dY > worst.d) worst = { i, d: dX + dY, sel: r[i].sel, asmSel: a[i].sel, rfx: r[i].bx, asmx: a[i].sx, rfy: r[i].by, asmy: a[i].sy };
  }
  console.log(`\nNODE ${nodeHex} gfx1=0x${gfx1.toString(16)}: asmParts=${a.length} rfParts=${r.length} matched=${n}`);
  console.log(`  maxDX=${maxDX.toFixed(2)}px maxDY=${maxDY.toFixed(2)}px  meanDX=${(sumDX/n).toFixed(3)} meanDY=${(sumDY/n).toFixed(3)}`);
  if (worst) console.log(`  worst part #${worst.i}: sel(rf=${worst.sel} asm=${worst.asmSel}) X(rf=${worst.rfx.toFixed(2)} asm=${worst.asmx}) Y(rf=${worst.rfy.toFixed(2)} asm=${worst.asmy})`);
  // print first 6 part-by-part rows
  console.log('   #  sel(rf/asm)   X rf / asm        Y rf / asm');
  for (let i = 0; i < Math.min(n, 8); i++)
    console.log(`  ${String(i).padStart(2)}  ${r[i].sel}/${a[i].sel}\t  ${r[i].bx.toFixed(2)} / ${a[i].sx}\t  ${r[i].by.toFixed(2)} / ${a[i].sy}`);
}
console.log(`vframe=${fr.vframe} render_frame quads=${quads} asmParts=${asm.length}`);
for (const [g, node] of Object.entries(GFX1_TO_NODE)) diffForNode(node, +g);
