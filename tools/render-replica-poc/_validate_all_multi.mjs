import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
import { ensureBodyTextures, decodeA } from '../../web/render-replica/body_decoder.mjs';
const path='../../_sentinel_scramble.mcrr';const GFX_DIR=fileURLToPath(new URL('../../web/render-replica/gfx/',import.meta.url));
const buf=new Uint8Array(readFileSync(path));const dv=new DataView(buf.buffer,buf.byteOffset,buf.byteLength);
let p=0;const u32=()=>{const v=dv.getUint32(p,true);p+=4;return v>>>0;};
u32();u32();const nS=u32(),nD=u32(),nF=u32(),vB=u32(),pB=u32();u32();
const reg=()=>{const a=u32(),l=u32();let t='';for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);}p+=8;return{addr:a>>>0,len:l,tag:t};};
const sR=Array.from({length:nS},reg),dR=Array.from({length:nD},reg);p+=vB;p+=pB;
const sD=sR.map(r=>{const b=buf.subarray(p,p+r.len);p+=r.len;return b;});
const fStart=p;const G=a=>(a>>>0)&0xFFFFFF;const ram=new Uint8Array(16*1024*1024);
sR.forEach((r,i)=>{if(r.tag==='ram16')ram.set(sD[i],0);else ram.set(sD[i],G(r.addr));});
p=fStart;const frames=[];
for(let f=0;f<nF;f++){u32();const vf=u32();const ts=u32();const dynOff=p;for(const r of dR)p+=r.len;const gfxOff=p;const nG=(p+4<=buf.length)?dv.getUint32(p,true):0;if(nG<=64){p+=4;for(let g=0;g<nG&&p+8<=buf.length;g++){const len=dv.getUint32(p+4,true);p+=8+len;}}const taOff=p;p+=ts;frames.push({vframe:vf,taSize:ts,dynOff,gfxOff,taOff});}
function aGfx(off){if(off+4>buf.length)return;const nG=dv.getUint32(off,true);off+=4;if(nG>64)return;for(let i=0;i<nG;i++){if(off+8>buf.length)break;const base=dv.getUint32(off,true);off+=4;const len=dv.getUint32(off,true);off+=4;if(len>0x800000||off+len>buf.length)break;ram.set(buf.subarray(off,off+len),G(base));off+=len;}}
const SLOTS=[0x8C268340,0x8C2688E4,0x8C268E88,0x8C26942C,0x8C2699D0,0x8C269F74];
const u8r=a=>ram[G(a)];const u32r=a=>(ram[G(a)]|(ram[G(a)+1]<<8)|(ram[G(a)+2]<<16)|(ram[G(a)+3]<<24))>>>0;
function aLocal(){const done=new Set();for(const b of SLOTS){if(u8r(b)===0)continue;const cid=u8r(b+1);const g1b=u32r(b+0x15C);if(!((g1b&0x0C000000)||(g1b&0x8C000000)))continue;const hex='PL'+cid.toString(16).toUpperCase().padStart(2,'0');let g1,g2;try{g1=new Uint8Array(readFileSync(GFX_DIR+hex+'_gfx1.bin'));g2=new Uint8Array(readFileSync(GFX_DIR+hex+'_gfx2.bin'));}catch{continue;}if(done.has(cid))continue;done.add(cid);ram.set(g1,G(g1b));ram.set(g2,G(u32r(b+0x160)));}}
const fr=frames[0];{let o=fr.dynOff;for(const r of dR){ram.set(buf.subarray(o,o+r.len),G(r.addr));o+=r.len;}}
aGfx(fr.gfxOff);aLocal();
const M=await createRenderFrame({locateFile:x=>x});
const rp=M._malloc(ram.length);M.HEAPU8.set(ram,rp);const op=M._malloc(262144);
const len=M._render_frame_ta(rp,op,262144);const quads=M._render_frame_quad_count();
const ta=M.HEAPU8.slice(op,op+len);const tdv=new DataView(ta.buffer,ta.byteOffset,ta.byteLength);
const sp=M._malloc(quads*2),gp=M._malloc(quads*4);M._render_frame_quad_sels(sp,quads);M._render_frame_quad_gfx1s(gp,quads);
const sels=new Uint16Array(M.HEAPU8.buffer.slice(sp,sp+quads*2));const gfxs=new Uint32Array(M.HEAPU8.buffer.slice(gp,gp+quads*4));
const crP=M._malloc(quads*8);M._render_frame_quad_colrow(crP,quads);const cr=new Int32Array(M.HEAPU8.buffer.slice(crP,crP+quads*8));
const vram=new Uint8Array(8*1024*1024);ensureBodyTextures(ram,vram,ta,quads,{},sels,gfxs,cr);
function twop(x,y,bx,by){let r=0,b=0;const sq=Math.min(bx,by);for(let i=0;i<sq;i++){r|=((x>>i)&1)<<b;b++;r|=((y>>i)&1)<<b;b++;}if(bx>by)r|=(x>>sq)<<b;else if(by>bx)r|=(y>>sq)<<b;return r;}
// for every (gfx1,sel) run with >1 tile, byte-check reassembly vs full decode
const runs=new Map();
for(let q=0;q<quads;q++){const g=gfxs[q]>>>0;if(!(g&0x0C000000)&&!(g&0x8C000000))continue;const sel=sels[q];const key=(g&0xFFFFFF).toString(16)+':'+sel;let r=runs.get(key);if(!r){r={g,sel,t:[]};runs.set(key,r);}const tcw=tdv.getUint32(q*96+0x0C,true);r.t.push({a:((tcw&0x1FFFFF)<<3)>>>0,col:cr[2*q]|0,row:cr[2*q+1]|0});}
let total=0,bad=0;
for(const r of runs.values()){if(r.t.length<=1)continue;
  const off=u32r(r.g+r.sel*4);const pb=G(r.g)+off;const W=ram[pb+2]*8,H=ram[pb+3]*8;
  const srt=[];{const nn=u32r(r.g)>>2;const s=new Set();for(let i=0;i<nn;i++)s.add(u32r(r.g+i*4));for(const v of s)srt.push(v);srt.sort((a,b)=>a-b);}
  let end=pb;for(const o of srt){if((G(r.g)+o)>pb){end=G(r.g)+o;break;}}if(end<=pb)end=pb+0x8000;
  const full=decodeA(ram,pb+4,end,(W*H)>>1);const ref=new Uint8Array(W*H);const bx=Math.log2(W),by=Math.log2(H);
  for(let y=0;y<H;y++)for(let x=0;x<W;x++){const ti=twop(x,y,bx,by);ref[y*W+x]=(ti&1)?(full[ti>>1]>>4)&0xF:full[ti>>1]&0xF;}
  let cols=1,rows=1;for(const t of r.t){if(t.col+1>cols)cols=t.col+1;if(t.row+1>rows)rows=t.row+1;}const m=(W/cols)|0;
  const asm=new Uint8Array(W*H);
  for(const t of r.t){for(let y=0;y<m;y++)for(let x=0;x<m;x++){const ti=twop(x,y,5,5);const b=vram[t.a+(ti>>1)];const v=(ti&1)?(b>>4)&0xF:b&0xF;const dx=t.col*m+x,dy=t.row*m+y;if(dx<W&&dy<H)asm[dy*W+dx]=v;}}
  let diff=0;for(let i=0;i<W*H;i++)if(asm[i]!==ref[i])diff++;total++;if(diff)bad++;
  console.log(`${diff===0?'OK ':'BAD'} sel${String(r.sel).padStart(3)} ${W}x${H} ${cols}x${rows} m=${m} tiles=${r.t.length} diff=${diff}/${W*H}`);}
console.log(`\n${total-bad}/${total} multi-tile runs BYTE-EXACT`);process.exit(bad?1:0);
