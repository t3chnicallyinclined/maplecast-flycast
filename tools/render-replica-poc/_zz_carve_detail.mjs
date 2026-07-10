// _zz_carve_detail.mjs — per-tile dump for ONE dense part: which chunk each order assigns,
// whether the region is nonzero, and which candidate matches the whole-part Y-first detwiddle.
// Usage: node _zz_carve_detail.mjs PL34 570      (char, sel)
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
const GFX = fileURLToPath(new URL('../../web/render-replica/gfx/', import.meta.url));
function _twiddleSlow(x,y,xs,ys){let rv=0,sh=0;xs>>=1;ys>>=1;while(xs||ys){if(ys){rv|=(y&1)<<sh;ys>>=1;y>>=1;sh++;}if(xs){rv|=(x&1)<<sh;xs>>=1;x>>=1;sh++;}}return rv;}
const _DETW=[[],[]];for(let s=0;s<11;s++){const ys=1<<s;_DETW[0][s]=new Int32Array(1024);_DETW[1][s]=new Int32Array(1024);for(let i=0;i<1024;i++){_DETW[0][s][i]=_twiddleSlow(i,0,1024,ys);_DETW[1][s][i]=_twiddleSlow(0,i,ys,1024);}}
const _ORD=[[0,0],[0,1],[1,0],[1,1],[0,2],[0,3],[1,2],[1,3],[2,0],[2,1],[3,0],[3,1],[2,2],[2,3],[3,2],[3,3]];
const _l2=v=>{let n=-1;while(v){v>>=1;n++;}return n;};
function detw(data,w,h){const bcx=_l2(w),bcy=_l2(h);const idx=new Uint8Array(w*h);for(let y=0;y<h;y+=4)for(let x=0;x<w;x+=4){const blk=((_DETW[0][bcy][x]+_DETW[1][bcx][y])/16)|0,base=blk*8;for(let i=0;i<16;i++){const cx=_ORD[i][0],cy=_ORD[i][1],b=(base+(i>>1)<data.length)?data[base+(i>>1)]:0;idx[(y+cy)*w+(x+cx)]=(i&1)?((b>>4)&0xF):(b&0xF);}}return idx;}
function decodeA(src,sp,srcEnd,destLen){const out=new Uint8Array(destLen);let o=0,bc=0,flags=0;while(o<destLen&&sp<srcEnd){if(bc===0){flags=src[sp++];bc=0x80;if(sp>=srcEnd)break;}if((flags&bc)===0){out[o++]=src[sp++];}else{const b=src[sp++];let s=o-(b>>4)-1;const cnt=(b&0x0F)+2;for(let k=0;k<cnt&&o<destLen;k++,s++)out[o++]=(s>=0&&s<o)?out[s]:0;}bc>>=1;}return out;}
function rowBandMajor(col,row,Tw,Th){const by=row&~1;const bh=(Th-by<2)?(Th-by):2;return by*Tw+col*bh+(row-by);}
function colPairChunk(col,row,Tw,Th){let t=0;for(let cp=0;cp<Tw;cp+=2){const cw=(Tw-cp<2)?(Tw-cp):2;for(let by=0;by<Th;by+=2){const bh=(Th-by<2)?(Th-by):2;for(let cx2=0;cx2<cw;cx2++)for(let ry=0;ry<bh;ry++){if(cp+cx2===col&&by+ry===row)return t;t++;}}}return -1;}
function twTileYFirst(col,row,Tw,Th){let rv=0,sh=0,xs=Tw>>1,ys=Th>>1,x=col,y=row;while(xs||ys){if(ys){rv|=(y&1)<<sh;ys>>=1;y>>=1;sh++;}if(xs){rv|=(x&1)<<sh;xs>>=1;x>>=1;sh++;}}return rv;}
const u8=(b,a)=>b[a],u32=(b,a)=>(b[a]|(b[a+1]<<8)|(b[a+2]<<16)|(b[a+3]<<24))>>>0;
const hex=process.argv[2].toUpperCase(), SEL=parseInt(process.argv[3],10);
const g1=new Uint8Array(readFileSync(GFX+hex+'_gfx1.bin'));
const tb=u32(g1,0), n=tb>>>2; const offs=new Uint32Array(n); for(let i=0;i<n;i++)offs[i]=u32(g1,i*4);
const srt=Uint32Array.from(new Set(offs)).sort((a,b)=>a-b);
const endOf=off=>{let lo=0,hi=srt.length;while(lo<hi){const m=(lo+hi)>>1;if(srt[m]<=off)lo=m+1;else hi=m;}return lo<srt.length?srt[lo]:off+0x4000;};
const off=offs[SEL], sw=u8(g1,off+2), sh=u8(g1,off+3), W=sw*8, H=sh*8, Tw=W/32|0, Th=H/32|0;
const raw=decodeA(g1,off+4,endOf(off),(W*H)>>1);
const ref=detw(raw,W,H);
function chunkBad(k,col,row){const o=k*512;const chunk=(k>=0&&o+512<=raw.length)?raw.subarray(o,o+512):new Uint8Array(512);const til=detw(chunk,32,32);for(let y=0;y<32;y++)for(let x=0;x<32;x++)if(til[y*32+x]!==ref[(row*32+y)*W+(col*32+x)])return 1;return 0;}
function nzRegion(col,row){for(let y=0;y<32;y++)for(let x=0;x<32;x++)if(ref[(row*32+y)*W+(col*32+x)])return 1;return 0;}
console.log(`${hex} sel${SEL}  ${W}x${H}  ${Tw}x${Th}  (raw ${raw.length}B = ${raw.length/512} chunks)`);
console.log('col row | nz | cpK rbK yfK |  colPair rowBand yFirst  (X=bad tile vs whole-part detwiddle)');
let cpBad=0,rbBad=0,yfBad=0;
for(let row=0;row<Th;row++)for(let col=0;col<Tw;col++){
  const nz=nzRegion(col,row);
  const cpK=colPairChunk(col,row,Tw,Th),rbK=rowBandMajor(col,row,Tw,Th),yfK=twTileYFirst(col,row,Tw,Th);
  const cB=chunkBad(cpK,col,row),rB=chunkBad(rbK,col,row),yB=chunkBad(yfK,col,row);
  if(cB)cpBad++;if(rB)rbBad++;if(yB)yfBad++;
  const mark=b=>b?' X ':' . ';
  const diff=(cpK!==yfK)?'  <== colPair!=yFirst':'';
  console.log(`  ${col}   ${row}  |  ${nz} | ${String(cpK).padStart(2)}  ${String(rbK).padStart(2)}  ${String(yfK).padStart(2)} |  ${mark(cB)}    ${mark(rB)}    ${mark(yB)}${diff}`);
}
console.log(`TOTAL bad tiles:  colPair=${cpBad}  rowBand=${rbBad}  yFirst=${yfBad}   (of ${Tw*Th})`);
