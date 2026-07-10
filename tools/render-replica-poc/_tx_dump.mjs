// _tx_dump.mjs — dump ONE node-frame: rf quad corners vs ASMTRACE parts, side by side.
//   node _tx_dump.mjs <file.mcrr> <asm.log> <vframe> <node8hex>
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';

const path = process.argv[2], asmPath = process.argv[3], wantVf = +process.argv[4], wantNode = process.argv[5];

const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); u32(); const nS=u32(), nD=u32(), nF=u32(), vb=u32(), pb=u32(); u32();
const region = () => { const a=u32(), l=u32(); let t=''; for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);} p+=8; return {addr:a>>>0, len:l, tag:t}; };
const sR = Array.from({length:nS}, region), dR = Array.from({length:nD}, region);
const ram = new Uint8Array(16*1024*1024);
let q = p + vb + pb;
for (const r of sR) { const b = buf.subarray(q, q+r.len); q+=r.len; if(r.tag==='ram16') ram.set(b,0); else ram.set(b, r.addr&0xFFFFFF); }
p = q; const frames = [];
for (let f=0; f<nF; f++){ u32(); const vf=u32(); const tail=u32(); const dof=p; for(const r of dR) p+=r.len; p+=tail; frames.push({vf, dof}); }

const asmParts = [];
for (const line of readFileSync(asmPath,'utf8').split('\n')) {
  if (!line || line[0]==='#') continue;
  const t = line.split(/\s+/);
  if (+t[0] !== wantVf || t[17] !== wantNode) continue;
  asmParts.push({ sel:+t[4], dx:+t[5], dy:+t[6], sx:+t[9], sy:+t[10], row:+t[12], flip:+t[13], flags:t[14] });
}

const M = await createRenderFrame({ locateFile: x => x });
const ramPtr = M._malloc(ram.length), outPtr = M._malloc(256*1024);
const fr = frames.find(f=>f.vf===wantVf);
if(!fr) throw new Error('vframe not in capture');
let o = fr.dof; for (const r of dR) { ram.set(buf.subarray(o, o+r.len), r.addr&0xFFFFFF); o+=r.len; }
M.HEAPU8.set(ram, ramPtr);
M._render_frame_ta(ramPtr, outPtr, 256*1024);
const quads = M._render_frame_quad_count();
const ta = M.HEAPU8.subarray(outPtr, outPtr + quads*96);
const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
const selsPtr = M._malloc(quads*2);
M._render_frame_quad_sels(selsPtr, quads);
const qSels = new Uint16Array(M.HEAPU8.buffer, selsPtr, quads).slice();
const objN = M._render_frame_obj_count();
let qi = 0, rf = [];
for (let b=0; b<objN; b++) {
  const node = ((M._render_frame_obj_node(b) >>> 0) & 0x0FFFFFFF).toString(16).padStart(8,'0');
  const nt = M._render_frame_obj_ntiles(b);
  if (node === wantNode)
    for (let k=0; k<nt; k++){ const off=(qi+k)*96;
      rf.push({ sel:qSels[qi+k],
                Ax:tdv.getFloat32(off+36,true), Ay:tdv.getFloat32(off+40,true),
                Bx:tdv.getFloat32(off+48,true), By:tdv.getFloat32(off+52,true),
                Cx:tdv.getFloat32(off+60,true), Cy:tdv.getFloat32(off+64,true),
                Dx:tdv.getFloat32(off+72,true), Dy:tdv.getFloat32(off+76,true) }); }
  qi += nt;
}
console.log(`vf=${wantVf} node=${wantNode}: rf ${rf.length} quads, asm ${asmParts.length} parts`);
console.log(`  k | asm sel  sx     sy    flip flags | rf sel  Ax     Ay     Bx     Cx     Cy     | Ax-sx   Bx-sx   Cy-sy  Ay-sy`);
for (let k=0; k<Math.max(rf.length, asmParts.length); k++){
  const a = asmParts[k], r = rf[k];
  const f = v => v===undefined?'   -  ':v.toFixed(1).padStart(6);
  console.log(`  ${String(k).padStart(2)} | ${a?String(a.sel).padStart(4):'  - '} ${a?f(a.sx):'   -  '} ${a?f(a.sy):'   -  '}  ${a?a.flip:'-'}  ${a?a.flags:' -  '} | ${r?String(r.sel).padStart(4):'  - '} ${r?f(r.Ax):'   -  '} ${r?f(r.Ay):'   -  '} ${r?f(r.Bx):'   -  '} ${r?f(r.Cx):'   -  '} ${r?f(r.Cy):'   -  '} | ${a&&r?f(r.Ax-a.sx):'   -  '} ${a&&r?f(r.Bx-a.sx):'   -  '} ${a&&r?f(r.Cy-a.sy):'   -  '} ${a&&r?f(r.Ay-a.sy):'   -  '}`);
}
