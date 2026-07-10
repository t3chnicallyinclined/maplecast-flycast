// mod_bridge.mjs — LOCAL live mod cockpit for the MapleCast stream.
// Serves a button panel on http://localhost:9099 and relays JSON commands to the
// loopback control WS (7211) through your ssh tunnel. RAM-write NEVER hits the
// public internet. Dependency-free (Node 22 built-in WebSocket + http).
//
//   1) tunnel:  ssh -N -L 7211:127.0.0.1:7211 root@149.28.44.118
//   2) bridge:  node mod_bridge.mjs
//   3) open:    http://localhost:9099   (keep the webgpu-test tab open to watch)
import http from 'node:http';

const CTRL = 'ws://127.0.0.1:7211', PORT = 9099;
const P1C1 = 0x268340, P1C2 = 0x268E88, P2C1 = 0x2688E4, STRIDE = 0x5A4;

// ---- persistent control-WS connection with request/reply matching ----
let ws = null, rid = 0; const pend = new Map();
function connect(){
  ws = new WebSocket(CTRL);
  ws.onopen = () => console.log('[bridge] control WS connected', CTRL);
  ws.onclose = () => { console.log('[bridge] control WS closed — retrying in 2s (is the tunnel up?)'); ws=null; setTimeout(connect,2000); };
  ws.onerror = () => {};
  ws.onmessage = ev => { try{ const m=JSON.parse(ev.data); const r=pend.get(m.reply_id); if(r){ pend.delete(m.reply_id); r(m); } }catch{} };
}
connect();
function ctrl(o){ return new Promise((res,rej)=>{
  if(!ws || ws.readyState!==1) return rej(new Error('control WS not connected — start the ssh tunnel'));
  const id='b'+(++rid); pend.set(id,res); ws.send(JSON.stringify({...o, reply_id:id}));
  setTimeout(()=>{ if(pend.has(id)){ pend.delete(id); rej(new Error('control WS timeout')); } }, 8000); }); }
const rd = async (off,size)=>{ const m=await ctrl({cmd:'ram_read',offset:off,size}); if(!m.ok) throw new Error(JSON.stringify(m)); return m.data.hex; };
const wr = async (off,hex)=>{ const m=await ctrl({cmd:'ram_write',offset:off,hex}); if(!m.ok) throw new Error(JSON.stringify(m)); return m; };

// ---- high-level mod actions ----
async function readSlots(){
  const c1=await rd(P1C1,STRIDE), c2=await rd(P1C2,STRIDE), p2=await rd(P2C1,0x40);
  const f32=(h,o)=>Buffer.from(h.slice(o*2,(o+4)*2),'hex').readFloatLE(0);
  const one=(h)=>({active:h.slice(0,2), cid:'0x'+h.slice(2,4), x:+f32(h,0x34).toFixed(1), y:+f32(h,0x38).toFixed(1), hp:parseInt(h.slice(0x420*2,0x420*2+2),16)});
  return { P1C1:one(c1), P1C2:one(c2), P2C1:one(p2) };
}
async function swapPoint(){
  const c1=await rd(P1C1,STRIDE), c2=await rd(P1C2,STRIDE);
  const pos1=c1.slice(0x34*2,(0x34+8)*2), pos2=c2.slice(0x34*2,(0x34+8)*2);
  const s1=c2.slice(0,0x34*2)+pos1+c2.slice((0x34+8)*2);   // C2's char into C1 slot, keep C1 pos
  const s2=c1.slice(0,0x34*2)+pos2+c1.slice((0x34+8)*2);
  await wr(P1C1,s1); await wr(P1C2,s2);
  return { swapped:true, note:'P1 point<->bench struct swap; engine re-derives point from content' };
}
async function teleportSides(){
  const c1=await rd(P1C1,8), p2=await rd(P2C1,8);
  const a=c1.slice(0x34*2,(0x34+8)*2), b=p2.slice(0x34*2,(0x34+8)*2);
  await wr(P1C1+0x34,b); await wr(P2C1+0x34,a);
  return { teleported:true };
}
async function poke(offHex, hex){
  const off=parseInt(offHex,16); if(!(off>=0)||!/^[0-9a-fA-F]+$/.test(hex)) throw new Error('bad offset/hex');
  await wr(off,hex); return { off:'0x'+off.toString(16), wrote:hex };
}
async function heal(slotOff){ await wr(slotOff+0x420,'90'); await wr(slotOff+0x424,'90'); return {healed:'0x'+slotOff.toString(16)}; }
async function meterMax(slotOff){ await wr(0x289646,'0005'); return {meter:'max-ish'}; }

const ACT = {
  read: readSlots, swap: swapPoint, teleport: teleportSides,
  healP1: ()=>heal(P1C1), healP2: ()=>heal(P2C1), meter: ()=>meterMax(),
  poke: (q)=>poke(q.off, q.hex),
};

// ---- HTTP server: panel + /cmd ----
const PANEL = `<!doctype html><meta charset=utf8><title>MapleCast Mod Cockpit</title>
<style>body{background:#111;color:#eee;font:14px/1.5 system-ui,monospace;margin:0;padding:20px;max-width:640px}
h1{font-size:18px;color:#0ff}button{background:#223;color:#eee;border:1px solid #46c;border-radius:6px;padding:10px 14px;margin:4px;cursor:pointer;font:inherit}
button:hover{background:#345}button.warn{border-color:#f80;color:#fc8}button.danger{border-color:#f44;color:#f88}
pre{background:#000;border:1px solid #333;border-radius:6px;padding:10px;white-space:pre-wrap;max-height:40vh;overflow:auto}
input{background:#000;color:#0f0;border:1px solid #444;border-radius:4px;padding:6px;font:inherit}
.row{margin:8px 0}small{color:#888}</style>
<h1>🎮 MapleCast Mod Cockpit</h1>
<small>Loopback control WS via your ssh tunnel. Watch results in the webgpu-test tab.</small>
<div class=row><button onclick=go('read')>🔄 Read slots</button>
<button onclick=go('swap')>🔀 Swap P1 point↔bench</button>
<button class=warn onclick=go('teleport')>↔️ Teleport P1↔P2 sides</button></div>
<div class=row><button onclick=go('healP1')>❤️ Heal P1</button>
<button onclick=go('healP2')>❤️ Heal P2</button>
<button onclick=go('meter')>⚡ P1 meter</button></div>
<div class=row><small>Raw poke — offset(hex) + bytes(hex):</small><br>
<input id=off placeholder=268340 size=10> <input id=hex placeholder=ff size=20>
<button class=danger onclick=poke()>💥 Poke</button></div>
<pre id=out>ready.</pre>
<script>
const out=document.getElementById('out');
async function call(action,q){ out.textContent='…';
  try{ const r=await fetch('/cmd',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({action,...q})});
    const j=await r.json(); out.textContent=JSON.stringify(j.reply??j,null,2); }
  catch(e){ out.textContent='ERROR: '+e.message; } }
function go(a){ call(a,{}); }
function poke(){ call('poke',{off:document.getElementById('off').value,hex:document.getElementById('hex').value}); }
go('read');
</script>`;

http.createServer(async (req,res)=>{
  if(req.method==='GET' && req.url==='/'){ res.writeHead(200,{'content-type':'text/html'}); return res.end(PANEL); }
  if(req.method==='POST' && req.url==='/cmd'){
    let body=''; req.on('data',d=>body+=d); req.on('end', async ()=>{
      try{ const q=JSON.parse(body||'{}'); const fn=ACT[q.action]; if(!fn) throw new Error('unknown action '+q.action);
        const reply=await fn(q); res.writeHead(200,{'content-type':'application/json'}); res.end(JSON.stringify({ok:true,reply})); }
      catch(e){ res.writeHead(200,{'content-type':'application/json'}); res.end(JSON.stringify({ok:false,error:e.message})); }
    }); return;
  }
  res.writeHead(404); res.end();
}).listen(PORT, ()=>console.log(`[bridge] cockpit -> http://localhost:${PORT}  (tunnel 7211 must be up)`));
