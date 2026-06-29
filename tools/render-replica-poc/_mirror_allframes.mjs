import { readFileSync } from 'node:fs';
import './webgpu-headless.mjs';
const W=new URL('../../web/webgpu/',import.meta.url);
const {FrameDecoder}=await import(new URL('frame-decoder.mjs',W));
const file=readFileSync(process.argv[2]); const dv=new DataView(file.buffer,file.byteOffset,file.byteLength);
const D=new FrameDecoder(); let off=0; const msgs=[];
while(off+4<=file.length){const len=dv.getUint32(off,true);off+=4;if(off+len>file.length)break;if(len>0){const m=file.subarray(off,off+len);off+=len;if(m[0]===0x5A&&m[1]===0x43&&m[2]===0x53&&m[3]===0x54)msgs.push(m);}}
function cnt(buf){let o=0,n=buf.length,c=0;while(o+32<=n){const pcw=buf.readUInt32LE(o);if(((pcw>>29)&7)===5){if(o+96>n)break;c++;o+=96;}else o+=32;}return c;}
let fi=0;for(const m of msgs){let fr=null;try{fr=D.applyFrame(m);}catch(e){continue;}if(!fr)continue;fi++;const buf=Buffer.from(fr.taBuffer.buffer||fr.taBuffer,fr.taBuffer.byteOffset||0,fr.taBuffer.byteLength||fr.taBuffer.length);if(fi%50===0||fr.frameNum<1900||fr.frameNum>2095)console.log(`fnum=${fr.frameNum} sprites=${cnt(buf)}`);}
