// capture_break.mjs — one-command reference capture of the LIVE prod render feed.
//
// Records the prod /replica-live stream to a scrubbable .mcrr (RAM regions + VRAM
// + PVR + N FRMx frames), NON-DISRUPTIVELY — no server restart, no interference
// with your play. Dependency-free: uses Node 22's built-in global WebSocket, so
// there is NOTHING to npm-install.
//
//   node capture_break.mjs                       # 400 frames (~7s) -> break.mcrr
//   node capture_break.mjs --out cape.mcrr --frames 600
//   node capture_break.mjs --url wss://nobd.net/replica-live
//   (or press Ctrl-C anytime to stop and write what was captured so far)
//
// Play the breaking animation (cape / flip / flash) DURING the recording window.
// It prints each frame's body-node count and flags over-read frames so you can
// see immediately if the walker is reading a bad slot table. The .mcrr feeds
// diag_tiling_slip.mjs / emit_live_ta.mjs / the converge gate.

import { writeFileSync } from 'node:fs';
import { zstdDecompressSync } from 'node:zlib';

function arg(n, d){ const i = process.argv.indexOf(n); return i>=0 ? process.argv[i+1] : d; }
const url  = arg('--url', 'wss://nobd.net/replica-live');
const out  = arg('--out', 'break.mcrr');
const want = +arg('--frames', '400');

const MAGIC_ZCST = 0x5453435A, MAGIC_MCRR = 0x5252434D, MAGIC_FRMX = 0x784D5246;

function unzcst(u8){
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  if (u8.length>=8 && dv.getUint32(0,true)===MAGIC_ZCST)
    return new Uint8Array(zstdDecompressSync(Buffer.from(u8.subarray(8))));
  return u8;
}
function parsePrefix(buf){
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  let p=0; const u32=()=>{const v=dv.getUint32(p,true);p+=4;return v>>>0;};
  if (u32()!==MAGIC_MCRR) throw new Error('bad MCRR magic');
  const version=u32(), nStatic=u32(), nDynamic=u32(), nFrames=u32(), vramBytes=u32(), pvrBytes=u32(); u32();
  const region=()=>{const addr=u32(),len=u32();let tag='';for(let i=0;i<8;i++){const c=buf[p+i];if(c)tag+=String.fromCharCode(c);}p+=8;return{addr,len,tag};};
  const staticRegs=Array.from({length:nStatic},region);
  const dynamicRegs=Array.from({length:nDynamic},region);
  return {version,nStatic,nDynamic,nFrames,vramBytes,pvrBytes,staticRegs,dynamicRegs,headerEnd:p,buf};
}
function seedRam(pre){
  const ram = new Uint8Array(16*1024*1024);
  let p = pre.headerEnd + pre.vramBytes + pre.pvrBytes;
  for (const r of pre.staticRegs){
    const bytes = pre.buf.subarray(p, p + r.len); p += r.len;
    if (r.tag==='ram16') ram.set(bytes, 0); else ram.set(bytes, r.addr & 0xFFFFFF);
  }
  return ram;
}
function applyDyn(ram, pre, frameBuf){
  const dv=new DataView(frameBuf.buffer,frameBuf.byteOffset,frameBuf.byteLength);
  const vframe=dv.getUint32(4,true), taSize=dv.getUint32(8,true);
  let p=12;
  for (const r of pre.dynamicRegs){ ram.set(frameBuf.subarray(p,p+r.len), r.addr & 0xFFFFFF); p += r.len; }
  return {vframe,taSize};
}
// Slot-walk count scan (render_sprites_0308c2 — signed byte count per layer).
function scanSlots(ram){
  const COUNT_BASE=0x2895E0, PTR_BASE=0x287DE0, STRIDE=0x180;
  const r8s = a => (ram[a]<<24)>>24;
  const r32 = a => (ram[a]|(ram[a+1]<<8)|(ram[a+2]<<16)|(ram[a+3]<<24))>>>0;
  let body=0, eff=0, total=0; const counts=[];
  for (let L=0; L<16; L++){
    const cnt = r8s(COUNT_BASE + L); counts.push(cnt);
    const base = PTR_BASE + L*STRIDE;
    for (let i=0; i<cnt; i++){
      const nodeG = r32(base + i*4); const node = nodeG & 0xFFFFFF; total++;
      if (node===0 || ((nodeG>>>24)&0x7F)!==0x0C) continue;
      if (((ram[node+3]<<24)>>24)===0) body++; else eff++;
    }
  }
  return {counts, body, eff, total};
}

const ws = new WebSocket(url); ws.binaryType = 'arraybuffer';
let pre=null, ram=null, seen=0, prefixBytes=null, dynTotal=0;
const chunks=[];   // [prefix][frmx][frmx]...

ws.onopen  = () => console.error('[capture] open', url, `— recording ${want} frames (Ctrl-C to stop early)`);
ws.onerror = (e) => { console.error('[capture] ws error', e.message||e.type); process.exit(1); };
ws.onclose = () => finish();
ws.onmessage = (ev) => {
  const u8 = new Uint8Array(ev.data);
  let raw; try { raw = unzcst(u8); } catch(e){ console.error('[capture] unzcst fail', e.message); return; }
  const dv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
  const magic = dv.getUint32(0,true);
  if (magic===MAGIC_MCRR && !pre){
    pre = parsePrefix(raw); prefixBytes = raw; ram = seedRam(pre);
    dynTotal = pre.dynamicRegs.reduce((a,r)=>a+r.len,0);
    console.error(`[capture] prefix: ${pre.nStatic} static / ${pre.nDynamic} dyn / vram=${pre.vramBytes} pvr=${pre.pvrBytes}`);
    return;
  }
  if (magic===MAGIC_FRMX && pre){
    // Each live FRMx = header(12) + dyn(dynTotal) + a VARIABLE on-change GFX tail
    // whose length is NOT the offset-8 field. Concatenated offline, frame
    // boundaries would be unrecoverable. Stamp offset-8 with the TRUE
    // post-dyn byte count so the existing offline parsers (diag_tiling_slip,
    // emit_live_ta: "read 12 + skip dyn + skip taSize") land exactly on the next
    // frame. The tail bytes are skipped by those tools (they rebuild TA from RAM),
    // so overwriting this field is lossless for the diagnostic pipeline.
    new DataView(raw.buffer, raw.byteOffset, raw.byteLength).setUint32(8, raw.length - 12 - dynTotal, true);
    chunks.push(raw);
    const {vframe} = applyDyn(ram, pre, raw);
    const s = scanSlots(ram);
    const flag = s.total>=1000 ? '  <<<< OVER-READ (bad slot table)' : (s.total>=256?'  (>256)':'');
    if (seen<8 || s.total>=256 || seen%30===0)
      console.error(`[f${seen}] vframe=${vframe} bodies=${s.body} eff=${s.eff} total=${s.total}${flag}`);
    if (++seen>=want) finish();
  }
};

let done=false;
function finish(){
  if (done) return; done=true;
  if (!pre || !chunks.length){ console.error('[capture] no frames captured (is a match live?)'); process.exit(1); }
  const pb = Buffer.from(prefixBytes);
  pb.writeUInt32LE(chunks.length, 16);   // patch nFrames into the header
  const file = Buffer.concat([pb, ...chunks.map(c=>Buffer.from(c))]);
  writeFileSync(out, file);
  console.error(`\n[capture] wrote ${out}: ${chunks.length} frames, ${(file.length/1048576).toFixed(1)} MB`);
  console.error(`[capture] next: node diag_tiling_slip.mjs ${out} <frameIndex>`);
  process.exit(0);
}
process.on('SIGINT', finish);   // Ctrl-C = stop + write what we have
