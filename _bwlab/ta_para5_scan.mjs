// ta_para5_scan.mjs — reconstruct the live TA via FrameDecoder (exact client
// logic) and histogram TR-list para5 sprite quads by TCW addr with Y ranges.
// Purpose: identify the HUD-FONT TCWs (text: combo counters, names, WINNER)
// vs character/effect TCWs for the CHARSTRIP spare-list (render-state/09).
// Usage: node ta_para5_scan.mjs [wss://nobd.net/ws] [seconds] [out.csv]
import WebSocket from 'ws';
import { FrameDecoder } from '../web/webgpu/frame-decoder.mjs';

const url = process.argv[2] || 'wss://nobd.net/ws';
const secs = parseInt(process.argv[3] || '240', 10);
const D = new FrameDecoder();
const hist = new Map();   // tcw -> {quads, hudQuads, yMin, yMax, xMin, xMax, frames:Set}
let frames = 0;

function scanTA(ta, taSize, frameNum) {
    let off = 0, curList = -1, isSpr = false, haveParam = false, inTr = false;
    let cObj = 0, curTcw = 0;
    const dv = new DataView(ta.buffer, ta.byteOffset, taSize);
    while (off + 32 <= taSize) {
        const pcw = dv.getUint32(off, true);
        const pt = (pcw >>> 29) & 7;
        if (pt === 0 || pt === 1 || pt === 2 || pt === 3 || pt === 6) {
            haveParam = false; inTr = false; if (pt === 0) curList = -1;
            off += 32; continue;
        }
        if (pt === 4) {
            const lt = (pcw >>> 24) & 7;
            if (curList === -1) curList = lt;
            if (curList === 1 || curList === 3) { haveParam = false; inTr = false; off += 32; continue; }
            cObj = pcw & 0xFF; isSpr = false; haveParam = true; inTr = false;
            const colType = (cObj >> 4) & 3, vol = (cObj >> 6) & 1;
            let sz;
            if (colType === 2 && !vol && ((cObj >> 2) & 1)) sz = (off + 64 <= taSize) ? 64 : 32;
            else if (colType >= 1 && vol) sz = (off + 64 <= taSize) ? 64 : 32;
            else sz = 32;
            off += sz; continue;
        }
        if (pt === 5) {
            const lt = (pcw >>> 24) & 7;
            if (curList === -1) curList = lt;
            cObj = pcw & 0xFF; isSpr = true; haveParam = true;
            inTr = (curList === 2);
            if (inTr) {
                curTcw = dv.getUint32(off + 12, true) & 0x1FFFFF;
                let s = hist.get(curTcw);
                if (!s) { s = { quads: 0, hudQuads: 0, yMin: 1e9, yMax: -1e9, xMin: 1e9, xMax: -1e9, hSum: 0 }; hist.set(curTcw, s); }
                s.quads++;
            }
            off += 32; continue;
        }
        if (pt === 7) {
            let sz;
            if (!haveParam) sz = 32;
            else if (isSpr && off + 64 <= taSize) sz = 64;
            else {
                const tex = (cObj >> 3) & 1, colType = (cObj >> 4) & 3, vol = (cObj >> 6) & 1;
                if (!tex) sz = 32;
                else if (!vol) sz = (colType === 1 && off + 64 <= taSize) ? 64 : 32;
                else sz = 32;
            }
            if (inTr && isSpr && sz === 64) {
                const s = hist.get(curTcw);
                const ys = [dv.getFloat32(off + 8, true), dv.getFloat32(off + 20, true), dv.getFloat32(off + 32, true), dv.getFloat32(off + 44, true)];
                const xs = [dv.getFloat32(off + 4, true), dv.getFloat32(off + 16, true), dv.getFloat32(off + 28, true), dv.getFloat32(off + 40, true)];
                for (const y of ys) { if (y < s.yMin) s.yMin = y; if (y > s.yMax) s.yMax = y; }
                for (const x of xs) { if (x < s.xMin) s.xMin = x; if (x > s.xMax) s.xMax = x; }
                s.hSum += Math.max(...ys) - Math.min(...ys);   // quad height
            }
            off += sz; continue;
        }
        off += 32;
    }
}

const ws = new WebSocket(url);
ws.on('open', () => console.log(`scanning ${url} for ${secs}s`));
ws.on('error', e => { console.log('WS error', e.message); process.exit(2); });
ws.on('message', d => {
    const b = new Uint8Array(d);
    // only legacy ZCST (self-contained chain) + SYNCs, exactly what FrameDecoder eats
    const magic = b.length >= 4 ? (b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24) >>> 0 : 0;
    const isZCST = b.length >= 8 && b[0] === 0x5A && b[1] === 0x43 && b[2] === 0x53 && b[3] === 0x54;
    const isSync = magic === 0x434E5953 || magic === 0x4E595346;
    if (!isZCST && !isSync) return;
    try {
        const fr = D.applyFrame(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength));
        if (fr && D.prevTASize > 0) { frames++; scanTA(D.prevTA, D.prevTASize, D.frameNum); }
    } catch (e) { /* mid-chain joins settle after first keyframe */ }
});

setTimeout(() => {
    console.log(`\nframes scanned: ${frames}, distinct TR-para5 TCWs: ${hist.size}`);
    const rows = [...hist.entries()].map(([tcw, s]) => ({ tcw, ...s, avgH: s.quads ? s.hSum / s.quads : 0 }));
    // FONT candidates: small quad height (text glyphs ~8-24px) + top band
    rows.sort((a, b) => a.yMin - b.yMin);
    console.log('-- topmost 25 TCWs (yMin asc) — HUD text lives here:');
    for (const r of rows.slice(0, 25))
        console.log(`  tcw=${r.tcw.toString(16).padStart(6, '0')} quads=${r.quads} y=[${r.yMin.toFixed(0)}..${r.yMax.toFixed(0)}] x=[${r.xMin.toFixed(0)}..${r.xMax.toFixed(0)}] avgH=${r.avgH.toFixed(1)}`);
    rows.sort((a, b) => b.quads - a.quads);
    console.log('-- top 10 by quads:');
    for (const r of rows.slice(0, 10))
        console.log(`  tcw=${r.tcw.toString(16).padStart(6, '0')} quads=${r.quads} y=[${r.yMin.toFixed(0)}..${r.yMax.toFixed(0)}] avgH=${r.avgH.toFixed(1)}`);
    // address-space clusters
    const clusters = new Map();
    for (const r of rows) { const k = r.tcw >> 12; clusters.set(k, (clusters.get(k) || 0) + r.quads); }
    console.log('-- TCW addr clusters (addr>>12 : quads):', [...clusters.entries()].sort((a, b) => a[0] - b[0]).map(([k, q]) => `${(k << 12).toString(16)}:${q}`).join(' '));
    process.exit(0);
}, secs * 1000);
