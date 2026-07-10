// _tx_detect.mjs — re_kb/51 TRANSPOSITION detector: per-frame per-node, index-pair
// render_frame parts vs ASMTRACE parts, and find the 2-part swaps (rf[i]≈asm[j] &&
// rf[j]≈asm[i], i≠j) that survive proper pairing. Handles the over-emission pairing
// shift by aligning with a 1-deletion pass when counts differ by 1.
//   node _tx_detect.mjs <file.mcrr> <asm.log> [pxTol=6]
import { readFileSync } from 'node:fs';
import createRenderFrame from './render_frame_node.mjs';

const path = process.argv[2], asmPath = process.argv[3], TOL = +(process.argv[4] || 6);

// ---- parse .mcrr (capture_break format) ----
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
for (let f=0; f<nF; f++){ const fm=u32(), vf=u32(), tail=u32(); const dof=p; for(const r of dR) p+=r.len; p+=tail; frames.push({vf, dof}); }
const vfMin = Math.min(...frames.map(f=>f.vf)), vfMax = Math.max(...frames.map(f=>f.vf));

// ---- ASMTRACE index: vf -> node -> [{sel,sx,sy}] in fire order ----
const asmByVf = new Map();
for (const line of readFileSync(asmPath,'utf8').split('\n')) {
  if (!line || line[0]==='#') continue;
  const t = line.split(/\s+/); const vf = +t[0];
  if (vf < vfMin || vf > vfMax) continue;
  let m = asmByVf.get(vf); if(!m){ m=new Map(); asmByVf.set(vf,m); }
  const node = t[17]; let a = m.get(node); if(!a){ a=[]; m.set(node,a); }
  a.push({ sel:+t[4], sx:+t[9], sy:+t[10] });
}

const M = await createRenderFrame({ locateFile: x => x });
const ramPtr = M._malloc(ram.length), outPtr = M._malloc(256*1024);

const d2 = (a,b) => Math.max(Math.abs(a.x-b.sx), Math.abs(a.y-b.sy));

// Align rf->asm when |rf| == |asm|+1: drop the one rf part that minimizes total residual.
function alignDrop1(rf, asm){
  let best=null, bestScore=Infinity;
  for (let drop=0; drop<rf.length; drop++){
    const cand = rf.filter((_,i)=>i!==drop);
    let s=0; for(let k=0;k<asm.length;k++) s+=d2(cand[k],asm[k]);
    if (s<bestScore){ bestScore=s; best={cand,drop}; }
  }
  return best;
}

let framesChecked=0, nfTotal=0, nfClean=0, nfCountMismatch=0;
const swaps=[];      // confirmed 2-part transpositions
const residual=[];   // node-frames with equal counts, >TOL parts, NOT explained by a swap
const shiftHist={};  // which vframe shift (-2..+2) best explains each divergent node-frame
for (let fi=0; fi<nF; fi++) {
  const fr = frames[fi];
  if (!asmByVf.get(fr.vf)) continue;
  let o = fr.dof; for (const r of dR) { ram.set(buf.subarray(o, o+r.len), r.addr&0xFFFFFF); o+=r.len; }
  M.HEAPU8.set(ram, ramPtr);
  M._render_frame_ta(ramPtr, outPtr, 256*1024);
  const quads = M._render_frame_quad_count();
  const ta = M.HEAPU8.subarray(outPtr, outPtr + quads*96);
  const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
  const objN = M._render_frame_obj_count();
  let qi = 0;
  for (let b=0; b<objN; b++) {
    const node = ((M._render_frame_obj_node(b) >>> 0) & 0x0FFFFFFF).toString(16).padStart(8,'0');
    const nt = M._render_frame_obj_ntiles(b);
    // ASMTRACE screen anchor: Y = quad max-y (C vertex) always (bottom-up carve);
    // X = LEFT edge (A) or RIGHT edge (B) depending on the node's facing — choose
    // per node-frame (facing is per-node constant), verified in _tx_dump: Bx-sx==0
    // exactly on the right-anchored family, Ax-sx==0 on the other.
    let rfA = [], rfB = [];
    for (let k=0; k<nt; k++){ const off=(qi+k)*96;
      const ax=tdv.getFloat32(off+36,true), bx=tdv.getFloat32(off+48,true), cy=tdv.getFloat32(off+64,true);
      rfA.push({ x:ax, y:cy, qi:qi+k }); rfB.push({ x:bx, y:cy, qi:qi+k }); }
    qi += nt;
    // pick vframe shift (-2..+2) AND x-corner whose asm list best matches this node
    let a=null, rf=null, bestShift=0, bestScore=Infinity;
    for (const sh of [0,-1,1,-2,2]){
      const am = asmByVf.get(fr.vf+sh); if(!am) continue;
      const cand = am.get(node); if(!cand) continue;
      for (const rf0 of [rfA, rfB]){
        let r2 = rf0;
        if (r2.length === cand.length+1){ const al=alignDrop1(r2,cand); r2=al.cand; }
        else if (r2.length !== cand.length) continue;
        let s=0; for(let k=0;k<cand.length;k++) s+=d2(r2[k],cand[k]);
        s/=cand.length;
        if (s<bestScore-1e-9){ bestScore=s; a=cand; rf=r2; bestShift=sh; }
      }
    }
    nfTotal++;
    if (!a){ nfCountMismatch++;
      // UNDER/OVER-EMISSION report (the "missing tile block" class): no shift produced
      // a count match — record rf vs asm tile counts at shift 0 for this node.
      const am0=asmByVf.get(fr.vf); const a0=am0&&am0.get(node);
      (globalThis._cmList=globalThis._cmList||[]).push({vf:fr.vf,node,rf:rf0.length,asm:a0?a0.length:0});
      continue; }
    if (bestScore>TOL) shiftHist[bestShift]=(shiftHist[bestShift]||0); // divergent even at best shift
    shiftHist[bestShift]=(shiftHist[bestShift]||0)+1;
    // index-paired residuals
    const off = rf.map((r,k)=>d2(r,a[k]));
    const bad = off.map((d,k)=>d>TOL?k:-1).filter(k=>k>=0);
    if (!bad.length){ nfClean++; continue; }
    // try to explain ALL bad parts by disjoint 2-swaps
    const explained = new Set(); const pairs=[];
    for (let ii=0; ii<bad.length; ii++){
      const i = bad[ii]; if (explained.has(i)) continue;
      for (let jj=ii+1; jj<bad.length; jj++){
        const j = bad[jj]; if (explained.has(j)) continue;
        if (d2(rf[i],a[j])<=TOL && d2(rf[j],a[i])<=TOL){
          pairs.push([i,j]); explained.add(i); explained.add(j); break;
        }
      }
    }
    if (pairs.length && explained.size===bad.length){
      for (const [i,j] of pairs)
        swaps.push({ vf:fr.vf, node, i, j, selI:a[i].sel, selJ:a[j].sel,
          rfI:[rf[i].x,rf[i].y].map(v=>v.toFixed(1)), rfJ:[rf[j].x,rf[j].y].map(v=>v.toFixed(1)) });
    } else {
      residual.push({ vf:fr.vf, node, nBad:bad.length, n:rf.length,
        maxOff:Math.max(...off).toFixed(1), sels:bad.slice(0,6).map(k=>a[k].sel) });
    }
  }
  framesChecked++;
}

console.log(`frames checked: ${framesChecked} (vf ${vfMin}..${vfMax}); node-frames: ${nfTotal}, clean: ${nfClean}, count-mismatch(skipped): ${nfCountMismatch}`);
if (globalThis._cmList && globalThis._cmList.length){
  console.log(`\nCOUNT-MISMATCH node-frames (under/over-emission — the missing-tile class):`);
  for (const c of globalThis._cmList.slice(0,30)) console.log(`  vf=${c.vf} node=${c.node} rf=${c.rf} asm=${c.asm} delta=${c.rf-c.asm}`);
  const byNode={}; for(const c of globalThis._cmList) byNode[c.node]=(byNode[c.node]||0)+1;
  console.log(`  by node: ${JSON.stringify(byNode)}`);
}
console.log(`best-shift histogram (node-frames): ${JSON.stringify(shiftHist)}`);
console.log(`\nCONFIRMED 2-PART TRANSPOSITIONS: ${swaps.length}`);
for (const s of swaps.slice(0,40))
  console.log(`  vf=${s.vf} node=${s.node} parts ${s.i}<->${s.j}  sels ${s.selI}<->${s.selJ}  rf@(${s.rfI})/(${s.rfJ})`);
// which sel pairs recur
const bySel = {};
for (const s of swaps){ const k = `${Math.min(s.selI,s.selJ)}<->${Math.max(s.selI,s.selJ)}`; bySel[k]=(bySel[k]||0)+1; }
console.log(`\nswap count by sel pair:`);
for (const [k,c] of Object.entries(bySel).sort((a,b)=>b[1]-a[1]).slice(0,15)) console.log(`  ${k}: ${c}`);
console.log(`\nUNEXPLAINED divergent node-frames (equal counts, not a clean swap): ${residual.length}`);
for (const r of residual.slice(0,20))
  console.log(`  vf=${r.vf} node=${r.node} bad=${r.nBad}/${r.n} maxOff=${r.maxOff} sels=[${r.sels}]`);
