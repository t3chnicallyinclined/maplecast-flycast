// _sprite_order_verdict.mjs — parse a saved hudq_tail.bin, isolate ParaType-5 (Sprite)
// quads, and for each run the PLANE TEST to identify P (the plane-interpolated vertex
// flycast's CaclulateSpritePlane computes) vs the 3 explicit corners A,B,C, then verify
// the capture's array order is the raw flycast expansion order [P, C, A, B].
//
// flycast AppendSpriteVertexA/B lays verts as cv=[P, C, A, B] where:
//   cv[2]=A=(x0,y0)/uv(u0,v0)   cv[3]=B=(x1,y1)/uv(u1,v1)   cv[1]=C=(x2,y2)/uv(u2,v2)
//   cv[0]=P=(x3,y3) with uv PLANE-INTERPOLATED: P.u=A_u+k1*(B_u-A_u)+k2*(C_u-A_u)
// where (k1,k2) solve P_xy = A + k1*(B-A) + k2*(C-A).
// So: of the 4 captured verts, the one whose UV is the PLANE INTERPOLATION of the other
// three's UV at its own XY position is P. In raw [P,C,A,B] order that is index 0.
//
//   node _sprite_order_verdict.mjs <outdir>
import { readFileSync } from 'node:fs';

const dir = process.argv[2] || '_hud_cap_fresh2';
const buf = readFileSync(`${dir}/hudq_tail.bin`);
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0;
const HUDQ_MAGIC = 0x48554451;
if (dv.getUint32(p, true) !== HUDQ_MAGIC) { console.error('bad magic'); process.exit(1); }
p += 4;
const n = dv.getUint32(p, true); p += 4;
const quads = [];
for (let i = 0; i < n; i++) {
  if (p + 96 > buf.length) break;
  const x = [0,1,2,3].map(k => dv.getFloat32(p + k*4, true));
  const y = [0,1,2,3].map(k => dv.getFloat32(p + 16 + k*4, true));
  const u = [0,1,2,3].map(k => dv.getFloat32(p + 32 + k*4, true));
  const v = [0,1,2,3].map(k => dv.getFloat32(p + 48 + k*4, true));
  const col = [0,1,2,3].map(k => dv.getUint32(p + 64 + k*4, true) >>> 0);
  const pcw = dv.getUint32(p+80,true)>>>0, isp=dv.getUint32(p+84,true)>>>0,
        tsp=dv.getUint32(p+88,true)>>>0, tcw=dv.getUint32(p+92,true)>>>0;
  quads.push({ i, x, y, u, v, col, pcw, isp, tsp, tcw });
  p += 96;
}

// Solve P_xy = A + k1*(B-A) + k2*(C-A); return predicted UV at P from A,B,C UVs.
function planePredictUV(P, A, B, C) {
  const abx = B.x - A.x, aby = B.y - A.y;
  const acx = C.x - A.x, acy = C.y - A.y;
  const det = abx * acy - aby * acx;
  if (Math.abs(det) < 1e-6) return null; // degenerate
  const px = P.x - A.x, py = P.y - A.y;
  const k1 = (px * acy - py * acx) / det;
  const k2 = (abx * py - aby * px) / det;
  return {
    u: A.u + k1 * (B.u - A.u) + k2 * (C.u - A.u),
    v: A.v + k1 * (B.v - A.v) + k2 * (C.v - A.v),
    k1, k2,
  };
}

// For each candidate "which index is P", test: does the plane through the OTHER 3
// predict this vert's captured UV? The true P has the SMALLEST residual.
function findP(q) {
  const verts = [0,1,2,3].map(k => ({ x:q.x[k], y:q.y[k], u:q.u[k], v:q.v[k] }));
  let best = { idx:-1, err:Infinity, k1:0, k2:0 };
  for (let pi = 0; pi < 4; pi++) {
    const others = [0,1,2,3].filter(k => k !== pi);
    // try all 3 assignments of which 'other' is A (the plane is symmetric in B,C but
    // A is the origin; UV interpolation is affine so any A works if it's truly planar).
    const [a,b,c] = others;
    const pred = planePredictUV(verts[pi], verts[a], verts[b], verts[c]);
    if (!pred) continue;
    const err = Math.hypot(pred.u - verts[pi].u, pred.v - verts[pi].v);
    if (err < best.err) best = { idx:pi, err, k1:pred.k1, k2:pred.k2 };
  }
  return best;
}

// A valid (non-bowtie) convex quad under a given index order [i0,i1,i2,i3] drawn as a
// strip (tris (0,1,2)+(1,2,3) for pvr2 _buildIndexBuffer => (v0,v1,v2)+(v1,v0,v2)... )
// — we test the simpler geometric question: are the 4 corners a simple (non-self-
// intersecting) quad when connected in array order 0-1-3-2 (perimeter) vs do they
// bowtie? We test signed-area sign consistency of the strip's two triangles.
function triArea(a, b, c) { return 0.5 * ((b.x-a.x)*(c.y-a.y) - (c.x-a.x)*(b.y-a.y)); }
function stripValid(q) {
  // pvr2 _buildIndexBuffer for a 4-vert strip [0,1,2,3]: i=0 (even) -> (0,1,2);
  // i=1 (odd, v0=1,v1=2,v2=3) -> (v1,v0,v2) = (2,1,3). So the two triangles are
  // (0,1,2) and (2,1,3), sharing the diagonal v1-v2. They must have the SAME
  // orientation for a non-degenerate convex quad; opposite => bowtie/garble.
  const V = [0,1,2,3].map(k=>({x:q.x[k],y:q.y[k]}));
  const t0 = triArea(V[0],V[1],V[2]);
  const t1 = triArea(V[2],V[1],V[3]);
  const a0 = Math.abs(t0), a1 = Math.abs(t1);
  const sameSign = (t0 >= 0) === (t1 >= 0);
  return { valid: sameSign && a0 > 0.5 && a1 > 0.5, t0, t1, sameSign, a0, a1 };
}

const sprites = quads.filter(q => ((q.pcw >>> 29) & 7) === 5);
console.log(`File: ${dir}/hudq_tail.bin  total quads=${quads.length}  ParaType-5 sprites=${sprites.length}\n`);

let cleanStrip = 0, bowtie = 0, pIsZero = 0, pNotZero = 0, planar = 0;
for (const q of sprites) {
  const P = findP(q);
  const strip = stripValid(q);
  const isPlanar = P.err < 0.05; // UV residual in [0,1] uv units
  if (isPlanar) planar++;
  if (P.idx === 0) pIsZero++; else if (P.idx >= 0) pNotZero++;
  if (strip.valid) cleanStrip++; else bowtie++;
  console.log(
    `[${String(q.i).padStart(2)}] tcw=${q.tcw.toString(16).padStart(8,'0')} ` +
    `x=[${q.x.map(v=>v.toFixed(0)).join(',')}] y=[${q.y.map(v=>v.toFixed(0)).join(',')}] ` +
    `u=[${q.u.map(v=>v.toFixed(3)).join(',')}] v=[${q.v.map(v=>v.toFixed(3)).join(',')}] ` +
    `| P=idx${P.idx}(err=${P.err.toExponential(1)}${isPlanar?' PLANAR':' NONPLANAR'}) ` +
    `| strip=${strip.valid?'VALID':'BOWTIE'}(t0=${strip.t0.toFixed(1)},t1=${strip.t1.toFixed(1)})`
  );
}
console.log(`\n=== VERDICT (${sprites.length} ParaType-5 sprites) ===`);
console.log(`  P identified at index 0 (raw [P,C,A,B] order): ${pIsZero}/${sprites.length}`);
console.log(`  P at a non-zero index (permuted):              ${pNotZero}/${sprites.length}`);
console.log(`  planar (true sprite, P reconstructible):       ${planar}/${sprites.length}`);
console.log(`  identity-strip VALID (non-bowtie):             ${cleanStrip}/${sprites.length}`);
console.log(`  identity-strip BOWTIE (garbles):               ${bowtie}/${sprites.length}`);
