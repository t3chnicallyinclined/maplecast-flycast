// G0 capture: mirror the WORKING native-client handshake (request_sync THEN
// subscribe mode:tdw) so the server seeds a TDWS dict + a streamStart TDW1.
// Writes [u32 LE len][msg] — exactly what `maplecast-native gate` reads.
// Re-triggers the subscribe if no TDWS arrives (forces requestTdwSnapshot).
import WebSocket from 'ws';
import fs from 'fs';
const [,,URL='wss://play.nobd.net/ws', OUT='cap.bin', SECS='90'] = process.argv;
const out = fs.createWriteStream(OUT);
let n=0, bytes=0, sawTDWS=false, sawStart=false, doneTimer=null;
const hist = new Map();
const magic = (b)=> (b.length>=4 && b[0]>=32 && b[0]<127) ? b.toString('latin1',0,4) : `0x${b.slice(0,4).toString('hex')}`;
const ws = new WebSocket(URL); ws.binaryType='nodebuffer';
function sub(){ ws.send(JSON.stringify({type:'request_sync'})); ws.send(JSON.stringify({type:'subscribe',mode:'tdw'})); }
ws.on('open', ()=>{ console.log('connected', URL); sub(); console.log('sent request_sync + subscribe{mode:tdw}'); });
// Re-trigger every 4s until the dict shows up (requestTdwSnapshot is idempotent).
const retrig = setInterval(()=>{ if(!sawTDWS){ console.log('no TDWS yet — re-subscribing'); sub(); } else clearInterval(retrig); }, 4000);
ws.on('message', (buf, isBinary) => {
  if (!isBinary) return;              // drop the telemetry JSON spam
  const m = magic(buf); hist.set(m, (hist.get(m)||0)+1);
  const len = Buffer.alloc(4); len.writeUInt32LE(buf.length, 0);
  out.write(len); out.write(buf); n++; bytes+=buf.length;
  if (m==='TDWS' && !sawTDWS){ sawTDWS=true; console.log(`>>> TDWS dict at msg#${n} (epoch=${buf[8]})`); }
  if (m==='TDW1' && buf.length>=6 && (buf[5]&1) && !sawStart){
    sawStart=true; console.log(`>>> streamStart TDW1 at msg#${n} (seq=${buf.readUInt16LE(6)}, epoch=${buf[4]})`);
  }
  if (sawTDWS && sawStart && !doneTimer){
    console.log('both anchors seen — capturing 8s more'); doneTimer=setTimeout(finish, 8000);
  }
});
function finish(){ clearInterval(retrig); out.end(()=>{
  console.log(`saved ${n} msgs ${bytes} B -> ${OUT}  (TDWS=${sawTDWS} streamStart=${sawStart})`);
  console.log('magic histogram:', JSON.stringify(Object.fromEntries([...hist].sort((a,b)=>b[1]-a[1]))));
  process.exit(sawTDWS && sawStart ? 0 : 3);
}); }
ws.on('error', e=>{ console.log('ERR', e.message); process.exit(4); });
setTimeout(finish, parseInt(SECS)*1000);
