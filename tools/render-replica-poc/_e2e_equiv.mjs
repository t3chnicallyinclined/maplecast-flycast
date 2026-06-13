// End-to-end: render the SAME real body node through render_object_full (body path) and
// render_object_full_satellite (the new loc_8c030af8 path); compare emitted quad anchors.
// This proves the satellite walker output == body walker output to <1px on a real node.
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';
const ram = readFileSync('./_ram_real.bin');
const M = await createRenderFrame({ locateFile: x => x });
// We can't call render_object_full_* directly (not exported); instead drive the full frame
// then ALSO reach into per-quad anchors. But the frame routes by cat. To compare paths on
// ONE node, temporarily flip the node's +0x3 cat byte in a copy and re-run.
const G = a => (a>>>0)&0xFFFFFF;
const node = 0x8c2688e4;
function runWith(catByte){
  const r = Uint8Array.from(ram); r[G(node)+3] = catByte;   // 0=body path, 4=satellite path
  const rp = M._malloc(r.length); M.HEAPU8.set(r, rp);
  const cap = 256*1024, op = M._malloc(cap);
  const len = M._render_frame_ta(rp, op, cap);
  const quads = M._render_frame_quad_count();
  const bodies = M._render_frame_body_count(), sats = M._render_frame_sat_count();
  const ta = M.HEAPU8.slice(op, op+len);
  const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
  const q=[]; for(let i=0;i<quads;i++){const o=i*96; q.push({bx:tdv.getFloat32(o+36,true), cy:tdv.getFloat32(o+64,true)});}
  M._free(rp); M._free(op);
  return {quads, bodies, sats, q};
}
const B = runWith(0);   // node as cat==0 BODY  -> render_object_full (loc_8c03093c)
const S = runWith(4);   // node as cat==4 SAT   -> render_object_full_satellite (loc_8c030af8)
console.log(`BODY path:      bodies=${B.bodies} sats=${B.sats} quads=${B.quads}`);
console.log(`SATELLITE path: bodies=${S.bodies} sats=${S.sats} quads=${S.quads}`);
// match each satellite-path quad to the nearest body-path quad
let maxDX=0,maxDY=0;
const n=Math.min(B.q.length,S.q.length);
for(let i=0;i<n;i++){ // both emit in the same walker order for this node
  const dx=Math.abs(B.q[i].bx-S.q[i].bx), dy=Math.abs(B.q[i].cy-S.q[i].cy);
  maxDX=Math.max(maxDX,dx); maxDY=Math.max(maxDY,dy);
}
console.log(`per-quad screen-anchor diff (body-path vs satellite-path, same node): maxDX=${maxDX.toFixed(4)}px maxDY=${maxDY.toFixed(4)}px over ${n} quads`);
console.log(maxDX<1 && maxDY<1 && S.quads===B.quads ? 'RESULT: PASS (satellite path renders the node at the body position, <1px)' : 'RESULT: REVIEW');
