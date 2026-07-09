// order_gate.mjs — proves the char-flip ORDER SPLICE offline on a capture:
// reference = TAParser translucent sequence of the FULL engine TA;
// candidate = TAParser of the STRIPPED TA + descriptor splice of body polys
// (exactly the page's _bodyMerge algorithm). PASS = per-position tcw sequences
// match wherever body quads cover the stripped set.
// Usage: node order_gate.mjs [capdir]
import { readFileSync } from 'fs';
import { FrameDecoder } from '../web/webgpu/frame-decoder.mjs';
import { Decompress, decompress } from '../web/webgpu/fzstd.mjs';
import { TAParser } from '../web/webgpu/ta-parser.mjs';
import createRenderFrame from '../web/render-replica/render_frame.mjs';

const dir = process.argv[2] || 'C:/Users/trist/projects/maplecast-flycast/_bwlab/_cap_userplay';
const data = readFileSync(dir + '/cap.bin');
const recs = [];
{
    let p = 0;
    while (p + 13 <= data.length) {
        const len = data.readUInt32LE(p), sock = data[p + 4];
        if (p + 13 + len > data.length) break;
        recs.push({ sock, b: data.subarray(p + 13, p + 13 + len) });
        p += 13 + len;
    }
}
const isM = (b, m) => b.length >= 4 && b[0] === m.charCodeAt(0) && b[1] === m.charCodeAt(1) && b[2] === m.charCodeAt(2) && b[3] === m.charCodeAt(3);
const SB = new Set([0x82, 0x83, 0x88, 0x89]);

// ---- server strip + descriptor, JS twin (block-set predicate, stage TCWs too)
const STAGE_ALLOW = new Set([0x9fc00, 0xa0000]);
function stripAndDescribe(ta, taSize) {
    const out = new Uint8Array(taSize); let w = 0, off = 0;
    let curList = -1, isSpr = false, haveParam = false, dropping = false, cObj = 0;
    let pendCls = -1, pendPushed = true;   // run entry lands at the param's FIRST VERTEX
    const runs = [];   // [cls, count]
    const push = (cls) => { if (runs.length && runs[runs.length - 1][0] === cls) runs[runs.length - 1][1]++; else runs.push([cls, 1]); };
    const dv = new DataView(ta.buffer, ta.byteOffset, taSize);
    const emit = n => { out.set(ta.subarray(off, off + n), w); w += n; };
    while (off + 32 <= taSize) {
        const pcw = dv.getUint32(off, true), pt = (pcw >>> 29) & 7;
        if (pt === 0 || pt === 1 || pt === 2 || pt === 3 || pt === 6) { haveParam = false; dropping = false; if (pt === 0) curList = -1; emit(32); off += 32; continue; }
        if (pt === 4) {
            const lt = (pcw >>> 24) & 7; if (curList === -1) curList = lt;
            if (curList === 1 || curList === 3) { haveParam = false; dropping = false; emit(32); off += 32; continue; }
            cObj = pcw & 0xFF; isSpr = false; haveParam = true;
            const colType = (cObj >> 4) & 3, vol = (cObj >> 6) & 1;
            let sz; if (colType === 2 && !vol && ((cObj >> 2) & 1)) sz = (off + 64 <= taSize) ? 64 : 32;
            else if (colType >= 1 && vol) sz = (off + 64 <= taSize) ? 64 : 32; else sz = 32;
            const tcw = dv.getUint32(off + 12, true);
            dropping = (curList === 0) && STAGE_ALLOW.has(tcw & 0x1FFFFF);
            pendCls = (curList === 2 && !dropping) ? 2 : -1; pendPushed = false;
            if (!dropping) emit(sz);
            off += sz; continue;
        }
        if (pt === 5) {
            const lt = (pcw >>> 24) & 7; if (curList === -1) curList = lt;
            cObj = pcw & 0xFF; isSpr = true; haveParam = true;
            const tcw = dv.getUint32(off + 12, true), addr = tcw & 0x1FFFFF;
            dropping = ((curList === 0) && STAGE_ALLOW.has(addr)) || (curList === 2 && SB.has(addr >>> 12));
            pendCls = (curList === 2) ? (dropping ? 0 : 1) : -1; pendPushed = false;
            if (!dropping) emit(32);
            off += 32; continue;
        }
        if (pt === 7) {
            let sz; if (!haveParam) sz = 32; else if (isSpr && off + 64 <= taSize) sz = 64;
            else { const tex = (cObj >> 3) & 1, colType = (cObj >> 4) & 3, vol = (cObj >> 6) & 1;
                if (!tex) sz = 32; else if (!vol) sz = (colType === 1 && off + 64 <= taSize) ? 64 : 32; else sz = 32; }
            if (haveParam && !pendPushed) { if (pendCls >= 0) push(pendCls); pendPushed = true; }
            if (!dropping) emit(sz);
            off += sz; continue;
        }
        emit(32); off += 32;
    }
    return { stripped: out.subarray(0, w), runs };
}

// ---- pairing anchors (same as garble_diff)
function soaInverse(v) {
    const dv = new DataView(v.buffer, v.byteOffset, v.byteLength);
    const v2Sec = dv.getUint32(76, true), nRuns = dv.getUint32(80, true);
    const offs = 84, lens = offs + nRuns * 4, dat = lens + nRuns * 2;
    let dataB = 0; for (let i = 0; i < nRuns; i++) dataB += dv.getUint16(lens + i * 2, true);
    const tailOff = 80 + v2Sec, legacySec = nRuns * 6 + dataB + 4;
    const out = new Uint8Array(80 + legacySec + (v.length - tailOff));
    out.set(v.subarray(0, 80), 0);
    const ov = new DataView(out.buffer);
    ov.setUint32(76, legacySec, true);
    let o = 80, dp = dat, prev = 0;
    for (let i = 0; i < nRuns; i++) {
        prev += dv.getUint32(offs + i * 4, true);
        const rl = dv.getUint16(lens + i * 2, true);
        ov.setUint32(o, prev, true); ov.setUint16(o + 4, rl, true); o += 6;
        out.set(v.subarray(dp, dp + rl), o); o += rl; dp += rl;
    }
    ov.setUint32(o, 0xFFFFFFFF, true); o += 4;
    out.set(v.subarray(tailOff), o);
    return out;
}
const anchors = [];
{
    let dec = null, chunks = [], clen = 0, epoch = -1, curVf = -1, curSoa = false, innerSize = 0;
    for (const r of recs) {
        if (r.sock !== 0 || !isM(r.b, 'ZCS2') || anchors.length >= 3) continue;
        const d = r.b, flags = d[5];
        const camLen = (flags & 8) ? 132 : 0, vfLen = (flags & 32) ? 4 : 0;
        if (flags & 1) { dec = new Decompress(c => { chunks.push(c); clen += c.length; }); chunks = []; clen = 0; epoch = d[4]; }
        else if (!dec || d[4] !== epoch) { dec = null; continue; }
        innerSize = d.readUInt32LE(6);
        curVf = vfLen ? d.readUInt32LE(10 + camLen) : -1;
        curSoa = (flags & 2) !== 0;
        try { dec.push(d.subarray(10 + camLen + vfLen)); } catch (e) { dec = null; continue; }
        if (clen >= innerSize && curVf >= 0) {
            const inner = new Uint8Array(innerSize); let o = 0;
            for (const c of chunks) { const n = Math.min(c.length, innerSize - o); inner.set(c.subarray(0, n), o); o += n; if (o >= innerSize) break; }
            chunks = []; clen = 0;
            const fin = curSoa ? soaInverse(inner) : inner;
            anchors.push({ vframe: curVf, frameNum: (fin[4] | fin[5] << 8 | fin[6] << 16 | fin[7] << 24) >>> 0 });
        } else { chunks = []; clen = 0; }
    }
}
const K = anchors[0].vframe - anchors[0].frameNum;
console.log('K =', K);

// ---- replica body TA per vframe (block-filtered like the page)
const wasmBinary = readFileSync(new URL('../web/render-replica/render_frame.wasm', import.meta.url));
const M = await createRenderFrame({ wasmBinary });
const guest = a => (a >>> 0) & 0xFFFFFF;
let ram = null, dynRegs = null;
const body = new Map();
for (const r of recs) {
    if (r.sock !== 1) continue;
    let u8 = r.b;
    if (isM(u8, 'ZCST')) u8 = Buffer.from(decompress(u8.subarray(8)));
    if (!ram) {
        if (u8.readUInt32LE(0) !== 0x5252434D) continue;
        let p = 8; const nStatic = u8.readUInt32LE(p); p += 4; const nDynamic = u8.readUInt32LE(p); p += 4; p += 4;
        const vramB = u8.readUInt32LE(p); p += 4; const pvrB = u8.readUInt32LE(p); p += 4; p += 4;
        const staticRegs = Array.from({ length: nStatic }, () => { const addr = u8.readUInt32LE(p), len = u8.readUInt32LE(p + 4); let tag = ''; for (let i = 0; i < 8; i++) { const c = u8[p + 8 + i]; if (c) tag += String.fromCharCode(c); } p += 16; return { addr, len, tag }; });
        dynRegs = Array.from({ length: nDynamic }, () => { const addr = u8.readUInt32LE(p), len = u8.readUInt32LE(p + 4); let tag = ''; for (let i = 0; i < 8; i++) { const c = u8[p + 8 + i]; if (c) tag += String.fromCharCode(c); } p += 16; return { addr, len, tag }; });
        p += vramB + pvrB;
        ram = new Uint8Array(16 * 1024 * 1024);
        for (const r2 of staticRegs) { const bb = u8.subarray(p, p + r2.len); p += r2.len; if (r2.tag === 'ram16') ram.set(bb, 0); else ram.set(bb, guest(r2.addr)); }
        continue;
    }
    if (u8.readUInt32LE(0) !== 0x784D5246) continue;
    const vframe = u8.readUInt32LE(4); let p = 12;
    for (const r2 of dynRegs) { if (r2.tag !== 'bodytex') ram.set(u8.subarray(p, p + r2.len), guest(r2.addr)); p += r2.len; }
    if (p + 4 <= u8.length) { const nGfx = u8.readUInt32LE(p);
        if (nGfx <= 64) { p += 4;
            for (let i = 0; i < nGfx && p + 8 <= u8.length; i++) { const base = u8.readUInt32LE(p), len = u8.readUInt32LE(p + 4); p += 8;
                if (len > 0x800000 || p + len > u8.length) break;
                ram.set(u8.subarray(p, p + len), guest(base)); p += len; } } }
    const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
    const cap = 256 * 1024, outPtr = M._malloc(cap);
    const len = M._render_frame_ta(ramPtr, outPtr, cap);
    const raw = M.HEAPU8.subarray(outPtr, outPtr + len);
    const flt = new Uint8Array(len); let w = 0;
    for (let o = 0; o + 96 <= len; o += 96) {
        const a = ((raw[o + 12] | raw[o + 13] << 8 | raw[o + 14] << 16 | raw[o + 15] << 24) >>> 0) & 0x1FFFFF;
        if (SB.has(a >>> 12)) { flt.set(raw.subarray(o, o + 96), w); w += 96; }
    }
    body.set(vframe, flt.slice(0, w));
    M._free(ramPtr); M._free(outPtr);
}
console.log('body frames:', body.size);

// ---- the gate: reference TR tcw-sequence vs stripped+spliced TR tcw-sequence
const D = new FrameDecoder();
const P1 = new TAParser(), P2 = new TAParser(), P3 = new TAParser();
let gated = 0, orderExact = 0, orderMismatch = 0, spliceSkew = 0, coverShort = 0;
const seqOf = g => (g.translucent || []).map(q => (q.tcw >>> 0) & 0x1FFFFF);
for (const r of recs) {
    if (r.sock !== 0) continue;
    const b = r.b;
    const magic = b.length >= 4 ? b.readUInt32LE(0) : 0;
    if (!isM(b, 'ZCST') && magic !== 0x434E5953 && magic !== 0x4E595346) continue;
    let fr = null;
    try { fr = D.applyFrame(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength)); } catch (e) { continue; }
    if (!fr || !D.prevTASize) continue;
    const ta = D.prevTA.subarray(0, D.prevTASize);
    const ref = P1.parse(ta, D.prevTASize);
    const { stripped, runs } = stripAndDescribe(ta, D.prevTASize);
    const bTa = body.get(D.frameNum + K - 1);
    if (!bTa) continue;
    gated++;
    const kept = P2.parse(stripped, stripped.length).translucent || [];
    const bp = P3.parse(bTa, bTa.length);
    const bodyPolys = bp.translucent || [];
    // page splice algorithm (param-identity)
    let kParams = 0; for (let i = 0; i < kept.length; i++) if (i === 0 || kept[i].param !== kept[i - 1].param) kParams++;
    let kSum = 0; for (const [cls, cnt] of runs) if (cls !== 0) kSum += cnt;
    if (kSum !== kParams) { spliceSkew++;
        if (spliceSkew === 1) {
            console.log('runs:', runs.map(r => 'SKP'[r[0]] + r[1]).join(' '));
            console.log('kSum', kSum, 'kParams', kParams, 'kept entries', kept.length);
            // classify parsed kept params: paraType from pcw
            const seq = [];
            for (let i = 0; i < kept.length; i++) {
                if (i === 0 || kept[i].param !== kept[i - 1].param) {
                    const pt = (kept[i].pcw >>> 29) & 7;
                    seq.push((pt === 5 ? 'k' : 'p') + ((kept[i].tcw >>> 0) & 0x1FFFFF).toString(16));
                }
            }
            console.log('parsed kept params:', seq.join(' '));
        }
        continue; }
    const merged = []; let bi = 0, ki = 0;
    let sTotal = 0; for (const [cls, cnt] of runs) if (cls === 0) sTotal += cnt;
    for (const [cls, cnt] of runs) {
        if (cls === 0) { const take = Math.min(cnt, bodyPolys.length - bi); for (let i = 0; i < take; i++) merged.push(bodyPolys[bi++]); }
        else { let seen = 0, last = -1;
            while (ki < kept.length) { const q = kept[ki];
                if (q.param !== last) { if (seen === cnt) break; seen++; last = q.param; }
                merged.push(q); ki++; } }
    }
    while (ki < kept.length) merged.push(kept[ki++]);
    while (bi < bodyPolys.length) merged.push(bodyPolys[bi++]);
    // compare tcw sequences (only when the body fully covers the stripped set;
    // shortfalls are the known 3% leak, not an ORDER question)
    if (bodyPolys.length !== sTotal) { coverShort++; continue; }
    const a = seqOf(ref), c = merged.map(q => (q.tcw >>> 0) & 0x1FFFFF);
    const same = a.length === c.length && a.every((v, i) => v === c[i]);
    if (same) orderExact++; else {
        orderMismatch++;
        if (orderMismatch <= 3) {
            let fd = -1; for (let i = 0; i < Math.min(a.length, c.length); i++) if (a[i] !== c[i]) { fd = i; break; }
            console.log(`fn${D.frameNum}: ORDER MISMATCH len ${a.length} vs ${c.length}, first at ${fd}: ref ${a[fd] ? a[fd].toString(16) : '-'} vs spliced ${c[fd] ? c[fd].toString(16) : '-'}`);
        }
    }
}
console.log(`\n== ORDER GATE: gated ${gated} | ORDER-EXACT ${orderExact} | mismatch ${orderMismatch} | splice-skew ${spliceSkew} | body-cover-short ${coverShort}`);
console.log(orderExact > 0 && orderMismatch === 0 && spliceSkew === 0 ? 'VERDICT: ORDER PASS' : 'VERDICT: FAIL');
