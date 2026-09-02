// mod_bridge.mjs — LOCAL live mod cockpit for the MapleCast stream.
// Serves a button panel on http://localhost:9099 and relays JSON commands to the
// loopback control WS (7211) through your ssh tunnel. RAM-write NEVER hits the
// public internet. Dependency-free (Node 22 built-in WebSocket + http).
//
//   1) tunnel:  ssh -N -L 7311:127.0.0.1:7211 -i ~/.ssh/ovh_maplecast ubuntu@15.204.141.58
//   2) bridge:  node mod_bridge.mjs 7311     (arg = your local tunnel port; default 7211)
//   3) open:    http://localhost:9099        (keep the webgpu-test tab open to watch)
// (Use a fresh local port like 7311 if 7211 is stuck "bind: Permission denied" from a
//  prior dead tunnel — Windows leaves the socket lingering a while.)
import http from 'node:http';
import { readFileSync, readdirSync, statSync } from 'node:fs';

// WebSocket: Node 22 has it global; Node 20 (prod) does not — fall back to the `ws` package.
let WS = globalThis.WebSocket;
if(!WS){ try{ WS = (await import('ws')).WebSocket || (await import('ws')).default; }
  catch{ console.error('[bridge] no global WebSocket and `ws` not installed — run: npm i ws'); process.exit(1); } }

const CTRL_PORT = process.argv[2] || process.env.MC_CTRL_PORT || '7211';
const CTRL = `ws://127.0.0.1:${CTRL_PORT}`, PORT = process.env.MC_MOD_PORT || 9099;
// When MC_MOD_KEY is set (prod behind nginx), /cmd REQUIRES ?key=<KEY> or X-Mod-Key.
// Local use (no env) leaves it open — it's already loopback-only via your ssh tunnel.
const MOD_KEY = process.env.MC_MOD_KEY || '';

// ---- addresses (RAM offsets = DC addr & 0xFFFFFF), all from docs/MVC2-MEMORY-MAP.md ----
const STRIDE = 0x5A4;
const SLOT = { P1C1:0x268340, P2C1:0x2688E4, P1C2:0x268E88, P2C2:0x26942C, P1C3:0x2699D0, P2C3:0x269F74 };
const OFF = { active:0x000, cid:0x001, x:0x034, y:0x038, vx:0x05C, vy:0x060, facing:0x110,
              sprite:0x144, animstate:0x1D0, xflip:0x1D2, hp:0x420, red:0x424,
              stance:0x1F9, speed:0x200, flight:0x201, armor:0x202, dmg:0x205, def:0x206,
              paltint:0x025, palid:0x52D };
const GLOBAL = { timer:0x289630, p1meterFill:0x289646, p2meterFill:0x289648,
                 p1meterLvl:0x28964A, p2meterLvl:0x28964B, p1combo:0x289670, p2combo:0x289672 };
const INPUT_DEC=0x2681DC, INMATCH=0x289624;
const REC_DIR = process.env.MAPLECAST_RECORDINGS_DIR || '/opt/maplecast/recordings';
let recordingOn = false;
const IBITS={up:0x2000,down:0x1000,left:0x0800,right:0x0400,lp:0x0200,hp:0x0100,lk:0x0040,hk:0x0020,a1:0x0080,a2:0x0010,start:0x8000};
const DIRS={'0,0':'N','0,1':'U','1,1':'UR','1,0':'R','1,-1':'DR','0,-1':'D','-1,-1':'DL','-1,0':'L','-1,1':'UL'};
function decInput(v){ const o={}; for(const k in IBITS) o[k]=(v&IBITS[k])?1:0; o.dir=DIRS[(o.right-o.left)+','+(o.up-o.down)]||'N'; return o; }
const u16le=(h,o)=>parseInt(h.slice((o+1)*2,(o+1)*2+2)+h.slice(o*2,o*2+2),16);
const HP_FULL = 0x90;  // 144 = full health for these chars (matches live read)
const CID = {'0x00':'Ryu','0x17':'Cable','0x23':'Dan','0x2a':'Storm','0x2c':'Magneto','0x32':'Colossus','0x34':'Sentinel','0x3a':'Servbot'};

// Resolve a slot name to a RAM base. 'P1*'/'P2*' = the ACTIVE (on-screen) char on that
// side — the team can be scrambled (swaps move the point char to any of C1/C2/C3), so
// targeting by fixed slot hits the wrong character. Picks active==1 nearest screen center.
async function resolveSlot(name){
  if(SLOT[name]) return SLOT[name];
  const m=/^P([12])\*$/.exec(name||''); if(!m) return SLOT.P1C1;
  const side=m[1], cand=[`P${side}C1`,`P${side}C2`,`P${side}C3`];
  let best=null, bestX=1e9;
  for(const n of cand){ const s=await readSlot(SLOT[n]); if(s.active===1 && Math.abs(s.x)<bestX){ bestX=Math.abs(s.x); best=SLOT[n]; } }
  return best || SLOT[cand[0]];
}

// ---- persistent control-WS connection with request/reply matching ----
let ws = null, rid = 0; const pend = new Map();
function connect(){
  ws = new WS(CTRL);
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
// MORPH IN PLACE (no tag animation): swap only the character IDENTITY between two slots,
// KEEPING each slot's situational state so the engine never sees a "point changed" event
// and never plays the tag sequence. Each slot stays where it was, same active/point flag,
// same velocity/facing/anim progress — but is now a different character.
//   KEEP (from self): active +0x000, pos +0x034/38, vel +0x05C/60, screen +0xE0/E4,
//                     facing-copy +0x110, anim_state/xflip/walkdir +0x1D0..0x1D3.
//   SWAP (from other): everything else = char_id, DAT/GFX/anim/hitbox pointers
//                     (+0x154..0x188), health, meter, palette, moveset.
// This is the experiment for re_kb/69's "clean instant swap". If the engine STILL tags,
// the hidden point-order field lives inside the swapped region — that finding narrows it.
const KEEP = [[0x000,1],[0x034,8],[0x05C,8],[0x0E0,8],[0x110,1],[0x1D0,4]];
function applyKeep(baseHex, selfHex){ let h=baseHex;
  for(const [off,len] of KEEP){ const a=off*2,b=(off+len)*2; h=h.slice(0,a)+selfHex.slice(a,b)+h.slice(b); }
  return h; }
async function morphInPlace(pair){
  const [aName,bName]=pair||['P1C1','P1C2']; const A=SLOT[aName], B=SLOT[bName];
  const ca=await rd(A,STRIDE), cb=await rd(B,STRIDE);
  await wr(A, applyKeep(cb, ca));   // A slot: B's identity, A's situation (stays point if it was)
  await wr(B, applyKeep(ca, cb));
  const va=await readSlot(A), vb=await readSlot(B);
  return { morphed:[aName,bName], now:{[aName]:`${va.name} active=${va.active}`,[bName]:`${vb.name} active=${vb.active}`},
           note:'no tag if point-order field is outside the struct — watch the stream' };
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

// ---- MOVEMENT: 8-way air dash / launch via velocity injection (+0x5C vx, +0x60 vy) ----
// Screen-absolute directions (R=+x, DOWN=+y in this space). float LE. Also sets stance=jump
// so the char is airborne + flight ON so gravity doesn't immediately cancel it. EXPERIMENTAL
// magnitudes — tune with the speed arg.
const fhex = v => { const b=Buffer.alloc(4); b.writeFloatLE(v); return b.toString('hex'); };
const DIR = { R:[1,0],L:[-1,0],U:[0,-1],D:[0,1], UR:[0.7,-0.7],UL:[-0.7,-0.7],DR:[0.7,0.7],DL:[-0.7,0.7] };
// A dash is a momentary VELOCITY KICK only — it does NOT set the flight flag or force a
// stance (that trapped the char in flight mode where the controller is ignored). Jump first
// with the pad, then dash in the air. Use RESET/LAND (■) to return to full control.
async function dash(base, dir, speed){
  const d=DIR[dir]; if(!d) throw new Error('bad dir'); const s=+speed||14;
  await wr(base+OFF.vx, fhex(d[0]*s)); await wr(base+OFF.vy, fhex(d[1]*s));
  return { dash:dir, speed:s, note:'velocity kick only — controller keeps control' };
}
// RESET/LAND — undo any stuck state (flight, forced stance/anim, residual velocity) so the
// controller regains control. This is the fix-it button if a mod traps the character.
async function stopMotion(base){
  await wr(base+OFF.vx, fhex(0)); await wr(base+OFF.vy, fhex(0));
  await wr(base+OFF.flight, '00');    // clear flight mode (the input-lockout culprit)
  await wr(base+OFF.stance, '00');    // back to standing
  await wr(base+OFF.animstate, '0000'); // neutral anim → engine resumes normal control
  return { reset:'landed — flight/stance/velocity/anim cleared, control returned' };
}

// ---- MOVES / ANIM: force the char into any of ITS animation groups (0x00-0x1B) or a move
// state. To use ANOTHER character's moves, MORPH first (that swaps in their moveset), then
// trigger. Writing anim_group (+0x158) plays that group; animation_state (+0x1D0) is the
// "which move to play" driver; special_move (+0x1E9) is the sp-move id. All EXPERIMENTAL —
// forcing arbitrary values mid-move can desync the anim walk; a neutral frame is safest. ----
async function playGroup(base, groupHex){ const g=parseInt(groupHex,16); if(!(g>=0&&g<=0x1B)) throw new Error('group 00-1B');
  await wr(base+0x158, g.toString(16).padStart(2,'0')); await wr(base+OFF.animstate, '0000'); return {playGroup:'0x'+g.toString(16)}; }
async function playState(base, valHex){ const v=parseInt(valHex,16); if(!(v>=0&&v<=0xFFFF)) throw new Error('bad state');
  const b=Buffer.alloc(2); b.writeUInt16LE(v); await wr(base+OFF.animstate, b.toString('hex')); return {animState:'0x'+v.toString(16)}; }
async function special(base, valHex){ const v=parseInt(valHex,16)&0xFF; await wr(base+0x1E9, v.toString(16).padStart(2,'0')); return {special:'0x'+v.toString(16)}; }

const ACT = {
  read: readSlots,
  dataset_record: async(q)=>{ recordingOn=!!q.on; const m=await ctrl({cmd:'dataset_record', on:!!q.on}); return m.ok ? m.data : m; },
  monitor: async()=>{
    const p1=await readSlot(SLOT.P1C1), p2=await readSlot(SLOT.P2C1);
    const g=await rd(GLOBAL.timer,0x50); const gg=a=>u8(g,a-GLOBAL.timer);
    const inMatch=parseInt(await rd(INMATCH,1),16)!==0;
    const inp=await rd(INPUT_DEC,0x28);
    const i1=decInput(u16le(inp,0x00)), i2=decInput(u16le(inp,0x14));
    let mctele=null;
    try{ const fl=readdirSync(REC_DIR).filter(f=>f.endsWith('.mctele')).map(f=>{const st=statSync(REC_DIR+'/'+f);return{name:f,size:st.size,mtime:st.mtimeMs};}).sort((a,b)=>b.mtime-a.mtime);
      if(fl[0]) mctele={name:fl[0].name,size:fl[0].size,frames:Math.max(0,Math.floor((fl[0].size-84)/8972))}; }catch(e){}
    return { recording:recordingOn, inMatch,
      p1:{name:p1.name,hp:p1.hp,red:p1.red,x:p1.x,meterLvl:gg(GLOBAL.p1meterLvl),combo:gg(GLOBAL.p1combo)},
      p2:{name:p2.name,hp:p2.hp,red:p2.red,x:p2.x,meterLvl:gg(GLOBAL.p2meterLvl),combo:gg(GLOBAL.p2combo)},
      timer:gg(GLOBAL.timer), input:{p1:i1,p2:i2}, mctele };
  },
  swapP1: ()=>swapPoint(['P1C1','P1C2']), swapP2: ()=>swapPoint(['P2C1','P2C2']),
  morphP1: ()=>morphInPlace(['P1C1','P1C2']), morphP2: ()=>morphInPlace(['P2C1','P2C2']),
  teleport: teleportSides,
  healP1: ()=>heal(SLOT.P1C1), healP2: ()=>heal(SLOT.P2C1), healAll,
  flipP1: ()=>flip(SLOT.P1C1), flipP2: ()=>flip(SLOT.P2C1),
  meterP1: ()=>meterMax('P1'), meterP2: ()=>meterMax('P2'),
  speedP1: ()=>buff(SLOT.P1C1,'speed'), flightP1: ()=>buff(SLOT.P1C1,'flight'), armorP1: ()=>buff(SLOT.P1C1,'armor'),
  // movement / 8-way air dash (q.slot picks the slot; 'P1*'/'P2*' = active char)
  dash: async(q)=>dash(await resolveSlot(q.slot), q.dir, q.speed),
  stop: async(q)=>stopMotion(await resolveSlot(q.slot)),
  // moves / anim
  playGroup: async(q)=>playGroup(await resolveSlot(q.slot), q.group),
  playState: async(q)=>playState(await resolveSlot(q.slot), q.val),
  special:   async(q)=>special(await resolveSlot(q.slot), q.val),
  // slot-aware heal/flip/buff (target the dropdown/active char, not a fixed slot)
  healSlot:  async(q)=>heal(await resolveSlot(q.slot)),
  flipSlot:  async(q)=>flip(await resolveSlot(q.slot)),
  buffSlot:  async(q)=>buff(await resolveSlot(q.slot), q.kind),
  poke: (q)=>poke(q.off, q.hex),
};

// ---- HTTP server: panel + /cmd ----
const PANEL = `<!doctype html><meta charset=utf8><title>MapleCast Command Center</title>
<style>body{background:#0d0d10;color:#eee;font:13px/1.5 system-ui,monospace;margin:0;padding:16px;max-width:720px}
h1{font-size:18px;color:#0ff;margin:0 0 4px}h3{font-size:12px;color:#8cf;margin:14px 0 4px;text-transform:uppercase;letter-spacing:1px}
button{background:#1a2230;color:#eee;border:1px solid #46c;border-radius:6px;padding:8px 12px;margin:3px;cursor:pointer;font:inherit}
button:hover{background:#26344a}button.warn{border-color:#f80;color:#fc8}button.danger{border-color:#f44;color:#f88}button.sm{padding:6px 9px;font-size:12px}
pre{background:#000;border:1px solid #333;border-radius:6px;padding:10px;white-space:pre-wrap;max-height:34vh;overflow:auto}
input,select{background:#000;color:#0f0;border:1px solid #444;border-radius:4px;padding:6px;font:inherit}
.row{margin:6px 0}small{color:#888}.pad{display:inline-grid;grid-template-columns:repeat(3,46px);gap:3px;vertical-align:middle}
.pad button{margin:0;padding:8px 0;width:46px}fieldset{border:1px solid #223;border-radius:8px;margin:8px 0;padding:8px 12px}
legend{color:#8cf;font-size:12px;padding:0 6px}</style>
<h1>🎮 MapleCast Command Center</h1>
<small>Live RAM control via your ssh tunnel. Addresses grounded in docs/MVC2-MEMORY-MAP.md +
MVC2-FRAMEDATA-FIELDS.md (anotak↔marvelous2). Watch the webgpu-test tab. ⚠️ = experimental.</small>

<div class=row><b>Target slot:</b>
<select id=slot><option>P1C1</option><option>P2C1</option><option>P1C2</option><option>P2C2</option><option>P1C3</option><option>P2C3</option></select>
<button class=sm onclick=go('read')>🔄 Read all + globals</button></div>

<fieldset><legend>📼 Dataset Recording (training capture)</legend>
<button id=recbtn onclick=recToggle() style="background:#c0392b;font-weight:bold">● Recording OFF</button>
<span id=recmsg style="margin-left:8px;color:#8a8"></span><br>
<small>Captures .mctele (state + both players' inputs) during matches → R2. OFF by default; flip per training session.</small></fieldset>

<fieldset><legend>Character</legend>
<button onclick=go('morphP1')>🎭 Morph P1 in-place (no tag)</button>
<button onclick=go('morphP2')>🎭 Morph P2 in-place</button>
<button onclick=go('swapP1')>🔀 Swap P1 (tag)</button><button onclick=go('swapP2')>🔀 Swap P2</button>
<button class=warn onclick=go('teleport')>↔️ Teleport sides</button></fieldset>

<fieldset><legend>Movement — 8-way air dash ⚠️</legend>
<span class=pad>
<button onclick=dash('UL')>↖</button><button onclick=dash('U')>↑</button><button onclick=dash('UR')>↗</button>
<button onclick=dash('L')>←</button><button class=sm onclick=slotCall('stop')>■</button><button onclick=dash('R')>→</button>
<button onclick=dash('DL')>↙</button><button onclick=dash('D')>↓</button><button onclick=dash('DR')>↘</button>
</span>
&nbsp; speed <input id=speed value=14 size=3> &nbsp;<small>writes velocity + airborne + flight on the target slot</small></fieldset>

<fieldset><legend>Moves / Anim ⚠️ (to use another char's moves: Morph first, then trigger)</legend>
anim group <input id=group placeholder=00-1B size=4><button class=sm onclick=slotCall('playGroup',{group:val('group')})>▶ play group</button><br>
anim state <input id=state placeholder=hex size=6><button class=sm onclick=slotCall('playState',{val:val('state')})>▶ set state</button>
&nbsp; sp-move <input id=sp placeholder=hex size=6><button class=sm onclick=slotCall('special',{val:val('sp')})>▶ special</button></fieldset>

<fieldset><legend>Buffs (P1) ⚠️</legend>
<button onclick=go('meterP1')>⚡ meter max</button><button onclick=go('speedP1')>💨 speed</button>
<button onclick=go('flightP1')>🕊️ flight</button><button onclick=go('armorP1')>🛡️ armor</button></fieldset>

<fieldset><legend>Health / Facing</legend>
<button onclick=go('healP1')>❤️ Heal P1</button><button onclick=go('healP2')>❤️ Heal P2</button>
<button onclick=go('healAll')>❤️ Heal ALL</button>
<button onclick=go('flipP1')>🔃 Flip P1</button><button onclick=go('flipP2')>🔃 Flip P2</button></fieldset>

<fieldset><legend>Raw poke</legend>
RAM offset(hex) <input id=off placeholder=268340 size=10> bytes(hex LE) <input id=hex placeholder=90 size=16>
<button class=danger onclick=poke()>💥 Poke</button></fieldset>

<pre id=out>ready.</pre>
<script>
const out=document.getElementById('out');
const val=id=>document.getElementById(id).value;
const slot=()=>document.getElementById('slot').value;
async function call(action,q){ out.textContent='…';
  try{ const r=await fetch('/cmd',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({action,...q})});
    const j=await r.json(); out.textContent=JSON.stringify(j.reply??j,null,2); }
  catch(e){ out.textContent='ERROR: '+e.message; } }
function go(a){ call(a,{}); }                                   // fixed-slot actions
function slotCall(a,q){ call(a,{slot:slot(),...(q||{})}); }     // target-slot actions
function dash(dir){ call('dash',{slot:slot(),dir,speed:val('speed')}); }
function poke(){ call('poke',{off:val('off'),hex:val('hex')}); }
let recOn=false;
async function recToggle(){
  try{ const r=await fetch('/cmd',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({action:'dataset_record',on:!recOn})});
    const j=await r.json(); recOn=!!((j.reply&&j.reply.recording)||j.recording); }
  catch(e){ out.textContent='ERROR: '+e.message; }
  const b=document.getElementById('recbtn'); b.textContent=recOn?'● Recording ON':'● Recording OFF'; b.style.background=recOn?'#27ae60':'#c0392b';
  document.getElementById('recmsg').textContent=recOn?'capturing this session…':''; }
go('read');
</script>`;

function keyOK(req){
  if(!MOD_KEY) return true;   // no key configured = local/loopback mode, open
  const u=new URL(req.url,'http://x'); return u.searchParams.get('key')===MOD_KEY || req.headers['x-mod-key']===MOD_KEY;
}
http.createServer(async (req,res)=>{
  const path=req.url.split('?')[0];
  if(req.method==='GET' && (path==='/'||path==='/panel')){ res.writeHead(200,{'content-type':'text/html'}); return res.end(PANEL); }
  if(req.method==='GET' && path==='/monitor'){ try{ res.writeHead(200,{'content-type':'text/html'}); return res.end(readFileSync('/opt/maplecast/recmon.html')); }catch(e){ res.writeHead(500); return res.end('recmon.html not found'); } }
  if(req.method==='POST' && path==='/cmd'){
    if(!keyOK(req)){ res.writeHead(403,{'content-type':'application/json'}); return res.end(JSON.stringify({ok:false,error:'bad or missing mod key'})); }
    let body=''; req.on('data',d=>body+=d); req.on('end', async ()=>{
      try{ const q=JSON.parse(body||'{}'); const fn=ACT[q.action]; if(!fn) throw new Error('unknown action '+q.action);
        const reply=await fn(q); res.writeHead(200,{'content-type':'application/json'}); res.end(JSON.stringify({ok:true,reply})); }
      catch(e){ res.writeHead(200,{'content-type':'application/json'}); res.end(JSON.stringify({ok:false,error:e.message})); }
    }); return;
  }
  res.writeHead(404); res.end();
}).listen(PORT, ()=>console.log(`[bridge] mod bridge on :${PORT} -> control ${CTRL}${MOD_KEY?' (key-gated)':' (OPEN — loopback only)'}`));
