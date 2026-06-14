// _emitter_engine_diff.mjs — VALIDATE the EMITTER body draw vs the ENGINE ground truth.
//   node _emitter_engine_diff.mjs <probe.log> <file.mcrr> <vframe> [baseURL]
//
// ENGINE GROUND TRUTH = the Oracle probe (PC 0x0C034864 part / 0x0C1248CC submit) for the
// captured frame:  per part -> cid, sid, sel, foot anchor (node+0xE0/E4), node, submit corners.
// Plus the per-OBJECT engine PVR DEPTH z = 1/(node+0xE8) read from the .mcrr RAM image (the
// homogeneous W transform_object_122560 deposits; render_frame z-fix d7a5d88c1). LARGER 1/W
// = NEARER.  The engine's true paint order = back(small 1/W) -> front(large 1/W).
//
// EMITTER = replay.html driven headlessly (?bodymode=emitter), reading window.__lastBodyDrawList
// (SpriteClient.buildEmitterDrawList output: per part {charId, slot, z, engZ, dx,dy,dw,dh, ...}).
//
// REPORTS:
//   1. per-(cid,sid) part COUNT  engine vs emitter (sel coverage / missing poses).
//   2. per-OBJECT (cid,sid) engine z (1/W) vs the emitter's engZ on its parts — MUST match.
//   3. DEPTH ORDER: the engine's object order sorted by 1/W ASCENDING vs the emitter draw
//      list's actual paint order (the order parts appear in __lastBodyDrawList). The emitter
//      is a PAINTER (later = on top), so its list order MUST be the engine's far->near order.
//   4. per-cid screen extent (bbox) engine submit corners vs emitter dx/dy/dw/dh.

import { readFileSync } from 'node:fs';
import puppeteer from 'puppeteer';

const probePath = process.argv[2], mcrrPath = process.argv[3], wantVF = +process.argv[4];
const BASE = process.argv[5] || 'http://127.0.0.1:8099';

// ---------- parse probe ----------
const u16le = (b) => b[0] | (b[1] << 8);
const f32le = (b) => { const a = new Uint8Array(b.slice(0, 4)).buffer; return new DataView(a).getFloat32(0, true); };
function parseProbe(txt) {
  const events = []; let cur = null, key = null;
  for (const ln of txt.split('\n')) {
    let m;
    if ((m = ln.match(/^\[PROBE pc=0x([0-9A-Fa-f]+) (\S+) vframe=(\d+) fire=(\d+)\]/))) {
      cur = { pc: parseInt(m[1], 16) >>> 0, label: m[2], vframe: +m[3], regs: {}, rmem: {} };
      events.push(cur); key = null; continue;
    }
    if (!cur) continue;
    if ((m = ln.match(/^\s*rmem\[r(\d+)\+0x([0-9A-Fa-f]+)=0x([0-9A-Fa-f]+)\.\.\+(\d+)\]:/))) {
      key = `r${m[1]}+0x${m[2]}`; cur.rmem[key] = { base: parseInt(m[3], 16) >>> 0, bytes: [] }; continue;
    }
    if ((m = ln.match(/^\s*([0-9A-Fa-f]{8}):\s*((?:[0-9A-Fa-f]{2}\s*)+)$/)) && key) {
      for (const hx of m[2].trim().split(/\s+/)) cur.rmem[key].bytes.push(parseInt(hx, 16)); continue;
    }
    for (const g of ln.matchAll(/r(\d+)\s*=\s*([0-9A-Fa-f]{8})/g)) cur.regs[+g[1]] = parseInt(g[2], 16) >>> 0;
  }
  return events;
}

// ---------- load .mcrr RAM at vframe (for node+0xE8 depth) ----------
function loadMcrrRam(path, wantVF) {
  const buf = new Uint8Array(readFileSync(path));
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
  if (u32() !== 0x5252434D) throw new Error('bad MCRR');
  u32(); const nStatic = u32(), nDynamic = u32(); u32(); const vramBytes = u32(), pvrBytes = u32(); u32();
  const region = () => { const a = u32(), l = u32(); let t = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) t += String.fromCharCode(c); } p += 8; return { addr: a >>> 0, len: l, tag: t }; };
  const sR = Array.from({ length: nStatic }, region), dR = Array.from({ length: nDynamic }, region);
  p += vramBytes; p += pvrBytes;
  const sD = sR.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
  const G = a => (a >>> 0) & 0xFFFFFF;
  const ram = new Uint8Array(16 * 1024 * 1024);
  sR.forEach((r, i) => { if (r.tag === 'ram16') ram.set(sD[i], 0); else ram.set(sD[i], G(r.addr)); });
  const dynTotal = dR.reduce((s, r) => s + r.len, 0);
  const FRMX = 0x784D5246; const frames = [];
  for (let q = p; q < buf.length - 12;) { if (dv.getUint32(q, true) !== FRMX) { q++; continue; } frames.push({ vframe: dv.getUint32(q + 4, true), dynOff: q + 12 }); q += 12 + dynTotal; }
  const fr = frames.find(f => f.vframe === wantVF) || frames[frames.length - 1];
  { let o = fr.dynOff; for (const r of dR) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }
  return { ram, vframe: fr.vframe };
}
const rf = (ram, a) => { const o = a & 0xFFFFFF; const b = new Uint8Array([ram[o], ram[o + 1], ram[o + 2], ram[o + 3]]).buffer; return new DataView(b).getFloat32(0, true); };

// ---------- engine objects @ vframe ----------
const events = parseProbe(readFileSync(probePath, 'utf8'));
const partVfs = [...new Set(events.filter(e => e.label === 'part').map(e => e.vframe))];
const vf = partVfs.includes(wantVF) ? wantVF : partVfs[partVfs.length - 1];
const { ram } = loadMcrrRam(mcrrPath, vf);

// group parts by node (== object). each object has ONE node+0xE8 -> one engine z.
const engObjs = new Map();  // node -> {cid,sid, node, z, sels:[], parts:n}
let engPartsTotal = 0;
for (const e of events) {
  if (e.vframe !== vf || e.label !== 'part') continue;
  engPartsTotal++;
  const cid = e.rmem['r14+0x1'] ? e.rmem['r14+0x1'].bytes[0] : -1;
  const sid = (e.rmem['r14+0x144'] ? u16le(e.rmem['r14+0x144'].bytes) : -1) & 0x7fff;
  const sel = e.rmem['r11+0x6'] ? u16le(e.rmem['r11+0x6'].bytes) : -1;
  const node = (e.regs[14] >>> 0);
  if (!engObjs.has(node)) {
    const W = rf(ram, node + 0xE8);
    const z = (W > 1e-6 && W === W) ? (1 / W) : null;
    engObjs.set(node, { cid, sid, node, W, z, sels: [], parts: 0 });
  }
  const o = engObjs.get(node); o.parts++; o.sels.push(sel);
}
const engList = [...engObjs.values()];
// engine paint order = ascending z (1/W): far (small) first -> near (large) last.
const engOrder = engList.slice().sort((a, b) => (a.z ?? -Infinity) - (b.z ?? -Infinity));

console.log(`\n=== ENGINE GROUND TRUTH @ vframe ${vf} (${engPartsTotal} parts, ${engList.length} objects) ===`);
for (const o of engOrder)
  console.log(`  cid ${String(o.cid).padStart(2)} sid ${String(o.sid).padStart(4)} node 0x${o.node.toString(16)}  W=${o.W?.toFixed(3)}  z=1/W=${o.z?.toFixed(6)}  parts=${o.parts}  sels=[${o.sels.join(',')}]`);
console.log(`  ENGINE PAINT ORDER (far->near): ${engOrder.map(o => `${o.cid}:${o.sid}`).join('  ->  ')}`);

// ---------- emitter via headless replay.html ----------
const recUrl = `${BASE}/tools/render-replica-poc/${mcrrPath.split(/[\\/]/).pop()}`;
const PAGE = `${BASE}/web/render-replica/replay.html?bodymode=emitter&rec=${encodeURIComponent(recUrl)}`;
const args = ['--enable-unsafe-webgpu', '--enable-webgpu-developer-features', '--ignore-gpu-blocklist', '--no-sandbox',
              '--enable-features=Vulkan', '--use-angle=vulkan', '--use-gl=angle'];
const browser = await puppeteer.launch({ headless: 'new', args });
const page = await browser.newPage();
await page.setViewport({ width: 900, height: 760, deviceScaleFactor: 1 });
const logs = [];
page.on('console', m => logs.push('[page] ' + m.text()));
page.on('pageerror', e => logs.push('[pageerror] ' + e.message));
await page.goto(PAGE, { waitUntil: 'domcontentloaded', timeout: 90000 });
try {
  await page.waitForFunction(() => window.__state && window.__state().nFrames > 0, { timeout: 120000 });
} catch (e) {
  console.log('[harness] nFrames never became >0; page logs:\n' + logs.slice(-40).join('\n'));
  await browser.close(); process.exit(2);
}

const emit = await page.evaluate(async (vf) => {
  // find the frame index whose vframe == vf
  let idx = -1;
  for (let i = 0; i < window.__state().nFrames; i++) { /* REC is module-scoped; use a hook */ }
  idx = window.__frameIndexForVframe ? window.__frameIndexForVframe(vf) : -1;
  if (idx < 0) idx = 0;
  window.__showFrame(idx);
  await new Promise(r => setTimeout(r, 400));   // let lazy atlas fetches land
  window.__showFrame(idx);                      // re-render with atlases resident
  await new Promise(r => setTimeout(r, 200));
  window.__showFrame(idx);
  const dl = window.__lastBodyDrawList || [];
  // group emitter parts by charId, preserving LIST ORDER (= paint order). engZ is per part.
  const order = [];   // first-appearance cid order
  const byCid = {};
  for (const it of dl) {
    if (!byCid[it.charId]) { byCid[it.charId] = { charId: it.charId, parts: 0, engZ: it.engZ, minZ: it.z, maxZ: it.z, bbox: [1e9, 1e9, -1e9, -1e9] }; order.push(it.charId); }
    const g = byCid[it.charId]; g.parts++;
    if (it.engZ != null && (g.engZ == null || it.engZ < g.engZ)) g.engZ = it.engZ;
    g.minZ = Math.min(g.minZ, it.z); g.maxZ = Math.max(g.maxZ, it.z);
    g.bbox[0] = Math.min(g.bbox[0], it.dx); g.bbox[1] = Math.min(g.bbox[1], it.dy);
    g.bbox[2] = Math.max(g.bbox[2], it.dx + it.dw); g.bbox[3] = Math.max(g.bbox[3], it.dy + it.dh);
  }
  const slots = (window._spriteclient ? window._spriteclient.slot : []).map(s => ({ a: s.active, cid: s.char_id, sid: s.sprite_id, engZ: s.engZ }));
  const objects = (window._spriteclient ? window._spriteclient.objects : []).map(o => ({ cid: o.cid, sid: o.sid, engZ: o.engZ }));
  return { n: dl.length, paintOrder: order, byCid, slots, objects, asmNote: window._spriteclient?._asmNote };
}, vf);

console.log(`\n=== EMITTER (buildEmitterDrawList) @ vframe ${vf} ===`);
console.log(`  asmNote: ${emit.asmNote}`);
console.log(`  total parts drawn: ${emit.n}`);
console.log(`  slots: ${JSON.stringify(emit.slots.filter(s => s.a))}`);
console.log(`  objects: ${JSON.stringify(emit.objects)}`);
console.log(`  EMITTER PAINT ORDER (list order, far->near): ${emit.paintOrder.map(c => `cid${c}`).join('  ->  ')}`);
for (const c of emit.paintOrder) {
  const g = emit.byCid[c];
  console.log(`    cid ${String(c).padStart(2)}: parts=${g.parts}  engZ(min 1/W)=${g.engZ != null ? g.engZ.toFixed(6) : 'null'}  bbox=[${g.bbox.map(v => v.toFixed(0)).join(',')}]`);
}

// ---------- DIFF ----------
console.log(`\n=== DIFF ===`);
// 1. engine z vs emitter engZ per cid
let zMatch = true;
const engZByCid = {};
for (const o of engList) { if (!(o.cid in engZByCid) || o.z < engZByCid[o.cid]) engZByCid[o.cid] = o.z; }
for (const cid in engZByCid) {
  const eg = emit.byCid[+cid];
  const ez = engZByCid[cid], gz = eg ? eg.engZ : null;
  const ok = (gz != null && ez != null && Math.abs(gz - ez) < 1e-5);
  if (!ok) zMatch = false;
  console.log(`  cid ${cid}: engine min-z=${ez?.toFixed(6)}  emitter min-engZ=${gz?.toFixed(6)}  ${ok ? 'MATCH' : 'MISMATCH'}`);
}
// 2. paint order: emitter cid order must match engine cid order (far->near, dedup by first-appearance)
const engCidOrder = []; for (const o of engOrder) if (!engCidOrder.includes(o.cid)) engCidOrder.push(o.cid);
const emCidOrder = emit.paintOrder.filter(c => c >= 0);
const orderMatch = JSON.stringify(engCidOrder) === JSON.stringify(emCidOrder.filter(c => engCidOrder.includes(c)));
console.log(`  ENGINE cid paint order: [${engCidOrder.join(',')}]`);
console.log(`  EMITTER cid paint order: [${emCidOrder.join(',')}]`);
console.log(`  depth-order ${orderMatch ? 'MATCH' : 'MISMATCH'}`);
// 3. part coverage per cid (engine parts vs emitter parts)
for (const o of engList) {
  const eg = emit.byCid[o.cid];
  console.log(`  cid ${o.cid} sid ${o.sid}: engine parts=${o.parts}  emitter cid total=${eg ? eg.parts : 0}`);
}

console.log(`\n--- page logs (tail) ---\n${logs.slice(-20).join('\n')}`);
console.log(`\nRESULT: z-values ${zMatch ? 'MATCH' : 'MISMATCH'}; depth-order ${orderMatch ? 'MATCH' : 'MISMATCH'}`);
await browser.close();
process.exit(zMatch && orderMatch ? 0 : 1);
