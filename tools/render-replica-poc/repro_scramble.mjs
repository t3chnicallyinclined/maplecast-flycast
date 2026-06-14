// repro_scramble.mjs — REPRODUCE the live scramble OFFLINE, exactly like replay.html.
//   parse MCRR -> seed 16MB RAM -> per frame: apply DYNAMIC regions + GFX tail +
//   applyLocalGfx overlay (loading web/render-replica/gfx/PL{NN}_gfx{1,2}.bin) ->
//   render_frame_ta -> dump per-body quad table (screenX/Y/w/h/sel) -> optionally
//   render the TA to PNG via the SAME pvr2 path body_decoder + render_ta use.
//
//   node repro_scramble.mjs <file.mcrr> <frameIndex> [--png out.png] [--no-overlay]
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
import { ensureBodyTextures } from '../../web/render-replica/body_decoder.mjs';

const args = process.argv.slice(2);
const path = args[0] || '../../_scramble_actual.mcrr';
const wantF = +(args[1] ?? 0);
const pngArg = args.includes('--png') ? args[args.indexOf('--png') + 1] : null;
const noOverlay = args.includes('--no-overlay');
const GFX_DIR = fileURLToPath(new URL('../../web/render-replica/gfx/', import.meta.url));

// ---- MCRR parse (verbatim from replay.html parseMCRR) ----
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR');
const version = u32(), nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr: addr >>> 0, len, tag }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
const vramOff = p; p += vramBytes; const pvrOff = p; p += pvrBytes;
const staticData = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const frameStart = p;

const G = a => (a >>> 0) & 0xFFFFFF;
const RAM = 16 * 1024 * 1024;
const ram = new Uint8Array(RAM);
staticRegs.forEach((r, i) => { if (r.tag === 'ram16') ram.set(staticData[i], 0); else ram.set(staticData[i], G(r.addr)); });

// frame table (replay.html parseMCRR, incl. the LEN-CARRYING GFX tail)
p = frameStart; const frames = [];
for (let f = 0; f < nFrames; f++) {
    const fm = u32(); if (fm !== 0x784D5246) throw new Error(`frame ${f}: bad FRMx`);
    const vframe = u32(); const taSize = u32();
    const dynOff = p; for (const r of dynamicRegs) p += r.len;
    const gfxOff = p;
    const nGfx = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0;
    if (nGfx <= 64) { p += 4; for (let g = 0; g < nGfx && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } }
    const taOff = p; p += taSize;
    frames.push({ vframe, taSize, dynOff, gfxOff, taOff });
}

function applyGfxTail(off) {
    if (off + 4 > buf.length) return off;
    const nGfx = dv.getUint32(off, true); off += 4;
    if (nGfx > 64) return off - 4;
    let n = 0;
    for (let i = 0; i < nGfx; i++) {
        if (off + 8 > buf.length) break;
        const base = dv.getUint32(off, true); off += 4;
        const len = dv.getUint32(off, true); off += 4;
        if (len > 0x800000 || off + len > buf.length) break;
        ram.set(buf.subarray(off, off + len), G(base)); off += len; n++;
    }
    return n;
}

// ---- local-GFX overlay (verbatim port of local_gfx_overlay.mjs applyLocalGfx, but
//      SYNCHRONOUS from disk — same SLOTS, same OFF_GFX1/2, same write addrs) ----
const SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
const u8r = a => ram[G(a)];
const u32r = a => (ram[G(a)] | (ram[G(a) + 1] << 8) | (ram[G(a) + 2] << 16) | (ram[G(a) + 3] << 24)) >>> 0;
const gfxCache = new Map();
function loadGfx(hexName) {
    if (gfxCache.has(hexName)) return gfxCache.get(hexName);
    let e = null;
    try {
        const g1 = new Uint8Array(readFileSync(GFX_DIR + hexName + '_gfx1.bin'));
        const g2 = new Uint8Array(readFileSync(GFX_DIR + hexName + '_gfx2.bin'));
        e = { gfx1: g1, gfx2: g2 };
    } catch { e = 'miss'; }
    gfxCache.set(hexName, e); return e;
}
function applyLocalGfx() {
    const done = new Set(); let overlaid = 0;
    for (const base of SLOTS) {
        if (u8r(base + 0x000) === 0) continue;
        const cid = u8r(base + 0x001);
        const g1b = u32r(base + 0x15C), g2b = u32r(base + 0x160);
        if (!((g1b & 0x0C000000) || (g1b & 0x8C000000))) continue;
        const hexName = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0');
        const ent = loadGfx(hexName);
        if (ent === 'miss' || done.has(cid)) continue; done.add(cid);
        const o1 = G(g1b), o2 = G(g2b);
        if (o1 + ent.gfx1.length <= ram.length) ram.set(ent.gfx1, o1);
        if (o2 + ent.gfx2.length <= ram.length) ram.set(ent.gfx2, o2);
        overlaid++;
        console.log(`  [overlay] ${hexName} slot@${base.toString(16)} cid=${cid} -> GFX1 ${ent.gfx1.length}B@0x${o1.toString(16)}  GFX2 ${ent.gfx2.length}B@0x${o2.toString(16)}`);
    }
    return overlaid;
}

// ---- apply frame ----
const fr = frames[wantF];
{ let o = fr.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }
const nTail = applyGfxTail(fr.gfxOff);
console.log(`file=${path} frame=${wantF} vframe=${fr.vframe} taSize(carried)=${fr.taSize} gfxTailRegions=${nTail}`);
// --stale <slot>: overwrite a tag-in body's seed GFX with a DIFFERENT char's art (PL00) to
// reproduce the prod "frozen previous-char residue" the connect seed serves for a tag-in body.
if (args.includes('--stale')) {
    const slot = parseInt(args[args.indexOf('--stale') + 1], 16);
    const g1b = u32r(slot + 0x15C), g2b = u32r(slot + 0x160), cid = u8r(slot + 1);
    const sg1 = new Uint8Array(readFileSync(GFX_DIR + 'PL00_gfx1.bin'));
    const sg2 = new Uint8Array(readFileSync(GFX_DIR + 'PL00_gfx2.bin'));
    ram.set(sg1.subarray(0, Math.min(sg1.length, ram.length - G(g1b))), G(g1b));
    ram.set(sg2.subarray(0, Math.min(sg2.length, ram.length - G(g2b))), G(g2b));
    console.log(`  [--stale] slot 0x${slot.toString(16)} (cid PL${cid.toString(16).toUpperCase().padStart(2,'0')}) GFX seeded with PL00 residue @0x${G(g1b).toString(16)}/0x${G(g2b).toString(16)}`);
}
if (!noOverlay) { const n = applyLocalGfx(); console.log(`  overlaid ${n} char GFX`); }
else console.log('  [--no-overlay] shipped/static GFX stands');

// ---- run render_frame ----
const M = await createRenderFrame({ locateFile: (x) => x });
const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
const cap = 256 * 1024, outPtr = M._malloc(cap);
const len = M._render_frame_ta(ramPtr, outPtr, cap);
const quads = M._render_frame_quad_count();
const bodies = M._render_frame_body_count();
const ta = M.HEAPU8.slice(outPtr, outPtr + len);
const selPtr = M._malloc(quads * 2 || 2), gfxPtr = M._malloc(quads * 4 || 4), crPtr = M._malloc(quads * 8 || 8);
M._render_frame_quad_sels(selPtr, quads);
M._render_frame_quad_gfx1s(gfxPtr, quads);
M._render_frame_quad_colrow(crPtr, quads);
const sels = new Uint16Array(M.HEAPU8.buffer.slice(selPtr, selPtr + quads * 2));
const gfxs = new Uint32Array(M.HEAPU8.buffer.slice(gfxPtr, gfxPtr + quads * 4));
const colrow = new Int32Array(M.HEAPU8.buffer.slice(crPtr, crPtr + quads * 8));
M._free(selPtr); M._free(gfxPtr); M._free(crPtr); M._free(ramPtr); M._free(outPtr);

// ---- dump per-body quad table ----
// TA layout: 96B textured-sprite param block. Corner A x,y at +32,+36 (float). TCW @ +0x0C.
const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
console.log(`\nrender_frame -> quads=${quads} bodies=${bodies} ta=${len}B`);
const byG = new Map();
for (let i = 0; i < quads; i++) { const g = gfxs[i] >>> 0; if (!byG.has(g)) byG.set(g, []); byG.get(g).push(i); }
for (const [g, idxs] of byG) {
    const uniq = new Set(idxs.map(i => sels[i]));
    const grid = uniq.size <= 2 && idxs.length >= 6;
    console.log(`\nBODY gfx1=0x${g.toString(16)} quads=${idxs.length} distinctSels=${uniq.size} ${grid ? '<<< GRID (one part repeated)' : '(coherent spread)'}`);
    console.log('   q  screenX screenY     w     h   sel       TCW');
    for (let k = 0; k < Math.min(12, idxs.length); k++) {
        const i = idxs[k], o = i * 96;
        const ax = tdv.getFloat32(o + 36, true), ay = tdv.getFloat32(o + 40, true);
        const cx = tdv.getFloat32(o + 60, true), cy = tdv.getFloat32(o + 64, true);
        const tcw = tdv.getUint32(o + 0x0C, true);
        const w = Math.abs(cx - ax), h = Math.abs(cy - ay);
        console.log(`  ${String(k).padStart(2)} ${ax.toFixed(1).padStart(8)} ${ay.toFixed(1).padStart(8)} ${w.toFixed(1).padStart(6)} ${h.toFixed(1).padStart(6)} ${String(sels[i]).padStart(5)}  0x${tcw.toString(16)}`);
    }
}

// ---- optional PNG render via the pvr2 path (same as replay.html / render_ta.mjs) ----
if (pngArg) {
    process.env.__TA = '1';
    await renderPNG(ta, quads, sels, gfxs, pngArg, colrow);
}

async function renderPNG(ta, quads, quadSels, quadGfx1s, outPng, quadColRow) {
    await import('./webgpu-headless.mjs');
    const { initDevice } = await import('./webgpu-headless.mjs');
    const { PNG } = await import('pngjs');
    const W_DIR = new URL('../../web/webgpu/', import.meta.url);
    const { PVR2Renderer } = await import(new URL('pvr2-renderer.mjs', W_DIR));
    const { TAParser } = await import(new URL('ta-parser.mjs', W_DIR));
    const { TextureManager } = await import(new URL('texture-manager.mjs', W_DIR));

    const vram = new Uint8Array(buf.subarray(vramOff, vramOff + vramBytes));
    const pvr = new Uint8Array(buf.subarray(pvrOff, pvrOff + pvrBytes));

    const { device } = await initDevice();
    const R = new PVR2Renderer(); R.dev = device; R.fmt = 'rgba8unorm'; R._init(640, 480);
    const T = new TextureManager(device);
    // PURE-STATE TEXTURES: decode body parts into vram at their TCW, exactly like replay.html.
    const cache = {};
    ensureBodyTextures(ram, vram, ta, quads, cache, quadSels, quadGfx1s, quadColRow);
    T.setDirtyPages(null, true); T.updatePalette(pvr);
    const parsed = new TAParser().parse(ta, ta.length);
    try { parsed && new TAParser().fillBGP && 0; } catch {}
    const color = device.createTexture({ size: [640, 480], format: R.fmt, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.TEXTURE_BINDING });
    const depth = device.createTexture({ size: [640, 480], format: 'depth32float', usage: GPUTextureUsage.RENDER_ATTACHMENT });
    const rt = { color, depth, colorView: color.createView(), depthView: depth.createView(), width: 640, height: 480 };
    const snap = new Uint32Array(16); const tx = (Math.round(640 / 32) - 1) & 0x3F, ty = (Math.round(480 / 32) - 1) & 0x3F; snap[0] = tx | (ty << 16);
    R.renderFrame(parsed, T, snap, vram, {}, rt);
    device.queue.submit([R._lastEncoder.finish()]);
    const bpr = Math.ceil(640 * 4 / 256) * 256;
    const rb = device.createBuffer({ size: bpr * 480, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: color }, { buffer: rb, bytesPerRow: bpr, rowsPerImage: 480 }, [640, 480, 1]);
    device.queue.submit([enc.finish()]);
    await rb.mapAsync(GPUMapMode.READ);
    const mapped = new Uint8Array(rb.getMappedRange()).slice(); rb.unmap();
    const out = new Uint8Array(640 * 480 * 4);
    for (let y = 0; y < 480; y++) for (let x = 0; x < 640; x++) { const s = y * bpr + x * 4, d = (y * 640 + x) * 4; out[d] = mapped[s]; out[d + 1] = mapped[s + 1]; out[d + 2] = mapped[s + 2]; out[d + 3] = mapped[s + 3]; }
    const png = new PNG({ width: 640, height: 480 }); png.data = Buffer.from(out.buffer);
    const { writeFileSync } = await import('node:fs'); writeFileSync(outPng, PNG.sync.write(png));
    let nz = 0; for (let i = 0; i < out.length; i += 4) if (out[i] | out[i + 1] | out[i + 2]) nz++;
    console.log(`\n[png] wrote ${outPng}: ${nz}/${640 * 480} non-black px`);
}
