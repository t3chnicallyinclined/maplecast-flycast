// probe_twiddle.mjs — KEY TEST: is the engine's per-part VRAM a sequence of independently
// twiddled 32x32 tiles, or one big twiddled WxH texture, or linear? Compare a RESIDENT
// Magneto part's decoded bytes vs resident VRAM under each layout hypothesis.
//   node probe_twiddle.mjs <file.mcrr> <frame> <gfx1hex> <sel>
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import createRenderFrame from './render_frame_node.mjs';
import { decodeA } from '../../web/render-replica/body_decoder.mjs';
const args = process.argv.slice(2);
const path = args[0]; const wantF = +(args[1] ?? 0);
const wantGfx = parseInt(args[2] ?? 'c810040', 16) >>> 0;
const wantSel = +(args[3] ?? 285);
const GFX_DIR = fileURLToPath(new URL('../../web/render-replica/gfx/', import.meta.url));
const buf = new Uint8Array(readFileSync(path));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0; const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
u32(); u32(); const nStatic = u32(), nDynamic = u32(), nFrames = u32(), vramBytes = u32(), pvrBytes = u32(); u32();
const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = buf[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr: addr >>> 0, len, tag }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
const vramOff = p; p += vramBytes; const pvrOff = p; p += pvrBytes;
const staticData = staticRegs.map(r => { const b = buf.subarray(p, p + r.len); p += r.len; return b; });
const frameStart = p;
const G = a => (a >>> 0) & 0xFFFFFF;
const ram = new Uint8Array(16 * 1024 * 1024);
staticRegs.forEach((r, i) => { if (r.tag === 'ram16') ram.set(staticData[i], 0); else ram.set(staticData[i], G(r.addr)); });
const vram = new Uint8Array(buf.subarray(vramOff, vramOff + vramBytes));
p = frameStart; const frames = [];
for (let f = 0; f < nFrames; f++) { u32(); const vframe = u32(); const taSize = u32(); const dynOff = p; for (const r of dynamicRegs) p += r.len; const gfxOff = p; const nGfx = (p + 4 <= buf.length) ? dv.getUint32(p, true) : 0; if (nGfx <= 64) { p += 4; for (let g = 0; g < nGfx && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; } } const taOff = p; p += taSize; frames.push({ vframe, taSize, dynOff, gfxOff, taOff }); }
function applyGfxTail(off) { if (off + 4 > buf.length) return; const nGfx = dv.getUint32(off, true); off += 4; if (nGfx > 64) return; for (let i = 0; i < nGfx; i++) { if (off + 8 > buf.length) break; const base = dv.getUint32(off, true); off += 4; const len = dv.getUint32(off, true); off += 4; if (len > 0x800000 || off + len > buf.length) break; ram.set(buf.subarray(off, off + len), G(base)); off += len; } }
const SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
const u8r = a => ram[G(a)]; const u32r = a => (ram[G(a)] | (ram[G(a) + 1] << 8) | (ram[G(a) + 2] << 16) | (ram[G(a) + 3] << 24)) >>> 0;
function applyLocalGfx() { const done = new Set(); for (const base of SLOTS) { if (u8r(base) === 0) continue; const cid = u8r(base + 1); const g1b = u32r(base + 0x15C); if (!((g1b & 0x0C000000) || (g1b & 0x8C000000))) continue; const hexName = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0'); let g1, g2; try { g1 = new Uint8Array(readFileSync(GFX_DIR + hexName + '_gfx1.bin')); g2 = new Uint8Array(readFileSync(GFX_DIR + hexName + '_gfx2.bin')); } catch { continue; } if (done.has(cid)) continue; done.add(cid); ram.set(g1, G(g1b)); ram.set(g2, G(u32r(base + 0x160))); } }
const fr = frames[wantF];
{ let o = fr.dynOff; for (const r of dynamicRegs) { ram.set(buf.subarray(o, o + r.len), G(r.addr)); o += r.len; } }
applyGfxTail(fr.gfxOff); applyLocalGfx();
const SLOTbase = SLOTS.find(b => (u32r(b + 0x15C) & 0xFFFFFF) === (wantGfx & 0xFFFFFF));
const g1abs = u32r(SLOTbase + 0x15C);
const nParts = u32r(g1abs) >>> 2; const offs = []; for (let i = 0; i < nParts; i++) offs.push(u32r(g1abs + i * 4));
const srt = [...new Set(offs)].sort((a, b) => a - b);
function endOf(off) { for (const s of srt) if (s > off) return s; return off + 0x4000; }
const pb = G(g1abs) + offs[wantSel];
const sw = ram[pb + 2], sh = ram[pb + 3]; const W = sw * 8, H = sh * 8, destLen = (W * H) >> 1;
const dec = decodeA(ram, pb + 4, G(g1abs) + endOf(offs[wantSel]), destLen);
console.log(`sel ${wantSel} ${W}x${H} destLen=${destLen}`);

// minVaddr from render_frame
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
const tileV = [];
for (let q = 0; q < quads; q++) { if ((gfxs[q] & 0xFFFFFF) !== (wantGfx & 0xFFFFFF) || sels[q] !== wantSel) continue; const tcw = tdv.getUint32(q * 96 + 0x0C, true); tileV.push(((tcw & 0x1FFFFF) << 3) >>> 0); }
tileV.sort((a, b) => a - b);
const minV = tileV[0];
console.log(`tiles ${tileV.length}, minV=0x${minV.toString(16)}, steps: ${tileV.map(v => '+0x' + (v - minV).toString(16)).join(' ')}`);

// twiddle index for size x size
function tw(x, y, sx, sy) { let r = 0, b = 0; let mx = Math.max(sx, sy); for (let i = 1; i < mx; i <<= 1) { if (x & i) r |= (1 << b); b++; if (y & i) r |= (1 << b); b++; } return r; }
// Build a 32x32 twiddled PAL4 tile (512B) from a linear-raster source sub-rect (tileX,tileY) of dec.
function twiddleTileFromLinear(tileX, tileY) {
    const out = new Uint8Array(512); const rowStrideBytes = W >> 1;
    for (let y = 0; y < 32; y++) for (let x = 0; x < 32; x++) {
        const sx = tileX * 32 + x, sy = tileY * 32 + y;
        let nib = 0;
        if (sx < W && sy < H) { const bo = sy * rowStrideBytes + (sx >> 1); nib = (sx & 1) ? ((dec[bo] >> 4) & 0xF) : (dec[bo] & 0xF); }
        const ti = tw(x, y, 32, 32); const oo = ti >> 1;
        if (ti & 1) out[oo] = (out[oo] & 0x0F) | (nib << 4); else out[oo] = (out[oo] & 0xF0) | nib;
    }
    return out;
}
function matchAt(vaddr, tileBytes) { let m = 0; for (let i = 0; i < 512; i++) if (vram[vaddr + i] === tileBytes[i]) m++; return m / 512; }

// Is the resident VRAM even present here?
let nz = 0; for (let i = 0; i < destLen && minV + i < vram.length; i++) if (vram[minV + i]) nz++;
console.log(`resident VRAM @ minV non-zero/${destLen}: ${nz}`);
if (nz < destLen / 8) { console.log('PART NOT RESIDENT — twiddle test inconclusive for this sel'); process.exit(0); }

const txN = Math.ceil(W / 32), tyN = Math.ceil(H / 32);
console.log(`\nFor each emitted tile vaddr, best-matching twiddled-from-linear sub-rect:`);
for (let i = 0; i < tileV.length; i++) {
    let best = { pct: -1, tx: -1, ty: -1 };
    for (let ty = 0; ty < tyN; ty++) for (let tx = 0; tx < txN; tx++) { const tb = twiddleTileFromLinear(tx, ty); const pct = matchAt(tileV[i], tb); if (pct > best.pct) best = { pct, tx, ty }; }
    console.log(`  vaddr +0x${(tileV[i] - minV).toString(16).padStart(4)} -> linear sub-rect (tx=${best.tx},ty=${best.ty}) twiddled match ${(best.pct * 100).toFixed(1)}%`);
}
