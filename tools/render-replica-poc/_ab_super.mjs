// _ab_super.mjs — A/B ARBITER for the super frame. Render the GSTA capture frame <fi> via
// render_frame and dump its quad TCW multiset + per-TCW count. Separately dump the mirror
// real-TA for ALL frames' TCW multisets. Find the mirror frame whose TCW set best matches
// the GSTA frame (Jaccard), then report: GSTA quads vs mirror quads, and the TCWs that GSTA
// emits MANY MORE of than the engine (= the over-tiled effect parts).
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';
import './webgpu-headless.mjs';
const W = new URL('../../web/webgpu/', import.meta.url);
const { FrameDecoder } = await import(new URL('frame-decoder.mjs', W));

const GSTA = process.argv[2] || '_live_fx3.gsta.mcrr';
const MIRROR = process.argv[3] || '_live_fx3.mirror.zcst';
const FI = parseInt(process.argv[4] || '854', 10);

// ---- GSTA: parse + render frame FI ----
const buf = new Uint8Array(readFileSync(GSTA));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
u32(); const nS = u32(), nD = u32(), nF = u32(), vB = u32(), pB = u32(); u32();
const reg = () => { const a = u32(), l = u32(); let t = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) t += String.fromCharCode(c); } p += 8; return { addr: a >>> 0, len: l, tag: t }; };
const sR = Array.from({ length: nS }, reg), dR = Array.from({ length: nD }, reg);
const headerEnd = p; const G = a => (a >>> 0) & 0xFFFFFF;
const baseRam = new Uint8Array(16 * 1024 * 1024);
{ let sp = headerEnd + vB + pB; for (const r of sR) { const b = buf.subarray(sp, sp + r.len); sp += r.len; if (r.tag === 'ram16') baseRam.set(b, 0); else baseRam.set(b, G(r.addr)); } }
const frames = [];
{ let fp = headerEnd + vB + pB; for (const r of sR) fp += r.len;
  for (let f = 0; f < nF; f++) {
    const vframe = dv.getUint32(fp + 4, true); const taSize = dv.getUint32(fp + 8, true);
    const dynOff = fp + 12; let q = dynOff; for (const r of dR) q += r.len;
    const nGfx = dv.getUint32(q, true); if (nGfx <= 64) { q += 4; for (let g = 0; g < nGfx && q + 8 <= buf.length; g++) { const len = dv.getUint32(q + 4, true); q += 8 + len; } }
    if (q + 4 <= buf.length) { const palLen = dv.getUint32(q, true); q += 4; if (palLen && q + palLen <= buf.length) q += palLen; }
    if (q + 8 <= buf.length && dv.getUint32(q, true) === 0x48554451) { q += 4; const nHud = dv.getUint32(q, true); q += 4 + nHud * 96; }
    q += taSize; frames.push({ dynOff, vframe }); fp = q;
  } }
const fr = frames[FI]; const ram = baseRam.slice();
{ let dp = fr.dynOff; for (const r of dR) { ram.set(buf.subarray(dp, dp + r.len), G(r.addr)); dp += r.len; } }
const M = await createRenderFrame({ locateFile: x => x });
const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
const cap = 1024 * 1024, outPtr = M._malloc(cap);
const taLen = M._render_frame_ta(ramPtr, outPtr, cap);
const ta = M.HEAPU8.slice(outPtr, outPtr + taLen);
const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
const gstaTcw = new Map();
{ let o = 0; while (o + 96 <= ta.length) { const pcw = tdv.getUint32(o, true); if (((pcw >> 29) & 7) === 5) { const tcw = tdv.getUint32(o + 12, true) & 0x1FFFFF; gstaTcw.set(tcw, (gstaTcw.get(tcw) || 0) + 1); o += 96; } else o += 32; } }
const gstaTotal = [...gstaTcw.values()].reduce((a, b) => a + b, 0);
console.log(`GSTA fi=${FI} vframe=${fr.vframe} quads=${gstaTotal} distinctTCW=${gstaTcw.size}`);

// ---- MIRROR: parse all frames, build TCW multiset per frame ----
const mfile = readFileSync(MIRROR); const mdv = new DataView(mfile.buffer, mfile.byteOffset, mfile.byteLength);
const D = new FrameDecoder(); let moff = 0; const mmsgs = [];
while (moff + 4 <= mfile.length) { const len = mdv.getUint32(moff, true); moff += 4; if (moff + len > mfile.length) break; if (len > 0) { const m = mfile.subarray(moff, moff + len); moff += len; if (m[0] === 0x5A && m[1] === 0x43 && m[2] === 0x53 && m[3] === 0x54) mmsgs.push(m); } }
function mirrorTcw(buf) { const map = new Map(); let o = 0, n = buf.length; while (o + 32 <= n) { const pcw = buf.readUInt32LE(o); if (((pcw >> 29) & 7) === 5) { if (o + 96 > n) break; const tcw = buf.readUInt32LE(o + 12) & 0x1FFFFF; map.set(tcw, (map.get(tcw) || 0) + 1); o += 96; } else o += 32; } return map; }
let best = null, bestJ = -1;
const gstaKeys = new Set(gstaTcw.keys());
for (const m of mmsgs) { let f = null; try { f = D.applyFrame(m); } catch (e) { continue; } if (!f) continue; const b = Buffer.from(f.taBuffer.buffer || f.taBuffer, f.taBuffer.byteOffset || 0, f.taBuffer.byteLength || f.taBuffer.length); const mt = mirrorTcw(b); let inter = 0; for (const k of mt.keys()) if (gstaKeys.has(k)) inter++; const uni = new Set([...mt.keys(), ...gstaKeys]).size; const j = inter / uni; if (j > bestJ) { bestJ = j; best = { fnum: f.frameNum, mt }; } }
const mTotal = [...best.mt.values()].reduce((a, b) => a + b, 0);
console.log(`MIRROR best-match fnum=${best.fnum} quads=${mTotal} distinctTCW=${best.mt.size} (Jaccard=${bestJ.toFixed(3)})`);
console.log(`\nTCWs where GSTA emits MANY MORE than the engine (over-tiling):`);
const rows = [];
for (const [tcw, gc] of gstaTcw) { const mc = best.mt.get(tcw) || 0; if (gc - mc >= 3) rows.push({ tcw, gc, mc, d: gc - mc }); }
rows.sort((a, b) => b.d - a.d);
for (const r of rows.slice(0, 20)) console.log(`  tcw=${r.tcw.toString(16)} GSTA=${r.gc} engine=${r.mc} excess=+${r.d}`);
console.log(`\nTotal excess GSTA quads vs engine = ${rows.reduce((a, b) => a + b.d, 0)}`);
