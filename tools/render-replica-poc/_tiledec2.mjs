import './webgpu-headless.mjs';
import { initDevice } from './webgpu-headless.mjs';
import { readFileSync, writeFileSync } from 'node:fs';
import { PNG } from 'pngjs';
const W_DIR = new URL('../../web/webgpu/', import.meta.url);
const { TextureManager } = await import(new URL('texture-manager.mjs', W_DIR));
const {device}=await initDevice();
const T=new TextureManager(device);
const vram=new Uint8Array(readFileSync('_hud_cap_namegap/vram_prefix.bin'));
const pvr=new Uint8Array(readFileSync('_hud_cap_namegap/pvr_prefix.bin'));
T.updatePalette(pvr);
// pull the actual name-sprite tcws from the capture
const buf=new Uint8Array(readFileSync('_hud_cap_namegap/hudq_tail.bin'));
const dv=new DataView(buf.buffer,buf.byteOffset,buf.byteLength);
const n=dv.getUint32(4,true);let p=8;const tcws=[];const tsps=[];
for(let i=0;i<n;i++){const pcw=dv.getUint32(p+80,true),tsp=dv.getUint32(p+88,true),tcw=dv.getUint32(p+92,true);
  if(((pcw>>>29)&7)===5){tcws.push(tcw);tsps.push(tsp);}p+=96;}
const N=Math.min(tcws.length,32);const film=new PNG({width:32*N,height:34});film.data.fill(30);
for(let k=0;k<N;k++){
  const tcw=tcws[k],tsp=tsps[k];const fmt=(tcw>>>27)&7,scan=(tcw>>>26)&1,addr=(tcw&0x1FFFFF)<<3,palSel=(tcw>>>21)&0x3F;
  const W=8<<((tsp>>>3)&7),H=8<<(tsp&7);const texU=((tsp>>>3)&7);
  const rgba=T._decode(vram,addr,fmt,W,H,palSel,scan,0,0,texU);
  if(!rgba){console.log(k,'null',W,H);continue;}
  for(let y=0;y<Math.min(H,32);y++)for(let x=0;x<Math.min(W,32);x++){const si=(y*W+x)*4,di=(y*32*N+(k*32+x))*4;const a=rgba[si+3]/255;
    film.data[di]=rgba[si]*a+30*(1-a);film.data[di+1]=rgba[si+1]*a+30*(1-a);film.data[di+2]=rgba[si+2]*a+30*(1-a);film.data[di+3]=255;}
}
writeFileSync('_hud_cap_namegap/_tiledec2.png',PNG.sync.write(film));
console.log('wrote _tiledec2.png; first tile fmt',(tcws[0]>>>27)&7,'palSel',(tcws[0]>>>21)&0x3F,'dims',8<<((tsps[0]>>>3)&7),'x',8<<(tsps[0]&7));
