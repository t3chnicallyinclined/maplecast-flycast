// _diff_sat_walk.mjs — VALIDATE the SATELLITE PART-WALK (the "animation walked" signal)
// against the ASMTRACE ground truth, syncing by NODE IDENTITY (NOT the 0x8C3496B0 counter,
// which is frozen/not-shipped in some captures). For a capture frame where <poolNode> is an
// engine-RENDERABLE satellite (cat 1..4, +0x12C != 0, and INSIDE the slot count), this:
//   1) force-renders the WHOLE frame (render_frame slot-walk) — the node renders iff the
//      engine would render it (same +0x12C gate, same slot-count walk: NO bypass).
//   2) extracts the node's emitted quads (by matching the node's GFX1 base) and reports
//      PART COUNT, each quad's SEL (the animation/part-list signal), and bottom-anchor X/Y.
//   3) diffs part-by-part vs the ASMTRACE lines for that node (col4 sel, col9/10 screenX/Y).
//
//   node _diff_sat_walk.mjs <file.mcrr> <asm_node_lines.txt> <poolNodeHex> [vframe]
//
// The ASMTRACE file is the pre-filtered per-node slice:  awk '$18=="<node>"' mc_assembly.log
// PASS requires: part COUNT matches, every ASMTRACE sel appears in the rendered sels (set-equal),
// and matched-part position diff < 1px. A COUNT or SEL mismatch = "animation walked wrong".
//
// NOTE 2026-06-13: on _satlive.mcrr this tool reports NO RENDERABLE SATELLITE — that capture has
// zero satellites passing the engine +0x12C gate / slot count (re_kb source:satlive_mcrr_no_visible_sats).
// Use a capture taken WHILE a Sentinel drone/Plasma-Storm is on-screen and IN the slot count.
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';

const path = process.argv[2], asmPath = process.argv[3];
const poolNode = (process.argv[4] || '').toLowerCase();
const wantV = process.argv[5] ? +process.argv[5] : null;
const nodeAddr = parseInt(poolNode, 16) >>> 0;
const nodeLo = nodeAddr & 0xFFFFFF;

const buf = new Uint8Array(readFileSync(path));
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
  p += taSize; frames.push({ vframe, dynOff });
}
function applyFrame(fr) { const ram = baseRam.slice(); let o = fr.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } return ram; }
const r32R = (ram, a) => (ram[G(a)] | (ram[G(a) + 1] << 8) | (ram[G(a) + 2] << 16) | (ram[G(a) + 3] << 24)) >>> 0;

// Is <node> a RENDERABLE satellite this frame? (cat 1..4, +0x12C!=0, inside slot count) — the
// EXACT condition the engine loc_8c0308c2 + loc_8c030af8 gate on. No bypass.
function renderable(ram) {
  for (let L = 0; L < 16; L++) {
    const c = ram[G(0x8C2895E0) + L]; if (c === 0 || c > 0x60) continue;
    for (let i = 0; i < c; i++) {
      const node = r32R(ram, 0x8C287DE0 + L * 0x180 + i * 4);
      if (G(node) !== nodeLo) continue;
      const cat = ram[nodeLo + 3], gate = ram[nodeLo + 0x12c], g160 = r32R(ram, nodeLo + 0x160);
      return { ok: cat >= 1 && cat < 5 && gate !== 0 && g160 !== 0, cat, gate, g160, L, i, cnt: c };
    }
  }
  return { ok: false, reason: 'node not in any slot-count window' };
}

let target = null, diag = null;
const candidates = wantV != null ? frames.filter(f => f.vframe === wantV) : frames;
for (const f of candidates) { const ram = applyFrame(f); const r = renderable(ram); if (r.ok) { target = f; break; } if (!diag) diag = r; }
if (!target) {
  console.error(`NO RENDERABLE SATELLITE for node 0x${poolNode} in this capture.`);
  console.error(`  last diag: ${JSON.stringify(diag)}`);
  console.error(`  (a satellite renders iff cat in 1..4 AND +0x12C!=0 AND it sits at idx<count in the slot table — loc_8c030af8 gate + loc_8c0308c2 walk).`);
  process.exit(2);
}
const ram = applyFrame(target);
const node_gfx1 = r32R(ram, nodeLo + 0x15C) >>> 0;
const sid = (ram[nodeLo + 0x144] | (ram[nodeLo + 0x144 + 1] << 8)) & 0xFFFF;
console.log(`TARGET vframe ${target.vframe}  node 0x${poolNode}  gfx1=0x${node_gfx1.toString(16)}  sid=0x${sid.toString(16)}(masked ${sid & 0x7fff})`);

// ---- render_frame (faithful slot-walk; satellite renders via render_frame_satellite_hook) ----
const M = await createRenderFrame({ locateFile: x => x });
const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
const cap = 256 * 1024, outPtr = M._malloc(cap);
const len = M._render_frame_ta(ramPtr, outPtr, cap);
const quads = M._render_frame_quad_count();
const selPtr = M._malloc(quads * 2), gfxPtr = M._malloc(quads * 4);
M._render_frame_quad_sels(selPtr, quads); M._render_frame_quad_gfx1s(gfxPtr, quads);
const ta = M.HEAPU8.slice(outPtr, outPtr + len);
const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
const sels = new Uint16Array(M.HEAPU16.buffer, selPtr, quads);
const gfxs = new Uint32Array(M.HEAPU32.buffer, gfxPtr >> 2, quads);
// quads whose owning GFX1 == this satellite node's GFX1 (its emitted parts)
const rf = [];
for (let i = 0; i < quads; i++) { if (gfxs[i] !== node_gfx1) continue; const o = i * 96; rf.push({ bx: tdv.getFloat32(o + 36, true), cy: tdv.getFloat32(o + 64, true), sel: sels[i] }); }
console.log(`render_frame: total quads=${quads}, THIS satellite's parts=${rf.length}  sels=[${rf.map(q => q.sel).join(',')}]`);

// ---- ASMTRACE for this node (pre-filtered slice). Pick the frame whose part list best matches. ----
const asmAll = readFileSync(asmPath, 'utf8').trim().split('\n').filter(l => l && !l.startsWith('#')).map(l => l.split(/\s+/)).filter(t => (t[17] || '').toLowerCase() === poolNode);
const byFrame = new Map();
for (const t of asmAll) { const fr = +t[0]; if (!byFrame.has(fr)) byFrame.set(fr, []); byFrame.get(fr).push({ sel: +t[4], sx: +t[9], sy: +t[10] }); }
// best ASMTRACE frame = the one with the same part count and most overlapping sels
let bestFr = null, bestScore = -1;
for (const [fr, parts] of byFrame) { const selSet = new Set(parts.map(q => q.sel)); let ov = 0; for (const q of rf) if (selSet.has(q.sel)) ov++; const score = ov - Math.abs(parts.length - rf.length); if (score > bestScore) { bestScore = score; bestFr = fr; } }
const asm = byFrame.get(bestFr) || [];
console.log(`ASMTRACE best-match frame ${bestFr}: parts=${asm.length} sels=[${asm.map(q => q.sel).join(',')}]`);

// ---- COUNT + SEL set-equality + per-part position diff ----
const countOK = asm.length === rf.length;
const asmSels = asm.map(q => q.sel).sort((a, b) => a - b).join(',');
const rfSels = rf.map(q => q.sel).sort((a, b) => a - b).join(',');
const selOK = asmSels === rfSels;
let maxDX = 0, maxDY = 0, matched = 0;
for (const a of asm) { let best = Infinity, bdx = 0, bdy = 0; for (const q of rf) { if (q.sel !== a.sel) continue; const dx = Math.abs(q.bx - a.sx), dy = Math.abs(q.cy - a.sy); const d = dx + dy; if (d < best) { best = d; bdx = dx; bdy = dy; } } if (best < 8) { matched++; maxDX = Math.max(maxDX, bdx); maxDY = Math.max(maxDY, bdy); } }
console.log(`PART COUNT: render=${rf.length} asm=${asm.length}  -> ${countOK ? 'MATCH' : 'MISMATCH'}`);
console.log(`PART SELs : ${selOK ? 'MATCH (set-equal)' : 'MISMATCH'}\n   render=${rfSels}\n   asm   =${asmSels}`);
console.log(`POSITION  : matched ${matched}/${asm.length} same-sel parts, maxDX=${maxDX.toFixed(3)}px maxDY=${maxDY.toFixed(3)}px`);
console.log((countOK && selOK && maxDX < 1 && maxDY < 1) ? 'RESULT: PASS (count+sel+<1px — satellite walk matches engine)' : 'RESULT: FAIL (animation/part-walk diverges)');
