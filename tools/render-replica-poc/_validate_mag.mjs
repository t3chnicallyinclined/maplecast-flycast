import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
import { ensureBodyTextures, decodeA } from '../../web/render-replica/body_decoder.mjs';
const path='../../_sentinel_scramble.mcrr';
const GFX_DIR=fileURLToPath(new URL('../../web/render-replica/gfx/',import.meta.url));
const buf=new Uint8Array(readFileSync(path));const dv=new DataView(buf.buffer,buf.byteOffset,buf.byteLength);
let p=0;const u32=()=>{const v=dv.getUint32(p,true);p+=4;return v>>>0;};
u32();u32();const nS=u32(),nD=u32(),nF=u32(),vB=u32(),pB=u32();u32();
const reg=()=>{const a=u32(),l=u32();let t='';for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);}p+=8;return{addr:a>>>0,len:l,tag:t};};
const sR=Array.from({length:nS},reg),dR=Array.from({length:nD},reg);
const vramOff=p;p+=vB;const pvrOff=p;p+=pB;
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
const vram=new Uint8Array(8*1024*1024);const cache={};(()=>{const crP=M._malloc(quads*8);M._render_frame_quad_colrow(crP,quads);const cr=new Int32Array(M.HEAPU8.buffer.slice(crP,crP+quads*8));M._free(crP);return ensureBodyTextures(ram,vram,ta,quads,cache,sels,gfxs,cr);})();
const g=0xc810040,sel=285,W=64,H=128,bx=6,by=7,Tw=W/32,Th=H/32;
function twop(x,y,bx,by){let r=0,b=0;const sq=Math.min(bx,by);for(let i=0;i<sq;i++){r|=((x>>i)&1)<<b;b++;r|=((y>>i)&1)<<b;b++;}if(bx>by)r|=(x>>sq)<<b;else if(by>bx)r|=(y>>sq)<<b;return r;}
const off2=u32r(g+sel*4);const pb2=G(g)+off2;const srt=[];{const nn=u32r(g)>>2;const s=new Set();for(let i=0;i<nn;i++)s.add(u32r(g+i*4));for(const v of s)srt.push(v);srt.sort((a,b)=>a-b);}
let end=pb2;for(const o of srt){if((G(g)+o)>pb2){end=G(g)+o;break;}}if(end<=pb2)end=pb2+0x4000;
const full=decodeA(ram,pb2+4,end,(W*H)>>1);
const ref=new Uint8Array(W*H);for(let y=0;y<H;y++)for(let x=0;x<W;x++){const ti=twop(x,y,bx,by);ref[y*W+x]=(ti&1)?(full[ti>>1]>>4)&0xF:full[ti>>1]&0xF;}
const sl=[];for(let q=0;q<quads;q++){if((gfxs[q]&0xFFFFFF)!==(g&0xFFFFFF)||sels[q]!==sel)continue;const tcw=tdv.getUint32(q*96+0x0C,true);const a=((tcw&0x1FFFFF)<<3)>>>0;const ax=tdv.getFloat32(q*96+36,true),ay=tdv.getFloat32(q*96+40,true);sl.push({a,ax,ay});}
const xs=[...new Set(sl.map(t=>Math.round(t.ax)))].sort((a,b)=>b-a);const ys=[...new Set(sl.map(t=>Math.round(t.ay)))].sort((a,b)=>b-a);
const asm=new Uint8Array(W*H);
for(const t of sl){const col=xs.indexOf(Math.round(t.ax)),row=ys.indexOf(Math.round(t.ay));for(let y=0;y<32;y++)for(let x=0;x<32;x++){const ti=twop(x,y,5,5);const b=vram[t.a+(ti>>1)];const ni=(ti&1)?(b>>4)&0xF:b&0xF;asm[(row*32+y)*W+(col*32+x)]=ni;}}
let diff=0,refnz=0;for(let i=0;i<W*H;i++){if(ref[i])refnz++;if(asm[i]!==ref[i])diff++;}
console.log(`Magneto sel285 ${W}x${H} (${Tw}x${Th}) tiles=${sl.length}  diff=${diff}/${W*H}  refnz=${refnz}  ${diff===0?'BYTE-EXACT ✓':'MISMATCH'}`);
