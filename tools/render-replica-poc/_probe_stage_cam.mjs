// _probe_stage_cam.mjs — read stage_id + the two camera matrices (M2 @0x8C2D6AD8, M1 @0x8C2D6B18)
// out of a live MCRR capture's RAM image, frame-by-frame, to find the failing STAGE gate.
//   node _probe_stage_cam.mjs <file.mcrr>
import { readFileSync } from 'node:fs';

const path = process.argv[2];
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
u32(); const nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr: addr >>> 0, len, tag }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
console.log('dynamic regions:', dynamicRegs.map(r => `${r.tag}@0x${r.addr.toString(16)}+${r.len}`).join(' '));
p += vramBytes + pvrBytes;
const staticData = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const frameStart = p;
const G = a => (a >>> 0) & 0xFFFFFF;
const baseRam = new Uint8Array(16 * 1024 * 1024);
staticRegs.forEach((r, i) => { if (r.tag === 'ram16') baseRam.set(staticData[i], 0); else baseRam.set(staticData[i], G(r.addr)); });

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

const ADDR_M1 = 0x8C2D6B18, ADDR_M2 = 0x8C2D6AD8, ADDR_STAGE_ID = 0x8C289638;
function mat16(ram, addr){ const m = new Float32Array(16); const d = new DataView(ram.buffer, ram.byteOffset, ram.byteLength); for(let i=0;i<16;i++) m[i]=d.getFloat32(G(addr+i*4),true); return m; }
function nz(m){ return m.some(v => v !== 0); }
function u8(ram,a){ return ram[G(a)]; }

console.log(`frames=${frames.length}`);
let firstNonZeroCam = -1, firstStageNonZero = -1;
for (let i = 0; i < frames.length; i++) {
  const ram = applyFrame(frames[i]);
  const sid = u8(ram, ADDR_STAGE_ID);
  const inmatch = u8(ram, 0x8C289624);
  const M1 = mat16(ram, ADDR_M1), M2 = mat16(ram, ADDR_M2);
  const camGate = (M2[15] === 0 && M2[0] === 0); // replay.html bails if true
  if (!camGate && firstNonZeroCam < 0) firstNonZeroCam = i;
  if (sid !== 0 && firstStageNonZero < 0) firstStageNonZero = i;
  if (i < 5 || i === frames.length - 1 || (i % Math.max(1, (frames.length>>3)) === 0)) {
    console.log(`f${i} vframe=${frames[i].vframe} stage_id=${sid} in_match=${inmatch} camBail=${camGate} M2[0]=${M2[0].toFixed(3)} M2[15]=${M2[15].toFixed(3)} M1[0]=${M1[0].toFixed(3)} M1[15]=${M1[15].toFixed(3)}`);
  }
}
console.log(`first frame with non-zero camera (gate passes): ${firstNonZeroCam}`);
console.log(`first frame with non-zero stage_id: ${firstStageNonZero}`);

// dump full matrices for a representative mid frame
const mid = frames[Math.min(frames.length-1, Math.floor(frames.length*0.6))];
const ram = applyFrame(mid);
console.log(`\n--- mid frame vframe=${mid.vframe} stage_id=${u8(ram,ADDR_STAGE_ID)} ---`);
console.log('M2 (0x8C2D6AD8):', [...mat16(ram,ADDR_M2)].map(v=>v.toFixed(4)).join(' '));
console.log('M1 (0x8C2D6B18):', [...mat16(ram,ADDR_M1)].map(v=>v.toFixed(4)).join(' '));
