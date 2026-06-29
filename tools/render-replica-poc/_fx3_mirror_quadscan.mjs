// _fx3_mirror_quadscan.mjs — decode the .zcst MIRROR real-TA (ground truth) and report
// per frame: frameNum + total sprite count (ParaType=5 quads) + distinct TCW count.
// Compare against the GSTA reconstruction's quad count for the same frameNum.
import { readFileSync } from 'node:fs';
import './webgpu-headless.mjs';
const W_DIR = new URL('../../web/webgpu/', import.meta.url);
const { FrameDecoder } = await import(new URL('frame-decoder.mjs', W_DIR));

const path = process.argv[2] || '_live_fx3.mirror.zcst';
const file = readFileSync(path);
const dv = new DataView(file.buffer, file.byteOffset, file.byteLength);
const D = new FrameDecoder();
let off = 0; const msgs = [];
while (off + 4 <= file.length) {
  const len = dv.getUint32(off, true); off += 4;
  if (off + len > file.length) break;
  if (len > 0) { const m = file.subarray(off, off + len); off += len;
    if (m.length >= 4 && m[0] === 0x5A && m[1] === 0x43 && m[2] === 0x53 && m[3] === 0x54) msgs.push(m);
  }
}
function parseTA(buf) {
  const out = []; let o = 0; const n = buf.length;
  while (o + 32 <= n) {
    const pcw = buf.readUInt32LE(o); const pt = (pcw >> 29) & 7;
    if (pt === 5) { if (o + 96 > n) break; const tcw = buf.readUInt32LE(o + 12); out.push(tcw & 0x1FFFFF); o += 96; continue; }
    o += 32;
  }
  return out;
}
let fi = 0, maxSp = 0, maxFnum = 0;
for (const m of msgs) {
  let fr = null; try { fr = D.applyFrame(m); } catch (e) { continue; }
  if (!fr) continue;
  fi++;
  const buf = Buffer.from(fr.taBuffer.buffer || fr.taBuffer, fr.taBuffer.byteOffset || 0, fr.taBuffer.byteLength || fr.taBuffer.length);
  const sp = parseTA(buf);
  if (sp.length > maxSp) { maxSp = sp.length; maxFnum = fr.frameNum; }
  if (sp.length > 120) {
    const tcws = new Set(sp);
    console.log(`mirror frame#${fi} fnum=${fr.frameNum} sprites=${sp.length} distinctTCW=${tcws.size}`);
  }
}
console.log(`-- ${fi} mirror frames; MAX sprites=${maxSp} at fnum=${maxFnum} --`);
