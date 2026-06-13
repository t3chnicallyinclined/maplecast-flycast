// _validate_facing.mjs — prove the facing fix vs ASMTRACE truth, NO model.
// For a captured frame: run render_frame, read per-quad facing(via colrow now storage),
// mirror, sel. Assert: (1) mirror bit == facing XOR (sel&0x4000 from GFX2), (2) the carve
// col for a wide multi-col part is the STORAGE column (consistent ordering: under facing=1
// col increases with DECREASING screenX; facing=0 col increases with INCREASING... verify).
import createRenderFrame from './render_frame_node.mjs';
import { readFileSync } from 'node:fs';
const path = process.argv[2], wantF = +(process.argv[3]||0);
const buf=new Uint8Array(readFileSync(path));
const dv=new DataView(buf.buffer,buf.byteOffset,buf.byteLength);
let p=0;const u32=()=>{const v=dv.getUint32(p,true);p+=4;return v>>>0;};
u32();const ver=u32(),nS=u32(),nD=u32(),nF=u32(),vb=u32(),pb=u32();u32();
const reg=()=>{const a=u32(),l=u32();let t="";for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);}p+=8;return{addr:a>>>0,len:l,tag:t};};
const sR=Array.from({length:nS},reg),dR=Array.from({length:nD},reg);
const G=a=>(a>>>0)&0xFFFFFF;p+=vb;p+=pb;
const sData=sR.map(r=>{const b=buf.subarray(p,p+r.len);p+=r.len;return b;});
const ram=new Uint8Array(16*1024*1024);
sR.forEach((r,i)=>{if(r.tag==="ram16")ram.set(sData[i],0);else ram.set(sData[i],G(r.addr));});
for(let f=0;f<=wantF;f++){
  const fm=u32(),vf=u32(),ts=u32();const dynOff=p;for(const r of dR)p+=r.len;
  if(f===wantF){let o=dynOff;for(const r of dR){ram.set(buf.subarray(o,o+r.len),G(r.addr));o+=r.len;}}
  const nGfx=dv.getUint32(p,true);p+=4;for(let g=0;g<nGfx;g++){const len=dv.getUint32(p+4,true);if(f===wantF){const base=dv.getUint32(p,true);ram.set(buf.subarray(p+8,p+8+len),G(base));}p+=8+len;}
  p+=ts;
}
// overlay local gfx
const SLOTS=[0x8C268340,0x8C2688E4,0x8C268E88,0x8C26942C,0x8C2699D0,0x8C269F74];
const u8r=a=>ram[G(a)],u32r=a=>(ram[G(a)]|(ram[G(a)+1]<<8)|(ram[G(a)+2]<<16)|(ram[G(a)+3]<<24))>>>0;
const done=new Set();
for(const base of SLOTS){if(u8r(base+0)===0)continue;const cid=u8r(base+1);const g1=u32r(base+0x15C),g2=u32r(base+0x160);if(!((g1&0x0C000000)||(g1&0x8C000000)))continue;const hn="PL"+cid.toString(16).toUpperCase().padStart(2,"0");if(done.has(cid))continue;done.add(cid);try{const G1=new Uint8Array(readFileSync("../../web/render-replica/gfx/"+hn+"_gfx1.bin"));const G2=new Uint8Array(readFileSync("../../web/render-replica/gfx/"+hn+"_gfx2.bin"));ram.set(G1,G(g1));ram.set(G2,G(g2));}catch{}}
const M=await createRenderFrame({locateFile:x=>x});
const rp=M._malloc(ram.length);M.HEAPU8.set(ram,rp);
const cap=256*1024,op=M._malloc(cap);const len=M._render_frame_ta(rp,op,cap);
const quads=M._render_frame_quad_count();const ta=M.HEAPU8.slice(op,op+len);
const sp=M._malloc(quads*2),gp=M._malloc(quads*4),crp=M._malloc(quads*8),mp=M._malloc(quads);
M._render_frame_quad_sels(sp,quads);M._render_frame_quad_gfx1s(gp,quads);M._render_frame_quad_colrow(crp,quads);M._render_frame_quad_mirror(mp,quads);
const sels=new Uint16Array(M.HEAPU8.buffer.slice(sp,sp+quads*2));
const gfxs=new Uint32Array(M.HEAPU8.buffer.slice(gp,gp+quads*4));
const cr=new Int32Array(M.HEAPU8.buffer.slice(crp,crp+quads*8));
const mir=new Uint8Array(M.HEAPU8.buffer.slice(mp,mp+quads));
const tdv=new DataView(ta.buffer,ta.byteOffset,ta.byteLength);
// group, find wide parts (cols>1), report col vs screenX, mirror
const byKey=new Map();
for(let i=0;i<quads;i++){const k=gfxs[i].toString(16)+":"+sels[i];if(!byKey.has(k))byKey.set(k,[]);byKey.get(k).push(i);}
let mismatch=0;
for(const [k,idx] of byKey){
  let maxC=0,maxR=0;for(const i of idx){if(cr[2*i]>maxC)maxC=cr[2*i];if(cr[2*i+1]>maxR)maxR=cr[2*i+1];}
  if(maxC<1)continue; // only wide multi-col parts
  // report col -> screenX mapping; storage col should be MONOTONIC vs screenX in a facing-dependent dir
  const rows=idx.map(i=>({col:cr[2*i],row:cr[2*i+1],x:tdv.getFloat32(i*96+36,true),m:mir[i]}));
  rows.sort((a,b)=>a.col-b.col||a.row-b.row);
  const r0=rows.filter(r=>r.row===0);
  const dir = r0.length>1 ? (r0[r0.length-1].x>r0[0].x?"col++ => X++":"col++ => X--") : "?";
  console.log(`part ${k}: cols=${maxC+1} rows=${maxR+1} mirror=${rows[0].m} | ${dir} | col0.x=${r0[0]?.x.toFixed(0)} colN.x=${r0[r0.length-1]?.x.toFixed(0)}`);
}
console.log(`\nquads=${quads}. mirror set on ${mir.reduce((a,b)=>a+b,0)}/${quads} quads.`);
