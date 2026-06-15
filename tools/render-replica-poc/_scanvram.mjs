// Scan shipped VRAM for PAL4 regions that decode to text-like (high-contrast, structured) tiles.
// Determines whether the name glyphs are present-but-mis-addressed vs absent.
import './webgpu-headless.mjs';
import { initDevice } from './webgpu-headless.mjs';
import { readFileSync, writeFileSync } from 'node:fs';
import { PNG } from 'pngjs';
const W_DIR = new URL('../../web/webgpu/', import.meta.url);
const { TextureManager } = await import(new URL('texture-manager.mjs', W_DIR));
const cap = process.argv[2] || '_hud_cap_namegap';
const {device}=await initDevice();
const T=new TextureManager(device);
const vram=new Uint8Array(readFileSync(`${cap}/vram_prefix.bin`));
const pvr=new Uint8Array(readFileSync(`${cap}/pvr_prefix.bin`));
T.updatePalette(pvr);
// non-zero histogram per 64KB block 0x400000..0x480000
for(let b=0x400000;b<0x480000;b+=0x10000){let nz=0;for(let i=0;i<0x10000;i++)if(vram[b+i])nz++;
  console.log(`block ${b.toString(16)}: ${(100*nz/0x10000).toFixed(1)}% non-zero`);}
// Decode a vertical filmstrip of 32x32 PAL4 tiles every 0x200 bytes across 0x415000..0x420000, palSel 16/17
const palSel=17, start=0x415000, end=0x41e000, step=0x200;
const tiles=[];for(let a=start;a<end;a+=step)tiles.push(a);
const COLS=24,ROWS=Math.ceil(tiles.length/COLS);
const film=new PNG({width:33*COLS,height:33*ROWS});film.data.fill(20);
for(let k=0;k<tiles.length;k++){const a=tiles[k];const rgba=T._decode(vram,a,5,32,32,palSel,0,0,0,2);
  const cx=(k%COLS)*33,cy=Math.floor(k/COLS)*33;
  for(let y=0;y<32;y++)for(let x=0;x<32;x++){const si=(y*32+x)*4,di=((cy+y)*33*COLS+(cx+x))*4;const al=rgba[si+3]/255;
    film.data[di]=rgba[si]*al+20*(1-al);film.data[di+1]=rgba[si+1]*al+20*(1-al);film.data[di+2]=rgba[si+2]*al+20*(1-al);film.data[di+3]=255;}}
writeFileSync(`${cap}/_scanvram.png`,PNG.sync.write(film));
console.log(`wrote ${cap}/_scanvram.png (${tiles.length} tiles, palSel ${palSel})`);
