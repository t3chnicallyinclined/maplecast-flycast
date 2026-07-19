// training_server.mjs — MapleCast Training Console (standalone)
// A DEDICATED operator console for the mvc2-ai dataset exporter: recording
// control, live monitor, session library, R2/storage totals, training status.
// Deliberately SEPARATE from the mod command center (mod_bridge.mjs) — its own
// service, port, and key. Reads state + flips recording via the loopback
// control WS (7211); rclone gives R2 totals. RAM never touches the public net.
//
//   run:   MC_TRAIN_KEY=... /usr/bin/node /opt/maplecast/training_server.mjs
//   nginx: location /training/ { proxy_pass http://127.0.0.1:9097/; }
import http from 'node:http';
import { readFileSync, readdirSync, statSync, openSync, readSync, closeSync } from 'node:fs';
import { execFile } from 'node:child_process';

// Node 20 (prod) has no global WebSocket — fall back to the `ws` package
// (present in /opt/maplecast/node_modules, so run with that cwd).
let WS = globalThis.WebSocket;
if(!WS){ try{ WS=(await import('ws')).WebSocket||(await import('ws')).default; }
  catch{ console.error('[training] need global WebSocket or `ws` package (npm i ws)'); process.exit(1); } }

const CTRL_PORT = process.env.MC_CTRL_PORT || '7211';
const CTRL = `ws://127.0.0.1:${CTRL_PORT}`;
const PORT = process.env.MC_TRAIN_PORT || 9097;
const KEY  = process.env.MC_TRAIN_KEY || '';              // gates /api/* when set
const REC_DIR = process.env.MAPLECAST_RECORDINGS_DIR || '/opt/maplecast/recordings';
const R2_REMOTE = process.env.MC_R2_REMOTE || 'r2:mvc2-dataset/recordings';  // matches r2-sync upload target
const PAGE = process.env.MC_TRAIN_PAGE || '/opt/maplecast/training.html';
const TRAIN_STATUS = process.env.MC_TRAIN_STATUS || '/opt/maplecast/training-status.json';
const SYNC_MARK = REC_DIR + '/.last-r2-sync';            // touched by r2-sync-recordings.sh
// Distributed node registry (Pillar 5). nginx proxies /hub/api/ -> :7220 with the
// full path, so the hub answers /hub/api/nodes on loopback too.
const HUB_NODES_URL = process.env.MC_HUB_NODES_URL || 'http://127.0.0.1:7220/hub/api/nodes';
const NODE_NAME = process.env.MC_NODE_NAME || 'nobd-main';   // this box's node in the registry (the local recorder)
const NODE_KEY = process.env.MC_NODE_KEY || '';             // shared key for the per-node /noderec agents

// ---- addresses (RAM offsets = DC addr & 0xFFFFFF) ----
const STRIDE = 0x5A4;
const SLOT = { P1C1:0x268340, P2C1:0x2688E4, P1C2:0x268E88, P2C2:0x26942C, P1C3:0x2699D0, P2C3:0x269F74 };
const OFF = { active:0x000, cid:0x001, x:0x034, hp:0x420, red:0x424, xflip:0x1D2 };
const GLOBAL = { timer:0x289630, p1meterLvl:0x28964A, p2meterLvl:0x28964B, p1combo:0x289670, p2combo:0x289672 };
const INPUT_DEC = 0x2681DC, INMATCH = 0x289624;
const CID = {'0x00':'Ryu','0x17':'Cable','0x23':'Dan','0x2a':'Storm','0x2c':'Magneto','0x32':'Colossus','0x34':'Sentinel','0x3a':'Servbot'};
const IBITS = {up:0x2000,down:0x1000,left:0x0800,right:0x0400,lp:0x0200,hp:0x0100,lk:0x0040,hk:0x0020,a1:0x0080,a2:0x0010,start:0x8000};
const DIRS = {'0,0':'N','0,1':'U','1,1':'UR','1,0':'R','1,-1':'DR','0,-1':'D','-1,-1':'DL','-1,0':'L','-1,1':'UL'};
function decInput(v){ const o={}; for(const k in IBITS) o[k]=(v&IBITS[k])?1:0; o.dir=DIRS[(o.right-o.left)+','+(o.up-o.down)]||'N'; return o; }

// ---- control WS (persistent, request/reply matched) ----
let ws=null, rid=0; const pend=new Map();
function connect(){ ws=new WS(CTRL);
  ws.onopen=()=>console.log('[training] control WS connected',CTRL);
  ws.onclose=()=>{ ws=null; setTimeout(connect,2000); };
  ws.onerror=()=>{};
  ws.onmessage=ev=>{ try{ const m=JSON.parse(ev.data); const r=pend.get(m.reply_id); if(r){ pend.delete(m.reply_id); r(m); } }catch{} }; }
connect();
function ctrl(o){ return new Promise((res,rej)=>{
  if(!ws || ws.readyState!==1) return rej(new Error('control WS not connected'));
  const id='t'+(++rid); pend.set(id,res); ws.send(JSON.stringify({...o, reply_id:id}));
  setTimeout(()=>{ if(pend.has(id)){ pend.delete(id); rej(new Error('control WS timeout')); } }, 8000); }); }
const rd  = async (off,size)=>{ const m=await ctrl({cmd:'ram_read',offset:off,size}); if(!m.ok) throw new Error(JSON.stringify(m)); return m.data.hex; };
const f32 = (h,o)=>Buffer.from(h.slice(o*2,(o+4)*2),'hex').readFloatLE(0);
const u8  = (h,o)=>parseInt(h.slice(o*2,o*2+2),16);
const u16le=(h,o)=>parseInt(h.slice((o+1)*2,(o+1)*2+2)+h.slice(o*2,o*2+2),16);
async function readSlot(base){ const h=await rd(base,STRIDE);
  const cid='0x'+h.slice(OFF.cid*2,OFF.cid*2+2);
  return { active:u8(h,OFF.active), cid, name:CID[cid]||cid, x:+f32(h,OFF.x).toFixed(1),
           hp:u8(h,OFF.hp), red:u8(h,OFF.red) }; }

// ---- .mctele header parse → exact frame count ----
// header: magic[8]"MCTELE01" + ver(4) + blob_len(4) + num_segs(4) + [addr,len]*n + first_frame(8) + reserved(16)
// per-frame record: frame(8) + blob(blob_len)
function teleInfo(path,size){
  try{ const fd=openSync(path,'r'); const b=Buffer.alloc(20); const n=readSync(fd,b,0,20,0); closeSync(fd);
    if(n<20 || b.toString('latin1',0,6)!=='MCTELE') return { frames:0, blobLen:0 };
    const blobLen=b.readUInt32LE(12), nsegs=b.readUInt32LE(16);
    const hdr=44+8*nsegs, per=8+blobLen;
    return { frames: per>0 ? Math.max(0,Math.floor((size-hdr)/per)) : 0, blobLen, segs:nsegs };
  }catch{ return { frames:0, blobLen:0 }; }
}
function listSessions(){
  try{ return readdirSync(REC_DIR).filter(f=>f.endsWith('.mctele')).map(f=>{
    const p=REC_DIR+'/'+f, st=statSync(p); const ti=teleInfo(p,st.size);
    return { name:f, size:st.size, mtime:st.mtimeMs, frames:ti.frames };
  }).sort((a,b)=>b.mtime-a.mtime); }catch{ return []; }
}
function lastSync(){ try{ return statSync(SYNC_MARK).mtimeMs; }catch{ return null; } }
function trainingStatus(){ try{ return JSON.parse(readFileSync(TRAIN_STATUS,'utf8')); }
  catch{ return { model:null, lastRun:null,
    metrics:{ 'Milestone A (input-only)':'+10.4pp vs repeat-last-input on decision frames' },
    note:'State-conditioned model pending — needs real 2-human matches to accumulate on R2.' }; } }

// ---- R2 totals via rclone (cached; refreshed in background) ----
let r2={ ok:false, names:new Set(), count:0, bytes:0, at:0, err:'(loading)' };
function refreshR2(){ execFile('rclone',['lsjson',R2_REMOTE,'--files-only'],{timeout:15000,maxBuffer:16*1024*1024},(e,out)=>{
  if(e){ r2.ok=false; r2.err=String(e.message||e).slice(0,200); return; }
  try{ const arr=JSON.parse(out); const names=new Set(); let bytes=0;
    for(const o of arr){ names.add(o.Name); bytes+=(o.Size||0); }
    r2={ ok:true, names, count:arr.length, bytes, at:Date.now(), err:null };
  }catch(pe){ r2.ok=false; r2.err='parse: '+pe.message; } }); }
setInterval(refreshR2, 60000); refreshR2();

// ---- API ----
let recordingOn=false;                                   // this console's intent flag
async function apiMonitor(){
  const p1=await readSlot(SLOT.P1C1), p2=await readSlot(SLOT.P2C1);
  const g=await rd(GLOBAL.timer,0x50); const gg=a=>u8(g,a-GLOBAL.timer);
  const inMatch=parseInt(await rd(INMATCH,1),16)!==0;
  const inp=await rd(INPUT_DEC,0x28);
  const i1=decInput(u16le(inp,0x00)), i2=decInput(u16le(inp,0x14));
  const sess=listSessions(); const cur=sess[0]||null;
  const writing = cur ? (Date.now()-cur.mtime) < 3000 : false;   // .mctele actively growing
  return { recording:recordingOn, writing, inMatch,
    p1:{name:p1.name,cid:p1.cid,hp:p1.hp,red:p1.red,x:p1.x,meterLvl:gg(GLOBAL.p1meterLvl),combo:gg(GLOBAL.p1combo)},
    p2:{name:p2.name,cid:p2.cid,hp:p2.hp,red:p2.red,x:p2.x,meterLvl:gg(GLOBAL.p2meterLvl),combo:gg(GLOBAL.p2combo)},
    timer:gg(GLOBAL.timer), input:{p1:i1,p2:i2},
    mctele: cur ? { name:cur.name, size:cur.size, frames:cur.frames } : null };
}
async function apiRecord(on){ recordingOn=!!on; const m=await ctrl({cmd:'dataset_record',on:!!on}); return m.ok?{recording:recordingOn}:{recording:recordingOn,warn:m}; }
function apiSessions(){ return { sessions: listSessions().map(s=>({ ...s, inR2: r2.names.has(s.name) })), r2ok:r2.ok, r2err:r2.err }; }
function apiStorage(){ const sess=listSessions();
  const localBytes=sess.reduce((a,s)=>a+s.size,0), localFrames=sess.reduce((a,s)=>a+s.frames,0);
  const uploaded=sess.filter(s=>r2.names.has(s.name)).length;
  return { local:{ count:sess.length, bytes:localBytes, frames:localFrames, uploaded, pending:sess.length-uploaded },
           r2:{ ok:r2.ok, count:r2.count, bytes:r2.bytes, at:r2.at, err:r2.err }, lastSync:lastSync() }; }

// ---- distributed node registry (Pillar 5 hub) ----
// recordability is NOT hub-tracked (no can_record field) — recording is a per-node
// env/toggle on that node's flycast. Today only the self node has the exporter wired,
// so we mark self=recordable and the rest view-only until per-node recording is plumbed.
let nodesCache={ at:0, nodes:[], err:null };
// query a remote node's /noderec agent (through its Caddy) for live recording state
async function nodeRecStatus(host){
  try{ const r=await fetch(`https://${host}/noderec/status?key=${encodeURIComponent(NODE_KEY)}`,{signal:AbortSignal.timeout(4000)});
    if(!r.ok) return { agentOk:false };
    const j=await r.json();
    return { agentOk:!!j.ok, recording:!!j.recording, writing:!!j.writing, recInMatch:!!j.inMatch,
             recFrames:(j.mctele&&j.mctele.frames)||0 };
  }catch{ return { agentOk:false }; }
}
async function apiNodes(){
  const now=Date.now();
  if(now-nodesCache.at < 8000 && nodesCache.nodes.length) return { nodes:nodesCache.nodes, self:NODE_NAME };
  try{
    const r=await fetch(HUB_NODES_URL,{signal:AbortSignal.timeout(6000)});
    const raw=(await r.json()).nodes||[];
    const nodes=raw.map(n=>{ const m=n.metrics||{}, g=n.geo||{}; const self=n.name===NODE_NAME;
      return { name:n.name, node_id:n.node_id, status:n.status, region:n.region, city:g.city||null,
               host:n.public_host, clients:m.clients||0, spectators:n.spectators||0,
               frames:m.frames_received||0, upstream:!!m.upstream_connected,
               inMatch:n.status==='in_match'||!!n.game, version:n.version, uptime_s:n.uptime_s||0,
               self, recordable:false, recording:false }; });
    // enrich each node with live recording state: self = local flag, remotes = /noderec agent
    await Promise.all(nodes.map(async n=>{
      if(n.self){ n.agentOk=true; n.recordable=true; n.recording=recordingOn; }
      else if(n.host){ Object.assign(n, await nodeRecStatus(n.host)); n.recordable=!!n.agentOk; }
    }));
    const rank=n=> n.self?0 : n.status==='ready'?1 : 2;              // self, then ready, then rest
    nodes.sort((a,b)=> rank(a)-rank(b) || String(a.name).localeCompare(String(b.name)));
    nodesCache={ at:now, nodes, err:null };
    return { nodes, self:NODE_NAME };
  }catch(e){ nodesCache.err=String(e.message||e); return { nodes:nodesCache.nodes, self:NODE_NAME, err:nodesCache.err }; }
}
// toggle recording on a node: self -> local control WS; remote -> its /noderec agent
async function apiNodeRecord(name, on){
  if(name===NODE_NAME) return await apiRecord(on);
  const node=(nodesCache.nodes||[]).find(n=>n.name===name);
  if(!node || !node.host) return { ok:false, error:'unknown node '+name };
  try{ const r=await fetch(`https://${node.host}/noderec/record?on=${on?1:0}&key=${encodeURIComponent(NODE_KEY)}`,
        {method:'POST',signal:AbortSignal.timeout(5000)});
    nodesCache.at=0;   // force a fresh status pull on next poll
    return await r.json();
  }catch(e){ return { ok:false, error:String(e.message||e) }; }
}

function auth(req,url){ if(!KEY) return true; const k=url.searchParams.get('key')||req.headers['x-train-key']; return k===KEY; }
function send(res,code,obj){ res.writeHead(code,{'content-type':'application/json'}); res.end(JSON.stringify(obj)); }

http.createServer(async (req,res)=>{
  const url=new URL(req.url,'http://x'); const path=url.pathname;
  if(req.method==='GET' && (path==='/'||path==='/index.html'||path==='/training')){
    try{ res.writeHead(200,{'content-type':'text/html'}); return res.end(readFileSync(PAGE)); }
    catch{ res.writeHead(500); return res.end('training.html not found at '+PAGE); }
  }
  if(path.startsWith('/api/')){
    if(!auth(req,url)) return send(res,401,{ok:false,error:'bad or missing key'});
    try{
      if(path==='/api/monitor')  return send(res,200,{ok:true, ...(await apiMonitor())});
      if(path==='/api/sessions') return send(res,200,{ok:true, ...apiSessions()});
      if(path==='/api/storage')  return send(res,200,{ok:true, ...apiStorage()});
      if(path==='/api/nodes')    return send(res,200,{ok:true, ...(await apiNodes())});
      if(path==='/api/node-record'){ const v=url.searchParams.get('on'); return send(res,200,{ok:true, ...(await apiNodeRecord(url.searchParams.get('node'), v==='1'||v==='true'))}); }
      if(path==='/api/training') return send(res,200,{ok:true, training:trainingStatus()});
      if(path==='/api/record'){ const v=url.searchParams.get('on'); return send(res,200,{ok:true, ...(await apiRecord(v==='1'||v==='true'))}); }
      return send(res,404,{ok:false,error:'no such endpoint'});
    }catch(e){ return send(res,500,{ok:false,error:String(e.message||e)}); }
  }
  res.writeHead(404); res.end('not found');
}).listen(PORT,'127.0.0.1',()=>console.log('[training] console http://127.0.0.1:'+PORT+' → control '+CTRL+(KEY?' (key-gated)':' (OPEN)')));
