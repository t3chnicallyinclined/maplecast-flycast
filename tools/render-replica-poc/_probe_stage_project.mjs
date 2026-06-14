// _probe_stage_project.mjs — project STG0B geometry through the LIVE camera matrices
// (read from _satlive.mcrr) using the SAME math as stage-client.mjs _projectEngine, and
// report how many vertices land on-screen (0..640, 0..480). This tells us whether the
// stage is geometrically visible or projects off-screen / behind the camera.
import { readFileSync } from 'node:fs';

// ---- read live matrices from the mcrr (reuse the parse) ----
const path = process.argv[2] || '_satlive.mcrr';
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
u32(); const nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr: addr >>> 0, len, tag }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
p += vramBytes + pvrBytes;
const staticData = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const frameStart = p;
const G = a => (a >>> 0) & 0xFFFFFF;
const baseRam = new Uint8Array(16 * 1024 * 1024);
staticRegs.forEach((r, i) => { if (r.tag === 'ram16') baseRam.set(staticData[i], 0); else baseRam.set(staticData[i], G(r.addr)); });
p = frameStart; const frames = [];
for (let f = 0; f < nFrames; f++) {
  if (u32() !== 0x784D5246) throw new Error(`frame ${f}: bad FRMx`);
  const vframe = u32(); const taSize = u32();
  const dynOff = p; for (const r of dynamicRegs) p += r.len;
  const nGfx = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0;
  if (nGfx <= 64) { p += 4; for (let g = 0; g < nGfx && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } }
  p += taSize;
  frames.push({ vframe, dynOff });
}
const fr = frames[Math.floor(frames.length * 0.6)];
const ram = baseRam.slice(); { let o = fr.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }
const rd = new DataView(ram.buffer, ram.byteOffset, ram.byteLength);
function mat16(addr){ const m=new Float32Array(16); for(let i=0;i<16;i++) m[i]=rd.getFloat32(G(addr+i*4),true); return m; }
const M1 = mat16(0x8C2D6B18), M2 = mat16(0x8C2D6AD8);
console.log('stage_id =', ram[G(0x8C289638)]);

// ---- XMTRX = M1·M2 (col-major matmul, stage-client _matmulColMaj) ----
function matmulColMaj(X, N){ const out=new Float32Array(16);
  for(let c=0;c<4;c++) for(let i=0;i<4;i++)
    out[c*4+i] = X[i]*N[c*4+0]+X[i+4]*N[c*4+1]+X[i+8]*N[c*4+2]+X[i+12]*N[c*4+3];
  return out; }
const X = matmulColMaj(M1, M2);
console.log('XMTRX:', [...X].map(v=>v.toFixed(3)).join(' '));

function projE(x,y,z){
  const fx = X[0]*x + X[4]*y + X[8]*z  + X[12];
  const fy = X[1]*x + X[5]*y + X[9]*z  + X[13];
  const fw = X[3]*x + X[7]*y + X[11]*z + X[15];
  const inv = 1.0/(fw||1e-6);
  return [fx*inv, fy*inv, inv];
}

// ---- load STG0B geometry ----
import { readFileSync as rfs } from 'node:fs';
const d = JSON.parse(rfs('../../web/test-atlas/stages/STG0B.json', 'utf8'));
let total=0, onscreen=0, behind=0;
let minSx=1e9,maxSx=-1e9,minSy=1e9,maxSy=-1e9, minD=1e9, maxD=-1e9;
const sample = [];
for (const m of d.meshes) for (const tr of m.tris) for (const v of tr) {
  const [sx,sy,inv] = projE(v.pos[0],v.pos[1],v.pos[2]);
  total++;
  if (inv <= 0) behind++;
  if (sx>=0&&sx<=640&&sy>=0&&sy<=480) onscreen++;
  if (sx<minSx)minSx=sx; if(sx>maxSx)maxSx=sx; if(sy<minSy)minSy=sy; if(sy>maxSy)maxSy=sy;
  if (inv<minD)minD=inv; if(inv>maxD)maxD=inv;
  if (sample.length<6) sample.push([v.pos.map(n=>n.toFixed(0)).join(','), sx.toFixed(1), sy.toFixed(1), inv.toExponential(2)]);
}
console.log(`verts=${total} onscreen=${onscreen} (${(100*onscreen/total).toFixed(1)}%) behind(inv<=0)=${behind}`);
console.log(`screen X range [${minSx.toFixed(1)}, ${maxSx.toFixed(1)}]  Y range [${minSy.toFixed(1)}, ${maxSy.toFixed(1)}]`);
console.log(`depth(1/w) range [${minD.toExponential(3)}, ${maxD.toExponential(3)}]`);
console.log('samples [world | sx | sy | depth]:');
for (const s of sample) console.log('  ', s.join('  |  '));
