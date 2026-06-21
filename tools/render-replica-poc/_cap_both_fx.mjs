// _cap_both_fx.mjs — poll for 7200 (mirror full TA) AND 7212 (GSTA replica-live)
// and connect BOTH the instant they listen, capturing the first ~SEC seconds.
// Mirror -> <out>.mirror.zcst (SYNC + ZCST frames, render_ta.mjs --mirror replayable):
//           length-framed raw messages [u32 len][bytes]...
// GSTA   -> <out>.gsta.mcrr  (decompressed MCRR prefix + FRMx records, _find_efx_nodes.mjs readable).
import { writeFileSync } from 'node:fs';
import { zstdDecompressSync } from 'node:zlib';
import { WebSocket } from 'ws';

function arg(n,d){const i=process.argv.indexOf(n);return i>=0?process.argv[i+1]:d;}
const out = arg('--out','_fxboth');
const SEC = +arg('--seconds','5');
const MAGIC_ZCST=0x5453435A, MAGIC_MCRR=0x5252434D, MAGIC_FRMX=0x784D5246;

function unzcst(u8){
  const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);
  if(u8.length>=8 && dv.getUint32(0,true)===MAGIC_ZCST)
    return new Uint8Array(zstdDecompressSync(Buffer.from(u8.subarray(8))));
  return u8;
}
function framed(chunks){
  let total=0;for(const c of chunks)total+=4+c.length;
  const buf=Buffer.alloc(total);let off=0;
  for(const c of chunks){buf.writeUInt32LE(c.length,off);off+=4;Buffer.from(c).copy(buf,off);off+=c.length;}
  return buf;
}

// ---- mirror (7200): keep raw messages, length-framed ----
const mir={chunks:[],connected:false};
// ---- gsta (7212): decompress, keep MCRR prefix + FRMx records ----
const gst={prefix:null, frames:[], connected:false};

function connectMirror(){
  let ws; try{ws=new WebSocket('ws://127.0.0.1:7200');}catch(e){return;}
  ws.binaryType='arraybuffer';
  ws.on('open',()=>{mir.connected=true;console.error(`[mirror] connected @${new Date().toISOString()}`);});
  ws.on('message',d=>mir.chunks.push(new Uint8Array(d)));
  ws.on('error',()=>{});
  ws.on('close',()=>{ if(!finishing) setTimeout(connectMirror,40); });
}
function connectGsta(){
  let ws; try{ws=new WebSocket('ws://127.0.0.1:7212');}catch(e){return;}
  ws.binaryType='arraybuffer';
  ws.on('open',()=>{console.error(`[gsta] connected @${new Date().toISOString()}`);});
  ws.on('message',d=>{
    let raw; try{raw=unzcst(new Uint8Array(d));}catch(e){return;}
    const dv=new DataView(raw.buffer,raw.byteOffset,raw.byteLength);
    const m=dv.getUint32(0,true);
    if(m===MAGIC_MCRR && !gst.prefix){ gst.prefix=raw; gst.connected=true; console.error('[gsta] prefix captured'); }
    else if(m===MAGIC_FRMX && gst.prefix){ gst.frames.push(raw); gst.connected=true; }
  });
  ws.on('error',()=>{});
  ws.on('close',()=>{ if(!finishing) setTimeout(connectGsta,40); });
}

let finishing=false;
connectMirror(); connectGsta();

const checkStart=setInterval(()=>{
  if(mir.connected||gst.connected){ clearInterval(checkStart);
    console.error(`[cap] a stream is live; capturing ${SEC}s`); setTimeout(finish,SEC*1000);}
},15);
setTimeout(()=>{ if(!mir.connected&&!gst.connected){console.error('[cap] nothing in 25s');process.exit(2);} },25000);

function finish(){
  finishing=true;
  if(mir.chunks.length){ writeFileSync(out+'.mirror.zcst', framed(mir.chunks)); console.error(`[mirror] wrote ${out}.mirror.zcst: ${mir.chunks.length} msgs`);}
  else console.error('[mirror] NO msgs');
  if(gst.prefix){
    const pb=Buffer.from(gst.prefix); pb.writeUInt32LE(gst.frames.length,16);
    const file=Buffer.concat([pb,...gst.frames.map(f=>Buffer.from(f))]);
    writeFileSync(out+'.gsta.mcrr',file);
    console.error(`[gsta] wrote ${out}.gsta.mcrr: prefix + ${gst.frames.length} frames`);
  } else console.error('[gsta] NO prefix');
  process.exit(0);
}
