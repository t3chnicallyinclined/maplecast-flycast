// _fx3_slotwalk_dump.mjs — for ONE frame (by fi index), walk the slot table EXACTLY like
// gen_walker_root.c and dump every visited node: layer L, idx i, slot count, node addr,
// cat(+0x3), gfx1(+0x15C), sel(+0x144), gate(+0x12C), anchor(+0xE0,+0xE4). Reveals whether
// the super-frame body explosion is duplicate slot entries (same node many times) or many
// distinct stale nodes that the engine would NOT draw.
//
//   node _fx3_slotwalk_dump.mjs <file.mcrr> <fi>
import { readFileSync } from 'node:fs';
const CAP = process.argv[2]; const FI = parseInt(process.argv[3] || '850', 10);
const buf = new Uint8Array(readFileSync(CAP));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
u32(); const nS = u32(), nD = u32(), nF = u32(), vB = u32(), pB = u32(); u32();
const reg = () => { const a = u32(), l = u32(); let t = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) t += String.fromCharCode(c); } p += 8; return { addr: a >>> 0, len: l, tag: t }; };
const sR = Array.from({ length: nS }, reg), dR = Array.from({ length: nD }, reg);
const headerEnd = p; const G = a => (a >>> 0) & 0xFFFFFF;
const baseRam = new Uint8Array(16 * 1024 * 1024);
{ let sp = headerEnd + vB + pB; for (const r of sR) { const b = buf.subarray(sp, sp + r.len); sp += r.len; if (r.tag === 'ram16') baseRam.set(b, 0); else baseRam.set(b, G(r.addr)); } }
// robust per-frame tail-walk (matches _fx3_gsta_quadscan.mjs)
const frames = [];
{ let fp = headerEnd + vB + pB; for (const r of sR) fp += r.len;
  for (let f = 0; f < nF; f++) {
    if (dv.getUint32(fp, true) !== 0x784D5246) throw new Error(`frame ${f}: bad FRMx @${fp}`);
    const vframe = dv.getUint32(fp + 4, true); const taSize = dv.getUint32(fp + 8, true);
    const dynOff = fp + 12; let q = dynOff; for (const r of dR) q += r.len;
    const nGfx = (q + 4 <= buf.length) ? dv.getUint32(q, true) : 0;
    if (nGfx <= 64) { q += 4; for (let g = 0; g < nGfx && q + 8 <= buf.length; g++) { const len = dv.getUint32(q + 4, true); q += 8 + len; } }
    if (q + 4 <= buf.length) { const palLen = dv.getUint32(q, true); q += 4; if (palLen && q + palLen <= buf.length) q += palLen; }
    if (q + 8 <= buf.length && dv.getUint32(q, true) === 0x48554451) { q += 4; const nHud = dv.getUint32(q, true); q += 4 + nHud * 96; }
    q += taSize;
    frames.push({ dynOff, vframe }); fp = q;
  } }
const fr = frames[FI];
const ram = baseRam.slice();
{ let dp = fr.dynOff; for (const r of dR) { ram.set(buf.subarray(dp, dp + r.len), G(r.addr)); dp += r.len; } }
const u8 = a => ram[G(a)]; const s8 = a => { const v = ram[G(a)]; return v > 127 ? v - 256 : v; };
const u32r = a => (ram[G(a)] | (ram[G(a) + 1] << 8) | (ram[G(a) + 2] << 16) | (ram[G(a) + 3] << 24)) >>> 0;
const u16r = a => (ram[G(a)] | (ram[G(a) + 1] << 8)) >>> 0;
const f32 = a => new Float32Array(new Uint32Array([u32r(a)]).buffer)[0];
const COUNT_BASE = 0x8C2895E0, PTR_BASE = 0x8C287DE0, LS = 0x180;
console.log(`FRAME fi=${FI} vframe=${fr.vframe}`);
const nodeCount = new Map();
let totalBody = 0, totalSat = 0;
for (let L = 0; L < 16; L++) {
  const cnt = s8(COUNT_BASE + L); if (cnt <= 0 || cnt > 0x60) continue;
  const ptrBase = PTR_BASE + L * LS;
  console.log(`  layer ${L} count=${cnt}`);
  for (let i = 0; i < cnt; i++) {
    const node = u32r(ptrBase + i * 4);
    if (node === 0 || ((node >>> 24) & 0x7F) !== 0x0C) { console.log(`    [${i}] node=${node.toString(16)} (skip non-area3)`); continue; }
    const cat = s8(node + 0x3), gfx1 = u32r(node + 0x15C), gfx2 = u32r(node + 0x160), sel = u16r(node + 0x144), gate = u16r(node + 0x12C);
    const ex = f32(node + 0xE0), ey = f32(node + 0xE4);
    const cell154 = u32r(node + 0x154);
    // GFX2 body-walker record count: cell = gfx2 + *(u32)(gfx2 + (sel&0x7FFF)*4); first u16 = record count
    let recCount = -1, cellPtr = 0;
    const mSel = sel & 0x7FFF;
    if (((gfx2 >>> 24) & 0x7F) === 0x0C) { const off = u32r(gfx2 + mSel * 4); cellPtr = (gfx2 + off) >>> 0; if (((cellPtr >>> 24) & 0x7F) === 0x0C) recCount = u16r(cellPtr); }
    nodeCount.set(node, (nodeCount.get(node) || 0) + 1);
    if (cat === 0) totalBody++; else totalSat++;
    console.log(`    [${i}] node=${node.toString(16)} cat=${cat} gfx1=${gfx1.toString(16)} gfx2=${gfx2.toString(16)} sel=${sel.toString(16)}(m${mSel.toString(16)}) gate=${gate} cell154=${cell154.toString(16)} recCount=${recCount} anchor=(${ex.toFixed(0)},${ey.toFixed(0)})`);
  }
}
const dup = [...nodeCount.entries()].filter(([, c]) => c > 1);
console.log(`TOTAL body(cat0)=${totalBody} sat=${totalSat} distinctNodes=${nodeCount.size}`);
console.log(`DUPLICATE nodes (visited >1x): ${dup.length}  ${dup.slice(0, 10).map(([n, c]) => n.toString(16) + 'x' + c).join(' ')}`);

// --- extra: for the first effect node, dump node+0xDC and tiledesc entries it indexes ---
