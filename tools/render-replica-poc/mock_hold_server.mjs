// mock_hold_server.mjs — FROZEN-FRAME variant of mock_replica_live_server.mjs.
// Serves the ZCST static prefix, then repeatedly sends ONE chosen FRMx vframe so the
// live GSTA client renders that exact frame forever -> POLY3D=1 vs POLY3D=0 shots are
// byte-identical game frames (no motion confound).
//   node mock_hold_server.mjs <rec.mcrr> <HOLD_VFRAME> [port] [fps]
// If HOLD_VFRAME==0 or 'list', prints the vframe list and exits.
import { readFileSync } from 'node:fs';
import { zstdCompressSync } from 'node:zlib';
import { WebSocketServer } from 'ws';

const REC_PATH = process.argv[2];
const HOLD = process.argv[3];
const PORT = Number(process.argv[4] || 7212);
const FPS = Number(process.argv[5] || 60);
const FRAME_MS = 1000 / FPS;

const buf = new Uint8Array(readFileSync(REC_PATH));
const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
let p = 0;
const u32 = () => { const v = dv.getUint32(p, true); p += 4; return v >>> 0; };
if (u32() !== 0x5252434D) throw new Error('bad MCRR magic');
const version = u32(); const nStatic = u32(); const nDynamic = u32(); const nFrames = u32();
const vramBytes = u32(); const pvrBytes = u32(); u32();
const region = () => { const addr = u32(); const len = u32(); p += 8; return { addr, len }; };
const staticRegs = Array.from({ length: nStatic }, region);
const dynamicRegs = Array.from({ length: nDynamic }, region);
p += vramBytes; p += pvrBytes; for (const r of staticRegs) p += r.len;
const prefixEnd = p; const prefix = buf.subarray(0, prefixEnd);

const frames = []; const vframes = [];
for (let f = 0; f < nFrames; f++) {
    const fStart = p;
    if (u32() !== 0x784D5246) throw new Error(`frame ${f}: bad FRMx magic at ${fStart}`);
    const vf = u32(); const taSize = u32();
    for (const r of dynamicRegs) p += r.len;
    if (p + 4 <= buf.length) {
        const nGfx = dv.getUint32(p, true);
        if (nGfx <= 64) {
            p += 4;
            for (let g = 0; g < nGfx && p + 8 <= buf.length; g++) { const len = dv.getUint32(p + 4, true); p += 8 + len; }
            if (p + 4 <= buf.length) { const palLen = dv.getUint32(p, true); p += 4; if (palLen === 0) {} else if (palLen <= 0x10000 && p + palLen <= buf.length) p += palLen; else p -= 4; }
            if (p + 8 <= buf.length && dv.getUint32(p, true) === 0x48554451) { p += 4; const nHud = dv.getUint32(p, true); p += 4; p += nHud * 96; }
            if (p + 8 <= buf.length && dv.getUint32(p, true) === 0x57435442) { p += 4; const nw = dv.getUint32(p, true); p += 4; p += nw * 4; }
            if (p + 8 <= buf.length && dv.getUint32(p, true) === 0x44334C50) { p += 4; const nb = dv.getUint32(p, true); p += 4; p += nb; }
        }
    }
    p += taSize;
    frames.push(buf.subarray(fStart, p)); vframes.push(vf);
}

if (!HOLD || HOLD === 'list' || HOLD === '0') {
    console.log(`nFrames=${nFrames} vframe range ${vframes[0]}..${vframes[vframes.length-1]}`);
    console.log('sample:', vframes.filter((_, i) => i % 200 === 0).join(' '));
    process.exit(0);
}
const target = Number(HOLD);
let idx = vframes.indexOf(target);
if (idx < 0) { // nearest
    let best = 0, bd = 1e18; for (let i = 0; i < vframes.length; i++) { const d = Math.abs(vframes[i] - target); if (d < bd) { bd = d; best = i; } } idx = best;
    console.log(`[hold] exact vframe ${target} not found; nearest = ${vframes[idx]} (idx ${idx})`);
}
const holdFrame = frames[idx];
console.log(`[hold] serving prefix + FROZEN vframe ${vframes[idx]} (idx ${idx}) on ws://127.0.0.1:${PORT}`);

const comp = zstdCompressSync(Buffer.from(prefix));
const env = Buffer.alloc(8 + comp.length);
env.writeUInt32LE(0x5453435A, 0); env.writeUInt32LE(prefix.length, 4); comp.copy(env, 8);

const wss = new WebSocketServer({ port: PORT });
wss.on('connection', (ws) => {
    ws.send(env);
    const timer = setInterval(() => {
        if (ws.readyState !== ws.OPEN) { clearInterval(timer); return; }
        if (ws.bufferedAmount > 4 * 1024 * 1024) return;
        ws.send(holdFrame);
    }, FRAME_MS);
    ws.on('close', () => clearInterval(timer));
    ws.on('error', () => clearInterval(timer));
});
wss.on('listening', () => console.log('[hold] listening'));
