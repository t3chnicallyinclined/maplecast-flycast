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
// ---- Phase 3a stage-strip gate --------------------------------------------
// JS port of the server's taStripStage walker (STG0B allowlist). The gate:
// after each frame, md5(zcs2 stripped-chain TA) == md5(stripLocal(legacy TA)).
import crypto from 'crypto';
const STRIP_ALLOW=new Set([0x9fc00,0xa0000]);
// CHARSTRIP mirror (render-state/09): TR-para5 with TCW addr in the decoded-GFX
// staging range. Applied only when the wire flags bit4 (16) say the server did.
const CS_LO=0x082000, CS_HI=0x08B000;
function taStripJS(ta,taSize,charStrip){
  const out=new Uint8Array(taSize); let off=0,w=0;
  let curList=-1,isSpr=false,haveParam=false,dropping=false,cObj=0;
  const u32=o=>ta[o]|ta[o+1]<<8|ta[o+2]<<16|ta[o+3]<<24;
  const emit=n=>{out.set(ta.subarray(off,off+n),w); w+=n;};
  while(off+32<=taSize){
    const pcw=u32(off)>>>0, pt=(pcw>>>29)&7;
    if(pt===0||pt===1||pt===2||pt===3||pt===6){haveParam=false;dropping=false;if(pt===0)curList=-1;emit(32);off+=32;continue;}
    if(pt===4){
      const lt=(pcw>>>24)&7; if(curList===-1)curList=lt;
      if(curList===1||curList===3){haveParam=false;dropping=false;emit(32);off+=32;continue;}
      cObj=pcw&0xFF; isSpr=false; haveParam=true;
      const colType=(cObj>>4)&3, vol=(cObj>>6)&1;
      let sz; if(colType===2&&!vol&&((cObj>>2)&1)) sz=(off+64<=taSize)?64:32;
      else if(colType>=1&&vol) sz=(off+64<=taSize)?64:32; else sz=32;
      const tcw=u32(off+12)>>>0;
      dropping=(curList===0)&&STRIP_ALLOW.has(tcw&0x1FFFFF);
      if(!dropping)emit(sz); off+=sz; continue;
    }
    if(pt===5){
      const lt=(pcw>>>24)&7; if(curList===-1)curList=lt;
      cObj=pcw&0xFF; isSpr=true; haveParam=true;
      const tcw=u32(off+12)>>>0, addr=tcw&0x1FFFFF;
      dropping=((curList===0)&&STRIP_ALLOW.has(addr))
             ||(charStrip&&curList===2&&addr>=CS_LO&&addr<CS_HI);
      if(!dropping)emit(32); off+=32; continue;
    }
    if(pt===7){
      let sz;
      if(!haveParam)sz=32;
      else if(isSpr&&off+64<=taSize)sz=64;
      else{const tex=(cObj>>3)&1,colType=(cObj>>4)&3,vol=(cObj>>6)&1;
        if(!tex)sz=32; else if(!vol)sz=(colType===1&&off+64<=taSize)?64:32; else sz=32;}
      if(!dropping)emit(sz); off+=sz; continue;
    }
    emit(32); off+=32;
  }
  if(off<taSize&&!dropping){out.set(ta.subarray(off,taSize),w); w+=taSize-off;}
  return out.subarray(0,w);
}
function applyTA(chain,inner){
  const dv=new DataView(inner.buffer,inner.byteOffset,inner.byteLength);
  const taSize=dv.getUint32(72,true), dPay=dv.getUint32(76,true);
  if(chain.buf.length<taSize){const n=new Uint8Array(taSize);n.set(chain.buf);chain.buf=n;}
  if(dPay===taSize){ chain.buf.set(inner.subarray(80,80+taSize),0); chain.has=true; }
  else if(chain.has){ let o=80; const end=80+dPay;
    while(o+4<=end){ const roff=dv.getUint32(o,true); o+=4; if(roff===0xFFFFFFFF)break;
      const rl=dv.getUint16(o,true); o+=2; chain.buf.set(inner.subarray(o,o+rl),roff); o+=rl; } }
  chain.size=taSize; return chain.has;
}
const md5=b=>crypto.createHash('md5').update(b).digest('hex');
const chainL={buf:new Uint8Array(0),size:0,has:false};
const chainZ={buf:new Uint8Array(0),size:0,has:false};
let stripPairs=0, stripExact=0, stripMismatch=0;
function stripGate(src,inner,frameNum){
  if(src==='zcst'){
    if(!zFlagsSeen)return;   // don't digest with a guessed charStrip predicate
    if(!applyTA(chainL,inner))return;
    chainL.dig=md5(taStripJS(chainL.buf.subarray(0,chainL.size),chainL.size,zCStrip));
    chainL.fn=frameNum;
  }else{
    if(!applyTA(chainZ,inner))return;
    chainZ.dig=md5(chainZ.buf.subarray(0,chainZ.size));
    chainZ.fn=frameNum;
  }
  if(chainL.dig&&chainZ.dig&&chainL.fn===chainZ.fn){
    stripPairs++;
    if(chainL.dig===chainZ.dig)stripExact++;
    else{stripMismatch++;
      if(stripMismatch===1){
        const ls=taStripJS(chainL.buf.subarray(0,chainL.size),chainL.size);
        const zs=chainZ.buf.subarray(0,chainZ.size);
        console.log(`frame ${frameNum}: STRIP-CHAIN MISMATCH localStrip=${ls.length}B wire=${zs.length}B`);
        import('fs').then(fs=>{ fs.writeFileSync('C:/Users/trist/projects/maplecast-flycast/_bwlab/_strip_local.bin',ls);
          fs.writeFileSync('C:/Users/trist/projects/maplecast-flycast/_bwlab/_strip_wire.bin',zs); });
      }
      else if(stripMismatch<4)console.log(`frame ${frameNum}: STRIP-CHAIN MISMATCH`);}
  }
}
let zSoaFrames=0, zStripFrames=0, zCStrip=false;
let zFlagsSeen=false;   // gate legacy-side strip digests until the wire's strip flags are KNOWN (connect race)
let zdec=null, zEpoch=-1, zSynced=false, zChunks=[], zLen=0, zSoa=false, zStrip=false;
function onZcs2(d) {
  const epoch=d[4], flags=d[5];
  zSoa=(flags&2)!==0; if(zSoa) zSoaFrames++;
  zStrip=(flags&4)!==0; if(zStrip) zStripFrames++;
  zCStrip=(flags&16)!==0; zFlagsSeen=true;
  const vfLen=(flags&32)?4:0;   // bit5: replica vframe stamp
  const innerSize = d[6] | d[7]<<8 | d[8]<<16 | d[9]<<24;
  if (flags & 1) {
    zdec = new ZStream((c)=>{ zChunks.push(c); zLen+=c.length; });
    zEpoch=epoch; zSynced=true; epochs++;
  } else if (!zSynced || epoch!==zEpoch) return;
  zBytes += d.length;
  const t0=performance.now();
  try { zdec.push(d.subarray(10+((d[5]&8)?132:0)+((d[5]&32)?4:0))); } catch(e){ console.log('fzstd err:',e.message); zSynced=false; return; }
  decMs += performance.now()-t0; decN++;
  if (zLen >= innerSize) {
    const inner = new Uint8Array(innerSize);
    let o=0; for (const c of zChunks){ const n=Math.min(c.length,innerSize-o); inner.set(c.subarray(0,n),o); o+=n; if(o>=innerSize) break; }
    zChunks=[]; zLen=0;
    const fin = zSoa ? soaInverse(inner) : inner;
    if (zStrip) {  // stripped frame: whole-frame pairing mismatches BY DESIGN;
                   // the gate is the strip-chain compare instead.
      const fn = fin[4]|fin[5]<<8|fin[6]<<16|fin[7]<<24;
      stripGate('zcs2', fin, fn>>>0);
    } else {
      verify('zcs2', fin);
    }
  }
}
function onZcst(d) {
  const sz = d[4] | d[5]<<8 | d[6]<<16 | d[7]<<24;
  if (sz > 1024*1024) return;   // compressed SYNC
  legBytes += d.length;
  try {
    const inner = zOneShot(d.subarray(8), new Uint8Array(sz));
    const fn = inner[4]|inner[5]<<8|inner[6]<<16|inner[7]<<24;
    stripGate('zcst', inner, fn>>>0);
    verify('zcst', inner);
  } catch(e){}
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
    if (zStripFrames>0)
      console.log(`STRIP GATE: stripFrames ${zStripFrames}  chainPairs ${stripPairs}  exact ${stripExact}  MISMATCH ${stripMismatch}`);
    const ok = mismatch===0 && stripMismatch===0 && (paired>100 || stripPairs>100);
    console.log(ok ? 'VERDICT: BYTE-EXACT PASS' : 'VERDICT: FAIL/INSUFFICIENT');
    clearInterval(iv); ws.close(); process.exit(ok ? 0 : 1);
  }
}, 1000);
