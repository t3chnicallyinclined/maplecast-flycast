// _decode_sprite_tex.mjs — decode a specific fmt=5 PAL4 sprite texture from a capture's
// vram_prefix.bin + pvr_prefix.bin via the REAL TextureManager, save a magnified PNG.
//   node _decode_sprite_tex.mjs --dir _hud_cap_fresh3 --tcw 2a082c00 --tsp 949004d2
import { readFileSync, writeFileSync } from 'node:fs';
import { PNG } from 'pngjs';
const W_DIR = new URL('../../web/webgpu/', import.meta.url);
const { TextureManager } = await import(new URL('texture-manager.mjs', W_DIR));

globalThis.GPUTextureUsage = { TEXTURE_BINDING:1, COPY_DST:2, RENDER_ATTACHMENT:4, COPY_SRC:8 };
globalThis.GPUBufferUsage = { INDEX:1, COPY_DST:2, MAP_READ:4, COPY_SRC:8 };
function arg(n,d){const i=process.argv.indexOf(n);return i>=0?process.argv[i+1]:d;}
const DIR=arg('--dir','_hud_cap_fresh3');
const TCW=parseInt(arg('--tcw','2a082c00'),16);
const TSP=parseInt(arg('--tsp','949004d2'),16);

const vram=new Uint8Array(readFileSync(`${DIR}/vram_prefix.bin`));
const pvr=new Uint8Array(readFileSync(`${DIR}/pvr_prefix.bin`));

// Build a fake device that captures writeTexture data instead of GPU upload.
let captured=null, capW=0, capH=0;
const fakeDev={
  createTexture:({size})=>({createView:()=>({}),destroy:()=>{}, _sz:size}),
  createSampler:()=>({}),
  createBuffer:()=>({}),
  queue:{ writeTexture:(_t,data,_layout,sz)=>{captured=data.slice();capW=sz[0];capH=sz[1];} },
};
const T=new TextureManager(fakeDev);
T.setDirtyPages(null,true);
T.updatePalette(pvr);
const entry=T.getTexture(TSP,TCW,vram);
console.log(`decoded ${capW}x${capH}  fmt=${(TCW>>>27)&7} scan=${(TCW>>>26)&1} palSel=${(TCW>>>21)&0x3F} addr=0x${((TCW&0x1FFFFF)<<3).toString(16)}`);

// captured is rgba8 wxh. Magnify 8x.
const sc=8, dw=capW*sc, dh=capH*sc, out=new Uint8Array(dw*dh*4);
for(let y=0;y<dh;y++)for(let x=0;x<dw;x++){const s=((y/sc|0)*capW+(x/sc|0))*4,d=(y*dw+x)*4;
  const a=captured[s+3]/255;
  out[d]=captured[s]*a+40*(1-a);out[d+1]=captured[s+1]*a+40*(1-a);out[d+2]=captured[s+2]*a+40*(1-a);out[d+3]=255;}
const OUT=`${DIR}/_TEX_${TCW.toString(16)}.png`;
const png=new PNG({width:dw,height:dh});png.data=Buffer.from(out.buffer,out.byteOffset,out.byteLength);
writeFileSync(OUT,PNG.sync.write(png));
console.log(`-> ${OUT}`);
