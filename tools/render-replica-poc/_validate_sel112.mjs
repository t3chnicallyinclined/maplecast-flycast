import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { decodeA } from '../../web/render-replica/body_decoder.mjs';
const GFX_DIR=fileURLToPath(new URL('../../web/render-replica/gfx/',import.meta.url));
const g1=new Uint8Array(readFileSync(GFX_DIR+'PL34_gfx1.bin'));
const ram=new Uint8Array(16*1024*1024);const gbase=0x420040;ram.set(g1,gbase);
const u32r=a=>(ram[a]|(ram[a+1]<<8)|(ram[a+2]<<16)|(ram[a+3]<<24))>>>0;
function twop(x,y,bx,by){let r=0,b=0;const sq=Math.min(bx,by);for(let i=0;i<sq;i++){r|=((x>>i)&1)<<b;b++;r|=((y>>i)&1)<<b;b++;}if(bx>by)r|=(x>>sq)<<b;else if(by>bx)r|=(y>>sq)<<b;return r;}
function check(sel){const off=u32r(gbase+sel*4);const pb=gbase+off;const W=ram[pb+2]*8,H=ram[pb+3]*8;
  const srt=[];{const n=u32r(gbase)>>2;const s=new Set();for(let i=0;i<n;i++)s.add(u32r(gbase+i*4));for(const v of s)srt.push(v);srt.sort((a,b)=>a-b);}
  let end=pb;for(const o of srt){if((gbase+o)>pb){end=gbase+o;break;}}if(end<=pb)end=pb+0x8000;
  const full=decodeA(ram,pb+4,end,(W*H)>>1);const bx=Math.log2(W),by=Math.log2(H);
  const img=new Uint8Array(W*H);for(let y=0;y<H;y++)for(let x=0;x<W;x++){const ti=twop(x,y,bx,by);img[y*W+x]=(ti&1)?(full[ti>>1]>>4)&0xF:full[ti>>1]&0xF;}
  // simulate 4x4 m32 carve+readback (same code path as body_decoder)
  const cols=W/32,rows=H/32,m=32;const asm=new Uint8Array(W*H);
  for(let row=0;row<rows;row++)for(let col=0;col<cols;col++){
    const tile=new Uint8Array(0x200);for(let y=0;y<m;y++)for(let x=0;x<m;x++){const v=img[(row*m+y)*W+(col*m+x)];const ti=twop(x,y,5,5);if(ti&1)tile[ti>>1]=(tile[ti>>1]&0x0F)|(v<<4);else tile[ti>>1]=(tile[ti>>1]&0xF0)|v;}
    for(let y=0;y<m;y++)for(let x=0;x<m;x++){const ti=twop(x,y,5,5);const b=tile[ti>>1];const v=(ti&1)?(b>>4)&0xF:b&0xF;asm[(row*m+y)*W+(col*m+x)]=v;}}
  let diff=0,nz=0;for(let i=0;i<W*H;i++){if(img[i])nz++;if(img[i]!==asm[i])diff++;}
  console.log(`sel${sel} ${W}x${H} ${cols}x${rows} m=${m}  diff=${diff}/${W*H} nz=${nz} ${diff===0?'BYTE-EXACT ✓':'MISMATCH'}`);}
check(112);check(124);
