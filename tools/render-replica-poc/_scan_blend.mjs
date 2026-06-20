// _scan_blend.mjs — full blend-signature histogram across all engine TA frames,
// plus per-frame distinct TCW vram-page set (to spot the effect-poly bank textures).
import { readFileSync } from 'node:fs';
import './webgpu-headless.mjs';
const W_DIR = new URL('../../web/webgpu/', import.meta.url);
const { FrameDecoder } = await import(new URL('frame-decoder.mjs', W_DIR));
const path = process.argv[2] || '_fxwin1.zcst';
const file = readFileSync(path);
const dv = new DataView(file.buffer, file.byteOffset, file.byteLength);
const D = new FrameDecoder();
let off=0; const msgs=[];
while(off+4<=file.length){const len=dv.getUint32(off,true);off+=4;if(off+len>file.length)break;
  if(len>0){const m=file.subarray(off,off+len);off+=len;if(m[0]===0x5A&&m[1]===0x43&&m[2]===0x53&&m[3]===0x54)msgs.push(m);}}
const SRC=['ZERO','ONE','OTHER','INVOTHER','SRCA','INVSRCA','DSTA','INVDSTA'];
const sigHist={}; let maxSprites=0,maxFrame=0;
function parseTA(buf){const out=[];let o=0;const n=buf.length;
  while(o+32<=n){const pcw=buf.readUInt32LE(o);const pt=(pcw>>29)&7;
    if(pt===5){if(o+96>n)break;const tsp=buf.readUInt32LE(o+8);const lt=(pcw>>24)&7;
      out.push({lt,srcI:(tsp>>29)&7,dstI:(tsp>>26)&7});o+=96;continue;}
    o+=32;}return out;}
let fi=0;
for(const m of msgs){let fr=null;try{fr=D.applyFrame(m);}catch(e){continue;}if(!fr)continue;fi++;
  const buf=Buffer.from(fr.taBuffer.buffer||fr.taBuffer,fr.taBuffer.byteOffset||0,fr.taBuffer.byteLength||fr.taBuffer.length);
  const sp=parseTA(buf);
  if(sp.length>maxSprites){maxSprites=sp.length;maxFrame=fi;}
  for(const s of sp){const k=`lt${s.lt} ${SRC[s.srcI]}->${SRC[s.dstI]}`;sigHist[k]=(sigHist[k]||0)+1;}
}
console.log(`frames=${fi}  maxSprites=${maxSprites} @frame#${maxFrame}`);
console.log('blend signatures (count over all frames):');
for(const [k,v] of Object.entries(sigHist).sort((a,b)=>b[1]-a[1])) console.log(`  ${k.padEnd(24)} ${v}`);
