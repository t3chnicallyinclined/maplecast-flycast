// _diff_proj.mjs — VALIDATE the cat 1..4 SATELLITE (projectile) render path
// (gen_walker_root.c -> render_object_full_satellite, loc_8c030af8 transpile) against the
// ASMTRACE ground truth on prod (/dev/shm/mc_assembly.log).
//
//   node _diff_proj.mjs <file.mcrr> <asm_sync.txt> <poolNodeHex> [vframe]
//
// MCRR (livecap) per-frame layout (reverse-engineered 2026-06-14):
//   FRMx(4) vframe(4) taSize(4) dyn(sum) nGfx(4) [base(4) len(4) data]*nGfx taData(taSize) pvr(32768) pad(4)
// nGfx>0 only on the first frame (the streamed-on-demand char textures); later frames reuse them.
//
// The diff: render_frame must EMIT the satellite node's parts; their per-part BOTTOM-LEFT
// screen anchor (render_frame Ax / Cy) must match the ASMTRACE engine ground truth cols 10/11
// (screenX/screenY) to <1px. ASMTRACE cols: frame sid slot cid sel dx dy accX accY screenX
// screenY pal row flip flags r11 r13 node  (node = last col).
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';

const path = process.argv[2], asmPath = process.argv[3];
const poolNode = (process.argv[4] || '').toLowerCase();
const wantV = process.argv[5] ? +process.argv[5] : null;
const nodeAddr = parseInt(poolNode, 16) >>> 0;

const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
const ver = u32(), nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr: addr >>> 0, len, tag }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
p += vramBytes + pvrBytes;
const staticData = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const frameStart = p;
const G = a => (a >>> 0) & 0xFFFFFF;

// seed base RAM from static prefix
const baseRam = new Uint8Array(16 * 1024 * 1024);
staticRegs.forEach((r, i) => { if (r.tag === 'ram16') baseRam.set(staticData[i], 0); else baseRam.set(staticData[i], G(r.addr)); });

// walk frames with the FULL livecap layout
const dynsum = dynamicRegs.reduce((a, r) => a + r.len, 0);
p = frameStart; const frames = [];
for (let f = 0; f < nFrames; f++) {
  if (u32() !== 0x784D5246) { console.error(`frame ${f}: bad FRMx @${p - 4}`); break; }
  const vframe = u32(); const taSize = u32();
  const dynOff = p; p += dynsum;
  const gfxOff = p; const nGfx = u32(); const gfxBlocks = [];
  for (let g = 0; g < nGfx; g++) { const base = u32(); const len = u32(); gfxBlocks.push({ base, off: p, len }); p += len; }
  p += taSize;
  // Trailing per-frame blob (pvr snapshot + pad) — size varies; robustly resync to the next
  // FRMx magic rather than assuming a fixed size (a mid-stream nGfx>0 frame shifts it).
  if (f + 1 < nFrames) {
    let q = p;
    while (q + 4 <= buf.length && dv.getUint32(q, true) !== 0x784D5246) q++;
    p = q;
  }
  frames.push({ vframe, dynOff, gfxBlocks });
}
console.error(`parsed ${frames.length}/${nFrames} frames, vframes ${frames[0]?.vframe}..${frames.at(-1)?.vframe}`);

// the streamed GFX blocks live on frame 0 (resident textures for the active chars). Apply them
// to baseRam ONCE; later frames reuse them.
for (const fr of frames) for (const b of fr.gfxBlocks) baseRam.set(buf.subarray(b.off, b.off + b.len), G(b.base));

function applyFrame(fr) { const ram = baseRam.slice(); let o = fr.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } return ram; }
function satActive(ram) {
  const cnt = G(0x8C2895E0), ptr = G(0x8C287DE0);
  for (let L = 0; L < 16; L++) { const c = (ram[cnt + L] << 24) >> 24; if (c <= 0 || c > 0x60) continue; const pb = ptr + L * 0x180;
    for (let i = 0; i < c; i++) { const node = (ram[pb + i * 4] | (ram[pb + i * 4 + 1] << 8) | (ram[pb + i * 4 + 2] << 16) | (ram[pb + i * 4 + 3] << 24)) >>> 0;
      if (G(node) === G(nodeAddr)) { const cat = (ram[G(node) + 3] << 24) >> 24; if (cat >= 1 && cat < 5) return cat; } } }
  return 0;
}

// ASMTRACE frames available for this node
const asmAll = readFileSync(asmPath, 'utf8').trim().split('\n').filter(l => l && !l.startsWith('#')).map(l => l.split(/\s+/));
const asmFramesForNode = new Set(asmAll.filter(t => (t[17] || '').toLowerCase() === poolNode).map(t => +t[0]));

// pick a target frame: prefer one synced to ASMTRACE with the node active as cat 1..4
let target = null;
if (wantV != null) target = frames.find(f => f.vframe === wantV);
else { for (const f of frames) { if (!asmFramesForNode.has(f.vframe)) continue; if (satActive(applyFrame(f))) { target = f; break; } } }
if (!target) { console.error(`No synced frame with active satellite ${poolNode}`); process.exit(2); }

const ram = applyFrame(target);
const cat = satActive(ram);
const f32 = a => { const b = ram.subarray(G(a), G(a) + 4); return new DataView(b.buffer, b.byteOffset, 4).getFloat32(0, true); };
console.log(`\nTARGET vframe ${target.vframe}  pool node 0x${poolNode}  cat=${cat}`);
console.log(`  +0xE0 anchorX=${f32(nodeAddr + 0xE0).toFixed(3)}  +0xE4 anchorY=${f32(nodeAddr + 0xE4).toFixed(3)}  +0xEC sx=${f32(nodeAddr + 0xEC).toFixed(5)}  +0xF0 sy=${f32(nodeAddr + 0xF0).toFixed(5)}  +0x12c=${ram[G(nodeAddr) + 0x12c]}  +0x160 gfx2=0x${((ram[G(nodeAddr)+0x160]|(ram[G(nodeAddr)+0x161]<<8)|(ram[G(nodeAddr)+0x162]<<16)|(ram[G(nodeAddr)+0x163]<<24))>>>0).toString(16)}`);

// ---- run render_frame ----
const M = await createRenderFrame({ locateFile: x => x });
const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
const cap = 256 * 1024, outPtr = M._malloc(cap);
const len = M._render_frame_ta(ramPtr, outPtr, cap);
const bodies = M._render_frame_body_count(), sats = M._render_frame_sat_count(), quads = M._render_frame_quad_count();
console.log(`\nrender_frame: bodies=${bodies}  satellites=${sats}  quads=${quads}`);
const ta = M.HEAPU8.slice(outPtr, outPtr + len);
const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
const rf = [];
for (let i = 0; i < quads; i++) { const o = i * 96; rf.push({ bx: tdv.getFloat32(o + 36, true), cy: tdv.getFloat32(o + 64, true) }); } // bottom-left = (Ax, Cy)

// ---- ASMTRACE parts for this node at the target frame ----
const asm = asmAll.filter(t => (t[17] || '').toLowerCase() === poolNode && +t[0] === target.vframe)
                  .map(t => ({ sx: +t[9], sy: +t[10], sel: +t[4] }));
console.log(`ASMTRACE node parts=${asm.length} @ vframe ${target.vframe}`);
let maxDX = 0, maxDY = 0, matched = 0;
for (const a of asm) {
  let best = Infinity, bdx = 0, bdy = 0;
  for (const q of rf) { const dx = Math.abs(q.bx - a.sx), dy = Math.abs(q.cy - a.sy); const d = dx + dy; if (d < best) { best = d; bdx = dx; bdy = dy; } }
  if (best < 6) { matched++; maxDX = Math.max(maxDX, bdx); maxDY = Math.max(maxDY, bdy); }
}
console.log(`matched ${matched}/${asm.length} satellite parts to a render_frame anchor (<6px window)`);
console.log(`SATELLITE position diff vs ASMTRACE: maxDX=${maxDX.toFixed(3)}px  maxDY=${maxDY.toFixed(3)}px`);
console.log((matched >= Math.ceil(asm.length * 0.9) && maxDX < 1 && maxDY < 1)
  ? 'RESULT: PASS (>=90% parts matched, <1px vs ASMTRACE)'
  : (matched === 0 ? 'RESULT: FAIL — projectile NOT emitted by render_frame' : 'RESULT: REVIEW'));
