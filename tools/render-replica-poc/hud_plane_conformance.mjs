// hud_plane_conformance.mjs (re_kb 36g VERIFY harness)
//
// Connects to a replica-live server, parses the HUDQ tail of an in-match frame,
// and runs flycast's CaclulateSpritePlane conformance test on every ParaType-5
// sprite quad. A clean capture (the 4 verts ARE the engine's expanded [P,C,A,B])
// passes for ~all sprites; the pre-fix garbage capture passed only 10/32.
//
//   node hud_plane_conformance.mjs --url ws://127.0.0.1:7212 --frames 400 \
//        --want-spr 28 [--save _hud_cap_fresh/hudq_tail.bin]
//
// Plane assignment (flycast ta_vtx.cpp CaclulateSpritePlane, EXACT):
//   A = cv2 = quad corner [2]   B = cv3 = quad corner [3]
//   C = cv1 = quad corner [1]   P = cv0 = quad corner [0]   (P closed by the plane)
// Test: recompute P.x/P.y? No — P.x,P.y are GIVEN; the plane solves k1,k2 from
// P.x,P.y then derives P.z/P.u/P.v. The geometric CONSISTENCY we test is the SAME
// invariant the captured array must satisfy IF cv0..cv3 are one coherent sprite:
// the 4 corners form a parallelogram P,A,B with C the diagonal i.e. cv0 (P) must
// equal A + (B-A) + (C-A) = B + C - A. That is exactly the relation flycast's
// AppendSpriteVertex builds (cv0 is the 4th parallelogram corner). A capture that
// straddles two glyphs violates it. Tolerance 1.0px (HUD coords are integer-ish).
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import { zstdDecompressSync } from 'node:zlib';
import { WebSocket } from 'ws';

function arg(n,d){const i=process.argv.indexOf(n);return i>=0?process.argv[i+1]:d;}
const url      = arg('--url','ws://127.0.0.1:7212');
const want     = +arg('--frames','400');
const wantSpr  = +arg('--want-spr','24');
const tol      = +arg('--tol','1.0');
const savePath = arg('--save', null);

const MAGIC_ZCST=0x5453435A, MAGIC_MCRR=0x5252434D, MAGIC_FRMX=0x784D5246, HUDQ_MAGIC=0x48554451;

function unzcst(u8){
  const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);
  if(u8.length>=8 && dv.getUint32(0,true)===MAGIC_ZCST)
    return new Uint8Array(zstdDecompressSync(Buffer.from(u8.subarray(8))));
  return u8;
}
function applyGfxTailLen(u8,p){
  const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);
  if(p+4>u8.length) return p;
  const nGfx=dv.getUint32(p,true); p+=4;
  if(nGfx>64) return p-4;
  for(let i=0;i<nGfx;i++){ p+=4; p+=0x20000; }
  return p;
}
function applyPvrPalLen(u8,p){
  const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);
  if(p+4>u8.length) return p;
  const palLen=dv.getUint32(p,true); p+=4;
  if(palLen===0) return p;
  if(palLen>0x10000||p+palLen>u8.length) return p-4;
  return p+palLen;
}
// Locate the HUDQ tail by scanning for its magic (robust to small layout drift in
// the prefix tails — we only care about the HUDQ block for this test).
function findHudTail(u8){
  const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);
  for(let p=0;p+8<=u8.length;p+=4){
    if(dv.getUint32(p,true)===HUDQ_MAGIC){
      const nHud=dv.getUint32(p+4,true);
      if(nHud>0 && nHud<=4096 && p+8+nHud*96<=u8.length) return {p, nHud};
    }
  }
  return null;
}
function parseHud(u8){
  const loc=findHudTail(u8); if(!loc) return null;
  const dv=new DataView(u8.buffer,u8.byteOffset,u8.byteLength);
  let p=loc.p+8; const quads=[];
  for(let i=0;i<loc.nHud;i++){
    if(p+96>u8.length) break;
    const x=[0,1,2,3].map(k=>dv.getFloat32(p+k*4,true));
    const y=[0,1,2,3].map(k=>dv.getFloat32(p+16+k*4,true));
    const u=[0,1,2,3].map(k=>dv.getFloat32(p+32+k*4,true));
    const v=[0,1,2,3].map(k=>dv.getFloat32(p+48+k*4,true));
    const pcw=dv.getUint32(p+80,true),tcw=dv.getUint32(p+92,true),tsp=dv.getUint32(p+88,true);
    quads.push({x,y,u,v,pcw,tcw,tsp}); p+=96;
  }
  return {quads, rawStart:loc.p, rawLen:8+loc.nHud*96, raw:u8.slice(loc.p,loc.p+8+loc.nHud*96)};
}

// Plane conformance: cv0 (P) must be the 4th parallelogram corner of A=cv2,B=cv3,C=cv1.
// P_expected = B + C - A. Returns the L-inf pixel error of (x,y).
function planeErr(q){
  const A={x:q.x[2],y:q.y[2]}, B={x:q.x[3],y:q.y[3]}, C={x:q.x[1],y:q.y[1]}, P={x:q.x[0],y:q.y[0]};
  const ex = B.x + C.x - A.x, ey = B.y + C.y - A.y;
  return Math.max(Math.abs(P.x-ex), Math.abs(P.y-ey));
}

const ws = new WebSocket(url, { perMessageDeflate:false, maxPayload: 64*1024*1024 });
let frames=0, done=false, best=null;
const t0=Date.now();
ws.binaryType='arraybuffer';
ws.on('open', ()=>console.error(`[plane] connected ${url}`));
ws.on('error', e=>{ console.error('[plane] ws error', e.message); });
ws.on('close', ()=>{ if(!done) finish(); });
ws.on('message', (data)=>{
  if(done) return;
  frames++;
  let u8 = new Uint8Array(data);
  try { u8 = unzcst(u8); } catch {}
  const hud = parseHud(u8);
  if(hud){
    const spr = hud.quads.filter(q => ((q.pcw>>>29)&7)===5);
    if(spr.length >= wantSpr || (best && spr.length>best.spr.length)){
      best = {spr, hud, total: hud.quads.length};
      if(spr.length >= wantSpr){ finish(); return; }
    } else if(!best && spr.length>0){
      best = {spr, hud, total: hud.quads.length};
    }
  }
  if(frames>=want) finish();
});

function finish(){
  if(done) return; done=true;
  try{ ws.close(); }catch{}
  if(!best || best.spr.length===0){
    console.log(`RESULT: NO ParaType-5 sprites captured in ${frames} frames (best=${best?best.total:0} quads). Is the binary in-match with MAPLECAST_HUD_TA armed?`);
    process.exit(2);
  }
  const spr=best.spr;
  let pass=0, fail=0; const fails=[];
  spr.forEach((q,i)=>{
    const e=planeErr(q);
    if(e<=tol) pass++; else { fail++; if(fails.length<8) fails.push({i, e:+e.toFixed(2), tcw:q.tcw.toString(16)}); }
  });
  console.log(`RESULT: plane-conformance ${pass}/${spr.length} sprites pass (tol=${tol}px); total HUD quads=${best.total}; frames scanned=${frames}; ${(Date.now()-t0)/1000}s`);
  if(fails.length) console.log('  first fails:', JSON.stringify(fails));
  if(savePath){ mkdirSync(dirname(savePath),{recursive:true}); writeFileSync(savePath, best.hud.raw); console.log(`  saved HUDQ tail -> ${savePath} (${best.hud.raw.length}B)`); }
  process.exit(pass>=Math.ceil(spr.length*0.9) ? 0 : 1);
}
