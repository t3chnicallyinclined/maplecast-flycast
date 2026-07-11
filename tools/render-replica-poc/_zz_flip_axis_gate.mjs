// _zz_flip_axis_gate.mjs — GATE for the 0x8000/0x4000 flip-axis swap in render_frame.
// TWO independent checks:
//  DIRECT MODEL (NEW build vs raw cell flags — authoritative, no OLD dependency): for every BODY
//    quad, assert  mir == facing XOR (flags&0x8000)  AND  mir_v == (flags&0x4000).  This proves
//    render_frame implements the disasm loc_8c0344d4 axes: 0x8000=H(texU,facing-composed),
//    0x4000=V(texV,no-facing).
//  NO-REGRESSION (OLD vs NEW): the 12 corner floats of every quad are byte-identical (the change
//    touches only the draw-time mirror attrs, never the walker/geometry) + report exactly which
//    quads' texU flipped and which gained texV (the intended fix; census by real flags).
//   node _zz_flip_axis_gate.mjs <file.mcrr> [maxFramesWithFlips=8]
import { readFileSync } from 'node:fs';
import createOld from './_OLD_render_frame_node.mjs';
import createNew from './render_frame_node.mjs';

const path = process.argv[2];
const MAXF = +(process.argv[3] || 8);

// ---- parse .mcrr (verbatim from _zz_rf_engine_diff.mjs) ----
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); u32(); const nS = u32(), nD = u32(), nF = u32(), vb = u32(), pb = u32(); u32();
const region = () => { const a = u32(), l = u32(); let t = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) t += String.fromCharCode(c); } p += 8; return { addr: a >>> 0, len: l, tag: t }; };
const sR = Array.from({ length: nS }, region), dR = Array.from({ length: nD }, region);
const ram = new Uint8Array(16 * 1024 * 1024);
let q = p + vb + pb;
for (const r of sR) { const b = buf.subarray(q, q + r.len); q += r.len; if (r.tag === 'ram16') ram.set(b, 0); else ram.set(b, r.addr & 0xFFFFFF); }
p = q; const frames = [];
for (let f = 0; f < nF; f++) { u32(); const vf = u32(); const tail = u32(); const dof = p; for (const r of dR) p += r.len; p += tail; frames.push({ vf, dof }); }

import { fileURLToPath } from 'node:url';
const HERE = fileURLToPath(new URL('.', import.meta.url));
// _OLD_render_frame_node.mjs embeds the name 'render_frame_node.wasm' -> redirect it to the
// saved OLD wasm, else the OLD module would load the NEW wasm and the comparison is void.
const MO = await createOld({ locateFile: (x) => x.endsWith('render_frame_node.wasm') ? HERE + '_OLD_render_frame_node.wasm' : x });
const MN = await createNew({ locateFile: (x) => x.endsWith('render_frame_node.wasm') ? HERE + 'render_frame_node.wasm' : x });
if (typeof MN._render_frame_quad_mirror_v !== 'function') { console.error('FAIL: new build missing _render_frame_quad_mirror_v'); process.exit(2); }
if (typeof MN._render_frame_quad_rawflags !== 'function') { console.error('FAIL: new build missing _render_frame_quad_rawflags'); process.exit(2); }

function runNew(M, ram) {
  const ramPtr = M._malloc(ram.length), outPtr = M._malloc(512 * 1024), b8 = M._malloc(8192), b16 = M._malloc(8192);
  M.HEAPU8.set(ram, ramPtr);
  M._render_frame_ta(ramPtr, outPtr, 512 * 1024);
  const quads = M._render_frame_quad_count();
  const ta = new Uint8Array(M.HEAPU8.subarray(outPtr, outPtr + quads * 96)).slice();
  const rd8 = (fn) => { M[fn](b8, quads); return new Uint8Array(M.HEAPU8.subarray(b8, b8 + quads)).slice(); };
  const mir = rd8('_render_frame_quad_mirror'), mv = rd8('_render_frame_quad_mirror_v');
  const fac = rd8('_render_frame_quad_facing'), eff = rd8('_render_frame_quad_is_effect');
  M._render_frame_quad_rawflags(b16, quads);
  const flags = new Uint16Array(M.HEAPU8.buffer.slice(b16, b16 + quads * 2));
  M._render_frame_quad_sels(b16, quads);
  const sels = new Uint16Array(M.HEAPU8.buffer.slice(b16, b16 + quads * 2));
  M._free(ramPtr); M._free(outPtr); M._free(b8); M._free(b16);
  return { quads, ta, mir, mv, fac, eff, flags, sels };
}
function runOld(M, ram) {
  const ramPtr = M._malloc(ram.length), outPtr = M._malloc(512 * 1024), b8 = M._malloc(8192);
  M.HEAPU8.set(ram, ramPtr);
  M._render_frame_ta(ramPtr, outPtr, 512 * 1024);
  const quads = M._render_frame_quad_count();
  const ta = new Uint8Array(M.HEAPU8.subarray(outPtr, outPtr + quads * 96)).slice();
  M._render_frame_quad_mirror(b8, quads);
  const mir = new Uint8Array(M.HEAPU8.subarray(b8, b8 + quads)).slice();
  M._free(ramPtr); M._free(outPtr); M._free(b8);
  return { quads, ta, mir };
}

let framesUsed = 0, totBody = 0, modelBadU = 0, modelBadV = 0, geomBad = 0, geomCmp = 0;
let cH = 0, cV = 0, cBoth = 0, cPlain = 0, gainH = 0, dropSpuriousU_gainV = 0, texUflipped = 0, texVset = 0;
const perSel = new Map();
const HEX = h => '0x' + h.toString(16);

for (const fr of frames) {
  let o = fr.dof; for (const r of dR) { ram.set(buf.subarray(o, o + r.len), r.addr & 0xFFFFFF); o += r.len; }
  const N = runNew(MN, ram);
  let hasFlip = false; for (let k = 0; k < N.quads; k++) if (N.flags[k] & 0xC000) { hasFlip = true; break; }
  if (!hasFlip) continue;
  const O = runOld(MO, ram);
  framesUsed++;
  const alignedGeom = (O.quads === N.quads);
  for (let k = 0; k < N.quads; k++) {
    const f = N.flags[k], h = (f & 0x8000) ? 1 : 0, v = (f & 0x4000) ? 1 : 0, fac = N.fac[k] & 1;
    if (!N.eff[k]) {
      totBody++;
      // DIRECT model check vs raw flags
      if ((N.mir[k] & 1) !== (fac ^ h)) modelBadU++;
      if ((N.mv[k] & 1) !== v) modelBadV++;
      if (h && v) cBoth++; else if (h) cH++; else if (v) cV++; else cPlain++;
    }
    // NO-REGRESSION geometry (aligned quad index): corner floats @36..83
    if (alignedGeom) { geomCmp++; for (let i = 36; i < 84; i++) if (O.ta[k * 96 + i] !== N.ta[k * 96 + i]) { geomBad++; break; } }
    // change census (old texU vs new texU/texV) on aligned quads
    if (alignedGeom) {
      if ((O.mir[k] & 1) !== (N.mir[k] & 1)) texUflipped++;
      if (N.mv[k] & 1) texVset++;
      if (h && !(O.mir[k] & 1 ^ fac)) gainH++;                 // 0x8000 part: old had no per-part U flip, new does
      if (v && !h && ((O.mir[k] & 1) !== fac) && !(N.mir[k] & 1 ^ fac)) dropSpuriousU_gainV++;
    }
    if (f & 0xC000) {
      let e = perSel.get(N.sels[k]); if (!e) { e = { n: 0, h: 0, v: 0, both: 0, uFlip: 0, vSet: 0 }; perSel.set(N.sels[k], e); }
      e.n++; if (h && v) e.both++; else if (h) e.h++; else if (v) e.v++;
      if (alignedGeom && (O.mir[k] & 1) !== (N.mir[k] & 1)) e.uFlip++; if (v) e.vSet++;
    }
  }
  if (framesUsed >= MAXF) break;
}

console.log(`\n=== FLIP-AXIS GATE — ${path}  (${framesUsed} frames w/ flip quads) ===`);
console.log(`\n[DIRECT MODEL vs raw cell flags]  body quads checked: ${totBody}`);
console.log(`  texU model  mir == facing XOR (flags&0x8000):  mismatches = ${modelBadU}   ${modelBadU === 0 ? 'PASS' : 'FAIL'}`);
console.log(`  texV model  mir_v == (flags&0x4000):           mismatches = ${modelBadV}   ${modelBadV === 0 ? 'PASS' : 'FAIL'}`);
console.log(`  flag census: plain=${cPlain}  0x8000-H=${cH}  0x4000-V=${cV}  0xC000-both=${cBoth}`);
console.log(`\n[NO-REGRESSION vs OLD build]  quads compared: ${geomCmp}`);
console.log(`  (0) corner-float changes old->new = ${geomBad}   ${geomBad === 0 ? 'PASS (geometry byte-identical)' : 'FAIL'}`);
console.log(`  (a) 0x8000 parts that GAIN the H mirror (old gave none): ${gainH}`);
console.log(`  (b) 0x4000 parts that drop spurious texU + gain texV:   ${dropSpuriousU_gainV}`);
console.log(`  texU bit flipped old->new: ${texUflipped}   texV now set: ${texVset}`);
console.log(`\n[per-sel, sels with a flip bit]  (uFlip = texU changed old->new; vSet = texV set)`);
for (const [sel, e] of [...perSel].sort((a, b) => a[0] - b[0]))
  console.log(`  sel${HEX(sel).padEnd(6)} n=${e.n}  H=${e.h} V=${e.v} both=${e.both}  uFlip=${e.uFlip} vSet=${e.vSet}`);
