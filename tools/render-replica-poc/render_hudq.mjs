// render_hudq.mjs — render a captured HUDQ tail through the SAME pvr2 path replay.html uses,
// against the shipped static VRAM/PVR prefix, headless via Dawn. Lets us see + iterate on the
// font garble / meter / tint without the live stream.
//
//   node render_hudq.mjs --hud _hud_cap/hudq_tail.bin --vram _hud_cap/vram_prefix.bin \
//        --pvr _hud_cap/pvr_frame.bin --out _hud_cap/hud_render.png [--order 0,1,2,3]
import './webgpu-headless.mjs';
import { initDevice } from './webgpu-headless.mjs';
import { readFileSync, writeFileSync } from 'node:fs';
import { PNG } from 'pngjs';

const W_DIR = new URL('../../web/webgpu/', import.meta.url);
const { PVR2Renderer }   = await import(new URL('pvr2-renderer.mjs', W_DIR));
const { TAParser }       = await import(new URL('ta-parser.mjs', W_DIR));
const { TextureManager } = await import(new URL('texture-manager.mjs', W_DIR));

function arg(n,d){const i=process.argv.indexOf(n);return i>=0?process.argv[i+1]:d;}
const HUD=arg('--hud','_hud_cap/hudq_tail.bin');
const VRAM=arg('--vram','_hud_cap/vram_prefix.bin');
const PVR=arg('--pvr','_hud_cap/pvr_frame.bin');
const OUT=arg('--out','_hud_cap/hud_render.png');
const ORDER=(arg('--order','0,1,2,3')).split(',').map(Number); // replay.html buildHudTA default (strip-order fix 2026-06-14)
const ONLY=arg('--only',null); // comma list of quad indices to draw
const W=640,H=480;
const HUDQ_MAGIC=0x48554451;

function parseHud(buf){
  const dv=new DataView(buf.buffer,buf.byteOffset,buf.byteLength);
  if(dv.getUint32(0,true)!==HUDQ_MAGIC) throw new Error('bad magic');
  const n=dv.getUint32(4,true); const q=[]; let p=8;
  for(let i=0;i<n;i++){
    const x=[0,1,2,3].map(k=>dv.getFloat32(p+k*4,true));
    const y=[0,1,2,3].map(k=>dv.getFloat32(p+16+k*4,true));
    const u=[0,1,2,3].map(k=>dv.getFloat32(p+32+k*4,true));
    const v=[0,1,2,3].map(k=>dv.getFloat32(p+48+k*4,true));
    const col=[0,1,2,3].map(k=>dv.getUint32(p+64+k*4,true));
    const pcw=dv.getUint32(p+80,true),isp=dv.getUint32(p+84,true),tsp=dv.getUint32(p+88,true),tcw=dv.getUint32(p+92,true);
    q.push({x,y,u,v,col,pcw,isp,tsp,tcw}); p+=96;
  }
  return q;
}

// buildHudTA — EXACT copy of replay.html's logic (order is a param here for calibration).
function buildHudTA(quads,order){
  const buf=new Uint8Array(quads.length*160+32);
  const dv=new DataView(buf.buffer); let o=0; const Z=1.0;
  for(const q of quads){
    const textured=(q.pcw>>3)&1;
    let pcw=(4<<29)|(2<<24)|(q.pcw&0x00FFFFFF);
    pcw=(pcw&~(3<<4))|(0<<4);
    pcw=(pcw&~1)|0;
    dv.setUint32(o,pcw,true);
    dv.setUint32(o+4,q.isp,true);
    dv.setUint32(o+8,q.tsp,true);
    dv.setUint32(o+12,textured?q.tcw:0,true);
    dv.setUint32(o+16,0,true);dv.setUint32(o+20,0,true);dv.setUint32(o+24,0,true);dv.setUint32(o+28,0,true);
    o+=32;
    for(let k=0;k<4;k++){
      const i=order[k],eos=(k===3)?1:0;
      dv.setUint32(o,(7<<29)|(eos<<28),true);
      dv.setFloat32(o+4,q.x[i],true);
      dv.setFloat32(o+8,q.y[i],true);
      dv.setFloat32(o+12,Z,true);
      dv.setFloat32(o+16,textured?q.u[i]:0,true);
      dv.setFloat32(o+20,textured?q.v[i]:0,true);
      dv.setUint32(o+24,q.col[i]>>>0,true);
      dv.setUint32(o+28,0,true);
      o+=32;
    }
  }
  dv.setUint32(o,0,true); o+=32;
  return buf.subarray(0,o);
}

function synthSnap(w,h){const s=new Uint32Array(16);const tx=(Math.round(w/32)-1)&0x3F,ty=(Math.round(h/32)-1)&0x3F;s[0]=tx|(ty<<16);return s;}
function makeRT(d,fmt,w,h){const c=d.createTexture({size:[w,h],format:fmt,usage:GPUTextureUsage.RENDER_ATTACHMENT|GPUTextureUsage.COPY_SRC});const z=d.createTexture({size:[w,h],format:'depth32float',usage:GPUTextureUsage.RENDER_ATTACHMENT});return{color:c,depth:z,colorView:c.createView(),depthView:z.createView(),width:w,height:h};}
async function readback(d,t,w,h,fmt){const bpr=Math.ceil(w*4/256)*256;const rb=d.createBuffer({size:bpr*h,usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ});const e=d.createCommandEncoder();e.copyTextureToBuffer({texture:t},{buffer:rb,bytesPerRow:bpr,rowsPerImage:h},[w,h,1]);d.queue.submit([e.finish()]);await rb.mapAsync(GPUMapMode.READ);const m=new Uint8Array(rb.getMappedRange()).slice();rb.unmap();rb.destroy();const out=new Uint8Array(w*h*4);const bgra=fmt.startsWith('bgra');for(let y=0;y<h;y++)for(let x=0;x<w;x++){const s=y*bpr+x*4,dd=(y*w+x)*4;if(bgra){out[dd]=m[s+2];out[dd+1]=m[s+1];out[dd+2]=m[s];out[dd+3]=m[s+3];}else{out[dd]=m[s];out[dd+1]=m[s+1];out[dd+2]=m[s+2];out[dd+3]=m[s+3];}}return out;}

const {device,info}=await initDevice();
console.log('[gpu]',info.description||info.vendor||'Dawn');
const R=new PVR2Renderer(); R.dev=device; R.fmt='rgba8unorm'; R._init(W,H);
const rt=makeRT(device,R.fmt,W,H);
const T=new TextureManager(device);

let quads=parseHud(new Uint8Array(readFileSync(HUD)));
if(ONLY){const set=new Set(ONLY.split(',').map(Number));quads=quads.filter((q,i)=>set.has(i));console.log('[filter] only',ONLY);}
const vram=new Uint8Array(readFileSync(VRAM));
let pvr; try{pvr=new Uint8Array(readFileSync(PVR));}catch{pvr=new Uint8Array(32768);}
console.log(`[hud] ${quads.length} quads, vram ${vram.length}, pvr ${pvr.length}, order [${ORDER}]`);

T.setDirtyPages(null,true);
T.updatePalette(pvr);
const ta=buildHudTA(quads,ORDER);
const P=new TAParser();
const parsed=P.parse(ta,ta.length);
console.log(`[parse] verts=${parsed.vertexCount} tr=${parsed.translucent.length}`);
const snap=synthSnap(W,H);
// dark backing so HUD is visible; transparentClear off
const CULLNONE=process.argv.includes('--cullnone');
R.renderFrame(parsed,T,snap,vram,{singlePass:true,noSort:true,drawOpaque:false,drawPunch:false,drawTrans:true,...(CULLNONE?{cullOverride:'none'}:{})},rt);
device.queue.submit([R._lastEncoder.finish()]);
const rgba=await readback(device,rt.color,W,H,R.fmt);
// composite over dark gray so transparent areas are visible
for(let i=0;i<rgba.length;i+=4){const a=rgba[i+3]/255;rgba[i]=rgba[i]*a+24*(1-a);rgba[i+1]=rgba[i+1]*a+28*(1-a);rgba[i+2]=rgba[i+2]*a+32*(1-a);rgba[i+3]=255;}
const png=new PNG({width:W,height:H}); png.data=Buffer.from(rgba.buffer,rgba.byteOffset,rgba.byteLength);
writeFileSync(OUT,PNG.sync.write(png));
let nz=0;for(let i=0;i<rgba.length;i+=4)if(rgba[i]>40||rgba[i+1]>40||rgba[i+2]>40)nz++;
console.log(`[done] wrote ${OUT}; ${nz} lit px`);
