// ZCS2 wire verifier — node twin of web/zcs2-test.html.
// Connects to the mirror WS, decodes BOTH envelopes, pairs inner frames by
// frameNum, byte-compares, reports per-second Mbps + exact/mismatch counts.
// Usage: node zcs2_verify.mjs [ws://127.0.0.1:7200] [seconds=30]
import WebSocket from 'ws';
import { decompress as zOneShot, Decompress as ZStream } from '../web/webgpu/fzstd.mjs';

const URL = process.argv[2] || 'ws://127.0.0.1:7200';
const SECS = parseInt(process.argv[3] || '30', 10);

let legBytes=0, zBytes=0, totLeg=0, totZ=0, secs=0;
let paired=0, exact=0, mismatch=0, decMs=0, decN=0, epochs=0;
const pend = new Map();

function verify(src, inner) {
  if (inner.length < 8) return;
  const fn = inner[4] | inner[5]<<8 | inner[6]<<16 | inner[7]<<24;
  const other = pend.get(fn);
  if (other && other.src !== src) {
    pend.delete(fn); paired++;
    const a = other.bytes, b = inner;
    let same = a.length === b.length;
    if (same) same = Buffer.compare(Buffer.from(a.buffer,a.byteOffset,a.length),
                                    Buffer.from(b.buffer,b.byteOffset,b.length)) === 0;
    if (same) exact++; else { mismatch++;
      console.log(`frame ${fn>>>0}: MISMATCH len ${a.length} vs ${b.length}`); }
  } else {
    pend.set(fn, {src, bytes: inner.slice()});
    if (pend.size > 240) pend.delete(pend.keys().next().value);
  }
}

function soaInverse(v){
  const dv=new DataView(v.buffer,v.byteOffset,v.byteLength);
  const v2Sec=dv.getUint32(76,true), nRuns=dv.getUint32(80,true);
  const offs=84, lens=offs+nRuns*4, dat=lens+nRuns*2;
  let dataB=0; for(let i=0;i<nRuns;i++) dataB+=dv.getUint16(lens+i*2,true);
  const tailOff=80+v2Sec, legacySec=nRuns*6+dataB+4;
  const out=new Uint8Array(80+legacySec+(v.length-tailOff));
  out.set(v.subarray(0,80),0);
  const ov=new DataView(out.buffer);
  ov.setUint32(76,legacySec,true);
  let o=80, dp=dat, prev=0;
  for(let i=0;i<nRuns;i++){
    prev+=dv.getUint32(offs+i*4,true);
    const rl=dv.getUint16(lens+i*2,true);
    ov.setUint32(o,prev,true); ov.setUint16(o+4,rl,true); o+=6;
    out.set(v.subarray(dp,dp+rl),o); o+=rl; dp+=rl;
  }
  ov.setUint32(o,0xFFFFFFFF,true); o+=4;
  out.set(v.subarray(tailOff),o);
  return out;
}
let zSoaFrames=0;
let zdec=null, zEpoch=-1, zSynced=false, zChunks=[], zLen=0, zSoa=false;
function onZcs2(d) {
  const epoch=d[4], flags=d[5];
  zSoa=(flags&2)!==0; if(zSoa) zSoaFrames++;
  const innerSize = d[6] | d[7]<<8 | d[8]<<16 | d[9]<<24;
  if (flags & 1) {
    zdec = new ZStream((c)=>{ zChunks.push(c); zLen+=c.length; });
    zEpoch=epoch; zSynced=true; epochs++;
  } else if (!zSynced || epoch!==zEpoch) return;
  zBytes += d.length;
  const t0=performance.now();
  try { zdec.push(d.subarray(10)); } catch(e){ console.log('fzstd err:',e.message); zSynced=false; return; }
  decMs += performance.now()-t0; decN++;
  if (zLen >= innerSize) {
    const inner = new Uint8Array(innerSize);
    let o=0; for (const c of zChunks){ const n=Math.min(c.length,innerSize-o); inner.set(c.subarray(0,n),o); o+=n; if(o>=innerSize) break; }
    zChunks=[]; zLen=0;
    verify('zcs2', zSoa ? soaInverse(inner) : inner);
  }
}
function onZcst(d) {
  const sz = d[4] | d[5]<<8 | d[6]<<16 | d[7]<<24;
  if (sz > 1024*1024) return;   // compressed SYNC
  legBytes += d.length;
  try { verify('zcst', zOneShot(d.subarray(8), new Uint8Array(sz))); } catch(e){}
}

const ws = new WebSocket(URL);
ws.binaryType = 'nodebuffer';
ws.on('open', ()=>console.log('connected', URL));
ws.on('message', (buf, isBinary) => {
  if (!isBinary || buf.length < 10) return;
  const d = new Uint8Array(buf);
  if (d[0]===0x5A && d[1]===0x43 && d[2]===0x53) {
    if (d[3]===0x54) onZcst(d);
    else if (d[3]===0x32) onZcs2(d);
  }
});

const iv = setInterval(()=>{
  secs++; totLeg+=legBytes; totZ+=zBytes;
  console.log(`[${secs}s] ZCST ${(legBytes*8/1e6).toFixed(2)} Mbps | ZCS2 ${(zBytes*8/1e6).toFixed(2)} Mbps | `+
    `ratio ${zBytes?(legBytes/zBytes).toFixed(2):'-'}x | paired ${paired} exact ${exact} mismatch ${mismatch} | dec ${(decN?decMs/decN:0).toFixed(2)}ms`);
  legBytes=0; zBytes=0;
  if (secs>=SECS) {
    console.log(`== TOTAL ${secs}s: ZCST ${(totLeg*8/1e6/secs).toFixed(3)} Mbps  ZCS2 ${(totZ*8/1e6/secs).toFixed(3)} Mbps  `+
      `ratio ${(totLeg/Math.max(1,totZ)).toFixed(2)}x  epochs ${epochs}  paired ${paired} exact ${exact} MISMATCH ${mismatch}  soaFrames ${zSoaFrames}`);
    console.log(mismatch===0 && paired>100 ? 'VERDICT: BYTE-EXACT PASS' : 'VERDICT: FAIL/INSUFFICIENT');
    clearInterval(iv); ws.close(); process.exit(mismatch===0 && paired>100 ? 0 : 1);
  }
}, 1000);
