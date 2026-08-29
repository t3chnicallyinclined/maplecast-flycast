// render_strategy.mjs — render Sentinel under a chosen texture-layout STRATEGY, to find
// which one makes the multi-column parts coherent. Ground truth = the rendered PNG.
//   node render_strategy.mjs <file.mcrr> <frame> <strategy> <out.png>
//   strategy: A=current contiguous blob; B=per-screen-tile twiddled 32x32 at each quad TCW
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
import { decodeA } from '../../web/render-replica/body_decoder.mjs';
const args = process.argv.slice(2);
const path = args[0]; const wantF = +(args[1] ?? 0); const strat = (args[2] || 'A').toUpperCase(); const outPng = args[3] || `_strat_${strat}.png`;
const GFX_DIR = fileURLToPath(new URL('../../web/render-replica/gfx/', import.meta.url));
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); u32(); const nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr: addr >>> 0, len, tag }; };
const staticRegs = Array.from({ length: nStatic }, region); const dynamicRegs = Array.from({ length: nDynamic }, region);
const vramOff = p; p += vramBytes; const pvrOff = p; p += pvrBytes;
const staticData = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const frameStart = p; const G = a => (a >>> 0) & 0xFFFFFF;
const ram = new Uint8Array(16 * 1024 * 1024);
staticRegs.forEach((r, i) => { if (r.tag === 'ram16') ram.set(staticData[i], 0); else ram.set(staticData[i], G(r.addr)); });
const vram = process.env.FRESH ? new Uint8Array(vramBytes) : new Uint8Array(buf.subarray(vramOff, vramOff + vramBytes)).slice();
const pvr = new Uint8Array(buf.subarray(pvrOff, pvrOff + pvrBytes));
p = frameStart; const frames = [];
for (let f = 0; f < nFrames; f++) { u32(); const vframe = u32(); const taSize = u32(); const dynOff = p; for (const r of dynamicRegs) p += r.len; const gfxOff = p; const nGfx = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0; if (nGfx <= 64) { p += 4; for (let g = 0; g < nGfx && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } } const taOff = p; p += taSize; frames.push({ vframe, taSize, dynOff, gfxOff, taOff }); }
function applyGfxTail(off) { if (off + 4 > buf.length) return; const nGfx = dv.getUint32(off, true); off += 4; if (nGfx > 64) return; for (let i = 0; i < nGfx; i++) { if (off + 8 > buf.length) break; const base = dv.getUint32(off, true); off += 4; const len = dv.getUint32(off, true); off += 4; if (len > 0x800000 || off + len > buf.length) break; ram.set(buf.subarray(off, off + len), G(base)); off += len; } }
const SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
const u8r = a => ram[G(a)]; const u32r = a => (ram[G(a)] | (ram[G(a) + 1] << 8) | (ram[G(a) + 2] << 16) | (ram[G(a) + 3] << 24)) >>> 0;
function applyLocalGfx() { const done = new Set(); for (const base of SLOTS) { if (u8r(base) === 0) continue; const cid = u8r(base + 1); const g1b = u32r(base + 0x15C); if (!((g1b & 0x0C000000) || (g1b & 0x8C000000))) continue; const hexName = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0'); let g1, g2; try { g1 = new Uint8Array(readFileSync(GFX_DIR + hexName + '_gfx1.bin')); g2 = new Uint8Array(readFileSync(GFX_DIR + hexName + '_gfx2.bin')); } catch { continue; } if (done.has(cid)) continue; done.add(cid); ram.set(g1, G(g1b)); ram.set(g2, G(u32r(base + 0x160))); } }
const fr = frames[wantF];
{ let o = fr.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }
applyGfxTail(fr.gfxOff); applyLocalGfx();

const M = await createRenderFrame({ locateFile: (x) => x });
const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
const outPtr = M._malloc(256 * 1024);
const len = M._render_frame_ta(ramPtr, outPtr, 256 * 1024);
const quads = M._render_frame_quad_count();
const ta = M.HEAPU8.slice(outPtr, outPtr + len);
const selPtr = M._malloc(quads * 2), gfxPtr = M._malloc(quads * 4);
M._render_frame_quad_sels(selPtr, quads); M._render_frame_quad_gfx1s(gfxPtr, quads);
const sels = new Uint16Array(M.HEAPU8.buffer.slice(selPtr, selPtr + quads * 2));
const gfxs = new Uint32Array(M.HEAPU8.buffer.slice(gfxPtr, gfxPtr + quads * 4));
const tdv = new DataView(ta.buffer, ta.byteOffset, ta.byteLength);
const QUAD = 96;

// ---- build texture VRAM per chosen strategy ----
// decode memo
const decCache = new Map();
const offCache = new Map();
function gfx1Offs(gfx1) { let e = offCache.get(gfx1); if (e) return e; const n = u32r(gfx1) >>> 2; const offs = []; for (let i = 0; i < n; i++) offs.push(u32r(gfx1 + i * 4)); const srt = [...new Set(offs)].sort((a, b) => a - b); e = { n, offs, srt }; offCache.set(gfx1, e); return e; }
function endOf(srt, off) { for (const s of srt) if (s > off) return s; return off + 0x4000; }
function decode(gfx1, sel) { const key = gfx1 + ':' + sel; let p = decCache.get(key); if (p !== undefined) return p; const Gt = gfx1Offs(gfx1); if (sel >= Gt.n) { decCache.set(key, null); return null; } const pb = G(gfx1) + Gt.offs[sel]; const sw = ram[pb + 2], sh = ram[pb + 3]; const W = sw * 8, H = sh * 8; if (W <= 0 || H <= 0 || W > 1024 || H > 1024) { decCache.set(key, null); return null; } const destLen = (W * H) >> 1; const bytes = decodeA(ram, pb + 4, G(gfx1) + endOf(Gt.srt, Gt.offs[sel]), destLen); p = { bytes, W, H, destLen }; decCache.set(key, p); return p; }
function tw32(x, y) { let r = 0, b = 0; for (let i = 0; i < 5; i++) { r |= ((x >> i) & 1) << b; b++; r |= ((y >> i) & 1) << b; b++; } return r; }

function strategyA() {
    // contiguous blob at min TCW per (gfx1,sel)  (current code)
    const runs = new Map();
    for (let q = 0; q < quads; q++) { const gfx1 = gfxs[q] >>> 0; if (!(gfx1 & 0x0C000000) && !(gfx1 & 0x8C000000)) continue; const tcw = tdv.getUint32(q * QUAD + 0x0C, true); const addr = ((tcw & 0x1FFFFF) << 3) >>> 0; const key = gfx1 + ':' + sels[q]; const r = runs.get(key); if (!r) runs.set(key, { gfx1, sel: sels[q], base: addr }); else if (addr < r.base) r.base = addr; }
    for (const { gfx1, sel, base } of runs.values()) { const d = decode(gfx1, sel); if (!d) continue; if (base + d.destLen <= vram.length) vram.set(d.bytes, base); }
}
function strategyB() {
    // per-quad: carve linear-raster decode into the screen-grid 32x32 tile, twiddle it, write
    // at this quad's OWN TCW. Screen grid derived per (gfx1,sel) run from XY.
    // group quads by run
    const runs = new Map();
    for (let q = 0; q < quads; q++) { const gfx1 = gfxs[q] >>> 0; if (!(gfx1 & 0x0C000000) && !(gfx1 & 0x8C000000)) continue; const o = q * QUAD; const tcw = tdv.getUint32(o + 0x0C, true); const addr = ((tcw & 0x1FFFFF) << 3) >>> 0; const ax = tdv.getFloat32(o + 36, true), ay = tdv.getFloat32(o + 40, true); const key = gfx1 + ':' + sels[q]; if (!runs.has(key)) runs.set(key, { gfx1, sel: sels[q], tiles: [] }); runs.get(key).tiles.push({ addr, ax, ay }); }
    for (const { gfx1, sel, tiles } of runs.values()) {
        const d = decode(gfx1, sel); if (!d) continue;
        const W = d.W, H = d.H, rowStride = W >> 1;
        // screen grid: cols by distinct X (descending = left-facing col0..), rows by Y descending (bottom row0)
        const xs = [...new Set(tiles.map(t => Math.round(t.ax)))].sort((a, b) => b - a);
        const ys = [...new Set(tiles.map(t => Math.round(t.ay)))].sort((a, b) => b - a);
        for (const t of tiles) {
            const col = xs.indexOf(Math.round(t.ax)), row = ys.indexOf(Math.round(t.ay));
            // linear sub-rect (col*32..,row*32..) -> twiddled 32x32 at t.addr
            const tile = new Uint8Array(512);
            for (let y = 0; y < 32; y++) for (let x = 0; x < 32; x++) {
                const sx = col * 32 + x, sy = row * 32 + y; let nib = 0;
                if (sx < W && sy < H) { const bo = sy * rowStride + (sx >> 1); nib = (sx & 1) ? ((d.bytes[bo] >> 4) & 0xF) : (d.bytes[bo] & 0xF); }
                const ti = tw32(x, y), oo = ti >> 1; if (ti & 1) tile[oo] = (tile[oo] & 0x0F) | (nib << 4); else tile[oo] = (tile[oo] & 0xF0) | nib;
            }
            if (t.addr + 512 <= vram.length) vram.set(tile, t.addr);
        }
    }
}
function twWH(x, y, W, H) { const bx = Math.log2(W), by = Math.log2(H), sq = Math.min(bx, by); let r = 0, b = 0; for (let i = 0; i < sq; i++) { r |= ((x >> i) & 1) << b; b++; r |= ((y >> i) & 1) << b; b++; } if (bx > by) r |= (x >> sq) << b; else if (by > bx) r |= (y >> sq) << b; return r; }
function strategyC() {
    // treat decodeA as LINEAR raster; twiddle the WHOLE WxH part; write contiguous at min TCW.
    const runs = new Map();
    for (let q = 0; q < quads; q++) { const gfx1 = gfxs[q] >>> 0; if (!(gfx1 & 0x0C000000) && !(gfx1 & 0x8C000000)) continue; const tcw = tdv.getUint32(q * QUAD + 0x0C, true); const addr = ((tcw & 0x1FFFFF) << 3) >>> 0; const key = gfx1 + ':' + sels[q]; const r = runs.get(key); if (!r) runs.set(key, { gfx1, sel: sels[q], base: addr }); else if (addr < r.base) r.base = addr; }
    for (const { gfx1, sel, base } of runs.values()) {
        const d = decode(gfx1, sel); if (!d) continue; const W = d.W, H = d.H, rowStride = W >> 1;
        const tw = new Uint8Array(d.destLen);
        for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { const bo = y * rowStride + (x >> 1); const nib = (x & 1) ? ((d.bytes[bo] >> 4) & 0xF) : (d.bytes[bo] & 0xF); const ti = twWH(x, y, W, H), oo = ti >> 1; if (ti & 1) tw[oo] = (tw[oo] & 0x0F) | (nib << 4); else tw[oo] = (tw[oo] & 0xF0) | nib; }
        if (base + tw.length <= vram.length) vram.set(tw, base);
    }
}
// Strategy D: decodeA is a sequence of 512B twiddled 32x32 tiles in SOME order; the contiguous
// VRAM blob expects them in render_frame's +0x200 emission order. Remap decodeA tile index ->
// emission tile index via a chosen grid-order pair (srcOrder -> dstOrder).
// We parameterize by env REMAP: 'transpose' swaps (col,row) within the whole-part twiddle grid.
function gridOrders(W, H) {
    // dst (emission) grid order = render_frame's: derived empirically below from quad XY.
    // src (decodeA) grid order = the engine's whole-part TILE-twiddle order over the txN x tyN grid.
    const txN = Math.ceil(W / 32), tyN = Math.ceil(H / 32);
    // whole-part TILE twiddle: order tiles by twiddling the tile-grid coords
    const order = [];
    function twg(tx, ty) { const bx = Math.log2(Math.max(txN,1)), by = Math.log2(Math.max(tyN,1)), sq = Math.min(bx, by); let r = 0, b = 0; for (let i = 0; i < sq; i++) { r |= ((tx >> i) & 1) << b; b++; r |= ((ty >> i) & 1) << b; b++; } if (bx > by) r |= (tx >> sq) << b; else if (by > bx) r |= (ty >> sq) << b; return r; }
    const cells = []; for (let ty = 0; ty < tyN; ty++) for (let tx = 0; tx < txN; tx++) cells.push({ tx, ty, k: twg(tx, ty) });
    cells.sort((a, b) => a.k - b.k); return { cells, txN, tyN };
}
function strategyD() {
    const remap = process.env.REMAP || 'tw'; // 'tw' = src order is whole-tile-twiddle; 'twT' transposed
    const runs = new Map();
    for (let q = 0; q < quads; q++) { const gfx1 = gfxs[q] >>> 0; if (!(gfx1 & 0x0C000000) && !(gfx1 & 0x8C000000)) continue; const o = q * QUAD; const tcw = tdv.getUint32(o + 0x0C, true); const addr = ((tcw & 0x1FFFFF) << 3) >>> 0; const ax = tdv.getFloat32(o + 36, true), ay = tdv.getFloat32(o + 40, true); const key = gfx1 + ':' + sels[q]; if (!runs.has(key)) runs.set(key, { gfx1, sel: sels[q], tiles: [] }); runs.get(key).tiles.push({ addr, ax, ay }); }
    for (const { gfx1, sel, tiles } of runs.values()) {
        const d = decode(gfx1, sel); if (!d) continue; const W = d.W, H = d.H;
        const { cells, txN, tyN } = gridOrders(W, H);
        // emission order: tiles sorted by vaddr -> their screen (col,row)
        tiles.sort((a, b) => a.addr - b.addr);
        const xs = [...new Set(tiles.map(t => Math.round(t.ax)))].sort((a, b) => b - a);
        const ys = [...new Set(tiles.map(t => Math.round(t.ay)))].sort((a, b) => b - a);
        // build map: screen (col,row) -> decodeA tile-slice index (position in `cells`)
        // src order: decodeA stores tiles in `cells` order (whole-tile-twiddle) over grid (tx,ty).
        // For 'tw', screen col==tx, row==ty. For 'twT', screen col==ty, row==tx (transpose).
        const sliceOf = new Map();
        cells.forEach((c, i) => {
            const col = remap === 'twT' ? c.ty : c.tx;
            const row = remap === 'twT' ? c.tx : c.ty;
            sliceOf.set(col + ',' + row, i);
        });
        for (const t of tiles) {
            const col = xs.indexOf(Math.round(t.ax)), row = ys.indexOf(Math.round(t.ay));
            const si = sliceOf.get(col + ',' + row); if (si === undefined) continue;
            const slice = d.bytes.subarray(si * 512, si * 512 + 512);
            if (t.addr + 512 <= vram.length) vram.set(slice, t.addr);
        }
    }
}
// Strategy E: decodeA stores tiles in ORDER `ord` over the part's (txN x tyN) tile grid.
// Write each 512B decodeA slice to the EMISSION vaddr of its screen-grid cell. ord in:
//   rowmaj (ty outer, tx inner), colmaj (tx outer, ty inner), tw (tile-twiddle), +T transpose
function strategyE() {
    const ORD = process.env.ORD || 'rowmaj';
    const runs = new Map();
    for (let q = 0; q < quads; q++) { const gfx1 = gfxs[q] >>> 0; if (!(gfx1 & 0x0C000000) && !(gfx1 & 0x8C000000)) continue; const o = q * QUAD; const tcw = tdv.getUint32(o + 0x0C, true); const addr = ((tcw & 0x1FFFFF) << 3) >>> 0; const ax = tdv.getFloat32(o + 36, true), ay = tdv.getFloat32(o + 40, true); const key = gfx1 + ':' + sels[q]; if (!runs.has(key)) runs.set(key, { gfx1, sel: sels[q], tiles: [] }); runs.get(key).tiles.push({ addr, ax, ay }); }
    for (const { gfx1, sel, tiles } of runs.values()) {
        const d = decode(gfx1, sel); if (!d) continue; const W = d.W, H = d.H;
        const txN = Math.max(1, Math.ceil(W / 32)), tyN = Math.max(1, Math.ceil(H / 32));
        // decodeA slice order -> (tx,ty)
        const slice2cell = [];
        function twg(tx, ty) { const bx = Math.log2(txN), by = Math.log2(tyN), sq = Math.min(bx, by); let r = 0, b = 0; for (let i = 0; i < sq; i++) { r |= ((tx >> i) & 1) << b; b++; r |= ((ty >> i) & 1) << b; b++; } if (bx > by) r |= (tx >> sq) << b; else if (by > bx) r |= (ty >> sq) << b; return r; }
        if (ORD.startsWith('rowmaj')) { for (let ty = 0; ty < tyN; ty++) for (let tx = 0; tx < txN; tx++) slice2cell.push([tx, ty]); }
        else if (ORD.startsWith('colmaj')) { for (let tx = 0; tx < txN; tx++) for (let ty = 0; ty < tyN; ty++) slice2cell.push([tx, ty]); }
        else { const cells = []; for (let ty = 0; ty < tyN; ty++) for (let tx = 0; tx < txN; tx++) cells.push({ tx, ty, k: twg(tx, ty) }); cells.sort((a, b) => a.k - b.k); for (const c of cells) slice2cell.push([c.tx, c.ty]); }
        const transpose = ORD.endsWith('T');
        // screen grid from XY
        const xs = [...new Set(tiles.map(t => Math.round(t.ax)))].sort((a, b) => b - a);
        const ys = [...new Set(tiles.map(t => Math.round(t.ay)))].sort((a, b) => b - a);
        const cell2addr = new Map();
        for (const t of tiles) { const col = xs.indexOf(Math.round(t.ax)), row = ys.indexOf(Math.round(t.ay)); cell2addr.set(col + ',' + row, t.addr); }
        slice2cell.forEach(([tx, ty], i) => {
            const col = transpose ? ty : tx, row = transpose ? tx : ty;
            const addr = cell2addr.get(col + ',' + row); if (addr === undefined) return;
            const slice = d.bytes.subarray(i * 512, i * 512 + 512);
            if (addr + 512 <= vram.length) vram.set(slice, addr);
        });
    }
}
// Strategy F: decodeA IS a full WxH twiddle (PROVEN _sel124_fulltw.png coherent). Each emitted
// quad k occupies a screen grid cell (col,row); the byte chunk for that cell in the full-WxH
// twiddle is chunk C = the twiddle index of the tile-grid coord. Re-slice decodeA's full-twiddle
// into per-cell 512B chunks and write each at the quad's OWN TCW. (This makes each tile's vaddr
// hold the 32x32-local-twiddle the renderer wants, sourced from the right grid cell.)
// EMPIRICAL: which 512B chunk of a full-WxH pixel-twiddle holds tile-grid cell (tx,ty)?
function twPix(x, y, W, H) { const bx = Math.log2(W), by = Math.log2(H), sq = Math.min(bx, by); let r = 0, b = 0; for (let i = 0; i < sq; i++) { r |= ((x >> i) & 1) << b; b++; r |= ((y >> i) & 1) << b; b++; } if (bx > by) r |= (x >> sq) << b; else if (by > bx) r |= (y >> sq) << b; return r; }
const _chunkCache = new Map();
function cellToChunkMap(W, H) {
    const k = W + 'x' + H; let m = _chunkCache.get(k); if (m) return m;
    // chunk = twPix(tile-origin)/1024 ; tile (tx,ty) origin pixel (tx*32,ty*32)
    m = new Map(); const txN = W >> 5, tyN = H >> 5;
    for (let ty = 0; ty < tyN; ty++) for (let tx = 0; tx < txN; tx++) { const chunk = twPix(tx * 32, ty * 32, W, H) >> 10; m.set(tx + ',' + ty, chunk); }
    _chunkCache.set(k, m); return m;
}
function fullTwChunkOf(tx, ty, W, H) { return cellToChunkMap(W, H).get(tx + ',' + ty); }
function strategyF() {
    const runs = new Map();
    for (let q = 0; q < quads; q++) { const gfx1 = gfxs[q] >>> 0; if (!(gfx1 & 0x0C000000) && !(gfx1 & 0x8C000000)) continue; const o = q * QUAD; const tcw = tdv.getUint32(o + 0x0C, true); const addr = ((tcw & 0x1FFFFF) << 3) >>> 0; const ax = tdv.getFloat32(o + 36, true), ay = tdv.getFloat32(o + 40, true); const key = gfx1 + ':' + sels[q]; if (!runs.has(key)) runs.set(key, { gfx1, sel: sels[q], tiles: [] }); runs.get(key).tiles.push({ addr, ax, ay }); }
    for (const { gfx1, sel, tiles } of runs.values()) {
        const d = decode(gfx1, sel); if (!d) continue; const W = d.W, H = d.H;
        if (W <= 32 || H <= 32) { // single column/row: contiguous blob is already correct
            tiles.sort((a, b) => a.addr - b.addr); const base = tiles[0].addr; if (base + d.destLen <= vram.length) vram.set(d.bytes, base); continue;
        }
        const xs = [...new Set(tiles.map(t => Math.round(t.ax)))].sort((a, b) => b - a);
        const ys = [...new Set(tiles.map(t => Math.round(t.ay)))].sort((a, b) => b - a);
        for (const t of tiles) {
            const col = xs.indexOf(Math.round(t.ax)), row = ys.indexOf(Math.round(t.ay));
            const chunk = fullTwChunkOf(col, row, W, H);
            const slice = d.bytes.subarray(chunk * 512, chunk * 512 + 512);
            if (t.addr + 512 <= vram.length) vram.set(slice, t.addr);
        }
    }
}
function strategyG() { // PAINT TEST: fill ONLY sel124 (the big 128x128 part) tiles solid bright
    strategyA();
    const want = +(process.env.PSEL || 124);
    for (let q = 0; q < quads; q++) { if ((gfxs[q] & 0xFFFFFF) !== 0x420040 || sels[q] !== want) continue; const tcw = tdv.getUint32(q * QUAD + 0x0C, true); const addr = ((tcw & 0x1FFFFF) << 3) >>> 0; for (let i = 0; i < 512 && addr + i < vram.length; i++) vram[addr + i] = 0x44; }
}
if (strat === 'A') strategyA(); else if (strat === 'B') strategyB(); else if (strat === 'C') strategyC(); else if (strat === 'D') strategyD(); else if (strat === 'E') strategyE(); else if (strat === 'F') strategyF(); else if (strat === 'G') strategyG();
console.log(`strategy ${strat}: built texture VRAM`);

// ---- render via pvr2 ----
process.env.__TA = '1';
await import('./webgpu-headless.mjs');
const { initDevice } = await import('./webgpu-headless.mjs');
const { PNG } = await import('pngjs');
const W_DIR = new URL('../../web/webgpu/', import.meta.url);
const { PVR2Renderer } = await import(new URL('pvr2-renderer.mjs', W_DIR));
const { TAParser } = await import(new URL('ta-parser.mjs', W_DIR));
const { TextureManager } = await import(new URL('texture-manager.mjs', W_DIR));
const { device } = await initDevice();
const R = new PVR2Renderer(); R.dev = device; R.fmt = 'rgba8unorm'; R._init(640, 480);
const T = new TextureManager(device);
T.setDirtyPages(null, true); T.updatePalette(pvr);
const parsed = new TAParser().parse(ta, ta.length);
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
for (let y = 0; y < 480; y++) for (let x = 0; x < 640; x++) { const s = y * bpr + x * 4, dd = (y * 640 + x) * 4; out[dd] = mapped[s]; out[dd + 1] = mapped[s + 1]; out[dd + 2] = mapped[s + 2]; out[dd + 3] = mapped[s + 3]; }
const png = new PNG({ width: 640, height: 480 }); png.data = Buffer.from(out.buffer); writeFileSync(outPng, PNG.sync.write(png));
let nz = 0; for (let i = 0; i < out.length; i += 4) if (out[i] | out[i + 1] | out[i + 2]) nz++;
console.log(`[png] wrote ${outPng}: ${nz}/${640 * 480} nonblack`);
