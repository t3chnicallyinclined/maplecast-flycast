// garble_diff.mjs — frame-by-frame char-flip garble hunt on a cap_userplay capture.
// GROUND TRUTH: the legacy ZCST chain (unstripped — real engine char quads).
// CANDIDATE:    render_frame body TA rebuilt from the captured FRMx stream.
// Pairing:      ZCS2 bit5 vframe stamps anchor frameNum <-> vframe (constant K).
// Compare:      in-range TR-para5 96B blocks, TACANON-normalized, per frame.
// Usage: node garble_diff.mjs [capdir]
import { readFileSync } from 'fs';
import { FrameDecoder } from '../web/webgpu/frame-decoder.mjs';
import { Decompress, decompress } from '../web/webgpu/fzstd.mjs';
import createRenderFrame from '../web/render-replica/render_frame.mjs';

const dir = process.argv[2] || 'C:/Users/trist/projects/maplecast-flycast/_bwlab/_cap_userplay';
const data = readFileSync(dir + '/cap.bin');

// ---- parse capture records
const recs = [];
{
    let p = 0;
    while (p + 13 <= data.length) {
        const len = data.readUInt32LE(p), sock = data[p + 4];
        if (p + 13 + len > data.length) break;
        recs.push({ sock, t: data.readDoubleLE(p + 5), b: data.subarray(p + 13, p + 13 + len) });
        p += 13 + len;
    }
    console.log(`records: ${recs.length}`);
}
const isMagic = (b, m) => b.length >= 4 && b[0] === m.charCodeAt(0) && b[1] === m.charCodeAt(1) && b[2] === m.charCodeAt(2) && b[3] === m.charCodeAt(3);

// ---- TACANON sprite rules on a 96B-block stream (param 24..32, vert 48..52 zeroed)
function canonBlocks(ta) {
    const out = Buffer.from(ta);
    for (let o = 0; o + 96 <= out.length; o += 96) { out.fill(0, o + 24, o + 32); out.fill(0, o + 32 + 48, o + 32 + 52); }
    return out;
}

// ---- extract in-range TR-para5 96B units from a full engine TA (same FSM as strip)
const CS_LO = 0x082000, CS_HI = 0x08B000;
function extractCharBlocks(ta, taSize) {
    const blocks = [];
    let off = 0, curList = -1, isSpr = false, haveParam = false, take = false, pstart = 0;
    const dv = new DataView(ta.buffer, ta.byteOffset, taSize);
    while (off + 32 <= taSize) {
        const pcw = dv.getUint32(off, true), pt = (pcw >>> 29) & 7;
        if (pt === 0 || pt === 1 || pt === 2 || pt === 3 || pt === 6) { haveParam = false; take = false; if (pt === 0) curList = -1; off += 32; continue; }
        if (pt === 4) {
            const lt = (pcw >>> 24) & 7; if (curList === -1) curList = lt;
            if (curList === 1 || curList === 3) { haveParam = false; take = false; off += 32; continue; }
            const cObj = pcw & 0xFF; isSpr = false; haveParam = true; take = false;
            const colType = (cObj >> 4) & 3, vol = (cObj >> 6) & 1;
            let sz; if (colType === 2 && !vol && ((cObj >> 2) & 1)) sz = (off + 64 <= taSize) ? 64 : 32;
            else if (colType >= 1 && vol) sz = (off + 64 <= taSize) ? 64 : 32; else sz = 32;
            off += sz; continue;
        }
        if (pt === 5) {
            const lt = (pcw >>> 24) & 7; if (curList === -1) curList = lt;
            isSpr = true; haveParam = true;
            const addr = dv.getUint32(off + 12, true) & 0x1FFFFF;
            take = (curList === 2 && addr >= CS_LO && addr < CS_HI);
            pstart = off; off += 32; continue;
        }
        if (pt === 7) {
            let sz;
            if (!haveParam) sz = 32;
            else if (isSpr && off + 64 <= taSize) sz = 64;
            else sz = 32;
            if (take && isSpr && sz === 64) blocks.push(Buffer.from(ta.subarray(pstart, pstart + 32 + 64)));
            take = false;   // one vertex block per sprite param
            off += sz; continue;
        }
        off += 32;
    }
    return blocks;
}

// ---- 1) legacy chain -> engine char blocks per frameNum
const D = new FrameDecoder();
const engine = new Map();   // frameNum -> Buffer[] blocks
let legacyFrames = 0;
for (const r of recs) {
    if (r.sock !== 0) continue;
    const b = r.b;
    const magic = b.length >= 4 ? b.readUInt32LE(0) : 0;
    const zcst = isMagic(b, 'ZCST'), sync = magic === 0x434E5953 || magic === 0x4E595346;
    if (!zcst && !sync) continue;
    try {
        const fr = D.applyFrame(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength));
        if (fr && D.prevTASize > 0) {
            const blocks = extractCharBlocks(D.prevTA, D.prevTASize);
            if (blocks.length) engine.set(D.frameNum, blocks.map(x => canonBlocks(x)));
            legacyFrames++;
        }
    } catch (e) { }
}
console.log(`legacy frames decoded: ${legacyFrames}, in-match frames w/ char quads: ${engine.size}`);

// ---- 2) ZCS2 anchors: (vframe from header) <-> (frameNum from inner)
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
    let dec = null, chunks = [], clen = 0, innerSize = 0, curVf = -1, curSoa = false, epoch = -1;
    for (const r of recs) {
        if (r.sock !== 0 || !isMagic(r.b, 'ZCS2') || anchors.length >= 6) continue;
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
            const frameNum = fin[4] | fin[5] << 8 | fin[6] << 16 | fin[7] << 24;
            anchors.push({ vframe: curVf, frameNum: frameNum >>> 0 });
        } else { chunks = []; clen = 0; }
    }
}
const Ks = anchors.map(a => a.vframe - a.frameNum);
console.log('anchors:', anchors.map(a => `vf${a.vframe}=fn${a.frameNum}`).join(' '), '-> K:', [...new Set(Ks)].join(','));
if (!Ks.length) { console.log('NO vframe anchors (bit5 frames absent — was the user in a match?)'); process.exit(1); }
const K = Ks[0];
const VOFF = parseInt(process.argv[3] || '0', 10);   // pairing offset under test

// ---- 3) replica -> body TA per vframe
const wasmBinary = readFileSync(new URL('../web/render-replica/render_frame.wasm', import.meta.url));
const M = await createRenderFrame({ wasmBinary });
const guest = a => (a >>> 0) & 0xFFFFFF;
let ram = null, dynRegs = null;
const body = new Map();   // vframe -> Buffer (canon body TA)
for (const r of recs) {
    if (r.sock !== 1) continue;
    let u8 = r.b;
    if (isMagic(u8, 'ZCST')) u8 = Buffer.from(decompress(u8.subarray(8)));
    if (!ram) {
        // MCRR prefix
        if (u8.readUInt32LE(0) !== 0x5252434D) continue;
        let p = 8; const nStatic = u8.readUInt32LE(p); p += 4; const nDynamic = u8.readUInt32LE(p); p += 4; p += 4;
        const vramB = u8.readUInt32LE(p); p += 4; const pvrB = u8.readUInt32LE(p); p += 4; p += 4;
        const region = () => { const addr = u8.readUInt32LE(p), len = u8.readUInt32LE(p + 4); p += 16; return { addr, len }; };
        const rgn = [];
        const staticRegs = Array.from({ length: nStatic }, () => { const addr = u8.readUInt32LE(p), len = u8.readUInt32LE(p + 4); let tag = ''; for (let i = 0; i < 8; i++) { const c = u8[p + 8 + i]; if (c) tag += String.fromCharCode(c); } p += 16; return { addr, len, tag }; });
        dynRegs = Array.from({ length: nDynamic }, () => { const addr = u8.readUInt32LE(p), len = u8.readUInt32LE(p + 4); let tag = ''; for (let i = 0; i < 8; i++) { const c = u8[p + 8 + i]; if (c) tag += String.fromCharCode(c); } p += 16; return { addr, len, tag }; });
        p += vramB + pvrB;
        ram = new Uint8Array(16 * 1024 * 1024);
        for (const r2 of staticRegs) { const bb = u8.subarray(p, p + r2.len); p += r2.len; if (r2.tag === 'ram16') ram.set(bb, 0); else ram.set(bb, guest(r2.addr)); }
        console.log(`replica seeded (${nStatic} static, ${nDynamic} dynamic)`);
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
    body.set(vframe, canonBlocks(Buffer.from(M.HEAPU8.slice(outPtr, outPtr + len))));
    M._free(ramPtr); M._free(outPtr);
}
console.log(`body frames: ${body.size}`);

// ---- 4) per-block content-multiset match, both pairing offsets
for (const voff of [0, -1]) {
    const blk = new Map();   // blockKey -> {match, onlyE, onlyB}
    let paired = 0, exactFrames = 0;
    for (const [fn, eBlocks] of engine) {
        const bTa = body.get(fn + K + voff);
        if (!bTa) continue;
        paired++;
        const bBlocks = [];
        for (let o = 0; o + 96 <= bTa.length; o += 96) bBlocks.push(bTa.subarray(o, o + 96));
        const em = new Map(), bm = new Map();
        for (const e of eBlocks) { const k = e.toString('hex'); em.set(k, (em.get(k) || 0) + 1); }
        for (const b2 of bBlocks) { const k = b2.toString('hex'); bm.set(k, (bm.get(k) || 0) + 1); }
        let frameExact = eBlocks.length === bBlocks.length;
        const upd = (key96, dE, dB, dM) => {
            const bk = ((parseInt(key96.slice(24, 32).match(/../g).reverse().join(''), 16)) & 0x1FF000) >>> 0;
            let st = blk.get(bk); if (!st) { st = { match: 0, onlyE: 0, onlyB: 0 }; blk.set(bk, st); }
            st.match += dM; st.onlyE += dE; st.onlyB += dB;
        };
        for (const [k, c] of em) { const m = Math.min(c, bm.get(k) || 0); upd(k, c - m, 0, m); if (c - m) frameExact = false; }
        for (const [k, c] of bm) { const m = Math.min(c, em.get(k) || 0); upd(k, 0, c - m, 0); if (c - m) frameExact = false; }
        if (frameExact) exactFrames++;
    }
    console.log(`
== VOFF ${voff}: paired ${paired}, content-exact frames ${exactFrames} (${(100 * exactFrames / Math.max(1, paired)).toFixed(1)}%)`);
    for (const [bk, st] of [...blk.entries()].sort((a, b) => a[0] - b[0])) {
        const tot = st.match + st.onlyE + st.onlyB;
        console.log(`   blk ${bk.toString(16)}: match ${st.match} (${(100 * st.match / Math.max(1, tot)).toFixed(1)}%)  engine-only ${st.onlyE}  body-only ${st.onlyB}`);
    }
}
