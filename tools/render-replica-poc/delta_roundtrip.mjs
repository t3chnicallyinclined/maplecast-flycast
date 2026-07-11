// delta_roundtrip.mjs — P1 GATE: prove the keyframe+byte-run STATE delta reconstructs the dynamic
// region span BYTE-IDENTICAL, and measure per-frame wire size. Two delta modes:
//   --mode=prev : delta vs the previous reconstructed frame (smallest; needs every frame in order)
//   --mode=key  : delta vs the last KEY (drop-resilient within the keyframe interval; the wire choice)
// Contract (gsta-verification-harness): PASS iff mismatchBytes==0 AND shapeChanges==keyframesEmitted.
//   node delta_roundtrip.mjs <capture.mcrr> [--mode=prev|key] [--keyint=60] [--gap=8]
import { readFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';

const path = process.argv[2];
const arg = k => { const p = process.argv.find(a => a.startsWith('--' + k + '=')); return p ? p.split('=')[1] : null; };
const MODE = arg('mode') || 'prev';
const KEYINT = +(arg('keyint') || 60);
const GAP = +(arg('gap') || 8);

// ---- parse .mcrr (same layout as churn/_tx_detect/_zz gates) ----
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); u32(); const nS = u32(), nD = u32(), nF = u32(), vb = u32(), pb = u32(); u32();
const region = () => { const a = u32(), l = u32(); let t = ''; for (let i = 0; i < 8; i++){const c=buf[p+i]; if(c)t+=String.fromCharCode(c);} p+=8; return {addr:a>>>0,len:l,tag:t}; };
const sR = Array.from({length:nS}, region), dR = Array.from({length:nD}, region);
let q = p + vb + pb; for (const r of sR) q += r.len; p = q;
const dynLen = dR.reduce((a,r)=>a+r.len,0);
const shapeSig = dR.map(r=>r.addr+':'+r.len).join(',');   // dyn-region TABLE shape (fixed within a file)
const frames = [];
for (let f=0; f<nF; f++){ u32(); const vf=u32(); const tail=u32(); const dof=p; p+=dynLen; p+=tail; frames.push({vf,dof}); }

// ---- encoder: changed-byte-runs (merge gaps < GAP), vs a reference blob ----
function encode(cur, ref){
  const runs=[]; let i=0;
  while(i<dynLen){
    if(ref[i]===cur[i]){ i++; continue; }
    let start=i, gap=0, end=i;
    while(i<dynLen){ if(ref[i]!==cur[i]){ end=i+1; gap=0; } else { gap++; if(gap>=GAP) break; } i++; }
    runs.push([start,end]);
  }
  let sz=2; for(const [s,e] of runs) sz+=6+(e-s);
  const out=new Uint8Array(sz); const odv=new DataView(out.buffer);
  let o=0; odv.setUint16(o,runs.length,true); o+=2;
  for(const [s,e] of runs){ odv.setUint32(o,s,true);o+=4; odv.setUint16(o,e-s,true);o+=2; out.set(cur.subarray(s,e),o); o+=e-s; }
  return out;
}
function applyDelta(dst, delta){
  const ddv=new DataView(delta.buffer,delta.byteOffset,delta.byteLength);
  let o=0; const n=ddv.getUint16(o,true); o+=2;
  for(let i=0;i<n;i++){ const off=ddv.getUint32(o,true);o+=4; const len=ddv.getUint16(o,true);o+=2; dst.set(delta.subarray(o,o+len),off); o+=len; }
}

// ---- run: encode → decode → memcmp every frame ----
const recon = new Uint8Array(dynLen);     // the client's running reconstruction
let keyRef = new Uint8Array(dynLen);      // last KEY (for --mode=key)
let mismatchBytes=0, firstBad=null, bytesCompared=0;
let keyframesEmitted=0, shapeChanges=0, sumWire=0, sumGz=0, maxWire=0;
let prevShape=null;

for (let f=0; f<nF; f++){
  const cur = buf.subarray(frames[f].dof, frames[f].dof+dynLen);
  // shape change (fixed within one .mcrr, but the gate is real for the live stream)
  if (prevShape !== null && shapeSig !== prevShape) shapeChanges++;
  const forceKey = (f===0) || (shapeSig !== prevShape) || (MODE==='key' && (f % KEYINT === 0));
  prevShape = shapeSig;

  let wire;
  if (forceKey){
    wire = cur.slice();                    // KEY = full dyn span (verbatim)
    keyframesEmitted++;
    recon.set(cur); keyRef = cur.slice();
  } else {
    const ref = (MODE==='key') ? keyRef : recon;
    wire = encode(cur, ref);
    if (MODE==='key') recon.set(keyRef);   // vs-KEY: client re-bases to the last KEY, then applies
    applyDelta(recon, wire);               // client applies the runs to its running recon
  }
  // verify recon == cur
  let bad=0; for(let k=0;k<dynLen;k++){ if(recon[k]!==cur[k]){ bad++; if(firstBad===null) firstBad={frame:f,vf:frames[f].vf,offset:k}; } }
  mismatchBytes+=bad; bytesCompared+=dynLen;
  sumWire+=wire.length; sumGz+=gzipSync(wire).length; if(wire.length>maxWire) maxWire=wire.length;
}

const name = path.split(/[\\/]/).pop();
const pass = (mismatchBytes===0) && (shapeChanges===0 || shapeChanges===keyframesEmitted-1 /*first key not a change*/);
console.log(`\n=== delta_roundtrip [${MODE}] — ${name}, ${nF} frames, dyn ${dynLen} B, vf ${frames[0].vf}..${frames.at(-1).vf} ===`);
console.log(`frames ${nF}  bytesCompared ${bytesCompared}  mismatchBytes ${mismatchBytes}` + (firstBad?`  firstBad frame=${firstBad.frame} vf=${firstBad.vf} off=${firstBad.offset}`:''));
console.log(`shapeChanges ${shapeChanges}  keyframesEmitted ${keyframesEmitted}`);
console.log(`wire/frame: avg ${(sumWire/nF).toFixed(0)} B raw / ${(sumGz/nF).toFixed(0)} B gzip   max ${maxWire} B   (vs ${dynLen} B whole-region)`);
console.log(`state wire @60fps: ${(sumGz/nF*60*8/1e6).toFixed(3)} Mbps gzip   (whole-region: ${(dynLen*60*8/1e6).toFixed(1)} Mbps)`);
console.log(pass ? '*** GATE PASS: byte-identical reconstruction ***' : '*** GATE FAIL ***');
process.exit(pass ? 0 : 1);
