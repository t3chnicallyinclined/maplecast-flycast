// decompose_live.mjs — capture the live wire and split every legacy ZCST delta
// frame into its sections (TA delta vs dirty pages full/ref vs headers), with
// the ZCS2 wire meter alongside. VCACHE-sentinel aware.
// Usage: node decompose_live.mjs [wss://nobd.net/ws] [seconds]
import WebSocket from 'ws';
import { decompress } from '../web/webgpu/fzstd.mjs';

const url = process.argv[2] || 'wss://nobd.net/ws';
const secs = parseInt(process.argv[3] || '120', 10);

let zcs2Wire = 0, zcstWire = 0, syncWire = 0, otherWire = 0;
let frames = 0;
const perFrame = [];   // {num, ta, pgFull, pgRef, nFull, nRef}
let totTA = 0, totFull = 0, totRef = 0, totHdr = 0;

const ws = new WebSocket(url);
ws.on('open', () => console.log(`connected ${url}, capturing ${secs}s (legacy ZCST decomposition + ZCS2 meter)`));
ws.on('error', e => { console.log('WS error', e.message); process.exit(2); });
ws.on('message', d => {
  const b = Buffer.from(d);
  if (b.length >= 10 && b.toString('ascii', 0, 4) === 'ZCS2') { zcs2Wire += b.length; return; }
  if (b.length < 8 || b.toString('ascii', 0, 4) !== 'ZCST') { otherWire += b.length; return; }
  const usz = b.readUInt32LE(4);
  if (usz > 1048576) { syncWire += b.length; return; }   // compressed SYNC
  zcstWire += b.length;
  let u;
  try { u = Buffer.from(decompress(b.subarray(8))); } catch (e) { return; }
  // frameSize(4)+frameNum(4)+pvr(64)+taSize(4)+deltaPayloadSize(4)+delta+cksum(4)+dirty
  if (u.length < 84) return;
  const num = u.readUInt32LE(4);
  const taSize = u.readUInt32LE(72);
  const deltaPayloadSize = u.readUInt32LE(76);
  let off = 80 + deltaPayloadSize + 4;
  if (off + 4 > u.length) return;
  let dirty = u.readUInt32LE(off); off += 4;
  let vc = false;
  if (dirty === 0xFFFFFFFF) { vc = true; dirty = u.readUInt32LE(off); off += 4; }
  let pgFull = 0, pgRef = 0, nFull = 0, nRef = 0;
  for (let i = 0; i < dirty && off < u.length; i++) {
    off += 5;                                   // regionId + pageIdx
    if (vc) {
      off += 8;                                 // hash
      const hasData = u[off]; off += 1;
      if (hasData) { pgFull += 4096 + 14; nFull++; off += 4096; }
      else { pgRef += 14; nRef++; }
    } else { pgFull += 4096 + 5; nFull++; off += 4096; }
  }
  const hdr = u.length - deltaPayloadSize - pgFull - pgRef;
  frames++;
  totTA += deltaPayloadSize; totFull += pgFull; totRef += pgRef; totHdr += Math.max(0, hdr);
  perFrame.push({ num, ta: deltaPayloadSize, pgFull, pgRef, nFull, nRef, tot: u.length });
});

setTimeout(() => {
  const mbps = (bytes) => (bytes * 8 / secs / 1e6).toFixed(3);
  console.log(`\n== WIRE (${secs}s): ZCS2 ${mbps(zcs2Wire)} Mbps | legacy-ZCST ${mbps(zcstWire)} Mbps | SYNC ${mbps(syncWire)} | other ${mbps(otherWire)} | frames ${frames}`);
  const tot = totTA + totFull + totRef + totHdr;
  const pc = (x) => (100 * x / Math.max(1, tot)).toFixed(1) + '%';
  console.log(`== UNCOMPRESSED composition: TA-delta ${pc(totTA)} (${(totTA/1e6).toFixed(1)}MB) | pages-FULL ${pc(totFull)} (${(totFull/1e6).toFixed(1)}MB) | pages-REF ${pc(totRef)} | headers ${pc(totHdr)}`);
  perFrame.sort((a, b) => b.tot - a.tot);
  console.log('== TOP-10 spike frames (uncompressed):');
  for (const f of perFrame.slice(0, 10))
    console.log(`   f${f.num}: total ${(f.tot/1024).toFixed(0)}KB = TA ${(f.ta/1024).toFixed(0)}KB + full ${f.nFull}pg (${(f.pgFull/1024).toFixed(0)}KB) + ref ${f.nRef}pg`);
  const busy = perFrame.filter(f => f.tot > 32768);
  const bTA = busy.reduce((s,f)=>s+f.ta,0), bF = busy.reduce((s,f)=>s+f.pgFull,0);
  console.log(`== BUSY frames (>32KB, n=${busy.length}): TA ${(100*bTA/Math.max(1,bTA+bF)).toFixed(1)}% vs pages-full ${(100*bF/Math.max(1,bTA+bF)).toFixed(1)}%`);
  process.exit(0);
}, secs * 1000);
