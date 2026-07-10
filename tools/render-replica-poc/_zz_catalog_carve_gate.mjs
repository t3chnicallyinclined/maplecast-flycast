// _zz_catalog_carve_gate.mjs — COMPREHENSIVE, CAPTURE-INDEPENDENT carve gate over the WHOLE
// MVC2 pose catalog. For EVERY character PL00..PL3A, it walks the GFX2 sprite-dispatch table
// (every sprite_id -> cell -> 8-byte records), resolves each record's GFX1 part + its per-record
// 0x4000 flip flag, then runs the REAL PRODUCTION carve (body_decoder.mjs ensureBodyTextures,
// native + linear paths, INCLUDING the scene10 flip4000 fix) on a synthetic single-instance emit
// and byte-gates every emitted tile against the whole-part Y-first detwiddle GROUND TRUTH.
//
// WHY THIS SUPERSEDES _zz_roster_carve_gate.mjs:
//   * _zz_roster walks GFX1 by SHAPE and tests the pure twTileYFirst CHUNK ORDER only for dense
//     (Tw>2 && Th>2) parts — it has NO GFX2 context, NO flip4000, and SKIPS 2x2 / 2xN / Nx2 / strip
//     parts (which the production NATIVE path DOES carve: cond = m==32 && pCols>1 && pRows>1, i.e.
//     Tw>=2 && Th>=2). Storm's Lightning-Strike flying pose (sel0x35d 64x64 = 2x2, flip4000) is in
//     THAT skipped class.
//   * This gate drives the ACTUAL ensureBodyTextures with the ACTUAL per-record flip4000 for every
//     reachable pose, so it certifies the shipped carve end-to-end, and enumerates every remaining
//     mismatch as a definitive bug map.
//
// GROUND TRUTH (re_kb/70 + re_kb/71, engine-VRAM-arbitrated): the engine DMAs each part to VRAM as
// ONE verbatim WxH PVR-twiddle blob (loc_8c033d78, shape-agnostic). So the correct content of grid
// tile (col,row) read by the pvr2 renderer as a standalone 32x32 PAL4_TW = the (col*m,row*m) m*m
// region of detwiddlePal4(rawBlob, W, H). The 0x4000 flag is a DRAW-TIME texU mirror ONLY
// (loc_8c0346c4, re_kb/24) — storage is facing-INDEPENDENT — so the carve must place each tile's
// RAW storage chunk; any storage-column reversal is a bug (the re_kb/71 double-apply).
//
// The gate READS BACK production's vram exactly as the GPU does: detwiddle each written 512B tile
// as a standalone 32x32 and compare its top-left m*m to the whole-part-detwiddle region. This is the
// same `cmp` _zz_roster uses, but fed from PRODUCTION output rather than a candidate order.
//
// Usage:  node _zz_catalog_carve_gate.mjs [--detail] [--class=native|strip|sub32] [PLxx ...]
//   --detail   list every (char,sid,sel,flip,shape) with bad>0 (default: summary + bug classes)
//   default    all PL00..PL3A
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { ensureBodyTextures, decodeA } from '../../web/render-replica/body_decoder.mjs';

const GFX = fileURLToPath(new URL('../../web/render-replica/gfx/', import.meta.url));
const ANIM = fileURLToPath(new URL('../../refs/anotak/animations/', import.meta.url));

const NAME = {
  0x00:'Ryu',0x01:'Zangief',0x02:'Guile',0x03:'Morrigan',0x04:'Anakaris',0x05:'Strider',
  0x06:'Cyclops',0x07:'Wolverine(metal)',0x08:'Psylocke',0x09:'Iceman',0x0A:'Rogue',
  0x0B:'Captain America',0x0C:'Spider-Man',0x0D:'Hulk',0x0E:'Venom',0x0F:'Dr. Doom',
  0x10:'Tron',0x11:'Jill',0x12:'Hayato',0x13:'Ruby Heart',0x14:'SonSon',0x15:'Amingo',
  0x16:'Marrow',0x17:'Cable',0x18:'Abyss1',0x19:'Abyss2',0x1A:'Abyss3',0x1B:'Chun-Li',
  0x1C:'Mega Man',0x1D:'Roll',0x1E:'Akuma',0x1F:'B.B.Hood',0x20:'Felicia',0x21:'Charlie',
  0x22:'Sakura',0x23:'Dan',0x24:'Cammy',0x25:'Dhalsim',0x26:'M.Bison',0x27:'Ken',
  0x28:'Gambit',0x29:'Juggernaut',0x2A:'Storm',0x2B:'Sabretooth',0x2C:'Magneto',
  0x2D:'Shuma-Gorath',0x2E:'War Machine',0x2F:'Silver Samurai',0x30:'Omega Red',0x31:'Spiral',
  0x32:'Colossus',0x33:'Iron Man',0x34:'Sentinel',0x35:'Blackheart',0x36:'Thanos',0x37:'Jin',
  0x38:'Captain Commando',0x39:'Wolverine(bone)',0x3A:'Servbot',
};

// ---- GROUND-TRUTH twiddle machinery (VERBATIM from body_decoder.mjs, production Y-first) ------
function _twiddleSlow(x, y, xs, ys) {
  let rv = 0, sh = 0; xs >>= 1; ys >>= 1;
  while (xs || ys) { if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; } if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; } }
  return rv;
}
const _DETW = [[], []];
for (let s = 0; s < 11; s++) { const ys = 1 << s; _DETW[0][s] = new Int32Array(1024); _DETW[1][s] = new Int32Array(1024); for (let i = 0; i < 1024; i++) { _DETW[0][s][i] = _twiddleSlow(i, 0, 1024, ys); _DETW[1][s][i] = _twiddleSlow(0, i, ys, 1024); } }
const _PAL4_ORDER = [[0,0],[0,1],[1,0],[1,1],[0,2],[0,3],[1,2],[1,3],[2,0],[2,1],[3,0],[3,1],[2,2],[2,3],[3,2],[3,3]];
function _log2i(v) { let n = -1; while (v) { v >>= 1; n++; } return n; }
function detwiddlePal4(data, w, h) {
  const bcx = _log2i(w), bcy = _log2i(h); const idx = new Uint8Array(w * h);
  for (let y = 0; y < h; y += 4) for (let x = 0; x < w; x += 4) {
    const blk = ((_DETW[0][bcy][x] + _DETW[1][bcx][y]) / 16) | 0; const base = blk * 8;
    for (let i = 0; i < 16; i++) { const cx = _PAL4_ORDER[i][0], cy = _PAL4_ORDER[i][1]; const b = (base + (i >> 1) < data.length) ? data[base + (i >> 1)] : 0; idx[(y + cy) * w + (x + cx)] = (i & 1) ? ((b >> 4) & 0xF) : (b & 0xF); }
  }
  return idx;
}
function detw32(chunk) { return detwiddlePal4(chunk, 32, 32); }

// ---- little readers ---------------------------------------------------------------------------
const bu8  = (b, a) => b[a];
const bu16 = (b, a) => b[a] | (b[a + 1] << 8);
const bs16 = (b, a) => { const v = b[a] | (b[a + 1] << 8); return v & 0x8000 ? v - 0x10000 : v; };
const bu32 = (b, a) => (b[a] | (b[a + 1] << 8) | (b[a + 2] << 16) | (b[a + 3] << 24)) >>> 0;

// ---- synthetic-quad harness driving the REAL ensureBodyTextures -------------------------------
// GFX1 lives at 0x8C400000 (masks to 0x400000 in the seeded 16MB image); passes the body-base
// guard (0x8C000000 set; not in the [0x0CED0000,0x0CEE0000) effect-poly range).
const GFX1_BASE = 0x8C400000;
const QUAD = 96;
// bf16 (top-16-of-float32) encoder — matches body_decoder's _f16 reader (setUint32(bits<<16)).
function bf16(f) { const dv = new DataView(new ArrayBuffer(4)); dv.setFloat32(0, f, true); return (dv.getUint32(0, true) >>> 16) & 0xFFFF; }

// Run production ensureBodyTextures for ONE single-instance part emit (full Tw x Th grid),
// return the written vram + per-tile addr. m = engine tile size (32 / 16 / 8). flip = 0x4000.
function runCarve(ram, cache, sel, W, H, m, flip4000) {
  const Tw = (W / m) | 0, Th = (H / m) | 0, nT = Tw * Th;
  const ta = new Uint8Array(nT * QUAD);
  const dv = new DataView(ta.buffer);
  const sels = new Uint16Array(nT), gfx1s = new Uint32Array(nT), cr = new Int32Array(nT * 2);
  const mir = new Uint8Array(nT), sd = new Uint8Array(nT * 4);
  const u1 = m / 32;                                   // usz=32 (TSP bits3-5=2) -> mq = round(u1*32) = m
  const addrOf = [];
  let q = 0;
  for (let row = 0; row < Th; row++) for (let col = 0; col < Tw; col++, q++) {
    const addr = q * 512;                              // unique, 512-aligned, TCW = addr>>3
    addrOf.push(addr);
    dv.setUint32(q * QUAD + 8, 0x10, true);            // TSP: (0x10>>3)&7 = 2 -> usz = 8<<2 = 32
    dv.setUint32(q * QUAD + 0x0C, (addr >>> 3) & 0x1FFFFF, true); // TCW -> addr
    const h = bf16(u1); dv.setUint16(q * QUAD + 86, h, true); dv.setUint16(q * QUAD + 90, h, true); dv.setUint16(q * QUAD + 94, h, true);
    sels[q] = sel; gfx1s[q] = GFX1_BASE >>> 0; cr[2 * q] = col; cr[2 * q + 1] = row; mir[q] = 0;
    // srcdesc: [m, cx=col(raw storage col), ry=(rows-row) so row2=row, flags= bit0 valid | bit1 flip4000]
    sd[4 * q + 0] = m; sd[4 * q + 1] = col; sd[4 * q + 2] = (Th - row) & 0xFF; sd[4 * q + 3] = 1 | (flip4000 ? 2 : 0);
  }
  const vram = new Uint8Array(nT * 512);
  ensureBodyTextures(ram, vram, ta, nT, cache, sels, gfx1s, cr, null, mir, sd);
  return { vram, addrOf, Tw, Th };
}

// ---- per-char gate ----------------------------------------------------------------------------
// Bug-class buckets. path in {native,strip,sub32,single}. flip in {0,1}.
function classify(W, H, m) {
  const Tw = (W / m) | 0, Th = (H / m) | 0;
  if (m === 32 && Tw > 1 && Th > 1) return { path: 'native', Tw, Th };   // production NATIVE path
  if (m === 32 && (Tw === 1 || Th === 1) && Tw * Th > 1) return { path: 'strip', Tw, Th };
  if (Tw * Th === 1) return { path: 'single', Tw, Th };
  return { path: 'sub32', Tw, Th };
}

function gateChar(cid) {
  const hex = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0');
  const g1p = GFX + hex + '_gfx1.bin', g2p = GFX + hex + '_gfx2.bin';
  if (!existsSync(g1p) || !existsSync(g2p)) return { cid, hex, missing: true };
  const g1 = new Uint8Array(readFileSync(g1p));
  const g2 = new Uint8Array(readFileSync(g2p));

  // GFX1 offset table
  const tableBytes = bu32(g1, 0);
  if (tableBytes < 4 || tableBytes > g1.length) return { cid, hex, badTable: true };
  const n1 = tableBytes >>> 2;
  const partDims = (sel) => {
    if (sel >= n1) return null;
    const off = bu32(g1, sel * 4);
    if (off < tableBytes || off + 4 > g1.length) return null;
    return { off, W: bu8(g1, off + 2) * 8, H: bu8(g1, off + 3) * 8 };
  };
  const sortedOffs = (() => { const s = new Set(); for (let i = 0; i < n1; i++) s.add(bu32(g1, i * 4)); return Uint32Array.from(s).sort((a, b) => a - b); })();
  const endOf = (off) => { let lo = 0, hi = sortedOffs.length; while (lo < hi) { const md = (lo + hi) >> 1; if (sortedOffs[md] <= off) lo = md + 1; else hi = md; } return lo < sortedOffs.length ? sortedOffs[lo] : off + 0x4000; };

  // seed the 16MB image with GFX1 at 0x400000 (reused across this char's parts)
  const ram = new Uint8Array(16 * 1024 * 1024);
  ram.set(g1, GFX1_BASE & 0xFFFFFF);
  const cache = {};

  // GFX2 dispatch walk: numSids = firstCellOffset/4
  const numSids = bu32(g2, 0) >>> 2;

  // dedup: key "sel:flip" -> {W,H,m,path,Tw,Th, sids:Set, multiInstanceSids:Set, npo2, decodeFail}
  const parts = new Map();
  const record = (sel, flip, sid, dupInSid) => {
    const d = partDims(sel);
    if (!d) return;
    const { W, H } = d;
    if (W <= 0 || H <= 0 || W > 1024 || H > 1024) return;
    const key = sel + ':' + flip;
    let e = parts.get(key);
    if (!e) {
      const npo2 = (W & (W - 1)) || (H & (H - 1));
      // engine tile size: 32 for min>=32; else min(W,H) (sub-32 square tile, the sel285-class case)
      const m = Math.min(W, H) >= 32 ? 32 : Math.min(W, H);
      const cl = classify(W, H, m);
      e = { sel, flip, W, H, m, ...cl, npo2, sids: new Set(), multiSids: new Set() };
      parts.set(key, e);
    }
    e.sids.add(sid);
    if (dupInSid) e.multiSids.add(sid);
  };

  for (let sid = 0; sid < numSids; sid++) {
    const cellOff = bu32(g2, sid * 4);
    if (cellOff < numSids * 4 || cellOff + 2 > g2.length) continue;
    const cnt = bu16(g2, cellOff);
    if (cnt === 0 || cnt > 64) continue;
    let p = cellOff + 2;
    if (p + cnt * 8 > g2.length) continue;
    const selCount = new Map();
    const recs = [];
    for (let i = 0; i < cnt; i++, p += 8) {
      const flags = bu16(g2, p + 4), sel = bu16(g2, p + 6);
      const flip = (flags >> 14) & 1;                  // 0x4000 = per-record draw-time texU flip
      recs.push({ sel, flip });
      selCount.set(sel, (selCount.get(sel) || 0) + 1);
    }
    for (const r of recs) record(r.sel, r.flip, sid, (selCount.get(r.sel) || 0) > 1);
  }

  // gate each unique (sel,flip)
  const results = [];
  for (const e of parts.values()) {
    if (e.npo2) { results.push({ ...e, gated: false, reason: 'npo2' }); continue; }
    if (e.path === 'single') { results.push({ ...e, gated: true, tiles: 1, bad: 0 }); continue; }
    // decode + whole-part detwiddle GT
    const off = e.off ?? partDims(e.sel).off;
    const destLen = (e.W * e.H) >> 1;
    const raw = decodeA(g1, off + 4, endOf(off), destLen);
    const lin = detwiddlePal4(raw, e.W, e.H);
    const { vram, addrOf, Tw, Th } = runCarve(ram, cache, e.sel, e.W, e.H, e.m, e.flip);
    let bad = 0, tiles = 0, written = 0;
    const m = e.m;
    for (let row = 0; row < Th; row++) for (let col = 0; col < Tw; col++) {
      const q = row * Tw + col, addr = addrOf[q];
      tiles++;
      // is this GT region nonzero? (a swapped blank tile cannot expose an order bug)
      let nz = false;
      for (let yy = 0; yy < m && !nz; yy++) for (let xx = 0; xx < m; xx++) { if (lin[(row * m + yy) * e.W + (col * m + xx)]) { nz = true; break; } }
      const til = detw32(vram.subarray(addr, addr + 512));
      let anyWritten = false, mism = false;
      for (let yy = 0; yy < m; yy++) for (let xx = 0; xx < m; xx++) {
        const got = til[yy * 32 + xx], want = lin[(row * m + yy) * e.W + (col * m + xx)];
        if (got) anyWritten = true;
        if (got !== want) mism = true;
      }
      if (anyWritten) written++;
      if (mism && nz) bad++;                            // count only discriminating (nonzero) mismatches
    }
    results.push({ ...e, off, Tw, Th, gated: true, tiles, bad, written });
  }
  return { cid, hex, name: NAME[cid], numSids, results };
}

// ---- anotak animation labels (best-effort; the anotak Sprite field is text-only, so we map by
//      GFX2 co-membership is not possible — we surface known re_kb move labels instead) ----------
const KNOWN = {
  // char_id -> { sid(hex) : 'move label' }  (from re_kb/63 storm_move_spawn_catalog + re_kb/71)
  0x2A: { 0x1ea:'Lightning Attack (upright)', 0x1eb:'Lightning Attack', 0x1f0:'Lightning Attack (flying)', 0x1f1:'Lightning Strike (flying pose)' },
};

// ---- main -------------------------------------------------------------------------------------
const args = process.argv.slice(2);
const DETAIL = args.includes('--detail');
const classFilter = (args.find(a => a.startsWith('--class=')) || '').split('=')[1] || null;
const argChars = args.filter(a => /^PL/i.test(a));
let ids;
if (argChars.length) ids = argChars.map(s => parseInt(s.replace(/^PL/i, ''), 16));
else { ids = []; for (let c = 0x00; c <= 0x3A; c++) ids.push(c); }

console.log('=== CATALOG-WIDE carve gate (GFX2 dispatch -> production ensureBodyTextures -> whole-part Y-first detwiddle GT) ===\n');
console.log('cid name              | sids  | uniq(sel,flip) parts | native | strip | sub32 | single | npo2skip | BAD parts | BAD tiles');
console.log('----+------------------+-------+----------------------+--------+-------+-------+--------+----------+-----------+----------');

const allBad = [];                 // detailed bad-part records
const classAgg = new Map();        // "path|flip" -> {parts, badParts, tiles, badTiles}
const bump = (k, badP, tiles, badT) => { let a = classAgg.get(k); if (!a) { a = { parts: 0, badParts: 0, tiles: 0, badTiles: 0 }; classAgg.set(k, a); } a.parts++; if (badP) a.badParts++; a.tiles += tiles; a.badTiles += badT; };

let TOT = { sids: 0, parts: 0, native: 0, strip: 0, sub32: 0, single: 0, npo2: 0, badParts: 0, badTiles: 0 };

for (const cid of ids) {
  const r = gateChar(cid);
  if (r.missing) { console.log(`${r.hex.slice(2)}  ${(NAME[cid] || '?').padEnd(17)}| MISSING GFX bin`); continue; }
  if (r.badTable) { console.log(`${r.hex.slice(2)}  ${(NAME[cid] || '?').padEnd(17)}| BAD GFX1 table`); continue; }
  let cNative = 0, cStrip = 0, cSub = 0, cSingle = 0, cNpo2 = 0, cBadP = 0, cBadT = 0;
  for (const e of r.results) {
    if (classFilter && e.path !== classFilter) continue;
    if (e.path === 'native') cNative++; else if (e.path === 'strip') cStrip++; else if (e.path === 'sub32') cSub++; else if (e.path === 'single') cSingle++;
    if (!e.gated) { cNpo2++; continue; }
    const badP = e.bad > 0;
    if (badP) { cBadP++; cBadT += e.bad; allBad.push({ hex: r.hex, name: r.name, ...e }); }
    bump(e.path + '|' + e.flip, badP, e.tiles, e.bad);
  }
  const uniq = r.results.length;
  console.log(`${r.hex.slice(2)}  ${(r.name || '?').padEnd(17)}| ${String(r.numSids).padStart(5)} | ${String(uniq).padStart(20)} | ${String(cNative).padStart(6)} | ${String(cStrip).padStart(5)} | ${String(cSub).padStart(5)} | ${String(cSingle).padStart(6)} | ${String(cNpo2).padStart(8)} | ${String(cBadP).padStart(9)} | ${String(cBadT).padStart(8)}`);
  TOT.sids += r.numSids; TOT.parts += uniq; TOT.native += cNative; TOT.strip += cStrip; TOT.sub32 += cSub; TOT.single += cSingle; TOT.npo2 += cNpo2; TOT.badParts += cBadP; TOT.badTiles += cBadT;
}
console.log('----+------------------+-------+----------------------+--------+-------+-------+--------+----------+-----------+----------');
console.log(`TOT roster             | ${String(TOT.sids).padStart(5)} | ${String(TOT.parts).padStart(20)} | ${String(TOT.native).padStart(6)} | ${String(TOT.strip).padStart(5)} | ${String(TOT.sub32).padStart(5)} | ${String(TOT.single).padStart(6)} | ${String(TOT.npo2).padStart(8)} | ${String(TOT.badParts).padStart(9)} | ${String(TOT.badTiles).padStart(8)}`);

console.log('\n=== BUG-CLASS breakdown (path|flip4000): parts / BAD parts / tiles / BAD tiles ===');
const order = ['native|0','native|1','strip|0','strip|1','sub32|0','sub32|1'];
for (const k of order) { const a = classAgg.get(k); if (!a) continue; const [p, f] = k.split('|'); console.log(`  ${p.padEnd(7)} flip4000=${f}:  parts=${String(a.parts).padStart(5)}  BADparts=${String(a.badParts).padStart(4)}  tiles=${String(a.tiles).padStart(6)}  BADtiles=${String(a.badTiles).padStart(5)}`); }

console.log('\n=== VERDICT ===');
if (TOT.badTiles === 0) console.log(`  CLEAN: production carve is byte-exact vs whole-part detwiddle for ALL ${TOT.parts} reachable (sel,flip) parts (${TOT.native} native + ${TOT.strip} strip + ${TOT.sub32} sub32) across ${ids.length} chars. (${TOT.npo2} non-power-of-2 parts ungated: twiddle undefined.)`);
else console.log(`  ${TOT.badParts} BAD parts / ${TOT.badTiles} BAD tiles remain. See detail below.`);

// detail listing
if (allBad.length) {
  console.log('\n=== BAD PARTS (char, sid list, sel, flip4000, WxH shape, m, path, bad/tiles, multi-instance) ===');
  allBad.sort((a, b) => (a.hex < b.hex ? -1 : a.hex > b.hex ? 1 : 0) || b.bad - a.bad);
  for (const e of allBad) {
    const sids = [...e.sids].sort((x, y) => x - y);
    const sidStr = sids.slice(0, 8).map(s => '0x' + s.toString(16)).join(',') + (sids.length > 8 ? `,+${sids.length - 8}` : '');
    const multi = e.multiSids.size ? ` MULTI-INSTANCE(sids ${[...e.multiSids].slice(0, 4).map(s => '0x' + s.toString(16)).join(',')})` : '';
    const label = (KNOWN[e.cid] && sids.map(s => KNOWN[e.cid][s]).filter(Boolean)[0]) || '';
    console.log(`  ${e.hex} ${(e.name || '').padEnd(14)} sel0x${e.sel.toString(16).padStart(3, '0')} flip=${e.flip} ${e.W}x${e.H} (${e.Tw}x${e.Th}) m=${e.m} ${e.path.padEnd(6)} bad=${e.bad}/${e.tiles}${multi}${label ? '  [' + label + ']' : ''}  sids:${sidStr}`);
  }
}

// always surface the priority cases: native flip4000 parts + the sel0x316-class 4x8 non-flip
if (DETAIL) {
  console.log('\n=== DETAIL: all NATIVE flip4000 parts (Storm Lightning-Strike class) ===');
  // re-walk to list them regardless of bad
  for (const cid of ids) {
    const r = gateChar(cid);
    if (r.missing || r.badTable) continue;
    for (const e of r.results) {
      if (e.path === 'native' && e.flip === 1 && e.gated) {
        const sids = [...e.sids].sort((x, y) => x - y).slice(0, 6).map(s => '0x' + s.toString(16)).join(',');
        const label = (KNOWN[cid] && [...e.sids].map(s => KNOWN[cid][s]).filter(Boolean)[0]) || '';
        console.log(`  ${r.hex} ${(r.name || '').padEnd(14)} sel0x${e.sel.toString(16)} ${e.W}x${e.H} (${e.Tw}x${e.Th}) bad=${e.bad}/${e.tiles}${label ? '  [' + label + ']' : ''}  sids:${sids}`);
      }
    }
  }
}
