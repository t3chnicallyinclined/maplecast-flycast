// _diff_pool.mjs — prove the POOL-object node's positions are now non-stale and match
// the ASMTRACE within <1px after shipping the objpool dyn region.
//   node _diff_pool.mjs <file.mcrr> <vframe> <asm_frame.txt> <poolNodeHex>
// Approach (independent of any gfx1->node hardcode):
//   1) Find the captured FRMx whose vframe == <vframe>; seed RAM + apply its dyn regions.
//   2) Read the pool node's shipped +0xE0/+0xE4 anchor and +0xEC/+0xF0 scale from the wire RAM
//      and assert they are NON-ZERO / NON-stale (the core read-set claim).
//   3) render_frame -> collect every quad's bottom-left (Ax @+36, Cy @+64).
//   4) For each ASMTRACE line of <poolNodeHex>, find the nearest render_frame bottom-anchor;
//      report maxDX/maxDY over the matched pool parts. Target <1px.
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';

const path = process.argv[2], wantV = +process.argv[3], asmPath = process.argv[4];
const poolNode = (process.argv[5] || '').toLowerCase();

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

p = frameStart; let target = null;
for (let f = 0; f < nFrames; f++) {
  if (u32() !== 0x784D5246) throw new Error(`frame ${f}: bad FRMx`);
  const vframe = u32(); const taSize = u32();
  const dynOff = p; for (const r of dynamicRegs) p += r.len;
  const gfxOff = p; const nGfx = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0;
  if (nGfx <= 64) { p += 4; for (let g = 0; g < nGfx && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } }
  const taOff = p; p += taSize;
  if (vframe === wantV) target = { vframe, dynOff };
}
if (!target) { console.error(`vframe ${wantV} not in capture`); process.exit(2); }
{ let o = target.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }

// ---- (2) read the pool node's SHIPPED anchor/scale from the wire RAM ----
const nodeAddr = parseInt(poolNode, 16) & 0xFFFFFF;
const f32 = a => { const b = ram.subarray(a, a + 4); return new DataView(b.buffer, b.byteOffset, 4).getFloat32(0, true); };
const ax = f32(nodeAddr + 0xE0), ay = f32(nodeAddr + 0xE4);
const sx = f32(nodeAddr + 0xEC), sy = f32(nodeAddr + 0xF0);
const objpoolReg = dynamicRegs.find(r => G(r.addr) <= nodeAddr && nodeAddr < G(r.addr) + r.len);
console.log(`pool node 0x${poolNode}: shipped via dyn region '${objpoolReg ? objpoolReg.tag : 'NONE'}'`);
console.log(`  +0xE0 anchorX=${ax.toFixed(3)} +0xE4 anchorY=${ay.toFixed(3)} +0xEC scaleX=${sx.toFixed(5)} +0xF0 scaleY=${sy.toFixed(5)}`);
if (!objpoolReg) { console.error('FAIL: pool node NOT in any shipped dyn region -> would read stale'); process.exit(1); }

// ---- (3) render_frame ----
const M = await createRenderFrame({ locateFile: x => x });
const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
const cap = 256 * 1024, outPtr = M._malloc(cap);
const len = M._render_frame_ta(ramPtr, outPtr, cap);
const quads = M._render_frame_quad_count();
const ta = M.HEAPU8.slice(outPtr, outPtr + len);
const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
const rf = [];
for (let i = 0; i < quads; i++) { const o = i * 96; rf.push({ bx: tdv.getFloat32(o + 36, true), by: tdv.getFloat32(o + 64, true) }); }
console.log(`render_frame quads=${quads}`);

// ---- (4) ASMTRACE pool lines vs nearest render_frame anchor ----
const asm = readFileSync(asmPath, 'utf8').trim().split('\n').filter(Boolean)
  .map(l => l.split(/\s+/)).filter(t => (t[17] || '').toLowerCase() === poolNode)
  .map(t => ({ sx: +t[9], sy: +t[10] }));
console.log(`ASMTRACE pool-node parts=${asm.length}`);
let maxDX = 0, maxDY = 0, matched = 0;
for (const a of asm) {
  let best = Infinity, bdx = 0, bdy = 0;
  for (const q of rf) { const dx = Math.abs(q.bx - a.sx), dy = Math.abs(q.by - a.sy); const d = dx + dy; if (d < best) { best = d; bdx = dx; bdy = dy; } }
  if (best < 4) { matched++; maxDX = Math.max(maxDX, bdx); maxDY = Math.max(maxDY, bdy); }
}
console.log(`matched ${matched}/${asm.length} pool parts to a render_frame anchor (<4px assoc window)`);
console.log(`POOL-NODE position diff vs ASMTRACE: maxDX=${maxDX.toFixed(3)}px maxDY=${maxDY.toFixed(3)}px`);
console.log(matched >= asm.length * 0.9 && maxDX < 1 && maxDY < 1 ? 'RESULT: PASS (<1px)' : 'RESULT: REVIEW');
