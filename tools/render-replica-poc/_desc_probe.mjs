import { readFileSync } from 'node:fs';
const CAP='_live_fx3.gsta.mcrr', FI=parseInt(process.argv[2]||'854',10);
const NODES=['8c282524','8c287884','8c268340'].map(s=>parseInt(s,16)>>>0);
const buf=new Uint8Array(readFileSync(CAP)); const dv=new DataView(buf.buffer,buf.byteOffset,buf.byteLength);
let p=0;const u32=()=>{const v=dv.getUint32(p,true);p+=4;return v>>>0;};
if(u32()!==0x5252434D)throw'bad';u32();const nS=u32(),nD=u32(),nF=u32(),vB=u32(),pB=u32();u32();
const reg=()=>{const a=u32(),l=u32();let t='';for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);}p+=8;return{addr:a>>>0,len:l,tag:t};};
const sR=Array.from({length:nS},reg),dR=Array.from({length:nD},reg);const headerEnd=p;const G=a=>(a>>>0)&0xFFFFFF;
const ram=new Uint8Array(16*1024*1024);
{let sp=headerEnd+vB+pB;for(const r of sR){const b=buf.subarray(sp,sp+r.len);sp+=r.len;if(r.tag==='ram16')ram.set(b,0);else ram.set(b,G(r.addr));}}
const frames=[];{let fp=headerEnd+vB+pB;for(const r of sR)fp+=r.len;for(let f=0;f<nF;f++){const taSize=dv.getUint32(fp+8,true);const dynOff=fp+12;let q=dynOff;for(const r of dR)q+=r.len;const nGfx=dv.getUint32(q,true);if(nGfx<=64){q+=4;for(let g=0;g<nGfx&&q+8<=buf.length;g++){const len=dv.getUint32(q+4,true);q+=8+len;}}if(q+4<=buf.length){const palLen=dv.getUint32(q,true);q+=4;if(palLen&&q+palLen<=buf.length)q+=palLen;}if(q+8<=buf.length&&dv.getUint32(q,true)===0x48554451){q+=4;const nHud=dv.getUint32(q,true);q+=4+nHud*96;}q+=taSize;frames.push({dynOff});fp=q;}}
const fr=frames[FI];{let dp=fr.dynOff;for(const r of dR){ram.set(buf.subarray(dp,dp+r.len),G(r.addr));dp+=r.len;}}
const u8r=a=>ram[G(a)];const u16r=a=>(ram[G(a)]|(ram[G(a)+1]<<8))>>>0;const u32r=a=>(ram[G(a)]|(ram[G(a)+1]<<8)|(ram[G(a)+2]<<16)|(ram[G(a)+3]<<24))>>>0;
const TILEDESC=0x8C1F9F9C;
console.log(`tiledesc shipped? region present`);
for(const NODE of NODES){
  const dc=u16r(NODE+0xDC); const cat=u8r(NODE+0x3); const sel=u16r(NODE+0x144);
  console.log(`\nNODE ${NODE.toString(16)} cat=${cat} sel=${sel.toString(16)} node+0xDC(=record base)=${dc}`);
  // for the first 4 records of this node, the inner-loop count = u8(tiledesc[(dc+k)].byte1)+1
  for(let k=0;k<4;k++){const e=TILEDESC+(dc+k)*4;const b0=u8r(e),b1=u8r(e+1),b2=u8r(e+2),b3=u8r(e+3);console.log(`  desc[${dc+k}] @${e.toString(16)} = [${b0},${b1},${b2},${b3}]  innerTileCount=u8(b1)+1=${b1+1}`);}
}
