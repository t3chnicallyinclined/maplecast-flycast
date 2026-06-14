// _emitter_geom_depth_validate.mjs — NODE-ONLY emitter vs engine validation (no browser).
//   node _emitter_geom_depth_validate.mjs <probe.log> <file.mcrr> <vframe>
//
// Runs the REAL SpriteClient.buildEmitterDrawList against the captured frame's RAM image,
// stubbing each char's atlas from the on-disk PLxx_asm.json (GEOMETRY is real; pixels not
// needed for the per-part screen-rect + depth-order validation). Feeds the engine's per-object
// PVR depth (node+0xE8 -> 1/W) exactly as replay.html does. Compares the emitter draw list
// (per part: charId, engZ, dx/dy/dw/dh, PAINT ORDER) against the Oracle ground truth.

import { readFileSync } from 'node:fs';
import { SpriteClient } from '../../web/webgpu/sprite-client.mjs';

const probePath = process.argv[2], mcrrPath = process.argv[3], wantVF = +process.argv[4];

// minimal window/performance shim so the emitter's window._* knobs read as defaults
globalThis.window = globalThis.window || {};
globalThis.performance = globalThis.performance || { now: () => 0 };

const u16le = (b) => b[0] | (b[1] << 8);
const f32 = (ram, a) => { const o = a & 0xFFFFFF; return new DataView(new Uint8Array([ram[o], ram[o + 1], ram[o + 2], ram[o + 3]]).buffer).getFloat32(0, true); };
const u32 = (ram, a) => { const o = a & 0xFFFFFF; return (ram[o] | (ram[o + 1] << 8) | (ram[o + 2] << 16) | (ram[o + 3] << 24)) >>> 0; };
const u8 = (ram, a) => ram[a & 0xFFFFFF];
const u16 = (ram, a) => { const o = a & 0xFFFFFF; return ram[o] | (ram[o + 1] << 8); };

// ---------- parse probe ----------
function parseProbe(txt) {
  const events = []; let cur = null, key = null;
  for (const ln of txt.split('\n')) {
    let m;
    if ((m = ln.match(/^\[PROBE pc=0x([0-9A-Fa-f]+) (\S+) vframe=(\d+) fire=(\d+)\]/))) {
      cur = { label: m[2], vframe: +m[3], regs: {}, rmem: {} }; events.push(cur); key = null; continue;
    }
    if (!cur) continue;
    if ((m = ln.match(/^\s*rmem\[r(\d+)\+0x([0-9A-Fa-f]+)=0x([0-9A-Fa-f]+)\.\.\+(\d+)\]:/))) { key = `r${m[1]}+0x${m[2]}`; cur.rmem[key] = { bytes: [] }; continue; }
    if ((m = ln.match(/^\s*([0-9A-Fa-f]{8}):\s*((?:[0-9A-Fa-f]{2}\s*)+)$/)) && key) { for (const hx of m[2].trim().split(/\s+/)) cur.rmem[key].bytes.push(parseInt(hx, 16)); continue; }
    for (const g of ln.matchAll(/r(\d+)\s*=\s*([0-9A-Fa-f]{8})/g)) cur.regs[+g[1]] = parseInt(g[2], 16) >>> 0;
  }
  return events;
}

// ---------- load .mcrr RAM (FRMx-scan, tolerant) ----------
function loadMcrrRam(path, wantVF) {
  const buf = new Uint8Array(readFileSync(path));
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  let p = 0; const rd = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
  if (rd() !== 0x5252434D) throw new Error('bad MCRR');
  rd(); const nStatic = rd(), nDynamic = rd(); rd(); const vramBytes = rd(), pvrBytes = rd(); rd();
  const region = () => { const a = rd(), l = rd(); let t = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) t += String.fromCharCode(c); } p += 8; return { addr: a >>> 0, len: l, tag: t }; };
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

// ---------- engine objects ----------
const events = parseProbe(readFileSync(probePath, 'utf8'));
const partVfs = [...new Set(events.filter(e => e.label === 'part').map(e => e.vframe))];
const vf = partVfs.includes(wantVF) ? wantVF : partVfs[partVfs.length - 1];
const { ram } = loadMcrrRam(mcrrPath, vf);

const engObjs = new Map();
for (const e of events) {
  if (e.vframe !== vf || e.label !== 'part') continue;
  const cid = e.rmem['r14+0x1'] ? e.rmem['r14+0x1'].bytes[0] : -1;
  const sid = (e.rmem['r14+0x144'] ? u16le(e.rmem['r14+0x144'].bytes) : -1) & 0x7fff;
  const node = e.regs[14] >>> 0;
  if (!engObjs.has(node)) { const W = f32(ram, node + 0xE8); engObjs.set(node, { cid, sid, node, W, z: (W > 1e-6 && W === W) ? 1 / W : null, parts: 0 }); }
  engObjs.get(node).parts++;
}
const engOrder = [...engObjs.values()].sort((a, b) => (a.z ?? -Infinity) - (b.z ?? -Infinity));
console.log(`\n=== ENGINE @ vframe ${vf}: ${engObjs.size} objects ===`);
for (const o of engOrder) console.log(`  cid ${String(o.cid).padStart(2)} sid ${String(o.sid).padStart(4)} W=${o.W.toFixed(3)} z=${o.z.toFixed(6)} parts=${o.parts}`);
const engCidOrder = []; for (const o of engOrder) if (!engCidOrder.includes(o.cid)) engCidOrder.push(o.cid);
console.log(`  ENGINE cid paint order (far->near): [${engCidOrder.join(',')}]`);

// ---------- build the SpriteClient exactly like replay.html, with stubbed atlases ----------
const sc = new SpriteClient();
sc.assemblyMode = true; sc.predict = false; sc.objectsOn = true;
sc.screenW = 640; sc.screenH = 480;

// stub each cid's atlas from the on-disk PLxx_asm.json (geometry real). img is a fake bitmap.
const CHARDIR = new URL('../../web/test-atlas/chars/', import.meta.url);
function stubAtlas(cid) {
  if (sc.asmChars[cid]) return;
  const hex = (cid & 0xff).toString(16).padStart(2, '0').toUpperCase();
  let raw;
  try { raw = JSON.parse(readFileSync(new URL(`PL${hex}_asm.json`, CHARDIR), 'utf8')); }
  catch { sc.asmChars[cid] = { img: null, parts: {}, asm: {}, missing: true }; return; }
  let partsJson = {};
  try { partsJson = JSON.parse(readFileSync(new URL(`PL${hex}_parts.json`, CHARDIR), 'utf8')); } catch {}
  const asm = raw.assemblies || raw.asm || raw;
  const parts = raw.parts || partsJson.parts || partsJson;
  sc.asmChars[cid] = { img: { width: 2048, height: 2048 }, parts, asm,
    palette: raw.palette || null, pal128: raw.pal128 || null,
    screenSpace: !!raw.screenSpace, selKeyed: !!raw.selKeyed, name: 'PL' + hex };
}
sc.loadAsmChar = (cid) => { stubAtlas(cid); };   // synchronous stub (no fetch)

// populate slots + objects from RAM (mirrors replay.html populateBodiesFromRAM/Objects)
const CHAR_SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
sc.inMatch = u8(ram, 0x8C289624);
for (let s = 0; s < 6; s++) {
  const base = CHAR_SLOTS[s], sl = sc.slot[s];
  sl.active = u8(ram, base + 0x000); sl.char_id = u8(ram, base + 0x001); sl.facing = u8(ram, base + 0x110);
  sl.palette = u8(ram, base + 0x52D); sl.pos_x = f32(ram, base + 0x034);
  sl.screen_x = f32(ram, base + 0x0E0); sl.screen_y = f32(ram, base + 0x0E4);
  const W = f32(ram, base + 0x0E8); sl.engZ = (W > 1e-6 && W === W) ? 1 / W : null;
  const rawSid = u16(ram, base + 0x144); sl.sprite_id = rawSid & 0x7fff; sl.sid_xform = (rawSid & 0x8000) ? 1 : 0;
  sl.scaleX = f32(ram, base + 0x050); sl.scaleY = f32(ram, base + 0x054);
  sl.pal12d = u8(ram, base + 0x12D); sl.pal12e = u8(ram, base + 0x12E); sl.overlay1a4 = u8(ram, base + 0x1A4);
  sl.draw_layer = 0xFF; sl.t = 0; sl.vx = 0; sl.vy = 0;
  if (sl.active || sc.inMatch) stubAtlas(sl.char_id);
}
// objects from the slot-table walk
const SLOT_COUNT_BASE = 0x8C2895E0, SLOT_PTR_BASE = 0x8C287DE0, SLOT_ROW_STRIDE = 0x180;
const objs = [];
for (let layer = 0; layer < 16; layer++) {
  const count = u8(ram, SLOT_COUNT_BASE + layer);
  if (count <= 0 || count > 0x60) continue;
  const row = SLOT_PTR_BASE + layer * SLOT_ROW_STRIDE;
  for (let i = 0; i < count; i++) {
    const node = u32(ram, row + i * 4) >>> 0;
    if (node < 0x8C000000 || node >= 0x8D000000) continue;
    let isBody = false; for (let s = 0; s < 6; s++) if (node === (CHAR_SLOTS[s] >>> 0)) isBody = true;
    if (isBody) continue;
    if (u8(ram, node + 0x12C) === 0) continue;
    const sid = u16(ram, node + 0x144); if (sid === 0) continue;
    const sx = f32(ram, node + 0xE0), sy = f32(ram, node + 0xE4);
    if (sx < -64 || sx > 704 || sy < -64 || sy > 544) continue;
    let slot = -1; const oA = u32(ram, node + 0x18) >>> 0, oB = u32(ram, node + 0x80) >>> 0;
    for (let s = 0; s < 6; s++) if (oA === (CHAR_SLOTS[s] >>> 0) || oB === (CHAR_SLOTS[s] >>> 0)) { slot = s; break; }
    const cid = slot >= 0 ? u8(ram, CHAR_SLOTS[slot] + 0x001) : 0;
    const gfxBase = u32(ram, node + 0x15C) >>> 0, gfxLow = gfxBase & 0x0FFFFFFF;
    const isEffect = (gfxLow >= 0x0CED0000 && gfxLow < 0x0CEE0000) ? 1 : 0;
    const ow = f32(ram, node + 0xE8); const oEngZ = (ow > 1e-6 && ow === ow) ? 1 / ow : null;
    objs.push({ cid, sid: sid & 0x7fff, type: layer, x: Math.round(sx), y: Math.round(sy), engZ: oEngZ, isEffect, blend: null });
    if (!isEffect) stubAtlas(cid);
  }
}
sc.objects = objs;

const dl = sc.buildEmitterDrawList(640, 480);

// emitter paint order (list order = paint order; dedup cids by first appearance)
const emCidOrder = []; const byCid = {};
for (const it of dl) {
  if (!byCid[it.charId]) { byCid[it.charId] = { parts: 0, engZ: it.engZ, bbox: [1e9, 1e9, -1e9, -1e9] }; emCidOrder.push(it.charId); }
  const g = byCid[it.charId]; g.parts++;
  if (it.engZ != null && (g.engZ == null || it.engZ < g.engZ)) g.engZ = it.engZ;
  g.bbox[0] = Math.min(g.bbox[0], it.dx); g.bbox[1] = Math.min(g.bbox[1], it.dy);
  g.bbox[2] = Math.max(g.bbox[2], it.dx + it.dw); g.bbox[3] = Math.max(g.bbox[3], it.dy + it.dh);
}
console.log(`\n=== EMITTER: ${dl.length} parts drawn === note: ${sc._asmNote}`);
console.log(`  active slots: ${sc.slot.filter(s => s.active).map(s => `cid${s.char_id}/sid${s.sprite_id}/engZ${s.engZ?.toFixed(6)}`).join('  ')}`);
console.log(`  objects: ${objs.map(o => `cid${o.cid}/sid${o.sid}/engZ${o.engZ?.toFixed(6)}`).join('  ') || '(none)'}`);
for (const c of emCidOrder) { const g = byCid[c]; console.log(`  cid ${String(c).padStart(2)}: parts=${g.parts} engZ(min)=${g.engZ?.toFixed(6)} bbox=[${g.bbox.map(v => v.toFixed(0)).join(',')}]`); }
console.log(`  EMITTER cid paint order: [${emCidOrder.join(',')}]`);

// ---------- DIFF ----------
console.log(`\n=== DIFF ===`);
let zMatch = true;
const engZByCid = {}; for (const o of engObjs.values()) if (!(o.cid in engZByCid) || o.z < engZByCid[o.cid]) engZByCid[o.cid] = o.z;
for (const cid in engZByCid) {
  const g = byCid[+cid]; const ez = engZByCid[cid], gz = g ? g.engZ : null;
  const ok = (gz != null && ez != null && Math.abs(gz - ez) < 1e-5);
  if (!ok) zMatch = false;
  console.log(`  cid ${cid}: engine min-z=${ez?.toFixed(6)} emitter min-engZ=${gz != null ? gz.toFixed(6) : 'null'} parts(eng/emit)=${[...engObjs.values()].filter(o => o.cid == cid).reduce((s, o) => s + o.parts, 0)}/${g ? g.parts : 0}  ${ok ? 'Z-MATCH' : 'Z-MISMATCH'}`);
}
const emFiltered = emCidOrder.filter(c => engCidOrder.includes(c));
const orderMatch = JSON.stringify(engCidOrder) === JSON.stringify(emFiltered);
console.log(`  ENGINE order [${engCidOrder.join(',')}] vs EMITTER order [${emFiltered.join(',')}]: ${orderMatch ? 'ORDER-MATCH' : 'ORDER-MISMATCH'}`);
console.log(`\nRESULT: ${zMatch && orderMatch ? 'PASS' : 'FAIL'} (z ${zMatch ? 'ok' : 'BAD'}, depth-order ${orderMatch ? 'ok' : 'BAD'})`);
process.exit(zMatch && orderMatch ? 0 : 1);
