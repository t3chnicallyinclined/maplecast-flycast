// sprite_bridge_smoke.mjs — headless gate for the ?bodysrc=sprite poly-entry
// bridge (webgpu-test.html window._spriteBridge): drives the REAL sprite machine
// (sprite-client.mjs buildAssemblyDrawList -> buildEmitterDrawList, local
// PLxx_parts/_asm atlases) from the captured GSTA/OBJS state in
// _cap_userplay/cap.bin, converts the draw list into pvr2 poly entries + 28B
// verts exactly like the page bridge, and gates:
//   1. quad count > 0 on every fight frame that has stripped char content
//   2. all verts finite, all UVs in [0,1]
//   3. the _bodyMerge descriptor splice consumes the bridge polys without
//      skewing the kept wire runs (kSum==kParams, kept order preserved,
//      every bridge poly lands in the merged list)
//   4. POSITION gate: bridge quad clusters vs the engine's stripped char-quad
//      clusters (TR para5, TCW block in {82,83,88,89}) — center-match rate.
// Modeled on _bwlab/body_feed_smoke.mjs + _bwlab/order_gate.mjs.
// Usage: node sprite_bridge_smoke.mjs [capdir]   (run from _bwlab or anywhere)
import { readFileSync } from 'fs';
import { FrameDecoder } from '../web/webgpu/frame-decoder.mjs';
import { TAParser } from '../web/webgpu/ta-parser.mjs';
import { SpriteClient } from '../web/webgpu/sprite-client.mjs';

const dir = process.argv[2] || new URL('./_cap_userplay', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1');
const atlasDir = new URL('../web/test-atlas/chars', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1');
const data = readFileSync(dir + '/cap.bin');
const recs = [];
{ let p = 0;
  while (p + 13 <= data.length) {
    const len = data.readUInt32LE(p), sock = data[p + 4];
    if (p + 13 + len > data.length) break;
    recs.push({ sock, b: data.subarray(p + 13, p + 13 + len) });
    p += 13 + len;
  } }
const isM = (b, m) => b.length >= 4 && b[0] === m.charCodeAt(0) && b[1] === m.charCodeAt(1) && b[2] === m.charCodeAt(2) && b[3] === m.charCodeAt(3);
const SB = new Set([0x82, 0x83, 0x88, 0x89]);

// ---- sprite machine, fed exactly like the page (GSTA/OBJS on the main socket)
const SC = new SpriteClient();
SC.predict = false;          // capture replay: no wall-clock extrapolation
SC.assemblyMode = true;      // page default (emitter path; parts atlases are local)

// preload asm atlases for every cid seen in the capture (page: loadAsmChar; here: disk)
function pngSize(buf) { return { width: buf.readUInt32BE(16), height: buf.readUInt32BE(20) }; }
{
  const cids = new Set();
  for (const r of recs) {
    if (r.sock !== 0) continue;
    if (isM(r.b, 'GSTA')) { for (let s = 0; s < 6; s++) cids.add(r.b[4 + 25 + s * 57 + 1]); }
    else if (isM(r.b, 'OBJS')) { const n = r.b[4], stride = (r.b.length - 5) / Math.max(1, n);
      if (Number.isInteger(stride)) for (let i = 0; i < n; i++) cids.add(r.b[5 + i * stride]); }
  }
  let loaded = 0, missing = [];
  for (const cid of cids) {
    if (cid == null || cid > 0x3A) continue;
    const hex = (cid & 0xff).toString(16).padStart(2, '0').toUpperCase();
    try {
      const partsJson = JSON.parse(readFileSync(`${atlasDir}/PL${hex}_parts.json`, 'utf8'));
      const asmRaw = JSON.parse(readFileSync(`${atlasDir}/PL${hex}_asm.json`, 'utf8'));
      const png = readFileSync(`${atlasDir}/PL${hex}_parts.png`);
      const { width, height } = pngSize(png);
      const asm = asmRaw.assemblies || asmRaw.asm || asmRaw;
      const parts = asmRaw.parts || partsJson.parts || partsJson;
      if (asmRaw.screenW) SC.screenW = asmRaw.screenW;
      if (asmRaw.screenH) SC.screenH = asmRaw.screenH;
      SC.asmChars[cid] = { img: { width, height }, parts, asm,
        palette: asmRaw.palette || partsJson.palette || null,
        pal128: asmRaw.pal128 || partsJson.pal128 || null,
        screenSpace: !!asmRaw.screenSpace, selKeyed: !!asmRaw.selKeyed, name: asmRaw.name || ('char' + cid) };
      loaded++;
    } catch (e) { missing.push(hex); }
  }
  console.log(`atlases: ${loaded} loaded${missing.length ? ', missing PL' + missing.join('/PL') : ''}`);
}

// ---- the page bridge conversion (geometry-identical twin of window._spriteBridge.build;
// no GPU: texObj omitted). Constants = the measured engine char-quad words.
const SPR_Z = 0.00923;
function bridgeBuild() {
  const items = SC.buildAssemblyDrawList(640, 480);
  if (!items || !items.length) return null;
  const vd = new Uint8Array(items.length * 4 * 28);
  const f32 = new Float32Array(vd.buffer);
  const polys = []; let vi = 0, param = 0, skipFx = 0, texMiss = 0;
  for (const it of items) {
    if (it.charId === 0xFE0 || it.charId === -1) { skipFx++; continue; }
    const src = SC.asmChars[it.charId];
    const img = src && src.img; if (!img) { texMiss++; continue; }
    const W = img.width, H = img.height;
    let u0 = it.sx / W, v0 = it.sy / H, u1 = (it.sx + it.sw) / W, v1 = (it.sy + it.sh) / H;
    if (it.flip) { const t = u0; u0 = u1; u1 = t; }
    if (it.flipY) { const t = v0; v0 = v1; v1 = t; }
    const x0 = it.dx, y0 = it.dy, x1 = it.dx + it.dw, y1 = it.dy + it.dh;
    if (!(isFinite(x0) && isFinite(y0) && isFinite(x1) && isFinite(y1))) continue;
    const put = (x, y, u, v) => { const fi = vi * 7, bi = vi * 28;
      f32[fi] = x; f32[fi + 1] = y; f32[fi + 2] = SPR_Z;
      vd[bi + 12] = 255; vd[bi + 13] = 255; vd[bi + 14] = 255; vd[bi + 15] = 255;
      f32[fi + 5] = u; f32[fi + 6] = v; vi++; };
    const first = vi;
    put(x0, y0, u0, v0); put(x1, y0, u1, v0); put(x0, y1, u0, v1); put(x1, y1, u1, v1);
    const add = it.blend != null && (it.blend & 0xf) === 1;
    polys.push({ first, count: 4, isp: (6 << 29) >>> 0, tsp: add ? 0x849004D2 : 0x949004D2,
      tcw: 0, pcw: 0xA2000009, tileclip: 0, param: --param });
  }
  if (!vi) return null;
  return { vertexData: vd.subarray(0, vi * 28), vertexCount: vi, translucent: polys, skipFx, texMiss };
}

// ---- server strip + descriptor, JS twin (same as order_gate.mjs)
const STAGE_ALLOW = new Set([0x9fc00, 0xa0000]);
function stripAndDescribe(ta, taSize) {
  const out = new Uint8Array(taSize); let w = 0, off = 0;
  let curList = -1, isSpr = false, haveParam = false, dropping = false, cObj = 0;
  let pendCls = -1, pendPushed = true;
  const runs = [];
  const push = (cls) => { if (runs.length && runs[runs.length - 1][0] === cls) runs[runs.length - 1][1]++; else runs.push([cls, 1]); };
  const dv = new DataView(ta.buffer, ta.byteOffset, taSize);
  const emit = n => { out.set(ta.subarray(off, off + n), w); w += n; };
  while (off + 32 <= taSize) {
    const pcw = dv.getUint32(off, true), pt = (pcw >>> 29) & 7;
    if (pt === 0 || pt === 1 || pt === 2 || pt === 3 || pt === 6) { haveParam = false; dropping = false; if (pt === 0) curList = -1; emit(32); off += 32; continue; }
    if (pt === 4) {
      const lt = (pcw >>> 24) & 7; if (curList === -1) curList = lt;
      if (curList === 1 || curList === 3) { haveParam = false; dropping = false; emit(32); off += 32; continue; }
      cObj = pcw & 0xFF; isSpr = false; haveParam = true;
      const colType = (cObj >> 4) & 3, vol = (cObj >> 6) & 1;
      let sz; if (colType === 2 && !vol && ((cObj >> 2) & 1)) sz = (off + 64 <= taSize) ? 64 : 32;
      else if (colType >= 1 && vol) sz = (off + 64 <= taSize) ? 64 : 32; else sz = 32;
      const tcw = dv.getUint32(off + 12, true);
      dropping = (curList === 0) && STAGE_ALLOW.has(tcw & 0x1FFFFF);
      pendCls = (curList === 2 && !dropping) ? 2 : -1; pendPushed = false;
      if (!dropping) emit(sz);
      off += sz; continue;
    }
    if (pt === 5) {
      const lt = (pcw >>> 24) & 7; if (curList === -1) curList = lt;
      cObj = pcw & 0xFF; isSpr = true; haveParam = true;
      const tcw = dv.getUint32(off + 12, true), addr = tcw & 0x1FFFFF;
      dropping = ((curList === 0) && STAGE_ALLOW.has(addr)) || (curList === 2 && SB.has(addr >>> 12));
      pendCls = (curList === 2) ? (dropping ? 0 : 1) : -1; pendPushed = false;
      if (!dropping) emit(32);
      off += 32; continue;
    }
    if (pt === 7) {
      let sz; if (!haveParam) sz = 32; else if (isSpr && off + 64 <= taSize) sz = 64;
      else { const tex = (cObj >> 3) & 1, colType = (cObj >> 4) & 3, vol = (cObj >> 6) & 1;
        if (!tex) sz = 32; else if (!vol) sz = (colType === 1 && off + 64 <= taSize) ? 64 : 32; else sz = 32; }
      if (haveParam && !pendPushed) { if (pendCls >= 0) push(pendCls); pendPushed = true; }
      if (!dropping) emit(sz);
      off += sz; continue;
    }
    emit(32); off += 32;
  }
  return { stripped: out.subarray(0, w), runs };
}

// ---- rect clustering (union-find on 6px-expanded overlap) + center matching
function clusters(rects) {
  const n = rects.length, parent = Array.from({ length: n }, (_, i) => i);
  const find = i => { while (parent[i] !== i) { parent[i] = parent[parent[i]]; i = parent[i]; } return i; };
  const E = 16;   // merge limbs/tiles of one body (engine tiles are 32px; parts can gap)
  for (let i = 0; i < n; i++) for (let j = i + 1; j < n; j++) {
    const a = rects[i], b = rects[j];
    if (a.x0 - E < b.x1 && b.x0 - E < a.x1 && a.y0 - E < b.y1 && b.y0 - E < a.y1) {
      const ri = find(i), rj = find(j); if (ri !== rj) parent[ri] = rj;
    }
  }
  const map = new Map();
  for (let i = 0; i < n; i++) {
    const r = find(i);
    let c = map.get(r); if (!c) { c = { x0: 1e9, y0: 1e9, x1: -1e9, y1: -1e9, n: 0 }; map.set(r, c); }
    const q = rects[i];
    c.x0 = Math.min(c.x0, q.x0); c.y0 = Math.min(c.y0, q.y0);
    c.x1 = Math.max(c.x1, q.x1); c.y1 = Math.max(c.y1, q.y1); c.n++;
  }
  return [...map.values()].map(c => ({ ...c, cx: (c.x0 + c.x1) / 2, cy: (c.y0 + c.y1) / 2 }));
}
function polyRects(g, list, filter) {
  const vf = new Float32Array(g.vertexData.buffer, g.vertexData.byteOffset, g.vertexCount * 7);
  const out = [];
  for (const pp of list || []) {
    if (pp.count < 3) continue;
    if (filter && !filter(pp)) continue;
    let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (let v = pp.first; v < pp.first + pp.count; v++) {
      const x = vf[v * 7], y = vf[v * 7 + 1];
      if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    out.push({ x0, y0, x1, y1 });
  }
  return out;
}

// ---- main walk: feed state in arrival order; gate on each decoded wire frame.
// WIRE ORDER (measured on this capture): per publish the server emits
// ZCST -> GSTA -> OBJS. So at TA-decode time the sprite state is ONE FRAME
// STALE. We gate BOTH pairings: LAG (state as-of decode, what a naive page
// sees) and EXACT (flush the pending TA frame after its own GSTA/OBJS arrive —
// what the page's velocity extrapolation approximates live).
const D = new FrameDecoder(), P1 = new TAParser(), P2 = new TAParser();
let fightFrames = 0, zeroQuadFrames = 0, quadsTot = 0;
let badVert = 0, badUV = 0;
let spliceOk = 0, spliceSkew = 0, spliceLost = 0, keptReorder = 0;
let engClustTot = 0, brClustTot = 0, extraBridge = 0;
let satQuadsTot = 0, missPoseTot = 0, assistDraw = 0, engWinMiss = 0, skipSelTot = 0;
const mk = () => ({ n: 0, m8: 0, m16: 0, sumDx: 0, sumDy: 0, sumAbs: 0, h: [0, 0, 0, 0, 0, 0] });
const LAG = mk(), EXACT = mk(), EXACT_CLEAN = mk(), WIN = mk();
const WINR = { w: 0, h: 0, n: 0 };
const measureSlots = (acc, ec, cleanAcc, slotAnchors) => {
  // per active body slot: bridge body bbox vs the anchor-WINDOWED engine bbox
  const items = SC.buildAssemblyDrawList(640, 480);
  const bySlot = new Map();
  const allRects = [];   // ALL bridge quads (bodies + sats) for the symmetric window metric
  let satQ = 0;
  for (const it of items) {
    if (it.charId === 0xFE0 || it.charId === -1) continue;
    if (!SC.asmChars[it.charId] || !SC.asmChars[it.charId].img) continue;
    allRects.push({ x0: it.dx, y0: it.dy, x1: it.dx + it.dw, y1: it.dy + it.dh });
    if (!(it.z > 0 && it.z < 100)) { satQ++; continue; }   // body items only (zBase=0 rank band)
    let g2 = bySlot.get(it.slot); if (!g2) { g2 = { x0: 1e9, y0: 1e9, x1: -1e9, y1: -1e9 }; bySlot.set(it.slot, g2); }
    g2.x0 = Math.min(g2.x0, it.dx); g2.y0 = Math.min(g2.y0, it.dy);
    g2.x1 = Math.max(g2.x1, it.dx + it.dw); g2.y1 = Math.max(g2.y1, it.dy + it.dh);
  }
  if (acc === EXACT) satQuadsTot += satQ;
  // min pairwise anchor distance -> "clean" frames have well-separated chars
  const anchors = [...bySlot.keys()].map(s => SC.slot[s]).filter(Boolean);
  let minSep = 1e9;
  for (let i = 0; i < anchors.length; i++) for (let j = i + 1; j < anchors.length; j++)
    minSep = Math.min(minSep, Math.abs(anchors[i].screen_x - anchors[j].screen_x));
  for (const [slot, g2] of bySlot) {
    const sl = SC.slot[slot]; if (!sl) continue;
    const ax = sl.screen_x, ay = sl.screen_y;
    // engine SB quads windowed around the slot anchor (identity only — the
    // engine bbox itself is the measured truth we compare against)
    let ex0 = 1e9, ey0 = 1e9, ex1 = -1e9, ey1 = -1e9, en = 0;
    for (const q of ec) {
      const qcx = (q.x0 + q.x1) / 2;
      if (qcx < ax - 140 || qcx > ax + 140) continue;
      if (q.y1 < ay - 300 || q.y0 > ay + 60) continue;
      ex0 = Math.min(ex0, q.x0); ey0 = Math.min(ey0, q.y0);
      ex1 = Math.max(ex1, q.x1); ey1 = Math.max(ey1, q.y1); en++;
    }
    if (!en) { if (acc === EXACT) { engWinMiss++; if (slot >= 2) assistDraw++; } continue; }
    const dx = (g2.x0 + g2.x1) / 2 - (ex0 + ex1) / 2;
    const dy = (g2.y0 + g2.y1) / 2 - (ey0 + ey1) / 2;
    const d = Math.hypot(dx, dy);
    const add = (a) => { a.n++; a.sumDx += dx; a.sumDy += dy; a.sumAbs += d;
      if (d <= 8) a.m8++; if (d <= 16) a.m16++;
      a.h[d < 2 ? 0 : d < 4 ? 1 : d < 8 ? 2 : d < 16 ? 3 : d < 32 ? 4 : 5]++; };
    add(acc);
    if (cleanAcc && minSep > 200) add(cleanAcc);
    // SYMMETRIC window metric (EXACT only): union of ALL bridge quads in the SAME
    // window vs the engine window bbox — "what we paint here" vs "what it painted
    // here" (separates sats-in-staging from body-anchor error).
    if (acc === EXACT) {
      let bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9, bn = 0;
      for (const q of allRects) {
        const qcx = (q.x0 + q.x1) / 2;
        if (qcx < ax - 140 || qcx > ax + 140) continue;
        if (q.y1 < ay - 300 || q.y0 > ay + 60) continue;
        bx0 = Math.min(bx0, q.x0); by0 = Math.min(by0, q.y0);
        bx1 = Math.max(bx1, q.x1); by1 = Math.max(by1, q.y1); bn++;
      }
      if (bn) {
        const wdx = (bx0 + bx1) / 2 - (ex0 + ex1) / 2, wdy = (by0 + by1) / 2 - (ey0 + ey1) / 2;
        const wd = Math.hypot(wdx, wdy);
        WIN.n++; WIN.sumDx += wdx; WIN.sumDy += wdy; WIN.sumAbs += wd;
        if (wd <= 8) WIN.m8++; if (wd <= 16) WIN.m16++;
        WIN.h[wd < 2 ? 0 : wd < 4 ? 1 : wd < 8 ? 2 : wd < 16 ? 3 : wd < 32 ? 4 : 5]++;
        WINR.w += (bx1 - bx0) / Math.max(1, ex1 - ex0); WINR.h += (by1 - by0) / Math.max(1, ey1 - ey0); WINR.n++;
      }
    }
    // per-cid drill-down (EXACT only): anchor bias split by character + scale
    // ratio (bridge bbox size / engine bbox size) + FEET offset (bottom edges)
    if (acc === EXACT) {
      const cid = sl.char_id;
      let pc = perCid.get(cid); if (!pc) { pc = { n: 0, m8: 0, sumDx: 0, sumDy: 0, sumW: 0, sumH: 0, sumFy: 0 }; perCid.set(cid, pc); }
      pc.n++; if (d <= 8) pc.m8++;
      pc.sumDx += dx; pc.sumDy += dy;
      pc.sumW += (g2.x1 - g2.x0) / Math.max(1, ex1 - ex0);
      pc.sumH += (g2.y1 - g2.y0) / Math.max(1, ey1 - ey0);
      pc.sumFy += g2.y1 - ey1;
    }
  }
};
const perCid = new Map();
let pending = null;   // decoded TA frame awaiting its own-frame GSTA/OBJS (EXACT pairing)
const gateFrame = (ref, stripped, runs) => {
  let sTotal = 0, kSumRuns = 0;
  for (const [cls, cnt] of runs) { if (cls === 0) sTotal += cnt; else kSumRuns += cnt; }
  if (SC.inMatch !== 1 || sTotal === 0) return;
  fightFrames++;

  // 1+2: bridge geometry
  const bp = bridgeBuild();
  if (!bp || !bp.translucent.length) { zeroQuadFrames++; return; }
  quadsTot += bp.translucent.length;
  const bf = new Float32Array(bp.vertexData.buffer, bp.vertexData.byteOffset, bp.vertexCount * 7);
  for (let v = 0; v < bp.vertexCount; v++) {
    const fi = v * 7;
    if (!isFinite(bf[fi]) || !isFinite(bf[fi + 1]) || !isFinite(bf[fi + 2])) badVert++;
    const u = bf[fi + 5], vv = bf[fi + 6];
    if (u < 0 || u > 1 || vv < 0 || vv > 1) badUV++;
  }

  // 3: the page's descriptor splice (param-identity), bridge polys as the body source
  const kept = P2.parse(stripped, stripped.length).translucent || [];
  let kParams = 0; for (let i = 0; i < kept.length; i++) if (i === 0 || kept[i].param !== kept[i - 1].param) kParams++;
  if (kSumRuns !== kParams) { spliceSkew++; return; }
  const bodyPolys = bp.translucent;
  const merged = []; let bi = 0, ki = 0;
  for (const [cls, cnt] of runs) {
    if (cls === 0) { const take = Math.min(cnt, bodyPolys.length - bi); for (let i = 0; i < take; i++) merged.push(bodyPolys[bi++]); }
    else { let seen = 0, last = -1;
      while (ki < kept.length) { const q = kept[ki];
        if (q.param !== last) { if (seen === cnt) break; seen++; last = q.param; }
        merged.push(q); ki++; } }
  }
  while (ki < kept.length) merged.push(kept[ki++]);
  while (bi < bodyPolys.length) merged.push(bodyPolys[bi++]);
  if (merged.length !== kept.length + bodyPolys.length) spliceLost++;
  else {
    let kj = 0, ok = true;
    for (const q of merged) if (kj < kept.length && q === kept[kj]) kj++;
    if (kj !== kept.length) { keptReorder++; ok = false; }
    if (ok) spliceOk++;
  }

  // 4: position gate (EXACT pairing — state includes this frame's GSTA/OBJS)
  const engRects = polyRects(ref, ref.translucent,
    pp => ((pp.pcw >>> 29) & 7) === 5 && SB.has((((pp.tcw >>> 0) & 0x1FFFFF) >>> 12)));
  measureSlots(EXACT, engRects, EXACT_CLEAN);
  missPoseTot += SC._asmMiss || 0;
  skipSelTot += SC._asmSkipSel || 0;
  // secondary: whole-frame cluster counts (extras = sats/assists/limb fragments)
  const ec = clusters(engRects);
  const bc = clusters(polyRects(bp, bp.translucent, null));
  engClustTot += ec.length; brClustTot += bc.length;
  const usedB = new Set();
  for (const e of ec) {
    let best = -1, bd = 1e9;
    for (let i = 0; i < bc.length; i++) { const d = Math.hypot(bc[i].cx - e.cx, bc[i].cy - e.cy); if (d < bd) { bd = d; best = i; } }
    if (best >= 0) usedB.add(best);
  }
  extraBridge += bc.length - usedB.size;
};
for (const r of recs) {
  if (r.sock !== 0) continue;
  const b = r.b;
  if (isM(b, 'GSTA')) { try { SC.onGSTA(b); } catch (e) {} continue; }
  if (isM(b, 'OBJS')) { try { SC.onOBJS(b); } catch (e) {} continue; }
  const magic = b.length >= 4 ? b.readUInt32LE(0) : 0;
  if (!isM(b, 'ZCST') && magic !== 0x434E5953 && magic !== 0x4E595346) continue;
  // EXACT pairing: the GSTA/OBJS for the pending TA frame have now arrived
  // (they follow it on the wire) — gate the pending frame first.
  if (pending) { gateFrame(pending.ref, pending.stripped, pending.runs); pending = null; }
  let fr = null;
  try { fr = D.applyFrame(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength)); } catch (e) { continue; }
  if (!fr || !D.prevTASize) continue;
  const ta = D.prevTA.subarray(0, D.prevTASize);
  const ref = P1.parse(ta, D.prevTASize);
  const { stripped, runs } = stripAndDescribe(ta, D.prevTASize);
  // LAG pairing (state as-of decode) — position metric only, on the live state
  if (SC.inMatch === 1) {
    const engRects = polyRects(ref, ref.translucent,
      pp => ((pp.pcw >>> 29) & 7) === 5 && SB.has((((pp.tcw >>> 0) & 0x1FFFFF) >>> 12)));
    if (engRects.length) measureSlots(LAG, engRects, null);
  }
  // deep-copy nothing: ref/stripped reuse parser buffers that survive until next parse
  pending = { ref: { translucent: ref.translucent, vertexData: ref.vertexData.slice(), vertexCount: ref.vertexCount },
              stripped: stripped.slice(), runs };
}
if (pending) gateFrame(pending.ref, pending.stripped, pending.runs);

console.log(`\n== SPRITE-BRIDGE SMOKE ==`);
console.log(`fight frames (stripped content): ${fightFrames}`);
console.log(`zero-quad frames: ${zeroQuadFrames}  avg quads/frame: ${(quadsTot / Math.max(1, fightFrames - zeroQuadFrames)).toFixed(1)}`);
console.log(`bad verts: ${badVert}  bad UVs: ${badUV}`);
console.log(`splice: ok ${spliceOk}  skew ${spliceSkew}  lost ${spliceLost}  kept-reorder ${keptReorder}`);
const rep = (name, a) => console.log(
  `position ${name}: n ${a.n}  <=8px ${a.m8}/${a.n} (${(100 * a.m8 / Math.max(1, a.n)).toFixed(1)}%)  <=16px ${a.m16}/${a.n} (${(100 * a.m16 / Math.max(1, a.n)).toFixed(1)}%)\n` +
  `  mean dx ${(a.sumDx / Math.max(1, a.n)).toFixed(2)} dy ${(a.sumDy / Math.max(1, a.n)).toFixed(2)}  mean |d| ${(a.sumAbs / Math.max(1, a.n)).toFixed(2)}px  hist[<2,<4,<8,<16,<32,>=32] ${a.h.join(',')}`);
rep('EXACT-pair (bridge body bbox vs anchor-windowed engine bbox)', EXACT);
rep('EXACT-pair CLEAN (chars >200px apart)', EXACT_CLEAN);
rep('LAG-pair (state 1 frame stale, the naive live pairing)', LAG);
rep('EXACT-pair WINDOWED (ALL bridge quads vs ALL engine quads, same window)', WIN);
console.log(`  windowed size ratio: w ${(WINR.w / Math.max(1, WINR.n)).toFixed(3)}  h ${(WINR.h / Math.max(1, WINR.n)).toFixed(3)}`);
for (const [cid, pc] of [...perCid.entries()].sort((a, b) => b[1].n - a[1].n))
  console.log(`  cid ${cid} (PL${cid.toString(16).padStart(2, '0').toUpperCase()}): n ${pc.n}  <=8px ${(100 * pc.m8 / pc.n).toFixed(1)}%  dx ${(pc.sumDx / pc.n).toFixed(2)}  dy ${(pc.sumDy / pc.n).toFixed(2)}  feetDy ${(pc.sumFy / pc.n).toFixed(2)}  wRatio ${(pc.sumW / pc.n).toFixed(3)}  hRatio ${(pc.sumH / pc.n).toFixed(3)}`);
console.log(`secondary: engine clusters ${engClustTot}  bridge clusters ${brClustTot}  extra-bridge ${extraBridge}`);
console.log(`  sat quads ${satQuadsTot}  missing poses ${missPoseTot}  skipped sels (uncaptured parts) ${skipSelTot}  engine-window miss ${engWinMiss} (assist-slot draws ${assistDraw})`);
const pass = fightFrames > 100 && zeroQuadFrames / Math.max(1, fightFrames) < 0.01
  && badVert === 0 && badUV === 0 && spliceSkew === 0 && spliceLost === 0 && keptReorder === 0;
console.log(pass ? 'STRUCTURAL PASS (position rate above is the visual gate — report it)' : 'STRUCTURAL FAIL');
process.exit(pass ? 0 : 1);
