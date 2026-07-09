// body_feed_smoke.mjs — node harness for the webgpu-test Phase B body pipeline:
// replica-live seed -> FRMx apply -> render_frame.wasm -> TAParser. Verifies the
// exact code path the page runs, headlessly, against the local rig (7212).
// Usage: node body_feed_smoke.mjs [ws://127.0.0.1:7212] [seconds]
import WebSocket from 'ws';
import { readFileSync } from 'fs';
import { decompress } from '../web/webgpu/fzstd.mjs';
import { TAParser } from '../web/webgpu/ta-parser.mjs';
import createRenderFrame from '../web/render-replica/render_frame.mjs';

const url = process.argv[2] || 'ws://127.0.0.1:7212';
const secs = parseInt(process.argv[3] || '15', 10);
const guest = a => (a >>> 0) & 0xFFFFFF;

const wasmBinary = readFileSync(new URL('../web/render-replica/render_frame.wasm', import.meta.url));
const M = await createRenderFrame({ wasmBinary });
console.log('render_frame.wasm instantiated');

let ram = null, dynRegs = null, frames = 0, quadsTot = 0, taLast = null, vframeLast = -1;
const ws = new WebSocket(url);
ws.binaryType = 'arraybuffer';
ws.on('open', () => console.log('replica feed connected', url));
ws.on('error', e => { console.log('WS error', e.message); process.exit(2); });
let seeded = false;
ws.on('message', d => {
    let u8 = new Uint8Array(d);
    try {
        if (!seeded) {
            if (u8.length >= 8 && u8[0] === 0x5A && u8[1] === 0x43 && u8[2] === 0x53 && u8[3] === 0x54) u8 = decompress(u8.subarray(8));
            const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength); let p = 0;
            const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
            if (u32() !== 0x5252434D) throw new Error('bad MCRR magic');
            u32(); const nStatic = u32(), nDynamic = u32(); u32(); const vramB = u32(), pvrB = u32(); u32();
            const region = () => { const addr = u32(), len = u32(); let tag = ''; for (let i = 0; i < 8; i++) { const c = u8[p + i]; if (c) tag += String.fromCharCode(c); } p += 8; return { addr, len, tag }; };
            const staticRegs = Array.from({ length: nStatic }, region);
            dynRegs = Array.from({ length: nDynamic }, region);
            p += vramB + pvrB;
            ram = new Uint8Array(16 * 1024 * 1024);
            for (const r of staticRegs) { const b = u8.subarray(p, p + r.len); p += r.len; if (r.tag === 'ram16') ram.set(b, 0); else ram.set(b, guest(r.addr)); }
            seeded = true;
            console.log(`seeded: ${nStatic} static + ${nDynamic} dynamic regions (prefix ${u8.length} B)`);
            return;
        }
        if (u8.length >= 8 && u8[0] === 0x5A && u8[1] === 0x43 && u8[2] === 0x53 && u8[3] === 0x54) u8 = decompress(u8.subarray(8));   // FRMx may be ZCST-wrapped too
        const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
        if (u8.length < 12 || dv.getUint32(0, true) !== 0x784D5246) return;
        const vframe = dv.getUint32(4, true); let p = 12;
        for (const r of dynRegs) { if (r.tag !== 'bodytex') ram.set(u8.subarray(p, p + r.len), guest(r.addr)); p += r.len; }
        if (p + 4 <= u8.length) { const nGfx = dv.getUint32(p, true);
            if (nGfx <= 64) { p += 4;
                for (let i = 0; i < nGfx && p + 8 <= u8.length; i++) { const base = dv.getUint32(p, true), len = dv.getUint32(p + 4, true); p += 8;
                    if (len > 0x800000 || p + len > u8.length) break;
                    ram.set(u8.subarray(p, p + len), guest(base)); p += len; } } }
        const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
        const cap = 256 * 1024, outPtr = M._malloc(cap);
        const len = M._render_frame_ta(ramPtr, outPtr, cap);
        taLast = M.HEAPU8.slice(outPtr, outPtr + len);
        M._free(ramPtr); M._free(outPtr);
        frames++; vframeLast = vframe; quadsTot += len / 96;
    } catch (e) { console.log('frame err:', e.message); }
});

setTimeout(() => {
    console.log(`\nframes: ${frames}, avg quads/frame: ${(quadsTot / Math.max(1, frames)).toFixed(1)}, last vframe ${vframeLast}`);
    if (!taLast || !taLast.length) { console.log('FAIL: no body TA emitted'); process.exit(1); }
    const P = new TAParser();
    const g = P.parse(taLast, taLast.length);
    console.log(`TAParser on last body TA: vertexCount=${g.vertexCount} translucent polys=${(g.translucent || []).length} opaque=${(g.opaque || []).length}`);
    // sanity: TCWs of parsed body polys should sit in the CHARSTRIP range
    let inRange = 0, total = 0;
    for (const q of g.translucent || []) { total++; if (((q.tcw >>> 0) & 0x1FFFFF) >= 0x082000 && ((q.tcw >>> 0) & 0x1FFFFF) < 0x08B000) inRange++; }
    console.log(`body TCWs in strip range: ${inRange}/${total}`);
    const pass = frames > 30 && g.vertexCount > 0 && (g.translucent || []).length > 0 && inRange === total && total > 0;
    console.log(pass ? 'SMOKE PASS' : 'SMOKE FAIL');
    process.exit(pass ? 0 : 1);
}, secs * 1000);
