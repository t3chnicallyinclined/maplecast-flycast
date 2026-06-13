// _diff_sat.mjs — VALIDATE the cat 1..4 SATELLITE render path (loc_8c030af8 transpile)
// against the ASMTRACE ground truth (NOT a model). For a synced capture frame whose
// vframe matches an ASMTRACE frame, render_frame must now EMIT the satellite's parts and
// their per-part bottom-anchor screenX/screenY must match the ASMTRACE (cols 10/11) <1px.
//
//   node _diff_sat.mjs <file.mcrr> <asm_frame.txt> <poolNodeHex> [vframe]
//
// If [vframe] omitted, auto-pick the capture frame whose slot table contains <poolNodeHex>
// as an ACTIVE cat 1..4 node, that also exists in the ASMTRACE.
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';

const path = process.argv[2], asmPath = process.argv[3];
const poolNode = (process.argv[4] || '').toLowerCase();
const wantV = process.argv[5] ? +process.argv[5] : null;
const nodeAddr = parseInt(poolNode, 16) >>> 0;

// ---- parse MCRR ----
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
u32(); const nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr: addr >>> 0, len, tag }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
p += vramBytes + pvrBytes;
const staticData = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const frameStart = p;
const G = a => (a >>> 0) & 0xFFFFFF;
const baseRam = new Uint8Array(16 * 1024 * 1024);
staticRegs.forEach((r, i) => { if (r.tag === 'ram16') baseRam.set(staticData[i], 0); else baseRam.set(staticData[i], G(r.addr)); });

// index the frames
p = frameStart; const frames = [];
for (let f = 0; f < nFrames; f++) {
  if (u32() !== 0x784D5246) throw new Error(`frame ${f}: bad FRMx`);
  const vframe = u32(); const taSize = u32();
  const dynOff = p; for (const r of dynamicRegs) p += r.len;
  const nGfx = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0;
  if (nGfx <= 64) { p += 4; for (let g = 0; g < nGfx && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } }
  p += taSize;
  frames.push({ vframe, dynOff });
}
function applyFrame(fr){ const ram = baseRam.slice(); let o = fr.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } return ram; }
function satActive(ram){ const cnt = G(0x8C2895E0), ptr = G(0x8C287DE0);
  for (let L = 0; L < 16; L++){ const c = ram[cnt+L]; if(c===0||c>0x60) continue; const pb = ptr+L*0x180;
    for (let i=0;i<c;i++){ const node=(ram[pb+i*4]|(ram[pb+i*4+1]<<8)|(ram[pb+i*4+2]<<16)|(ram[pb+i*4+3]<<24))>>>0;
      if(G(node)===G(nodeAddr)){ const cat=ram[G(node)+3]; if(cat>=1&&cat<5) return true; } } }
  return false; }

// ---- ASMTRACE frames available ----
const asmAll = readFileSync(asmPath, 'utf8').trim().split('\n').filter(l => l && !l.startsWith('#')).map(l => l.split(/\s+/));
const asmFrames = new Set(asmAll.filter(t => (t[17]||'').toLowerCase() === poolNode).map(t => +t[0]));

// ---- pick target frame ----
let target = null;
if (wantV != null) target = frames.find(f => f.vframe === wantV);
else {
  for (const f of frames) { if (!asmFrames.has(f.vframe)) continue; const ram = applyFrame(f); if (satActive(ram)) { target = f; break; } }
  if (!target) { // fall back: any capture frame with the node active (even if asm frame differs)
    for (const f of frames) { const ram = applyFrame(f); if (satActive(ram)) { target = f; break; } }
  }
}
if (!target) { console.error(`No capture frame with active satellite ${poolNode} found (and none synced to ASMTRACE).`); process.exit(2); }
const ram = applyFrame(target);
const f32 = a => { const b = ram.subarray(G(a), G(a) + 4); return new DataView(b.buffer, b.byteOffset, 4).getFloat32(0, true); };
console.log(`TARGET vframe ${target.vframe} (asm has node @this frame: ${asmFrames.has(target.vframe)})`);
console.log(`pool node 0x${poolNode}: +0xE0 anchorX=${f32(nodeAddr+0xE0).toFixed(3)} +0xE4 anchorY=${f32(nodeAddr+0xE4).toFixed(3)} +0xEC sx=${f32(nodeAddr+0xEC).toFixed(5)} +0xF0 sy=${f32(nodeAddr+0xF0).toFixed(5)} +0x3 cat=${ram[G(nodeAddr)+3]} +0x12c=${ram[G(nodeAddr)+0x12c]}`);

// ---- render_frame ----
const M = await createRenderFrame({ locateFile: x => x });
const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
const cap = 256 * 1024, outPtr = M._malloc(cap);
const len = M._render_frame_ta(ramPtr, outPtr, cap);
const bodies = M._render_frame_body_count(), sats = M._render_frame_sat_count(), quads = M._render_frame_quad_count();
console.log(`render_frame: bodies=${bodies} satellites=${sats} quads=${quads}`);
const ta = M.HEAPU8.slice(outPtr, outPtr + len);
const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
const rf = [];
for (let i = 0; i < quads; i++) { const o = i * 96; rf.push({ bx: tdv.getFloat32(o + 36, true), cy: tdv.getFloat32(o + 64, true) }); } // bottom-left = (Ax, Cy)

// ---- ASMTRACE parts for this node at the target frame (or nearest available) ----
let asmFrameUsed = target.vframe;
let asm = asmAll.filter(t => (t[17]||'').toLowerCase() === poolNode && +t[0] === target.vframe).map(t => ({ sx: +t[9], sy: +t[10], sel: +t[4] }));
if (!asm.length && asmFrames.size) { // pick the asm frame closest to target vframe
  asmFrameUsed = [...asmFrames].reduce((a, b) => Math.abs(b - target.vframe) < Math.abs(a - target.vframe) ? b : a);
  asm = asmAll.filter(t => (t[17]||'').toLowerCase() === poolNode && +t[0] === asmFrameUsed).map(t => ({ sx: +t[9], sy: +t[10], sel: +t[4] }));
  console.log(`(no exact-vframe asm; using nearest asm frame ${asmFrameUsed})`);
}
console.log(`ASMTRACE node parts=${asm.length} (frame ${asmFrameUsed})`);
let maxDX = 0, maxDY = 0, matched = 0;
for (const a of asm) {
  let best = Infinity, bdx = 0, bdy = 0;
  for (const q of rf) { const dx = Math.abs(q.bx - a.sx), dy = Math.abs(q.cy - a.sy); const d = dx + dy; if (d < best) { best = d; bdx = dx; bdy = dy; } }
  if (best < 4) { matched++; maxDX = Math.max(maxDX, bdx); maxDY = Math.max(maxDY, bdy); }
}
console.log(`matched ${matched}/${asm.length} satellite parts to a render_frame anchor (<4px window)`);
console.log(`SATELLITE position diff vs ASMTRACE: maxDX=${maxDX.toFixed(3)}px maxDY=${maxDY.toFixed(3)}px`);
const exactFrame = asmFrames.has(target.vframe);
console.log((exactFrame && matched >= asm.length * 0.9 && maxDX < 1 && maxDY < 1) ? 'RESULT: PASS (<1px, exact-vframe)'
  : (matched >= asm.length * 0.9 && maxDX < 1 && maxDY < 1) ? 'RESULT: PASS-geometry (<1px, but asm frame != capture vframe — informational)'
  : 'RESULT: REVIEW');
