// _zz_flag_coverage.mjs — WHOLE-CATALOG render-flag coverage scan.
// Walks the GFX2 cell dispatch of all 59 chars and tabulates EVERY per-record
// flags-u16 (record+0x4) that appears in the game, per bit + per distinct value,
// so we can see which flag bits render_frame must handle vs which it currently
// ignores. Companion to _zz_catalog_carve_gate.mjs (that gates the texel carve;
// this audits the FLAG surface for render_frame gaps like the 0x8000 flip).
//   node _zz_flag_coverage.mjs [--per-char] [--detail-bit=0x8000]
import { readFileSync, existsSync } from 'node:fs';

const GFXDIR = new URL('../../web/render-replica/gfx/', import.meta.url);
const arg = n => process.argv.includes(n);
const argv = k => { const p = process.argv.find(a => a.startsWith(k+'=')); return p ? p.split('=')[1] : null; };

// what render_frame.c CURRENTLY reads from the cell flags (r11+0x4):
//   line 206: (flags & 0x4000)  -> texU/X mirror (HANDLED)
// everything else in the flags u16 is currently IGNORED by render_frame.
const HANDLED = 0x4000;
const KNOWN = { 0x4000:'texU/X mirror (HANDLED)', 0x8000:'second flip (UNHANDLED - the Storm LS bug)',
                0x0020:'?', 0x0010:'?', 0x2000:'?', 0x1000:'?' };

const r16 = (b,a)=> b[a] | (b[a+1]<<8);
const r32 = (b,a)=> (b[a]|(b[a+1]<<8)|(b[a+2]<<16)|(b[a+3]<<24))>>>0;

const bitTotals = new Map();     // bit -> count of records with that bit set
const valTotals = new Map();     // full flags value -> count
const perChar = new Map();       // cid -> Set(flag values)
let totalRecords = 0, totalSids = 0, chars = 0;

for (let cid = 0; cid <= 0x3A; cid++) {
  const nn = cid.toString(16).toUpperCase().padStart(2,'0');
  const f = new URL(`PL${nn}_gfx2.bin`, GFXDIR);
  if (!existsSync(f)) continue;
  chars++;
  const g2 = new Uint8Array(readFileSync(f));
  const nsid = (r32(g2,0) >>> 2);                 // dispatch head: entry[0] = n*4
  if (nsid === 0 || nsid > 0x40000) continue;
  const seen = new Set();
  for (let sid = 0; sid < nsid; sid++) {
    const disp = r32(g2, sid*4);
    if (disp === 0 || disp + 2 > g2.length) continue;
    const cellBase = disp;                          // cell = GFX2 + GFX2[sid*4]; offsets are GFX2-relative
    const nrec = r16(g2, cellBase);
    if (nrec === 0 || nrec > 256) continue;
    if (seen.has(cellBase)) continue; seen.add(cellBase); totalSids++;
    let rec = cellBase + 2;
    for (let r = 0; r < nrec && rec+8 <= g2.length; r++, rec += 8) {
      const flags = r16(g2, rec + 4);
      totalRecords++;
      valTotals.set(flags, (valTotals.get(flags)||0)+1);
      for (let b = 0; b < 16; b++) if (flags & (1<<b)) { const bit=1<<b; bitTotals.set(bit,(bitTotals.get(bit)||0)+1); }
      if (!perChar.has(cid)) perChar.set(cid, new Set());
      perChar.get(cid).add(flags);
    }
  }
}

console.log(`\n=== RENDER-FLAG COVERAGE: ${chars} chars / ${totalSids} sids / ${totalRecords} cell records ===\n`);
console.log('BIT frequency across all records (which flag bits the game USES):');
console.log(' bit      count     render_frame handles?   note');
for (const [bit,cnt] of [...bitTotals].sort((a,b)=>b[1]-a[1])) {
  const handled = (bit & HANDLED) ? 'YES' : 'NO  <-- GAP';
  console.log(`  0x${bit.toString(16).padStart(4,'0')}  ${String(cnt).padStart(8)}     ${handled.padEnd(12)}   ${KNOWN[bit]||''}`);
}
console.log('\nDISTINCT flag VALUES (full u16, top 20):');
console.log(' flags     count    unhandled bits (render_frame ignores)');
for (const [val,cnt] of [...valTotals].sort((a,b)=>b[1]-a[1]).slice(0,20)) {
  const unh = val & ~HANDLED & 0xFFFF;
  console.log(`  0x${val.toString(16).padStart(4,'0')}  ${String(cnt).padStart(8)}    ${unh?('0x'+unh.toString(16).padStart(4,'0')):'(none - fully handled)'}`);
}
// which chars use the unhandled 0x8000 (or a --detail-bit)
const detail = argv('--detail-bit') ? parseInt(argv('--detail-bit')) : 0x8000;
console.log(`\nCHARS using 0x${detail.toString(16)} (the flagged gap):`);
const hits = [];
for (const [cid,vals] of perChar) if ([...vals].some(v => v & detail)) hits.push('PL'+cid.toString(16).toUpperCase().padStart(2,'0'));
console.log('  '+(hits.join(' ')||'(none)'));
if (arg('--per-char')) {
  console.log('\nper-char distinct flag values:');
  for (const [cid,vals] of [...perChar].sort((a,b)=>a[0]-b[0]))
    console.log(`  PL${cid.toString(16).toUpperCase().padStart(2,'0')}: ${[...vals].sort((a,b)=>a-b).map(v=>'0x'+v.toString(16)).join(' ')}`);
}
