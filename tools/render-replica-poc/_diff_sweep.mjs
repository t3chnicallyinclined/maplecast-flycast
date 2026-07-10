// _diff_sweep.mjs — sweep EVERY frame of a capture, per-NODE match render_frame vs
// ASMTRACE ground truth, and rank the worst mis-positioned parts.
//   node _diff_sweep.mjs <file.mcrr> <asm.log> [pxThreshold=20]
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';

const path = process.argv[2], asmPath = process.argv[3], TH = +(process.argv[4] || 20);

// ---- parse .mcrr (capture_break format: header + dyn + stamped tail) ----
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); const ver=u32(), nS=u32(), nD=u32(), nF=u32(), vb=u32(), pb=u32(); u32();
const region = () => { const a=u32(), l=u32(); let t=''; for(let i=0;i<8;i++){const c=buf[p+i];if(c)t+=String.fromCharCode(c);} p+=8; return {addr:a>>>0, len:l, tag:t}; };
const sR = Array.from({length:nS}, region), dR = Array.from({length:nD}, region);
const ram = new Uint8Array(16*1024*1024);
let q = p + vb + pb;
for (const r of sR) { const b = buf.subarray(q, q+r.len); q+=r.len; if(r.tag==='ram16') ram.set(b,0); else ram.set(b, r.addr&0xFFFFFF); }
p = q; const frames = [];
for (let f=0; f<nF; f++){ const fm=u32(), vf=u32(), tail=u32(); const dof=p; for(const r of dR) p+=r.len; p+=tail; frames.push({vf, dof}); }
const vfMin = Math.min(...frames.map(f=>f.vf)), vfMax = Math.max(...frames.map(f=>f.vf));

// ---- index the ASMTRACE by vframe -> node -> [parts], only for our range ----
const asmByVf = new Map();
for (const line of readFileSync(asmPath,'utf8').split('\n')) {
  if (!line || line[0]==='#') continue;
  const t = line.split(/\s+/); const vf = +t[0];
  if (vf < vfMin || vf > vfMax) continue;
  let m = asmByVf.get(vf); if(!m){ m=new Map(); asmByVf.set(vf,m); }
  const node = t[17]; let a = m.get(node); if(!a){ a=[]; m.set(node,a); }
  a.push({ sel:+t[4], sx:+t[9], sy:+t[10], flags:parseInt(t[14],16)||0 });
}

const M = await createRenderFrame({ locateFile: x => x });
const RAM = ram.length, cap = 256*1024;
const ramPtr = M._malloc(RAM), outPtr = M._malloc(cap);

const worst = [];   // {vf, node, part, sel, dX, dY}
const cover = [];   // {vf, node, rf, asm, extra} — over-emission
let framesChecked = 0;
for (let fi=0; fi<nF; fi++) {
  const fr = frames[fi];
  const asm = asmByVf.get(fr.vf); if (!asm) continue;
  // seed this frame's dyn regions
  let o = fr.dof; for (const r of dR) { ram.set(buf.subarray(o, o+r.len), r.addr&0xFFFFFF); o+=r.len; }
  M.HEAPU8.set(ram, ramPtr);
  M._render_frame_ta(ramPtr, outPtr, cap);
  const quads = M._render_frame_quad_count();
  const ta = M.HEAPU8.slice(outPtr, outPtr + quads*96);
  const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
  const objN = M._render_frame_obj_count();
  // partition rf quads by owning node (body-major, obj_ntiles each)
  let qi = 0;
  for (let b=0; b<objN; b++) {
    const node = ((M._render_frame_obj_node(b) >>> 0) & 0x0FFFFFFF).toString(16).padStart(8,'0');
    const nt = M._render_frame_obj_ntiles(b);
    const a = asm.get(node);
    const aLen = a ? a.length : 0;
    // COVERAGE: render_frame emitting MORE parts than the engine = extra quads drawn
    // where the engine draws nothing (the out-of-place solid sprite class). Or 0 asm =
    // render_frame drew a node the engine didn't render this frame at all.
    if (nt > aLen) cover.push({ vf:fr.vf, node, rf:nt, asm:aLen, extra: nt-aLen });
    if (a) {
      const n = Math.min(nt, a.length);
      for (let k=0; k<n; k++) {
        const off = (qi+k)*96;
        const ax = tdv.getFloat32(off+36, true), cy = tdv.getFloat32(off+64, true);
        const dX = Math.abs(ax - a[k].sx), dY = Math.abs(cy - a[k].sy);
        if (dX > TH || dY > TH) worst.push({ vf:fr.vf, node, part:k, sel:a[k].sel, flags:a[k].flags, dX, dY });
      }
    }
    qi += nt;
  }
  framesChecked++;
}
worst.sort((x,y) => (y.dX+y.dY) - (x.dX+x.dY));
console.log(`swept ${framesChecked} frames (vframe ${vfMin}..${vfMax}); ${worst.length} parts > ${TH}px off`);
console.log(`\nworst 20 mis-positioned parts:`);
console.log(` vframe   node       part sel   flags   dX     dY`);
for (const w of worst.slice(0,20))
  console.log(`  ${w.vf}  ${w.node.padEnd(9)} ${String(w.part).padStart(3)}  ${String(w.sel).padStart(4)}  0x${w.flags.toString(16).padStart(4,'0')}  ${w.dX.toFixed(0).padStart(4)}  ${w.dY.toFixed(0).padStart(4)}`);
// which nodes recur
const byNode = {};
for (const w of worst) byNode[w.node] = (byNode[w.node]||0)+1;
console.log(`\ndivergent-part count by node:`);
for (const [n,c] of Object.entries(byNode).sort((a,b)=>b[1]-a[1]).slice(0,8)) console.log(`  ${n}: ${c}`);
// OVER-EMISSION: render_frame draws parts the engine didn't (the out-of-place solid sprite)
cover.sort((x,y) => y.extra - x.extra);
console.log(`\nOVER-EMISSION (render_frame parts > engine parts): ${cover.length} node-frames`);
console.log(` vframe   node       rf  asm  extra`);
for (const c of cover.slice(0,20))
  console.log(`  ${c.vf}  ${c.node.padEnd(9)} ${String(c.rf).padStart(3)} ${String(c.asm).padStart(4)}  ${String(c.extra).padStart(4)}`);
