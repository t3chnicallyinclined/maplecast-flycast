// mod_bridge.mjs — LOCAL live mod cockpit for the MapleCast stream.
// Serves a button panel on http://localhost:9099 and relays JSON commands to the
// loopback control WS (7211) through your ssh tunnel. RAM-write NEVER hits the
// public internet. Dependency-free (Node 22 built-in WebSocket + http).
//
//   1) tunnel:  ssh -N -L 7311:127.0.0.1:7211 root@149.28.44.118
//   2) bridge:  node mod_bridge.mjs 7311     (arg = your local tunnel port; default 7211)
//   3) open:    http://localhost:9099        (keep the webgpu-test tab open to watch)
// (Use a fresh local port like 7311 if 7211 is stuck "bind: Permission denied" from a
//  prior dead tunnel — Windows leaves the socket lingering a while.)
import http from 'node:http';

const CTRL_PORT = process.argv[2] || process.env.MC_CTRL_PORT || '7211';
const CTRL = `ws://127.0.0.1:${CTRL_PORT}`, PORT = 9099;

// ---- addresses (RAM offsets = DC addr & 0xFFFFFF), all from docs/MVC2-MEMORY-MAP.md ----
const STRIDE = 0x5A4;
const SLOT = { P1C1:0x268340, P2C1:0x2688E4, P1C2:0x268E88, P2C2:0x26942C, P1C3:0x2699D0, P2C3:0x269F74 };
const OFF = { active:0x000, cid:0x001, x:0x034, y:0x038, vx:0x05C, vy:0x060, facing:0x110,
              sprite:0x144, animstate:0x1D0, xflip:0x1D2, hp:0x420, red:0x424,
              stance:0x1F9, speed:0x200, flight:0x201, armor:0x202, dmg:0x205, def:0x206,
              paltint:0x025, palid:0x52D };
const GLOBAL = { timer:0x289630, p1meterFill:0x289646, p2meterFill:0x289648,
                 p1meterLvl:0x28964A, p2meterLvl:0x28964B, p1combo:0x289670, p2combo:0x289672 };
const HP_FULL = 0x90;  // 144 = full health for these chars (matches live read)
const CID = {'0x00':'Ryu','0x17':'Cable','0x23':'Dan','0x2a':'Storm','0x2c':'Magneto','0x34':'Sentinel','0x3a':'Servbot'};

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

// ---- helpers ----
const f32 = (h,o)=>Buffer.from(h.slice(o*2,(o+4)*2),'hex').readFloatLE(0);
const u8  = (h,o)=>parseInt(h.slice(o*2,o*2+2),16);
async function readSlot(base){ const h=await rd(base,STRIDE);
  const cid='0x'+h.slice(OFF.cid*2,OFF.cid*2+2);
  return { active:u8(h,OFF.active), cid, name:CID[cid]||cid, x:+f32(h,OFF.x).toFixed(1), y:+f32(h,OFF.y).toFixed(1),
           hp:u8(h,OFF.hp), red:u8(h,OFF.red), xflip:u8(h,OFF.xflip) }; }

// ---- high-level mod actions ----
async function readSlots(){
  const out={};
  for(const [name,base] of Object.entries(SLOT)) out[name]=await readSlot(base);
  const g=await rd(GLOBAL.timer,0x50);   // one read spanning the global block (0x289630..)
  out.globals={ timer:u8(g,0), p1meterLvl:u8(g, GLOBAL.p1meterLvl-GLOBAL.timer),
                p2meterLvl:u8(g, GLOBAL.p2meterLvl-GLOBAL.timer),
                p1combo:u8(g, GLOBAL.p1combo-GLOBAL.timer) };
  return out;
}
// Full point<->bench struct swap, preserving each slot's on-screen position. The DAT/GFX/
// anim pointers (+0x15C/160/164/168) travel with content, so art follows the char. The
// engine re-derives "point" from struct CONTENT (re_kb/69) — it reorganizes, not a clean tag.
async function swapPoint(pair){
  const [aName,bName]=pair||['P1C1','P1C2']; const A=SLOT[aName], B=SLOT[bName];
  const ca=await rd(A,STRIDE), cb=await rd(B,STRIDE);
  const px=OFF.x*2, pxEnd=(OFF.x+8)*2;   // keep +0x34 x AND +0x38 y (8 bytes) per slot
  const sa=cb.slice(0,px)+ca.slice(px,pxEnd)+cb.slice(pxEnd);
  const sb=ca.slice(0,px)+cb.slice(px,pxEnd)+ca.slice(pxEnd);
  await wr(A,sa); await wr(B,sb);
  const va=await readSlot(A), vb=await readSlot(B);
  return { swapped:[aName,bName], now:{[aName]:va.name,[bName]:vb.name} };
}
async function teleportSides(){
  const a=await rd(SLOT.P1C1,8), b=await rd(SLOT.P2C1,8);
  const pa=a.slice(OFF.x*2,(OFF.x+8)*2), pb=b.slice(OFF.x*2,(OFF.x+8)*2);
  await wr(SLOT.P1C1+OFF.x,pb); await wr(SLOT.P2C1+OFF.x,pa);
  return { teleported:true };
}
async function heal(base){ await wr(base+OFF.hp, HP_FULL.toString(16)); await wr(base+OFF.red, HP_FULL.toString(16)); return {healed:'0x'+base.toString(16)}; }
async function healAll(){ for(const b of Object.values(SLOT)) await heal(b); return {healed:'all 6'}; }
async function flip(base){ const h=await rd(base+OFF.xflip,1); const v=u8(h,0)?'00':'01'; await wr(base+OFF.xflip,v); return {xflip:v}; }
async function meterMax(side){ // meter_level byte to 5 (5 bars) — EXPERIMENTAL max value
  const lvl = side==='P2'?GLOBAL.p2meterLvl:GLOBAL.p1meterLvl; await wr(lvl,'05'); return {meterLevel:5,side:side||'P1',note:'experimental max'}; }
async function buff(base, kind){ // speed/flight/armor toggles — EXPERIMENTAL, value 01=on
  const off = {speed:OFF.speed,flight:OFF.flight,armor:OFF.armor}[kind]; if(off===undefined) throw new Error('bad buff');
  const h=await rd(base+off,1); const v=u8(h,0)?'00':'01'; await wr(base+off,v); return {[kind]:v,note:'experimental'}; }
async function poke(offHex, hex){
  const off=parseInt(offHex,16); if(!(off>=0)||!/^[0-9a-fA-F]+$/.test(hex)||hex.length%2) throw new Error('bad offset/hex');
  if(off+hex.length/2>16*1024*1024) throw new Error('past 16MB RAM');
  await wr(off,hex); return { off:'0x'+off.toString(16), wrote:hex };
}

const ACT = {
  read: readSlots,
  swapP1: ()=>swapPoint(['P1C1','P1C2']), swapP2: ()=>swapPoint(['P2C1','P2C2']),
  teleport: teleportSides,
  healP1: ()=>heal(SLOT.P1C1), healP2: ()=>heal(SLOT.P2C1), healAll,
  flipP1: ()=>flip(SLOT.P1C1), flipP2: ()=>flip(SLOT.P2C1),
  meterP1: ()=>meterMax('P1'), meterP2: ()=>meterMax('P2'),
  speedP1: ()=>buff(SLOT.P1C1,'speed'), flightP1: ()=>buff(SLOT.P1C1,'flight'), armorP1: ()=>buff(SLOT.P1C1,'armor'),
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
<small>Loopback control WS via your ssh tunnel. Addresses from docs/MVC2-MEMORY-MAP.md.
Watch results in the webgpu-test tab.</small>
<div class=row><b>Swap / teleport</b><br><button onclick=go('swapP1')>🔀 Swap P1 point↔bench</button>
<button onclick=go('swapP2')>🔀 Swap P2 point↔bench</button>
<button class=warn onclick=go('teleport')>↔️ Teleport P1↔P2 sides</button></div>
<div class=row><b>Health</b><br><button onclick=go('healP1')>❤️ Heal P1</button>
<button onclick=go('healP2')>❤️ Heal P2</button>
<button onclick=go('healAll')>❤️ Heal ALL 6</button></div>
<div class=row><b>Facing</b><br><button onclick=go('flipP1')>🔃 Flip P1</button>
<button onclick=go('flipP2')>🔃 Flip P2</button></div>
<div class=row><b>Buffs (experimental)</b><br><button onclick=go('meterP1')>⚡ P1 meter max</button>
<button onclick=go('speedP1')>💨 P1 speed</button><button onclick=go('flightP1')>🕊️ P1 flight</button>
<button onclick=go('armorP1')>🛡️ P1 armor</button></div>
<div class=row><small>Raw poke — RAM offset(hex, e.g. 268340) + bytes(hex, LE):</small><br>
<input id=off placeholder=268340 size=10> <input id=hex placeholder=90 size=20>
<button class=danger onclick=poke()>💥 Poke</button></div>
<div class=row><button onclick=go('read')>🔄 Read all slots + globals</button></div>
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
