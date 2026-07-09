// _extract_ram.mjs — seed a 16MB RAM image from one .mcrr frame -> ram.bin
//   node _extract_ram.mjs cape.mcrr <frameIndex> [ram.bin]
import { readFileSync, writeFileSync } from 'node:fs';
const path = process.argv[2] || 'cape.mcrr';
const fi   = +(process.argv[3] || 0);
const out  = process.argv[4] || 'ram.bin';
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p=0; const u32=()=>{const v=dv.getUint32(p,true);p+=4;return v>>>0;};
if (u32()!==0x5252434D) throw new Error('bad MCRR');
const ver=u32(),nS=u32(),nD=u32(),nF=u32(),vb=u32(),pb=u32();u32();
const reg=()=>{const a=u32(),l=u32();let t='';for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);}p+=8;return{addr:a,len:l,tag:t};};
const sR=Array.from({length:nS},reg), dR=Array.from({length:nD},reg);
const ram=new Uint8Array(16*1024*1024);
let q=p+vb+pb;                             // skip vram + pvr, static data follows
for(const r of sR){ const bytes=buf.subarray(q,q+r.len); q+=r.len;
  if(r.tag==='ram16') ram.set(bytes,0); else ram.set(bytes, r.addr&0xFFFFFF); }
const frameStart=q;
// each frame record = magic(4)+vframe(4)+taSize(4)+dyn+tail ; capture_break stamped
// offset-8 = post-dyn tail length so we can skip to the requested frame.
p=frameStart; const frames=[];
for(let f=0;f<nF;f++){ const fm=u32(),vf=u32(),ts=u32(); const dof=p; for(const r of dR)p+=r.len; const tof=p; p+=ts; frames.push({vf,dof}); }
const fr=frames[fi]; if(!fr) throw new Error(`frame ${fi} out of range (${nF})`);
let off=fr.dof; for(const r of dR){ ram.set(buf.subarray(off,off+r.len), r.addr&0xFFFFFF); off+=r.len; }
writeFileSync(out, Buffer.from(ram));
console.error(`wrote ${out} (16MB) from frame ${fi} vframe=${fr.vf}`);
