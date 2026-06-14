// validate_tagin_overlay.mjs — prove the tag-in GFX gap and that applyLocalGfx (CURRENT) fixes it.
//
// The real prod bug (re_kb 25 finding:replica_live_stray_field_pin): a body INACTIVE at connect
// (tag-in P1C2, node 0x8C268E88) has its GFX1/GFX2 served ONLY from the frozen ram16 seed. On a
// prod connection that seed region can hold STALE/garbage art -> the walker reads bad cell records
// -> q45..q91 collapse to sel=0 and march LEFT off-screen.
//
// The local captures all happen to carry a WARM seed (engine pre-loads all 6 chars), so to
// reproduce the prod garbage we SCRAMBLE the tag-in body's seed GFX (exactly what validate_gfx_fix
// does for P1C3), then render:
//   [A] no-overlay, stale seed     -> EXPECT garbage (grid: <=2 distinct sels, marching X)
//   [B] CURRENT applyLocalGfx       -> EXPECT coherent (overlay rewrites PL{cid} from disc)
//
//   node validate_tagin_overlay.mjs <file.mcrr> <frameIndex>
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
import { applyLocalGfx, _injectCache } from '../../web/render-replica/local_gfx_overlay.mjs';

const path = process.argv[2] || '../../_satlive.mcrr';
const wantF = +(process.argv[3] ?? 1);
const GFX_DIR = fileURLToPath(new URL('../../web/render-replica/gfx/', import.meta.url));
const TAGIN_SLOT = 0x8C268E88;   // P1C2 node (the tag-in body in side/rot/satlive captures)

// ---- MCRR parse (verbatim) ----
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
u32(); const nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr: addr >>> 0, len, tag }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
p += vramBytes; p += pvrBytes;
const staticData = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const frameStart = p;

const G = a => (a >>> 0) & 0xFFFFFF;
function seedRam() {
    const ram = new Uint8Array(16 * 1024 * 1024);
    staticRegs.forEach((r, i) => { if (r.tag === 'ram16') ram.set(staticData[i], 0); else ram.set(staticData[i], G(r.addr)); });
    return ram;
}
// frame table
p = frameStart; const frames = [];
for (let f = 0; f < nFrames; f++) {
    const fm = u32(); if (fm !== 0x784D5246) throw new Error(`frame ${f} bad FRMx`);
    const vframe = u32(); const taSize = u32();
    const dynOff = p; for (const r of dynamicRegs) p += r.len;
    const gfxOff = p; const nGfx = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0;
    if (nGfx <= 64) { p += 4; for (let g = 0; g < nGfx && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } }
    const taOff = p; p += taSize;
    frames.push({ vframe, taSize, dynOff, gfxOff, taOff });
}
const fr = frames[wantF];
function applyFrame(ram) { let o = fr.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }

const u8r = (ram, a) => ram[G(a)];
const u32r = (ram, a) => (ram[G(a)] | (ram[G(a) + 1] << 8) | (ram[G(a) + 2] << 16) | (ram[G(a) + 3] << 24)) >>> 0;

// pre-warm the overlay cache from disk (the live module fetches over HTTP; node injects).
function warmCache() {
    const SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
    const ram = seedRam(); applyFrame(ram);
    for (const b of SLOTS) {
        if (u8r(ram, b) === 0) continue;
        const cid = u8r(ram, b + 1);
        const hex = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0');
        try {
            _injectCache(cid, new Uint8Array(readFileSync(GFX_DIR + hex + '_gfx1.bin')),
                              new Uint8Array(readFileSync(GFX_DIR + hex + '_gfx2.bin')));
        } catch { /* miss */ }
    }
}
warmCache();

const M = await createRenderFrame({ locateFile: x => x });
function render(ram) {
    const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
    const cap = 256 * 1024, outPtr = M._malloc(cap);
    const len = M._render_frame_ta(ramPtr, outPtr, cap);
    const quads = M._render_frame_quad_count();
    const ta = M.HEAPU8.slice(outPtr, outPtr + len);
    const selPtr = M._malloc(quads * 2 || 2), gfxPtr = M._malloc(quads * 4 || 4);
    M._render_frame_quad_sels(selPtr, quads); M._render_frame_quad_gfx1s(gfxPtr, quads);
    const sels = new Uint16Array(M.HEAPU8.buffer.slice(selPtr, selPtr + quads * 2));
    const gfxs = new Uint32Array(M.HEAPU8.buffer.slice(gfxPtr, gfxPtr + quads * 4));
    M._free(selPtr); M._free(gfxPtr); M._free(ramPtr); M._free(outPtr);
    const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
    return { quads, sels, gfxs, tdv, len };
}
function summarizeBody(r, gfx1Base) {
    const idxs = []; for (let i = 0; i < r.quads; i++) if ((r.gfxs[i] >>> 0) === (gfx1Base >>> 0)) idxs.push(i);
    const uniq = new Set(idxs.map(i => r.sels[i]));
    const xs = idxs.map(i => Math.round(r.tdv.getFloat32(i * 96 + 36, true)));
    const dims = idxs.slice(0, 1).map(i => { const ax = r.tdv.getFloat32(i*96+36,true), ay=r.tdv.getFloat32(i*96+40,true), cx=r.tdv.getFloat32(i*96+60,true), cy=r.tdv.getFloat32(i*96+64,true); return [Math.abs(cx-ax)|0, Math.abs(cy-ay)|0]; })[0] || [0,0];
    const grid = uniq.size <= 2 && idxs.length >= 6;
    const offscreen = xs.filter(x => x < 0).length;
    return { n: idxs.length, distinctSels: uniq.size, grid, sampleSels: [...uniq].slice(0, 10),
             xspan: idxs.length ? `${Math.min(...xs)}..${Math.max(...xs)}` : '-', offscreen, dims, xs: xs.slice(0,9) };
}

// the tag-in body's gfx1 base (live)
const ramProbe = seedRam(); applyFrame(ramProbe);
const taginActive = u8r(ramProbe, TAGIN_SLOT) !== 0;
const taginCid = u8r(ramProbe, TAGIN_SLOT + 1);
const taginG1 = u32r(ramProbe, TAGIN_SLOT + 0x15C);
const taginG2 = u32r(ramProbe, TAGIN_SLOT + 0x160);
console.log(`tag-in slot 0x${TAGIN_SLOT.toString(16)} active=${taginActive} cid=PL${taginCid.toString(16).toUpperCase().padStart(2,'0')} gfx1=0x${taginG1.toString(16)} gfx2=0x${taginG2.toString(16)}`);

// helper: stale the tag-in body's GFX1+GFX2 in the SEED with a DIFFERENT char's real art
// (the prod "frozen previous-char residue" — a structurally-decodable but WRONG cell stream,
// which is what drives the marching grid, not random noise). PL00 Ryu = a smaller char.
const STALE_G1 = new Uint8Array(readFileSync(GFX_DIR + 'PL00_gfx1.bin'));
const STALE_G2 = new Uint8Array(readFileSync(GFX_DIR + 'PL00_gfx2.bin'));
function staleTagin(ram) {
    const g1 = G(taginG1), g2 = G(taginG2);
    // write PL00's GFX at PL34's bases (mismatched extents => the walker reads PL00 cell records
    // through PL34's offsets = the frozen-wrong-art tile march).
    ram.set(STALE_G1.subarray(0, Math.min(STALE_G1.length, ram.length - g1)), g1);
    ram.set(STALE_G2.subarray(0, Math.min(STALE_G2.length, ram.length - g2)), g2);
}

console.log(`\n=== [A] STALE seed, NO overlay (reproduce prod garbage) ===`);
{ const ram = seedRam(); applyFrame(ram); staleTagin(ram); const r = render(ram);
  const b = summarizeBody(r, taginG1);
  console.log(`  P1C2 gfx1=0x${taginG1.toString(16)}: quads=${b.n} distinctSels=${b.distinctSels} ${b.grid?'GRID':'coherent'} dims=${b.dims} xspan=${b.xspan} offscreenX=${b.offscreen}`);
  console.log(`  sample sels=${b.sampleSels}  q-screenX(first9)=${b.xs}`);
  globalThis.__A = b;
}

console.log(`\n=== [B] STALE seed, CURRENT applyLocalGfx overlay ===`);
{ const ram = seedRam(); applyFrame(ram); staleTagin(ram);
  const res = applyLocalGfx(ram, (m)=>console.log('   '+m));
  console.log(`  overlay result:`, JSON.stringify(res));
  const r = render(ram);
  const b = summarizeBody(r, taginG1);
  console.log(`  P1C2 gfx1=0x${taginG1.toString(16)}: quads=${b.n} distinctSels=${b.distinctSels} ${b.grid?'GRID':'coherent'} dims=${b.dims} xspan=${b.xspan} offscreenX=${b.offscreen}`);
  console.log(`  sample sels=${b.sampleSels}  q-screenX(first9)=${b.xs}`);
  globalThis.__B = b;
}

// [C] WARM baseline (no stale, no overlay) = ground truth for the tag-in body.
console.log(`\n=== [C] WARM seed, no overlay (ground truth) ===`);
let C;
{ const ram = seedRam(); applyFrame(ram); const r = render(ram); C = summarizeBody(r, taginG1);
  console.log(`  P1C2 gfx1=0x${taginG1.toString(16)}: quads=${C.n} distinctSels=${C.distinctSels} xspan=${C.xspan}  sels=${C.sampleSels}`); }

const A = globalThis.__A, B = globalThis.__B;
const selsEq = JSON.stringify([...B.sampleSels].sort()) === JSON.stringify([...C.sampleSels].sort());
const staleDiffers = JSON.stringify([...A.sampleSels].sort()) !== JSON.stringify([...C.sampleSels].sort()) || A.xspan !== C.xspan;
console.log(`\n=== RESULT ===`);
console.log(`  [A] STALE no-overlay  : sels=${A.sampleSels} xspan=${A.xspan}  (WRONG char's parts — garbage)`);
console.log(`  [B] STALE +overlay    : sels=${B.sampleSels} xspan=${B.xspan}  (overlay-recovered)`);
console.log(`  [C] WARM ground-truth : sels=${C.sampleSels} xspan=${C.xspan}`);
// GATE: overlay-on-stale [B] must EQUAL the warm ground truth [C] (same sels, xspan, quad count),
// and the stale-no-overlay [A] must DIFFER from it (proves the bug was real). The ground truth may
// itself have off-screen quads (a body at the screen edge) — that is correct game state, not garbage.
const fixed = staleDiffers && selsEq && B.n === C.n && B.xspan === C.xspan && B.offscreen === C.offscreen;
console.log(fixed ? '  PASS: stale tag-in renders the WRONG art; applyLocalGfx restores it to the WARM ground truth (sels+xspan+quadCount match). Tag-in body COVERED.'
                  : '  FAIL: overlay output does not match the warm ground truth.');
