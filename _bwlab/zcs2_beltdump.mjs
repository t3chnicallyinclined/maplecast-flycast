// Repro tool: exact worker decode logic; on first frameSize-belt failure, dump
// the v2 inner + inverse output + msg header for offline analysis.
import WebSocket from 'ws';
import fs from 'fs';
import { Decompress as ZStream } from '../web/webgpu/fzstd.mjs';
const URL_=process.argv[2]||'ws://127.0.0.1:7200';
let dec=null, epoch=-1, synced=false, chunks=[], clen=0, n=0, dumped=false;
function soaInverse(v){
  const dv=new DataView(v.buffer,v.byteOffset,v.byteLength);
  const v2Sec=dv.getUint32(76,true), nRuns=dv.getUint32(80,true);
  const offs=84, lens=offs+nRuns*4, dat=lens+nRuns*2;
  let dataB=0; for(let i=0;i<nRuns;i++) dataB+=dv.getUint16(lens+i*2,true);
  const tailOff=80+v2Sec, legacySec=nRuns*6+dataB+4;
  console.log(`  [inv] v2Sec=${v2Sec} nRuns=${nRuns} dataB=${dataB} tailOff=${tailOff} vlen=${v.length} tail=${v.length-tailOff} legacySec=${legacySec} out=${80+legacySec+(v.length-tailOff)}`);
  const out=new Uint8Array(80+legacySec+(v.length-tailOff));
  out.set(v.subarray(0,80),0);
  const ov=new DataView(out.buffer); ov.setUint32(76,legacySec,true);
  let o=80, dp=dat, prev=0;
  for(let i=0;i<nRuns;i++){ prev+=dv.getUint32(offs+i*4,true);
    const rl=dv.getUint16(lens+i*2,true);
    ov.setUint32(o,prev,true); ov.setUint16(o+4,rl,true); o+=6;
    out.set(v.subarray(dp,dp+rl),o); o+=rl; dp+=rl; }
  ov.setUint32(o,0xFFFFFFFF,true); o+=4;
  out.set(v.subarray(tailOff),o);
  return out;
}
const ws=new WebSocket(URL_); ws.binaryType='nodebuffer';
ws.on('message',(buf,isBin)=>{
  if(!isBin) return; const d=new Uint8Array(buf);
  if(!(d.length>=10&&d[0]===0x5A&&d[1]===0x43&&d[2]===0x53&&d[3]===0x32)) return;
  const flags=d[5], ep=d[4], soa=(flags&2)!==0, strip=(flags&4)!==0;
  const innerSize=(d[6]|d[7]<<8|d[8]<<16|d[9]<<24)>>>0;
  if(flags&1){ chunks=[];clen=0; dec=new ZStream(c=>{chunks.push(c);clen+=c.length;}); epoch=ep; synced=true; }
  else if(!synced||ep!==epoch){ return; }
  try{ dec.push(d.subarray(10+((d[5]&8)?132:0))); }catch(e){ console.log('fzstd',e.message); synced=false; return; }
  if(clen>=innerSize){
    const inner=new Uint8Array(innerSize); let o=0;
    for(const c of chunks){ const m=Math.min(c.length,innerSize-o); inner.set(c.subarray(0,m),o); o+=m; if(o>=innerSize)break; }
    const extra=clen-innerSize; chunks=[]; clen=0;
    if(extra>0){ console.log('overrun',extra); synced=false; return; }
    n++;
    const fin= soa? soaInverse(inner): inner;
    const fsz=(fin[0]|fin[1]<<8|fin[2]<<16|fin[3]<<24)>>>0;
    if(fsz+4!==fin.length){
      console.log(`FAIL frame#${n} soa=${soa} strip=${strip} innerSize=${innerSize} fsz=${fsz} finLen=${fin.length}`);
      if(!dumped){ dumped=true;
        fs.writeFileSync('C:/Users/trist/projects/maplecast-flycast/_bwlab/_belt_v2.bin', inner);
        fs.writeFileSync('C:/Users/trist/projects/maplecast-flycast/_bwlab/_belt_fin.bin', fin);
        console.log('dumped _belt_v2.bin/_belt_fin.bin'); process.exit(1);
      }
    }
  }
});
setTimeout(()=>{ console.log('OK: '+n+' frames, no belt failures'); process.exit(0); },30000);
