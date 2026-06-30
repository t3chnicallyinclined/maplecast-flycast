// _cap_persist.mjs — persistent dual recorder. Reconnects forever to 7200 (mirror)
// and 7212 (gsta). Logs each TA-frame / prefix / FRMx arrival so we can see EXACTLY
// when (if ever) in_match flips and the GSTA prefix builds. Runs until SEC seconds
// AFTER the first GSTA FRMx frame (or HARD seconds, whichever first), then writes files.
import { writeFileSync } from 'node:fs';
import { zstdDecompressSync } from 'node:zlib';
import { WebSocket } from 'ws';
function arg(n,d){const i=process.argv.indexOf(n);return i>=0?process.argv[i+1]:d;}
const out=arg('--out','_fxp'); const HARD=+arg('--hard','30'); const AFTER=+arg('--after','5');
const MAGIC_ZCST=0x5453435A, MAGIC_MCRR=0x5252434D, MAGIC_FRMX=0x784D5246;
function unzcst(u8){const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);
  if(u8.length>=8&&dv.getUint32(0,true)===MAGIC_ZCST)return new Uint8Array(zstdDecompressSync(Buffer.from(u8.subarray(8))));
  return u8;}
function framed(chunks){let total=0;for(const c of chunks)total+=4+c.length;
  const buf=Buffer.alloc(total);let off=0;
  for(const c of chunks){buf.writeUInt32LE(c.length,off);off+=4;Buffer.from(c).copy(buf,off);off+=c.length;}return buf;}
const mir={chunks:[],ta:0}; const gst={prefix:null,frames:[]};
let finishing=false, firstFrmxAt=0;
const t0=Date.now(); const ms=()=>((Date.now()-t0)/1000).toFixed(2);
function connectMirror(){let ws;try{ws=new WebSocket('ws://127.0.0.1:7200');}catch(e){return;}
  ws.binaryType='arraybuffer';
  ws.on('open',()=>console.error(`[${ms()}] mirror open`));
  ws.on('message',d=>{const u8=new Uint8Array(d);mir.chunks.push(u8);
    const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);const m=dv.getUint32(0,true);
    if(m===MAGIC_ZCST){mir.ta++; if(mir.ta<=3||mir.ta%30===0)console.error(`[${ms()}] mirror ZCST #${mir.ta}`);}});
  ws.on('error',()=>{});ws.on('close',()=>{if(!finishing)setTimeout(connectMirror,40);});}
function connectGsta(){let ws;try{ws=new WebSocket('ws://127.0.0.1:7212');}catch(e){return;}
  ws.binaryType='arraybuffer';
  ws.on('open',()=>console.error(`[${ms()}] gsta open`));
  ws.on('message',d=>{let raw;try{raw=unzcst(new Uint8Array(d));}catch(e){return;}
    const dv=new DataView(raw.buffer,raw.byteOffset,raw.byteLength);const m=dv.getUint32(0,true);
    if(m===MAGIC_MCRR){ if(!gst.prefix){gst.prefix=raw;console.error(`[${ms()}] gsta PREFIX`);} }
    else if(m===MAGIC_FRMX&&gst.prefix){ gst.frames.push(raw);
      if(!firstFrmxAt){firstFrmxAt=Date.now();console.error(`[${ms()}] gsta FIRST FRMx -> capturing ${AFTER}s more`);setTimeout(finish,AFTER*1000);}
      if(gst.frames.length<=3||gst.frames.length%30===0)console.error(`[${ms()}] gsta FRMx #${gst.frames.length}`);}});
  ws.on('error',()=>{});ws.on('close',()=>{if(!finishing)setTimeout(connectGsta,40);});}
connectMirror();connectGsta();
setTimeout(finish,HARD*1000);
function finish(){if(finishing)return;finishing=true;
  // gsta FIRST (the key file — replica-live wire carrying the effect-poly state; smaller, always fits)
  if(gst.prefix){const pb=Buffer.from(gst.prefix);pb.writeUInt32LE(gst.frames.length,16);
    writeFileSync(out+'.gsta.mcrr',Buffer.concat([pb,...gst.frames.map(f=>Buffer.from(f))]));
    console.error(`[gsta] prefix + ${gst.frames.length} frames -> ${out}.gsta.mcrr`);}
  else console.error('[gsta] NO prefix (in_match never flipped to 1)');
  // mirror can exceed Node's 2GB single-write limit — guard it so a failure never loses the gsta file
  try{ if(mir.chunks.length)writeFileSync(out+'.mirror.zcst',framed(mir.chunks));
    console.error(`[mirror] ${mir.chunks.length} msgs (${mir.ta} ZCST TA) -> ${out}.mirror.zcst`); }
  catch(e){ console.error(`[mirror] write FAILED (${e.code}): too big for one write — use a shorter --hard window. gsta IS saved.`); }
  process.exit(0);}
