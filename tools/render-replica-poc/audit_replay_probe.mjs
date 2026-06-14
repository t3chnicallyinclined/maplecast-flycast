// audit_replay_probe.mjs — drive the REAL render_frame.wasm over a 16-byte-header MCRR
// capture and report, per frame, the chased-pointer values render_frame would read AND
// the resulting quad geometry (Ax spread = stray detector). Truth, not model.
//
// This is the definitive check on whether the tab_ptr/ggp_ptr +4 shift seen in the
// reconstruction actually reaches render_frame (i.e. is a real wire bug) or is a
// reconstruction artifact. We splat the SAME seed+dyn overlay the client does, read the
// pointers straight out of the wasm heap, and run the render.
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';

const path = process.argv[2] || 'side_cap.mcrr';
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
const version = u32(), nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr, len }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
p += vramBytes + pvrBytes;
const staticData = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const frameStart = p;

const RAM = 16 * 1024 * 1024;
const ram = new Uint8Array(RAM);
staticRegs.forEach((r, i) => { if (r.len === RAM) ram.set(staticData[i], 0); else ram.set(staticData[i], r.addr & 0xFFFFFF); });

// locate the evenly-spaced 16-byte-header frame chain
const dynbytes = dynamicRegs.reduce((s, r) => s + r.len, 0);
const STRIDE = dynbytes + 16;
const positions = [];
for (let i = 0; i + 4 <= buf.length; i++) if (buf[i] === 0x46 && buf[i+1] === 0x52 && buf[i+2] === 0x4D && buf[i+3] === 0x78) positions.push(i);
const posset = new Set(positions);
let best = [0, frameStart];
for (const x of positions) { let n = 0, c = x; while (posset.has(c) && dv.getUint32(c, true) === 0x784D5246) { n++; c += STRIDE; } if (n > best[0]) best = [n, x]; }
const [chainN, chainStart] = best;
console.error(`frames=${chainN} stride=${STRIDE.toString(16)} start=${chainStart.toString(16)}`);

function applyDyn(f) {
  let off = chainStart + f * STRIDE + 12;  // 12-byte frame header (FRMx+vframe+taSize), +4 trailer
  for (const r of dynamicRegs) { ram.set(buf.subarray(off, off + r.len), r.addr & 0xFFFFFF); off += r.len; }
}
const r32 = (g) => { const i = g & 0xFFFFFF; return (ram[i] | (ram[i+1]<<8) | (ram[i+2]<<16) | (ram[i+3]<<24)) >>> 0; };

const Mod = await createRenderFrame();
const ramPtr = Mod._malloc(RAM); const cap = 512 * 1024; const outPtr = Mod._malloc(cap);

const want = process.argv[3] ? process.argv[3].split(',').map(Number) : [0, 60, 120, 180, chainN - 1];
for (const f of want) {
  if (f < 0 || f >= chainN) continue;
  applyDyn(f);
  const idxptr = r32(0x8C2DAD3C), recptr = r32(0x8C2DAD4C), ggp = r32(0x8C26823C);
  Mod.HEAPU8.set(ram, ramPtr);
  const len = Mod._render_frame_ta(ramPtr, outPtr, cap);
  const bodies = Mod._render_frame_body_count();
  const sats = Mod._render_frame_sat_count ? Mod._render_frame_sat_count() : -1;
  const quads = Mod._render_frame_quad_count();
  // The TA buffer is a stream of 96-byte PVR2 sprite records; per wasm_entry_frame.c the
  // screen X (q->Ax) is at byte +36, screen Y (q->Ay) at +40 of each record. Walk them to
  // get the on-screen X distribution -> strays are quads whose X sits far outside the body
  // cluster (the user's "fragments pushed OUTWARD to the far left/right").
  const odv = new DataView(Mod.HEAPU8.buffer, outPtr, len);
  const xs = [];
  for (let o = 0; o + 96 <= len; o += 96) {
    const ax = odv.getFloat32(o + 36, true);
    if (Number.isFinite(ax)) xs.push(ax);
  }
  xs.sort((a, b) => a - b);
  const med = xs.length ? xs[xs.length >> 1] : 0;
  // strays: X more than 200px from the median screen X (bodies cluster within ~150px)
  const left = xs.filter(x => x < med - 200);
  const right = xs.filter(x => x > med + 200);
  const lo = xs.length ? xs[0] : 0, hi = xs.length ? xs[xs.length - 1] : 0;
  console.log(`f${f}: bodies=${bodies} sats=${sats} quads=${quads} X[${lo.toFixed(0)}..${hi.toFixed(0)}] med=${med.toFixed(0)} strayL=${left.length} strayR=${right.length}` +
    (left.length ? ` Lxs=[${left.slice(0,6).map(x=>x.toFixed(0))}]` : '') +
    (right.length ? ` Rxs=[${right.slice(0,6).map(x=>x.toFixed(0))}]` : ''));
}
Mod._free(ramPtr); Mod._free(outPtr);
