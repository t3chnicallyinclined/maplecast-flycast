// _render_sprites_only.mjs — render ONLY the ParaType-5 sprites from a capture, zoomed,
// to isolate the sprite-render result from the (working) para4 polys.
//   node _render_sprites_only.mjs --dir _hud_cap_fresh3 --out _hud_cap_fresh3/_SPRITES.png
import './webgpu-headless.mjs';
import { initDevice } from './webgpu-headless.mjs';
import { readFileSync, writeFileSync } from 'node:fs';
import { PNG } from 'pngjs';
const W_DIR = new URL('../../web/webgpu/', import.meta.url);
const { PVR2Renderer }   = await import(new URL('pvr2-renderer.mjs', W_DIR));
const { TAParser }       = await import(new URL('ta-parser.mjs', W_DIR));
const { TextureManager } = await import(new URL('texture-manager.mjs', W_DIR));

function arg(n,d){const i=process.argv.indexOf(n);return i>=0?process.argv[i+1]:d;}
const DIR=arg('--dir','_hud_cap_fresh3');
const OUT=arg('--out',`${DIR}/_SPRITES.png`);
const ORDER=arg('--order','0,1,2,3').split(',').map(Number);
const W=640,H=480,HUDQ_MAGIC=0x48554451;

function extractBuildHudTA(htmlPath){
  const html=readFileSync(htmlPath,'utf8');
  const i=html.indexOf('function buildHudTA(');let d=0,s=false,j=i;
  for(;j<html.length;j++){if(html[j]==='{'){d++;s=true;}else if(html[j]==='}'){d--;if(s&&d===0){j++;break;}}}
  return new Function('window','quads', html.slice(i,j)+'\nreturn buildHudTA(quads);');
}
const buildHudTA = extractBuildHudTA(new URL('../../web/render-replica/replay.html', import.meta.url));

function parseHud(buf){const dv=new DataView(buf.buffer,buf.byteOffset,buf.byteLength);
  const n=dv.getUint32(4,true);const q=[];let p=8;
  for(let i=0;i<n;i++){const x=[0,1,2,3].map(k=>dv.getFloat32(p+k*4,true));const y=[0,1,2,3].map(k=>dv.getFloat32(p+16+k*4,true));
    const u=[0,1,2,3].map(k=>dv.getFloat32(p+32+k*4,true));const v=[0,1,2,3].map(k=>dv.getFloat32(p+48+k*4,true));
    const col=[0,1,2,3].map(k=>dv.getUint32(p+64+k*4,true));
    const pcw=dv.getUint32(p+80,true),isp=dv.getUint32(p+84,true),tsp=dv.getUint32(p+88,true),tcw=dv.getUint32(p+92,true);
    q.push({x,y,u,v,col,pcw,isp,tsp,tcw});p+=96;}
  return q;}
function synthSnap(w,h){const s=new Uint32Array(16);const tx=(Math.round(w/32)-1)&0x3F,ty=(Math.round(h/32)-1)&0x3F;s[0]=tx|(ty<<16);return s;}
function makeRT(d,fmt,w,h){const c=d.createTexture({size:[w,h],format:fmt,usage:GPUTextureUsage.RENDER_ATTACHMENT|GPUTextureUsage.COPY_SRC});const z=d.createTexture({size:[w,h],format:'depth32float',usage:GPUTextureUsage.RENDER_ATTACHMENT});return{color:c,depth:z,colorView:c.createView(),depthView:z.createView(),width:w,height:h};}
async function readback(d,t,w,h,fmt){const bpr=Math.ceil(w*4/256)*256;const rb=d.createBuffer({size:bpr*h,usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ});const e=d.createCommandEncoder();e.copyTextureToBuffer({texture:t},{buffer:rb,bytesPerRow:bpr,rowsPerImage:h},[w,h,1]);d.queue.submit([e.finish()]);await rb.mapAsync(GPUMapMode.READ);const m=new Uint8Array(rb.getMappedRange()).slice();rb.unmap();rb.destroy();const out=new Uint8Array(w*h*4);const bgra=fmt.startsWith('bgra');for(let y=0;y<h;y++)for(let x=0;x<w;x++){const s=y*bpr+x*4,dd=(y*w+x)*4;if(bgra){out[dd]=m[s+2];out[dd+1]=m[s+1];out[dd+2]=m[s];out[dd+3]=m[s+3];}else{out[dd]=m[s];out[dd+1]=m[s+1];out[dd+2]=m[s+2];out[dd+3]=m[s+3];}}return out;}

const {device}=await initDevice();
let quads=parseHud(new Uint8Array(readFileSync(`${DIR}/hudq_tail.bin`)));
quads = quads.filter(q => ((q.pcw>>>29)&7) === 5);   // ONLY sprites
console.log(`sprites: ${quads.length}`);
const vram=new Uint8Array(readFileSync(`${DIR}/vram_prefix.bin`));
let pvr;try{pvr=new Uint8Array(readFileSync(`${DIR}/pvr_prefix.bin`));}catch{pvr=new Uint8Array(32768);}

const R=new PVR2Renderer();R.dev=device;R.fmt='rgba8unorm';R._init(W,H);
const rt=makeRT(device,R.fmt,W,H);const T=new TextureManager(device);
T.setDirtyPages(null,true);T.updatePalette(pvr);
const ta=buildHudTA({_hudQuadOrder:ORDER},quads);
const P=new TAParser();const parsed=P.parse(ta,ta.length);
R.renderFrame(parsed,T,synthSnap(W,H),vram,{singlePass:true,noSort:true,drawOpaque:false,drawPunch:false,drawTrans:true},rt);
device.queue.submit([R._lastEncoder.finish()]);
const rgba=await readback(device,rt.color,W,H,R.fmt);
for(let i=0;i<rgba.length;i+=4){const a=rgba[i+3]/255;rgba[i]=rgba[i]*a+30*(1-a);rgba[i+1]=rgba[i+1]*a+30*(1-a);rgba[i+2]=rgba[i+2]*a+30*(1-a);rgba[i+3]=255;}
// crop bottom band y=400..480 full width, 3x
const Y0=395,Y1=480,bh=Y1-Y0,sc=3;
const cw=W*sc, ch=bh*sc, out=new Uint8Array(cw*ch*4);
for(let y=0;y<ch;y++)for(let x=0;x<cw;x++){const sy=(y/sc|0)+Y0, sx=(x/sc|0);const s=(sy*W+sx)*4,d=(y*cw+x)*4;out[d]=rgba[s];out[d+1]=rgba[s+1];out[d+2]=rgba[s+2];out[d+3]=255;}
const png=new PNG({width:cw,height:ch});png.data=Buffer.from(out.buffer,out.byteOffset,out.byteLength);
writeFileSync(OUT,PNG.sync.write(png));
console.log(`[done] order=[${ORDER}] -> ${OUT}`);
