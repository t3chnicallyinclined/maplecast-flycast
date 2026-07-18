// LIVE verify of the state-wire v2 server: connect to the local replica-live WS,
// confirm the frames are FRM2, decode them with the SAME client module the browser
// uses, and measure the live bandwidth. Proves the server<->client integration
// end-to-end against the running headless (build-headless-win, V2=1).
// Usage: node statewire_v2_liveverify.mjs [ws://127.0.0.1:7212] [nframes]
import WebSocket from 'ws';
import zlib from 'zlib';
import { decodeV2, decodeV2Len } from '../web/render-replica/statewire_v2.mjs';

const URL = process.argv[2] || 'ws://127.0.0.1:7212';
const WANT = parseInt(process.argv[3] || '300');

function zcst(u8) {   // "ZCST"(4)+u32 rawsize+zstd blob  -> raw bytes
  const m = u8[0] | (u8[1]<<8) | (u8[2]<<16) | (u8[3]<<24);
  if (u8.length >= 8 && m === 0x5453435A) return new Uint8Array(zlib.zstdDecompressSync(Buffer.from(u8.subarray(8))));
  return u8;
}
const rdu32 = (u8, o) => (u8[o] | (u8[o+1]<<8) | (u8[o+2]<<16) | (u8[o+3]<<24)) >>> 0;

let dynTotal = 0, dynRegs = [], gotPrefix = false, liveKey = null;
let n = 0, frm2 = 0, frmx = 0, decodeOk = 0, decodeErr = 0, wireBytes = 0, innerBytes = 0;
let firstErr = ''; const wireSizes = [];

const ws = new WebSocket(URL);
ws.binaryType = 'arraybuffer';
ws.on('open', () => console.log('connected', URL));
ws.on('error', e => { console.log('ERR', e.message); process.exit(4); });
ws.on('message', (data) => {
  const u8 = new Uint8Array(data);
  try {
    if (!gotPrefix) {
      const pre = zcst(u8);
      // MCRR prefix: 32B header, then nStatic + nDynamic region records {u32 addr,u32 len,8B tag}
      const nS = rdu32(pre, 8), nD = rdu32(pre, 12);
      let p = 32 + nS*16;
      for (let i = 0; i < nD; i++) { const len = rdu32(pre, p+4); dynRegs.push(len); dynTotal += len; p += 16; }
      gotPrefix = true;
      console.log(`prefix: ${nS} static, ${nD} dynamic regions; dynTotal=${dynTotal} B (v1 raw dynamic/frame)`);
      return;
    }
    const inner = zcst(u8);
    const magic = rdu32(inner, 0);
    wireBytes += u8.length; innerBytes += inner.length; wireSizes.push(u8.length);
    if (magic === 0x324D5246) {                 // "FRM2"
      frm2++;
      const enc = inner.subarray(12);
      // self-consistency: the block length must sit inside the frame (tails follow it)
      const blk = decodeV2Len(enc);
      if (12 + blk > inner.length) throw new Error(`blockLen ${blk} overruns frame ${inner.length}`);
      if (enc[0] !== 1 && !liveKey) { /* delta before first keyframe: skip */ }
      else {
        const dyn = decodeV2(enc, liveKey, dynTotal);
        if (enc[0] === 1) liveKey = dyn;
        if (dyn.length === dynTotal) decodeOk++; else throw new Error(`blob ${dyn.length} != ${dynTotal}`);
      }
    } else if (magic === 0x784D5246) { frmx++; }   // "FRMx" (v1 — gate off?)
    n++;
    if (n >= WANT) finish();
  } catch (e) { decodeErr++; if (!firstErr) firstErr = e.message; }
});

function finish() {
  try { ws.close(); } catch {}
  console.log(`\n== live v2 verify (${n} frames) ==`);
  console.log(`FRM2 frames        : ${frm2}   FRMx (v1) frames: ${frmx}`);
  console.log(`decodeV2 ok        : ${decodeOk}   errors: ${decodeErr}${firstErr ? '  ('+firstErr+')' : ''}`);
  const sorted = [...wireSizes].sort((a,b)=>a-b);
  const p50 = sorted.length ? sorted[Math.floor(sorted.length/2)] : 0;
  const half = wireSizes.slice(Math.floor(wireSizes.length/2));   // steady state (after connect burst)
  const steady = half.length ? Math.round(half.reduce((s,x)=>s+x,0)/half.length) : 0;
  console.log(`WIRE bytes/frame   : mean ${n?Math.round(wireBytes/n):0} | p50 ${p50} | steady(2nd half) ${steady}  (compressed, off socket)`);
  console.log(`inner bytes/frame  : mean ${n?Math.round(innerBytes/n):0}  (decompressed record incl GFX/pal/HUD tails)`);
  const pass = frm2 > 0 && decodeErr === 0 && frmx === 0;
  console.log(`\nVERDICT: ${pass ? 'PASS — server emits valid FRM2, client decodes clean' : 'FAIL — see above'}`);
  process.exit(pass ? 0 : 1);
}
setTimeout(() => { if (n === 0) { console.log('no frames in 40s (server not in-match?)'); process.exit(3); } finish(); }, 40000);
