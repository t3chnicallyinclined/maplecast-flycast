// _zz_roster_carve_gate.mjs — ROSTER-WIDE, CAPTURE-INDEPENDENT byte-gate of the re_kb/70
// COLUMN-PAIR-MAJOR native-chunk texel carve, over EVERY dense (Tw>2 AND Th>2) GFX1 part of
// EVERY character PL00..PL3A.
//
// WHY CAPTURE-INDEPENDENT IS VALID (and strictly more complete than _zz_realblob_gate.mjs):
// the ground truth in BOTH gates is `ref = detwiddlePal4(raw, W, H)` — the WHOLE-PART Y-first
// detwiddle of the engine-byte-exact decodeA output (raw = the part's PVR twiddle blob). The
// per-tile test is: does the standalone 32x32 detwiddle of raw[order(col,row)*512 ..] equal the
// (col*32,row*32) 32x32 region of `ref`? That is a PURE property of raw's twiddle layout — it
// does NOT need a live capture. _zz_realblob_gate restricts to the (col,row) tiles render_frame
// happens to emit for captured poses; here we test the FULL Tw x Th grid of every dense part, so
// every character's dense art is covered whether or not it appears in any capture.
//
// Candidates (verbatim from body_decoder.mjs / _zz_realblob_gate.mjs):
//   colPairChunk  = the SHIPPED fix (re_kb/70), render_frame rebuild_tile_grid emit order.
//   rowBandMajor  = the OLD carve (k=(row&~1)*Tw + col*bh + row-in-band) — what re_kb/70 replaced.
//   twTileYFirst  = pure whole-part Y-first block twiddle (the "Model A" order). Coincides with
//                   colPairChunk for SQUARE grids (4x4 Sentinel); diverges — if at all — only for
//                   NON-SQUARE dense grids, which is exactly what a roster sweep hunts for.
//
// DISCRIMINATION BOOKKEEPING (the re_kb/70 lesson: a swapped BLANK tile can't tell two orders
// apart): per part we count nzTiles (ref-region nonzero) and, among the tiles where colPair and
// rowBand assign DIFFERENT chunks, nzDiscrim (those that are also nonzero) — the tiles that
// actually exercise the difference.
//
// Usage:  node _zz_roster_carve_gate.mjs [PLXX ...]     (default = all PL00..PL3A)
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const GFX = fileURLToPath(new URL('../../web/render-replica/gfx/', import.meta.url));

// ---- roster char_id -> name (re_kb/05_characters.surql) --------------------------------------
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

// ---- twiddle machinery (VERBATIM from body_decoder.mjs, production Y-first) -------------------
function _twiddleSlow(x, y, xs, ys) {
  let rv = 0, sh = 0; xs >>= 1; ys >>= 1;
  while (xs || ys) {
    if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; }
    if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; }
  }
  return rv;
}
const _DETW = [[], []];
for (let s = 0; s < 11; s++) {
  const ys = 1 << s; _DETW[0][s] = new Int32Array(1024); _DETW[1][s] = new Int32Array(1024);
  for (let i = 0; i < 1024; i++) { _DETW[0][s][i] = _twiddleSlow(i, 0, 1024, ys); _DETW[1][s][i] = _twiddleSlow(0, i, ys, 1024); }
}
const _PAL4_ORDER = [[0,0],[0,1],[1,0],[1,1],[0,2],[0,3],[1,2],[1,3],[2,0],[2,1],[3,0],[3,1],[2,2],[2,3],[3,2],[3,3]];
function _log2i(v) { let n = -1; while (v) { v >>= 1; n++; } return n; }
function detwiddlePal4(data, w, h) {
  const bcx = _log2i(w), bcy = _log2i(h);
  const idx = new Uint8Array(w * h);
  for (let y = 0; y < h; y += 4) for (let x = 0; x < w; x += 4) {
    const blk = ((_DETW[0][bcy][x] + _DETW[1][bcx][y]) / 16) | 0;
    const base = blk * 8;
    for (let i = 0; i < 16; i++) {
      const cx = _PAL4_ORDER[i][0], cy = _PAL4_ORDER[i][1];
      const b = (base + (i >> 1) < data.length) ? data[base + (i >> 1)] : 0;
      idx[(y + cy) * w + (x + cx)] = (i & 1) ? ((b >> 4) & 0xF) : (b & 0xF);
    }
  }
  return idx;
}
function detw32(chunk) { return detwiddlePal4(chunk, 32, 32); }

// ---- candidate chunk-index orders (VERBATIM) -------------------------------------------------
function rowBandMajor(col, row, Tw, Th) { const by = row & ~1; const bh = (Th - by < 2) ? (Th - by) : 2; return by * Tw + col * bh + (row - by); }
function colPairChunk(col, row, Tw, Th) {
  let t = 0;
  for (let cp = 0; cp < Tw; cp += 2) {
    const cw = (Tw - cp < 2) ? (Tw - cp) : 2;
    for (let by = 0; by < Th; by += 2) {
      const bh = (Th - by < 2) ? (Th - by) : 2;
      for (let cx2 = 0; cx2 < cw; cx2++) { for (let ry = 0; ry < bh; ry++) { if (cp + cx2 === col && by + ry === row) return t; t++; } }
    }
  }
  return -1;
}
function twTileYFirst(col, row, Tw, Th) {
  let rv = 0, sh = 0, xs = Tw >> 1, ys = Th >> 1, x = col, y = row;
  while (xs || ys) {
    if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; }
    if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; }
  }
  return rv;
}

// ---- the validated GFX1 LZSS decoder (bank03 loc_8c0354c0), VERBATIM --------------------------
function decodeA(src, sp, srcEnd, destLen) {
  const out = new Uint8Array(destLen);
  let o = 0, bc = 0, flags = 0;
  while (o < destLen && sp < srcEnd) {
    if (bc === 0) { flags = src[sp++]; bc = 0x80; if (sp >= srcEnd) break; }
    if ((flags & bc) === 0) { out[o++] = src[sp++]; }
    else { const b = src[sp++]; let s = o - (b >> 4) - 1; const cnt = (b & 0x0F) + 2; for (let k = 0; k < cnt && o < destLen; k++, s++) out[o++] = (s >= 0 && s < o) ? out[s] : 0; }
    bc >>= 1;
  }
  return out;
}

// ---- pure-math permutation diff over the power-of-2 grid set ----------------------------------
function permOf(fn, Tw, Th) { const p = new Array(Tw * Th).fill(-1); for (let r = 0; r < Th; r++) for (let c = 0; c < Tw; c++) { const k = fn(c, r, Tw, Th); if (k >= 0 && k < p.length) p[k] = r * Tw + c; } return p; }
function permEq(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }

function pureMathTable() {
  const dims = [1, 2, 4, 8, 16, 32];
  console.log('\n=== PURE-MATH permutation identity over power-of-2 grids (Tw x Th) ===');
  console.log('  legend: cp=colPairChunk  yf=twTileYFirst  rb=rowBandMajor');
  const rows = [];
  for (const Tw of dims) for (const Th of dims) {
    const cp = permOf(colPairChunk, Tw, Th), yf = permOf(twTileYFirst, Tw, Th), rb = permOf(rowBandMajor, Tw, Th);
    const cpEqYf = permEq(cp, yf), cpEqRb = permEq(cp, rb);
    const dense = Tw > 2 && Th > 2;
    rows.push({ Tw, Th, dense, cpEqYf, cpEqRb });
  }
  for (const r of rows) {
    if (!(r.Tw > 2 && r.Th > 2)) continue; // only dense shapes are governed by the carve
    console.log(`  ${String(r.Tw).padStart(2)}x${String(r.Th).toString().padEnd(2)} dense  cp==yf:${r.cpEqYf ? 'YES' : 'NO '}  cp==rowband:${r.cpEqRb ? 'YES' : 'NO '}`);
  }
  // report any dense shape where colPair and Y-first DISAGREE (the discriminating shapes)
  const disagree = rows.filter(r => r.dense && !r.cpEqYf);
  if (disagree.length) {
    console.log('  ** DISCRIMINATING dense shapes (colPair != Y-first-twiddle): ' + disagree.map(r => `${r.Tw}x${r.Th}`).join(', '));
    console.log('     -> for these, whole-part-detwiddle ground truth favors whichever == 0 bad;');
    console.log('        engine-authoritative arbitration would need a live VRAM capture of that shape.');
  } else {
    console.log('  ** colPair == Y-first-twiddle for ALL dense power-of-2 shapes -> ground truth self-consistent.');
  }
}

// ---- GFX1 part inventory + per-part gate ------------------------------------------------------
const u8  = (b, a) => b[a];
const u32 = (b, a) => (b[a] | (b[a + 1] << 8) | (b[a + 2] << 16) | (b[a + 3] << 24)) >>> 0;

function gateChar(cid) {
  const hex = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0');
  const path = GFX + hex + '_gfx1.bin';
  if (!existsSync(path)) return { cid, hex, missing: true };
  const g1 = new Uint8Array(readFileSync(path));
  const tableBytes = u32(g1, 0);
  if (tableBytes < 4 || tableBytes > g1.length) return { cid, hex, badTable: true };
  const n = tableBytes >>> 2;
  const offs = new Uint32Array(n);
  for (let i = 0; i < n; i++) offs[i] = u32(g1, i * 4);
  const srt = Uint32Array.from(new Set(offs)).sort((a, b) => a - b);
  const endOf = (off) => { let lo = 0, hi = srt.length; while (lo < hi) { const m = (lo + hi) >> 1; if (srt[m] <= off) lo = m + 1; else hi = m; } return lo < srt.length ? srt[lo] : off + 0x4000; };

  const shapes = new Map();  // "TwxTh" -> count
  let bigParts = 0, colPairBad = 0, rowBandBad = 0, twYFirstBad = 0;
  let nzTilesTot = 0, nzDiscrimTot = 0, tilesTot = 0;
  const exceptions = [];      // parts where colPairBad>0 (potential universality break)
  const nonSquare = [];       // dense parts with Tw!=Th (the discriminators)
  const seen = new Set();     // dedup identical part offsets (repeated sels)

  for (let sel = 0; sel < n; sel++) {
    const off = offs[sel];
    if (off < tableBytes || off + 4 > g1.length) continue;   // points into table or past EOF
    if (seen.has(off)) continue; seen.add(off);
    const sw = u8(g1, off + 2), sh = u8(g1, off + 3);
    const W = sw * 8, H = sh * 8;
    if (W <= 0 || H <= 0 || W > 1024 || H > 1024) continue;
    const Tw = (W / 32) | 0, Th = (H / 32) | 0;
    if (!(Tw > 2 && Th > 2)) continue;                        // ONLY the carve-governed dense parts
    if ((W & (W - 1)) || (H & (H - 1))) {                     // non-power-of-2 dim -> twiddle undefined
      exceptions.push({ sel, W, H, Tw, Th, npo2: true });
      continue;
    }
    bigParts++;
    const key = `${Tw}x${Th}`; shapes.set(key, (shapes.get(key) || 0) + 1);
    if (Tw !== Th) nonSquare.push({ sel, Tw, Th });

    const destLen = (W * H) >> 1;
    const raw = decodeA(g1, off + 4, endOf(off), destLen);
    const ref = detwiddlePal4(raw, W, H);

    let cpBad = 0, rbBad = 0, yfBad = 0, nzTiles = 0, nzDiscrim = 0;
    for (let row = 0; row < Th; row++) for (let col = 0; col < Tw; col++) {
      // is this ref region nonzero?
      let nz = false;
      for (let yy = 0; yy < 32 && !nz; yy++) for (let xx = 0; xx < 32; xx++) { if (ref[(row * 32 + yy) * W + (col * 32 + xx)]) { nz = true; break; } }
      if (nz) nzTiles++;
      const kCp = colPairChunk(col, row, Tw, Th), kRb = rowBandMajor(col, row, Tw, Th), kYf = twTileYFirst(col, row, Tw, Th);
      const cmp = (k) => {
        const o = k * 512; const chunk = (k >= 0 && o + 512 <= raw.length) ? raw.subarray(o, o + 512) : new Uint8Array(512);
        const til = detw32(chunk); let bad = 0;
        for (let yy = 0; yy < 32; yy++) for (let xx = 0; xx < 32; xx++) { if (til[yy * 32 + xx] !== ref[(row * 32 + yy) * W + (col * 32 + xx)]) { bad = 1; yy = 32; break; } }
        return bad;
      };
      if (cmp(kCp)) cpBad++;
      if (cmp(kRb)) rbBad++;
      if (cmp(kYf)) yfBad++;
      if (kCp !== kRb && nz) nzDiscrim++;   // a tile that actually distinguishes the two orders
      tilesTot++;
    }
    nzTilesTot += nzTiles; nzDiscrimTot += nzDiscrim;
    colPairBad += cpBad; rowBandBad += rbBad; twYFirstBad += yfBad;
    if (cpBad > 0) exceptions.push({ sel, W, H, Tw, Th, cpBad, rbBad, yfBad, nzTiles });
  }
  return { cid, hex, name: NAME[cid], bigParts, tilesTot, nzTilesTot, nzDiscrimTot, colPairBad, rowBandBad, twYFirstBad, shapes, nonSquare, exceptions };
}

// ---- main ------------------------------------------------------------------------------------
const argChars = process.argv.slice(2);
let ids;
if (argChars.length) ids = argChars.map(s => parseInt(s.replace(/^PL/i, ''), 16));
else { ids = []; for (let c = 0x00; c <= 0x3A; c++) ids.push(c); }

pureMathTable();

console.log('\n=== ROSTER-WIDE dense-part carve gate (ground truth = whole-part Y-first detwiddle) ===');
console.log('cid | name              | #big(>2x>2) | tiles | nzDiscrim | colPair_bad | rowBand_bad | twYFirst_bad');
console.log('----+-------------------+-------------+-------+-----------+-------------+-------------+-------------');
let T = { big: 0, tiles: 0, nzDiscrim: 0, cp: 0, rb: 0, yf: 0 };
const withBig = [], noBig = [], ungated = [], allExceptions = [], allNonSquareShapes = new Set();
for (const cid of ids) {
  const r = gateChar(cid);
  if (r.missing) { ungated.push(`${r.hex} (${NAME[cid] || '?'}) — no GFX1 bin`); continue; }
  if (r.badTable) { ungated.push(`${r.hex} (${NAME[cid] || '?'}) — bad GFX1 offset table`); continue; }
  const tag = r.bigParts ? '' : '   (none)';
  console.log(`${r.hex.slice(2)}  | ${(r.name || '?').padEnd(17)} | ${String(r.bigParts).padStart(11)} | ${String(r.tilesTot).padStart(5)} | ${String(r.nzDiscrimTot).padStart(9)} | ${String(r.colPairBad).padStart(11)} | ${String(r.rowBandBad).padStart(11)} | ${String(r.twYFirstBad).padStart(11)}${tag}`);
  T.big += r.bigParts; T.tiles += r.tilesTot; T.nzDiscrim += r.nzDiscrimTot; T.cp += r.colPairBad; T.rb += r.rowBandBad; T.yf += r.twYFirstBad;
  if (r.bigParts) withBig.push(`${r.hex} ${r.name} (${r.bigParts}; shapes ${[...r.shapes.entries()].map(([k, v]) => k + 'x' + v).join(',')})`);
  else noBig.push(`${r.hex} ${r.name}`);
  for (const ns of r.nonSquare) allNonSquareShapes.add(`${ns.Tw}x${ns.Th}`);
  for (const e of r.exceptions) allExceptions.push({ hex: r.hex, name: r.name, ...e });
}
console.log('----+-------------------+-------------+-------+-----------+-------------+-------------+-------------');
console.log(`TOT | ${'roster'.padEnd(17)} | ${String(T.big).padStart(11)} | ${String(T.tiles).padStart(5)} | ${String(T.nzDiscrim).padStart(9)} | ${String(T.cp).padStart(11)} | ${String(T.rb).padStart(11)} | ${String(T.yf).padStart(11)}`);

console.log('\n=== CHARS WITH dense >2x>2 parts (the Sentinel/Colossus/Juggernaut class) ===');
withBig.forEach(s => console.log('  ' + s));
console.log('\n=== CHARS WITH NO dense >2x>2 parts (carve never triggers; unaffected either way) ===');
console.log('  ' + noBig.join('  |  '));
console.log('\n=== NON-SQUARE dense shapes seen across roster (Tw!=Th, the colPair-vs-Yfirst discriminators): ' + ([...allNonSquareShapes].join(', ') || '(none)') + ' ===');
if (ungated.length) { console.log('\n=== UNGATED (assets missing) ==='); ungated.forEach(s => console.log('  ' + s)); }
if (allExceptions.length) {
  console.log('\n=== !!! EXCEPTIONS (colPair_bad>0 OR non-power-of-2 dim) — POTENTIAL UNIVERSALITY BREAK !!! ===');
  for (const e of allExceptions) console.log('  ' + e.hex + ' ' + e.name + ' sel' + e.sel + ' ' + e.W + 'x' + e.H + ' ' + e.Tw + 'x' + e.Th + (e.npo2 ? ' NON-POWER-OF-2 (twiddle undefined; skipped from gate)' : ` cpBad=${e.cpBad} rbBad=${e.rbBad} yfBad=${e.yfBad} nz=${e.nzTiles}`));
} else {
  console.log('\n=== NO EXCEPTIONS: every decodable dense part gated colPair_bad=0. ===');
}

console.log('\n=== VERDICT ===');
if (T.cp === 0 && !allExceptions.some(e => e.cpBad)) {
  console.log(`  colPairChunk is UNIVERSAL across ${ids.length - ungated.length} decodable chars: ${T.cp}/${T.tiles} bad tiles (0), over ${T.big} dense parts, ${T.nzDiscrim} nonzero DISCRIMINATING tiles.`);
  console.log(`  The OLD row-band-major carve broke ${T.rb}/${T.tiles} tiles across the roster.`);
} else {
  console.log(`  !!! colPairChunk NOT universal: ${T.cp} bad tiles. See EXCEPTIONS above. !!!`);
}
