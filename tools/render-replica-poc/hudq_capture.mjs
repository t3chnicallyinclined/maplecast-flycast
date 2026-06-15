// hudq_capture.mjs — connect to prod replica-live, parse the HUDQ tail, dump a
// CLASSIFIED quad inventory + save (a) the raw HUDQ tail bytes for the SAME frame and
// (b) the VRAM prefix (for offline FONT.BIN decode analysis).
//
//   node hudq_capture.mjs --url wss://nobd.net/replica-live --frames 200 \
//        --outdir _hud_cap --want-hud 40
//
// Mirrors the replay.html live parse order EXACTLY:
//   FRMx(4)+vframe(4)+taSize(4) + dyn regions + GFX tail + PALETTE tail + HUDQ tail + engine_ta
import { writeFileSync, mkdirSync } from 'node:fs';
import { zstdDecompressSync } from 'node:zlib';
import { WebSocket } from 'ws';

function arg(n,d){const i=process.argv.indexOf(n);return i>=0?process.argv[i+1]:d;}
const url     = arg('--url','wss://nobd.net/replica-live');
const want    = +arg('--frames','300');
const outdir  = arg('--outdir','_hud_cap');
const wantHud = +arg('--want-hud','30');
mkdirSync(outdir,{recursive:true});

const MAGIC_ZCST=0x5453435A, MAGIC_MCRR=0x5252434D, MAGIC_FRMX=0x784D5246, HUDQ_MAGIC=0x48554451;

function unzcst(u8){
  const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);
  if(u8.length>=8 && dv.getUint32(0,true)===MAGIC_ZCST)
    return new Uint8Array(zstdDecompressSync(Buffer.from(u8.subarray(8))));
  return u8;
}

// prefix: MCRR header -> static + dynamic region tables, then vram + pvr + static payload.
function parsePrefix(buf){
  const dv=new DataView(buf.buffer,buf.byteOffset,buf.byteLength);
  let p=0; const u32=()=>{const v=dv.getUint32(p,true);p+=4;return v>>>0;};
  if(u32()!==MAGIC_MCRR) throw new Error('bad MCRR magic');
  const version=u32(),nStatic=u32(),nDynamic=u32(),nFrames=u32(),vramBytes=u32(),pvrBytes=u32(); u32();
  const region=()=>{const addr=u32(),len=u32();let tag='';for(let i=0;i<8;i++){const c=buf[p+i];if(c)tag+=String.fromCharCode(c);}p+=8;return{addr,len,tag};};
  const staticRegs=Array.from({length:nStatic},region);
  const dynamicRegs=Array.from({length:nDynamic},region);
  const dynTotal=dynamicRegs.reduce((a,r)=>a+r.len,0);
  // extract vram + pvr blobs right after the header
  const vram = buf.slice(p, p+vramBytes);
  const pvr  = buf.slice(p+vramBytes, p+vramBytes+pvrBytes);
  return {version,nStatic,nDynamic,nFrames,vramBytes,pvrBytes,staticRegs,dynamicRegs,dynTotal,headerEnd:p,vram,pvr,buf};
}

function applyGfxTailLen(u8,p){ // u32 nGfx, then nGfx*(u32 base + 0x20000 bytes)
  const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);
  if(p+4>u8.length) return p;
  const nGfx=dv.getUint32(p,true); p+=4;
  if(nGfx>64) return p-4;
  for(let i=0;i<nGfx;i++){ p+=4; p+=0x20000; }
  return p;
}
function applyPvrPalLen(u8,p){
  const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);
  if(p+4>u8.length) return {p,pvr:null};
  const palLen=dv.getUint32(p,true); p+=4;
  if(palLen===0) return {p,pvr:null};
  if(palLen>0x10000||p+palLen>u8.length) return {p:p-4,pvr:null};
  const pvr=u8.slice(p,p+palLen); p+=palLen;
  return {p,pvr};
}
function parseHudTail(u8,p){
  const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);
  if(p+8>u8.length) return {p,quads:[],raw:null,present:false};
  if(dv.getUint32(p,true)!==HUDQ_MAGIC) return {p,quads:[],raw:null,present:false};
  const start=p; p+=4;
  const nHud=dv.getUint32(p,true); p+=4;
  if(nHud>4096) return {p,quads:[],raw:null,present:false};
  const quads=[];
  for(let i=0;i<nHud;i++){
    if(p+96>u8.length) break;
    const x=[dv.getFloat32(p,true),dv.getFloat32(p+4,true),dv.getFloat32(p+8,true),dv.getFloat32(p+12,true)];
    const y=[dv.getFloat32(p+16,true),dv.getFloat32(p+20,true),dv.getFloat32(p+24,true),dv.getFloat32(p+28,true)];
    const u=[dv.getFloat32(p+32,true),dv.getFloat32(p+36,true),dv.getFloat32(p+40,true),dv.getFloat32(p+44,true)];
    const v=[dv.getFloat32(p+48,true),dv.getFloat32(p+52,true),dv.getFloat32(p+56,true),dv.getFloat32(p+60,true)];
    const col=[dv.getUint32(p+64,true),dv.getUint32(p+68,true),dv.getUint32(p+72,true),dv.getUint32(p+76,true)];
    const pcw=dv.getUint32(p+80,true),isp=dv.getUint32(p+84,true),tsp=dv.getUint32(p+88,true),tcw=dv.getUint32(p+92,true);
    quads.push({x,y,u,v,col,pcw,isp,tsp,tcw});
    p+=96;
  }
  const raw=u8.slice(start,p);
  return {p,quads,raw,present:true};
}

function classify(q){
  const fmt=(q.tcw>>27)&7, addr=(q.tcw&0x1FFFFF)<<3, textured=(q.pcw>>3)&1;
  const xs=q.x, ys=q.y;
  const w=Math.max(...xs)-Math.min(...xs), h=Math.max(...ys)-Math.min(...ys), cy=(Math.min(...ys)+Math.max(...ys))/2;
  let cls;
  if(!textured) cls='untex-bar';
  else if(addr===0x0809BE00 || (fmt===1 && addr>=0x0809BC00 && addr<=0x0809C200)) cls='FONT(name/digit)';
  else if(addr===0x0809DE00||addr===0x0809E000) cls='portrait/icon';
  else cls='tex-other';
  return {fmt,addr,textured,w,h,cy,cls};
}

const ws=new WebSocket(url); ws.binaryType='arraybuffer';
let pre=null, seen=0, savedHud=false, capturedFrames=0;
const inv={}; // cls -> count

ws.on('open',()=>console.error('[hudcap] open',url));
ws.on('error',e=>{console.error('[hudcap] ws error',e.message);process.exit(1);});
ws.on('close',()=>finish());

ws.on('message',(data)=>{
  let raw; try{raw=unzcst(new Uint8Array(data));}catch(e){return;}
  const dv=new DataView(raw.buffer,raw.byteOffset,raw.byteLength);
  const magic=dv.getUint32(0,true);
  if(magic===MAGIC_MCRR && !pre){
    pre=parsePrefix(raw);
    console.error(`[hudcap] prefix: ${pre.nStatic} static / ${pre.nDynamic} dyn / vram=${pre.vramBytes} pvr=${pre.pvrBytes}`);
    writeFileSync(`${outdir}/vram_prefix.bin`,Buffer.from(pre.vram));
    writeFileSync(`${outdir}/pvr_prefix.bin`,Buffer.from(pre.pvr));
    console.error(`[hudcap] wrote vram_prefix.bin (${pre.vram.length}) + pvr_prefix.bin (${pre.pvr.length})`);
    return;
  }
  if(magic===MAGIC_FRMX && pre){
    const vframe=dv.getUint32(4,true), taSize=dv.getUint32(8,true);
    let p=12+pre.dynTotal;
    p=applyGfxTailLen(raw,p);
    const pal=applyPvrPalLen(raw,p); p=pal.p;
    const ht=parseHudTail(raw,p);
    seen++;
    if(ht.present && ht.quads.length>0){
      capturedFrames++;
      if(!savedHud && ht.quads.length>=wantHud){
        savedHud=true;
        writeFileSync(`${outdir}/hudq_tail.bin`,Buffer.from(ht.raw));
        if(pal.pvr) writeFileSync(`${outdir}/pvr_frame.bin`,Buffer.from(pal.pvr));
        console.error(`\n[hudcap] *** SAVED hudq_tail.bin (vframe=${vframe}, ${ht.quads.length} quads) ***\n`);
        // full classified dump
        const lines=[];
        lines.push(`# HUDQ capture vframe=${vframe} nQuads=${ht.quads.length}`);
        const cnt={};
        ht.quads.forEach((q,i)=>{
          const c=classify(q);
          cnt[c.cls]=(cnt[c.cls]||0)+1;
          lines.push(`[${String(i).padStart(2)}] ${c.cls.padEnd(16)} fmt=${c.fmt} texAddr=0x${c.addr.toString(16).padStart(6,'0')} tex=${c.textured} `+
            `w=${c.w.toFixed(0).padStart(4)} h=${c.h.toFixed(0).padStart(3)} cy=${c.cy.toFixed(0).padStart(3)} `+
            `pcw=${q.pcw.toString(16).padStart(8,'0')} tsp=${q.tsp.toString(16).padStart(8,'0')} tcw=${q.tcw.toString(16).padStart(8,'0')} `+
            `col=[${q.col.map(c=>c.toString(16).padStart(8,'0')).join(',')}] `+
            `x=[${q.x.map(v=>v.toFixed(0)).join(',')}] y=[${q.y.map(v=>v.toFixed(0)).join(',')}] `+
            `u=[${q.u.map(v=>v.toFixed(3)).join(',')}] v=[${q.v.map(v=>v.toFixed(3)).join(',')}]`);
        });
        lines.push('');
        lines.push('# class histogram: '+JSON.stringify(cnt));
        writeFileSync(`${outdir}/hudq_inventory.txt`,lines.join('\n'));
        console.error(lines.slice(0,Math.min(lines.length,60)).join('\n'));
      }
    }
    if(seen<5||seen%30===0) console.error(`[f${seen}] vframe=${vframe} hud=${ht.quads.length} palTail=${pal.pvr?pal.pvr.length:0}`);
    if(seen>=want||(savedHud&&capturedFrames>=wantHud)) finish();
  }
});

let done=false;
function finish(){
  if(done) return; done=true;
  console.error(`\n[hudcap] frames seen=${seen} withHud=${capturedFrames} savedHud=${savedHud}`);
  process.exit(savedHud?0:2);
}
setTimeout(()=>{console.error('[hudcap] timeout');finish();},120000);
