// Validate the re-tile occupancy across PL34 + PL17 multi-tile sels vs each char's part-atlas.
import { readFileSync } from 'node:fs';
import { decodeA } from '../../web/render-replica/body_decoder.mjs';
import zlib from 'node:zlib';
function twiddleSlow(x,y,xs,ys){let rv=0,sh=0;xs>>=1;ys>>=1;while(xs||ys){if(ys){rv|=(y&1)<<sh;ys>>=1;y>>=1;sh++;}if(xs){rv|=(x&1)<<sh;xs>>=1;x>>=1;sh++;}}return rv;}
const DETW=[[],[]];for(let s=0;s<11;s++){const ys=1<<s;DETW[0][s]=new Int32Array(1024);DETW[1][s]=new Int32Array(1024);for(let i=0;i<1024;i++){DETW[0][s][i]=twiddleSlow(i,0,1024,ys);DETW[1][s][i]=twiddleSlow(0,i,ys,1024);}}
const ORD=[[0,0],[0,1],[1,0],[1,1],[0,2],[0,3],[1,2],[1,3],[2,0],[2,1],[3,0],[3,1],[2,2],[2,3],[3,2],[3,3]];
function detwiddle(data,w,h){const bcx=Math.log2(w)|0,bcy=Math.log2(h)|0;const idx=new Uint8Array(w*h);for(let y=0;y<h;y+=4)for(let x=0;x<w;x+=4){const blk=((DETW[0][bcy][x]+DETW[1][bcx][y])/16)|0;const base=blk*8;for(let i=0;i<16;i++){const[cx,cy]=ORD[i];const b=base+(i>>1)<data.length?data[base+(i>>1)]:0;idx[(y+cy)*w+(x+cx)]=(i&1)?((b>>4)&0xF):(b&0xF);}}return idx;}
function readPNG(path){const b=new Uint8Array(readFileSync(path));let p=8,W=0,H=0,idat=[];while(p<b.length){const len=(b[p]<<24)|(b[p+1]<<16)|(b[p+2]<<8)|b[p+3];const t=String.fromCharCode(b[p+4],b[p+5],b[p+6],b[p+7]);if(t==='IHDR'){W=(b[p+8]<<24)|(b[p+9]<<16)|(b[p+10]<<8)|b[p+11];H=(b[p+12]<<24)|(b[p+13]<<16)|(b[p+14]<<8)|b[p+15];}if(t==='IDAT')idat.push(b.subarray(p+8,p+8+len));if(t==='IEND')break;p+=12+len;}const raw=zlib.inflateSync(Buffer.concat(idat.map(x=>Buffer.from(x))));const out=new Uint8Array(W*H*4);const st=W*4;for(let y=0;y<H;y++){const f=raw[y*(st+1)];const ro=y*(st+1)+1;for(let x=0;x<st;x++){let v=raw[ro+x];const a=x>=4?out[y*st+x-4]:0;const bb=y>0?out[(y-1)*st+x]:0;if(f===1)v=(v+a)&255;else if(f===2)v=(v+bb)&255;else if(f===3)v=(v+((a+bb)>>1))&255;else if(f===4){const c=(x>=4&&y>0)?out[(y-1)*st+x-4]:0;let pp=a+bb-c,pa=Math.abs(pp-a),pb2=Math.abs(pp-bb),pc=Math.abs(pp-c);v=(v+((pa<=pb2&&pa<=pc)?a:(pb2<=pc?bb:c)))&255;}out[y*st+x]=v;}}return{W,H,rgba:out};}
function testChar(hex){
  const gfx1=new Uint8Array(readFileSync(`../../web/render-replica/gfx/${hex}_gfx1.bin`));
  const dvg=new DataView(gfx1.buffer);const r32=a=>dvg.getUint32(a,true),r8=a=>gfx1[a];
  const n=r32(0)>>>2;const offs=[];for(let i=0;i<n;i++)offs.push(r32(i*4));
  const srt=Array.from(new Set(offs)).sort((a,b)=>a-b);const endOf=off=>{for(const s of srt)if(s>off)return s;return off+0x4000;};
  const pj=JSON.parse(readFileSync(`../../web/test-atlas/chars/${hex}_parts.json`,'utf8'));
  const png=readPNG(`../../web/test-atlas/chars/${hex}_parts.png`);
  let nMulti=0, fail=0, totPx=0, samePx=0;
  for(const k in pj){ const sel=+k; const rect=pj[k]; if(!rect||rect.w<=0)continue;
    const cols=Math.ceil(rect.w/32),rows=Math.ceil(rect.h/32); if(cols*rows<2)continue; // multi-tile only
    if(sel>=offs.length)continue;
    const pb=offs[sel];const sw=r8(pb+2),sh=r8(pb+3);const W=sw*8,H=sh*8;if(W<=0||H<=0||W>1024||H>1024)continue;
    if(W!==rect.w||H!==rect.h)continue; // dims must agree
    const dl=(W*H)>>1; const lin=detwiddle(decodeA(gfx1,pb+4,endOf(pb),dl),W,H);
    nMulti++; let st=0,sm=0;
    for(let row=0;row<rows;row++)for(let col=0;col<cols;col++)for(let yy=0;yy<32;yy++)for(let xx=0;xx<32;xx++){
      const px=col*32+xx,py=row*32+yy; if(px>=W||py>=H)continue; const ax=rect.x+px,ay=rect.y+py;
      const occA=png.rgba[(ay*png.W+ax)*4+3]>16?1:0; const occT=lin[py*W+px]!==0?1:0; st++; if(occA===occT)sm++;
    }
    totPx+=st; samePx+=sm; if(sm/st<0.98)fail++;
  }
  console.log(`${hex}: ${nMulti} multi-tile parts, occupancy ${(100*samePx/totPx).toFixed(2)}%, parts<98%=${fail}`);
}
testChar('PL34'); testChar('PL17');
