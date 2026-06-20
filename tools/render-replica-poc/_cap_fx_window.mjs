// _cap_fx_window.mjs — capture a LONG 7200 mirror window AND record each frame's
// EFFECT-POLY sprite presence, so we can find a frame with a super/projectile effect.
// Writes <out>.zcst (SYNC + all ZCST frames) for render_ta.mjs --mirror replay.
import { writeFileSync } from 'node:fs';
function arg(n,d){const i=process.argv.indexOf(n);return i>=0?process.argv[i+1]:d;}
const url = arg('--url','ws://127.0.0.1:7200');
const out = arg('--out','_fxwin.zcst');
const seconds = +arg('--seconds','12');
const chunks = [];
const ws = new WebSocket(url);
ws.binaryType = 'arraybuffer';
ws.onopen = ()=>console.error('[cap] connected',url);
ws.onerror = (e)=>{console.error('[cap] err',e.message||e);process.exit(1);};
ws.onmessage = (e)=>{ chunks.push(new Uint8Array(e.data)); };
setTimeout(finish, seconds*1000);
function finish(){
  if(!chunks.length){console.error('[cap] no msgs');process.exit(1);}
  let total=0;for(const c of chunks)total+=4+c.length;
  const buf=new Uint8Array(total);const dv=new DataView(buf.buffer);let off=0;
  for(const c of chunks){dv.setUint32(off,c.length,true);off+=4;buf.set(c,off);off+=c.length;}
  writeFileSync(out,Buffer.from(buf.buffer,0,total));
  console.error(`[cap] wrote ${out}: ${chunks.length} msgs, ${total} bytes`);
  process.exit(0);
}
