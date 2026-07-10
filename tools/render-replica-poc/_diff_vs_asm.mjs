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
  // capture_break.mjs stamps offset-8 with the post-dyn tail length (the on-change
  // GFX tail; the live replica FRMx carries no engine TA — render_frame regenerates
  // it from RAM). So a frame = header(12) + dyn + tail, and we skip the tail opaquely.
  const vframe = u32(); const tailLen = u32();
  const dynOff = p; for (const r of dynamicRegs) p += r.len;
  p += tailLen;
  frames.push({ vframe, dynOff });
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

// ---- parse ASMTRACE frame (cols: frame sid slot cid sel dx dy accX accY screenX screenY pal row flip flags r11 r13 node) ----
const asmLines = readFileSync(asmPath, 'utf8').trim().split('\n').filter(l => l && !l.startsWith('#'));
const asm = asmLines.map(l => { const t = l.split(/\s+/); return { cid:+t[3], sel:+t[4], sx:+t[9], sy:+t[10], flip:+t[13], flags:parseInt(t[14],16)||0, node:t[17] }; });

// node -> its gfx1 straight from the seeded RAM (node+0x15C), so no stale hardcoded map.
const r32 = a => { const i=(a>>>0)&0xFFFFFF; return (ram[i]|ram[i+1]<<8|ram[i+2]<<16|ram[i+3]<<24)>>>0; };

function diffForNode(nodeHex) {
  const nodeGuest = parseInt(nodeHex, 16);
  const gfx1 = r32(nodeGuest + 0x15C) >>> 0;
  const a = asm.filter(x => x.node === nodeHex);
  const r = rfQuads.filter(x => x.gfx1 === gfx1);
  const n = Math.min(a.length, r.length);
  const totalY = a.filter(x => x.flags & 0x20).length, totalX = a.filter(x => x.flags & 0x10).length;
  let maxDX = 0, maxDY = 0, selMiss = 0;
  for (let i = 0; i < n; i++) {
    maxDX = Math.max(maxDX, Math.abs(r[i].bx - a[i].sx));
    maxDY = Math.max(maxDY, Math.abs(r[i].by - a[i].sy));
    if (r[i].sel !== a[i].sel) selMiss++;
  }
  const cov = (a.length === r.length && n === a.length) ? 'COVER-OK' : `COVER-MISMATCH(asm=${a.length} rf=${r.length})`;
  console.log(`\nNODE ${nodeHex} cid=${a[0]?.cid} gfx1=0x${gfx1.toString(16)}: ${cov} matched=${n}  maxDX=${maxDX.toFixed(1)} maxDY=${maxDY.toFixed(1)} selMiss=${selMiss}  asm-flags Ymir(0x20)=${totalY} Xmir(0x10)=${totalX}`);
  for (let i = 0; i < n; i++) if (a[i].flags) {
    const dX = Math.abs(r[i].bx - a[i].sx), dY = Math.abs(r[i].by - a[i].sy);
    console.log(`   part${String(i).padStart(2)} sel(rf=${r[i].sel}/asm=${a[i].sel}) FLAGS=0x${a[i].flags.toString(16)}${a[i].flags&0x20?' [Y-MIRROR: render_frame drops]':''}${a[i].flags&0x10?' [X-mirror]':''}  pos rf(${r[i].bx.toFixed(0)},${r[i].by.toFixed(0)}) asm(${a[i].sx},${a[i].sy}) dX=${dX.toFixed(1)} dY=${dY.toFixed(1)}`);
  }
}
console.log(`vframe=${fr.vframe} render_frame quads=${quads} asmParts=${asm.length}`);
const nodes = [...new Set(asm.map(x => x.node))];
console.log(`nodes in frame: ${nodes.length}`);
for (const node of nodes) diffForNode(node);
