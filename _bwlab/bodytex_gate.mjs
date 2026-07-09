// bodytex_gate.mjs — THE ?bodytex=local TEXEL GATE (offline, falsifiable).
//
// Question it answers: if each render_frame body quad's TEXTURE comes from the client-side
// body_decoder (ensureBodyTextures over the replica-feed RAM image — the path byte-gated 5/5
// vs the mirror in 189544592) instead of TextureManager+wire-VRAM, are the texels the same?
//
// Method (mirrors the LIVE page exactly, on _bwlab/_cap_userplay/cap.bin, a DUAL capture:
// sock0 = the main wire incl. the LEGACY ZCST chain that maintains full VRAM; sock1 = the
// /replica-live feed):
//   * sock1 FRMx -> apply dyn regions + GFX tail -> render_frame.wasm -> filter to the strip
//     set {82,83,88,89} -> per-quad meta exports -> ensureBodyTextures into a local 8MB
//     staging image -> snapshot each staged quad's 512B PAL4_TW tile (ring, like the page).
//   * sock0 ZCST/SYNC -> FrameDecoder -> pair ring vf == frameNum+K-1 (the proven body(V) ==
//     engine TA V+1 pairing; K from the ZCS2 vframe anchors) -> for every staged quad compare
//     the SAMPLED-REGION texels (the quad's own engine UV bbox — u,v <= m/32; MEASURED: the
//     unsampled remainder of the engine's 512B staging chunk is neighboring-part bytes the
//     draw never reads, so whole-512B equality is the WRONG question) both ways with the
//     IDENTICAL palette:
//       (a) LOCAL  = the snapshotted body_decoder tile
//       (b) WIRE   = D.vram at the quad's own TCW (offline, per-page VCACHE-staleness-tracked)
//     and report sampled quad/pixel match + mean per-channel |delta|, SPLIT by SLOT ALIGNMENT
//     (engine char-quad count == ours; a count delta shifts the engine's staging cursor so
//     every downstream slot's wire bytes belong to a DIFFERENT part = not a decode question).
//
// MEASURED 2026-07-09 (full 3911-frame userplay capture):
//   sampled-region: 81.76% quads / 89.66% px exact overall;
//   SLOT-ALIGNED frames (1322/3911): 96.47% quads / 98.21% px exact;
//   residual classes: (1) misaligned frames = walker-vs-engine quad-set divergence (the top
//   ranked lead — a geometry/coverage question upstream of textures); (2) aligned-frame edge
//   tiles = our zero-pad vs the engine's contiguous-storage wrap; (3) torn-input satellites
//   (engine drew another sel's art than the shipped cell — the C++ certification's known
//   input skew; --hunt proved wire art == a DIFFERENT sel's decode).
//
// Usage: node bodytex_gate.mjs [capdir] [--sample N] [--max M] [--localgfx] [--sweep] [--dump N] [--hunt N]
//   --sample N  compare every Nth paired fight frame (default 1 = all)
//   --max M     stop after M compared frames (default unlimited)
//   --localgfx  overlay the local disc GFX1 (web/render-replica/gfx) like ?bodytex=local does
//   --sweep     pairing-offset diagnostic; --dump/--hunt N dump/content-search N aligned mismatches
import { readFileSync, existsSync } from 'fs';
import { FrameDecoder } from '../web/webgpu/frame-decoder.mjs';
import { Decompress, decompress } from '../web/webgpu/fzstd.mjs';
import { TextureManager } from '../web/webgpu/texture-manager.mjs';
import { ensureBodyTextures, decodeA } from '../web/render-replica/body_decoder.mjs';
import { applyLocalGfx, _injectCache } from '../web/render-replica/local_gfx_overlay.mjs';
import createRenderFrame from '../web/render-replica/render_frame.mjs';

const args = process.argv.slice(2);
const dir = args.filter((a, i) => !a.startsWith('--') && !['--sample', '--max', '--dump', '--hunt'].includes(args[i - 1]))[0]
    || 'C:/Users/trist/projects/maplecast-flycast/_bwlab/_cap_userplay';
const argN = (k, d) => { const i = args.indexOf(k); return i >= 0 ? parseInt(args[i + 1], 10) : d; };
const SAMPLE = argN('--sample', 1), MAXF = argN('--max', 0);
const LOCALGFX = args.includes('--localgfx');
const SWEEP = args.includes('--sweep');
const DUMP = argN('--dump', 0); let dumped = 0;
const HUNT = argN('--hunt', 0); let hunted = 0;
const sweep = new Map(); let sweepN = 0;

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

// ---- K anchor (ZCS2 vframe <-> legacy frameNum), verbatim from order_gate.mjs -------------
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
if (!anchors.length) { console.log('FAIL: no ZCS2 vframe anchors in capture'); process.exit(2); }
const K = anchors[0].vframe - anchors[0].frameNum;
console.log('K =', K, LOCALGFX ? '(local disc GFX1 overlay ON)' : '(shipped GFX only)');

// ---- replica side: seed + per-FRMx stage (EXACTLY the page's _bodyApplyFrame/_btexStage) --
const wasmBinary = readFileSync(new URL('../web/render-replica/render_frame.wasm', import.meta.url));
const M = await createRenderFrame({ wasmBinary });
const guest = a => (a >>> 0) & 0xFFFFFF;
let ram = null, dynRegs = null;
const lvram = new Uint8Array(8 * 1024 * 1024);
const decCache = {};
const ring = [];                          // {vf, ta, mask, tiles, nq, meta}
let decMsSum = 0, decMsMax = 0, decFrames = 0, _lastMeta = null;

// --localgfx: preload the disc GFX1 segments synchronously (fetch-free _injectCache), so
// applyLocalGfx overlays exactly like the live page (GFX1 pixels only, GFX2 untouched).
if (LOCALGFX) {
    for (let cid = 0; cid < 64; cid++) {
        const hx = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0');
        const p1 = new URL(`../web/render-replica/gfx/${hx}_gfx1.bin`, import.meta.url);
        const p2 = new URL(`../web/render-replica/gfx/${hx}_gfx2.bin`, import.meta.url);
        if (existsSync(p1) && existsSync(p2)) _injectCache(cid, new Uint8Array(readFileSync(p1)), new Uint8Array(readFileSync(p2)));
    }
}

function applyReplica(u8) {
    if (!ram) {
        if (u8.readUInt32LE(0) !== 0x5252434D) return;
        let p = 8; const nStatic = u8.readUInt32LE(p); p += 4; const nDynamic = u8.readUInt32LE(p); p += 4; p += 4;
        const vramB = u8.readUInt32LE(p); p += 4; const pvrB = u8.readUInt32LE(p); p += 4; p += 4;
        const region = () => { const addr = u8.readUInt32LE(p), len = u8.readUInt32LE(p + 4); let tag = ''; for (let i = 0; i < 8; i++) { const c = u8[p + 8 + i]; if (c) tag += String.fromCharCode(c); } p += 16; return { addr, len, tag }; };
        const staticRegs = Array.from({ length: nStatic }, region);
        dynRegs = Array.from({ length: nDynamic }, region);
        p += vramB + pvrB;
        ram = new Uint8Array(16 * 1024 * 1024);
        for (const r2 of staticRegs) { const bb = u8.subarray(p, p + r2.len); p += r2.len; if (r2.tag === 'ram16') ram.set(bb, 0); else ram.set(bb, guest(r2.addr)); }
        return;
    }
    if (u8.readUInt32LE(0) !== 0x784D5246) return;
    const vframe = u8.readUInt32LE(4); let p = 12;
    for (const r2 of dynRegs) { if (r2.tag !== 'bodytex') ram.set(u8.subarray(p, p + r2.len), guest(r2.addr)); p += r2.len; }
    if (p + 4 <= u8.length) { const nGfx = u8.readUInt32LE(p);
        if (nGfx <= 64) { p += 4;
            for (let i = 0; i < nGfx && p + 8 <= u8.length; i++) { const base = u8.readUInt32LE(p), len = u8.readUInt32LE(p + 4); p += 8;
                if (len > 0x800000 || p + len > u8.length) break;
                ram.set(u8.subarray(p, p + len), guest(base)); p += len; } } }
    const t0 = performance.now();
    if (LOCALGFX) applyLocalGfx(ram, null);
    const ramPtr = M._malloc(ram.length); M.HEAPU8.set(ram, ramPtr);
    const cap = 256 * 1024, outPtr = M._malloc(cap);
    const len = M._render_frame_ta(ramPtr, outPtr, cap);
    const raw = M.HEAPU8.subarray(outPtr, outPtr + len);
    const flt = new Uint8Array(len); let w = 0; const kept = []; let qi = 0;
    for (let o = 0; o + 96 <= len; o += 96, qi++) {
        const a = ((raw[o + 12] | raw[o + 13] << 8 | raw[o + 14] << 16 | raw[o + 15] << 24) >>> 0) & 0x1FFFFF;
        if (SB.has(a >>> 12)) { flt.set(raw.subarray(o, o + 96), w); w += 96; kept.push(qi); }
    }
    const ta = flt.slice(0, w);
    M._free(ramPtr); M._free(outPtr);
    const nq = kept.length;
    let mask = null, tiles = null;
    _lastMeta = null;
    if (nq) {
        const nqAll = M._render_frame_quad_count();
        const sp = M._malloc(nqAll * 2 || 2), gp = M._malloc(nqAll * 4 || 4), cp = M._malloc(nqAll * 8 || 8), mp = M._malloc(nqAll || 1);
        const sdp = M._render_frame_quad_srcdesc ? M._malloc(nqAll * 4 || 4) : 0;
        M._render_frame_quad_sels(sp, nqAll); M._render_frame_quad_gfx1s(gp, nqAll);
        M._render_frame_quad_colrow(cp, nqAll); M._render_frame_quad_mirror(mp, nqAll);
        if (sdp) M._render_frame_quad_srcdesc(sdp, nqAll);
        const HS = new Uint16Array(M.HEAPU8.buffer, sp, nqAll), HG = new Uint32Array(M.HEAPU8.buffer, gp, nqAll),
              HC = new Int32Array(M.HEAPU8.buffer, cp, nqAll * 2), H8 = M.HEAPU8;
        const kS = new Uint16Array(nq), kG = new Uint32Array(nq), kCR = new Int32Array(nq * 2), kM = new Uint8Array(nq);
        const kSD = sdp ? new Uint8Array(nq * 4) : null;
        for (let i = 0; i < nq; i++) { const q = kept[i];
            kS[i] = HS[q]; kG[i] = HG[q]; kCR[2 * i] = HC[2 * q]; kCR[2 * i + 1] = HC[2 * q + 1]; kM[i] = H8[mp + q];
            if (kSD) { kSD[4 * i] = H8[sdp + 4 * q]; kSD[4 * i + 1] = H8[sdp + 4 * q + 1]; kSD[4 * i + 2] = H8[sdp + 4 * q + 2]; kSD[4 * i + 3] = H8[sdp + 4 * q + 3]; } }
        M._free(sp); M._free(gp); M._free(cp); M._free(mp); if (sdp) M._free(sdp);
        const opts = { mask: new Uint8Array(nq) };
        ensureBodyTextures(ram, lvram, ta, nq, decCache, kS, kG, kCR, opts, kM, kSD);
        tiles = new Uint8Array(nq * 512);
        for (let i = 0; i < nq; i++) { if (!opts.mask[i]) continue;
            const o = i * 96 + 12;
            const tcw = (ta[o] | ta[o + 1] << 8 | ta[o + 2] << 16 | ta[o + 3] << 24) >>> 0;
            const a = ((tcw & 0x1FFFFF) << 3) >>> 0;
            if (a + 512 <= lvram.length) tiles.set(lvram.subarray(a, a + 512), i * 512);
            else opts.mask[i] = 0; }
        mask = opts.mask;
        _lastMeta = { kS, kG, kCR, kM, kSD };
        const ms = performance.now() - t0;
        decMsSum += ms; if (ms > decMsMax) decMsMax = ms; decFrames++;
    }
    ring.push({ vf: vframe, ta, mask, tiles, nq, meta: _lastMeta });
    if (ring.length > 64) ring.shift();
}

// ---- engine char-TA extractor: the quads the SERVER WOULD STRIP (para5, TR list, strip
// banks) from the engine's full TA, in order — order_gate's walker with the emit flipped.
// Used to split texel stats by "our TA == engine char TA" (TA-eq) vs divergent frames.
const STAGE_ALLOW = new Set([0x9fc00, 0xa0000]);
function extractCharTA(ta, taSize) {
    const out = new Uint8Array(taSize); let w = 0, off = 0;
    let curList = -1, isSpr = false, haveParam = false, taking = false, cObj = 0;
    const dv = new DataView(ta.buffer, ta.byteOffset, taSize);
    while (off + 32 <= taSize) {
        const pcw = dv.getUint32(off, true), pt = (pcw >>> 29) & 7;
        if (pt === 0 || pt === 1 || pt === 2 || pt === 3 || pt === 6) { haveParam = false; taking = false; if (pt === 0) curList = -1; off += 32; continue; }
        if (pt === 4) {
            const lt = (pcw >>> 24) & 7; if (curList === -1) curList = lt;
            if (curList === 1 || curList === 3) { haveParam = false; taking = false; off += 32; continue; }
            cObj = pcw & 0xFF; isSpr = false; haveParam = true; taking = false;
            const colType = (cObj >> 4) & 3, vol = (cObj >> 6) & 1;
            let sz; if (colType === 2 && !vol && ((cObj >> 2) & 1)) sz = (off + 64 <= taSize) ? 64 : 32;
            else if (colType >= 1 && vol) sz = (off + 64 <= taSize) ? 64 : 32; else sz = 32;
            off += sz; continue;
        }
        if (pt === 5) {
            const lt = (pcw >>> 24) & 7; if (curList === -1) curList = lt;
            cObj = pcw & 0xFF; isSpr = true; haveParam = true;
            const tcw = dv.getUint32(off + 12, true), a = tcw & 0x1FFFFF;
            taking = (curList === 2) && SB.has(a >>> 12);
            if (taking) { out.set(ta.subarray(off, off + 32), w); w += 32; }
            off += 32; continue;
        }
        if (pt === 7) {
            let sz; if (!haveParam) sz = 32; else if (isSpr && off + 64 <= taSize) sz = 64;
            else { const tex = (cObj >> 3) & 1, colType = (cObj >> 4) & 3, vol = (cObj >> 6) & 1;
                if (!tex) sz = 32; else if (!vol) sz = (colType === 1 && off + 64 <= taSize) ? 64 : 32; else sz = 32; }
            if (taking) { out.set(ta.subarray(off, off + sz), w); w += sz; }
            off += sz; continue;
        }
        off += 32;
    }
    return out.subarray(0, w);
}

// ---- --hunt helper: which sel's art does the WIRE hold at a mismatched TCW? --------------
// General rectangular PVR detwiddle (port of body_decoder detwiddlePal4, PAL4 2px/byte).
function _twSlow(x, y, xs, ys) { let rv = 0, sh = 0; xs >>= 1; ys >>= 1;
    while (xs || ys) { if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; } if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; } } return rv; }
function detwLin(dataB, W, H) {
    const out = new Uint8Array(W * H);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
        const ti = _twSlow(x, y, W, H);
        const b = dataB[ti >> 1] || 0;
        out[y * W + x] = (ti & 1) ? (b >> 4) & 0xF : b & 0xF;
    }
    return out;
}
const _huntLin = new Map();   // "gfx1:sel" -> {lin,W,H} | null
function huntPart(gfx1, sel) {
    const k = gfx1.toString(16) + ':' + sel;
    let e = _huntLin.get(k);
    if (e !== undefined) return e;
    const g = gfx1 & 0xFFFFFF;
    const n = ((ram[g] | ram[g + 1] << 8 | ram[g + 2] << 16 | ram[g + 3] << 24) >>> 0) >>> 2;
    e = null;
    if (sel < n) {
        const offV = (ram[g + sel * 4] | ram[g + sel * 4 + 1] << 8 | ram[g + sel * 4 + 2] << 16 | ram[g + sel * 4 + 3] << 24) >>> 0;
        const pb = g + offV;
        const sw = ram[pb + 2], sh = ram[pb + 3], W = sw * 8, H = sh * 8;
        if (W > 0 && H > 0 && W <= 1024 && H <= 1024) {
            const raw = decodeA(ram, (pb + 4) & 0xFFFFFF, (pb + 0x8000) & 0xFFFFFF, (W * H) >> 1);
            e = { lin: detwLin(raw, W, H), W, H };
        }
    }
    _huntLin.set(k, e);
    return e;
}
// Scan every sel of gfx1 for an m-grid cell equal to the wire's sampled region gw[y0..y1,x0..x1].
function huntWireArt(gfx1, gw, x0, x1, y0, y1) {
    const m = Math.max(x1 - x0, y1 - y0), hits = [];
    const g = gfx1 & 0xFFFFFF;
    const n = Math.min(4096, ((ram[g] | ram[g + 1] << 8 | ram[g + 2] << 16 | ram[g + 3] << 24) >>> 0) >>> 2);
    for (let sel = 0; sel < n; sel++) {
        const p = huntPart(gfx1, sel);
        if (!p) continue;
        const gc = Math.max(1, (p.W / m) | 0), gr = Math.max(1, (p.H / m) | 0);
        for (let ry = 0; ry < gr; ry++) for (let cx = 0; cx < gc; cx++) {
            let ok = true;
            for (let y = y0; y < y1 && ok; y++) for (let x = x0; x < x1; x++) {
                const px = cx * m + (x - x0), py = ry * m + (y - y0);
                const v = (px < p.W && py < p.H) ? p.lin[py * p.W + px] : 0;
                if (v !== gw[y * 32 + x]) { ok = false; break; }
            }
            if (ok) { hits.push({ sel, cx, ry }); if (hits.length > 40) return hits; }
        }
    }
    return hits;
}

// ---- the compare -----------------------------------------------------------
// TWO-PASS VCACHE PRE-WARM + PER-PAGE STALENESS: the capture's wire uses content-addressed
// VCACHE pages; a fresh decoder misses every ref whose hasData page it hasn't seen (measured
// 2440 misses/600 frames -> STALE wire VRAM = invalid ground truth). Fixes:
//   1. pass 1 harvests EVERY hasData page hash->bytes into a NO-CLEAR map (content-addressed,
//      so surviving SYNC epochs is sound: a hash identifies its exact 4KB content);
//   2. pass 2 replays with that cache pre-warmed (also no-clear);
//   3. a page-walker mirrors the VCACHE section per frame and keeps a STALE-page set (ref
//      whose hash is still unknown = the page's true content never appears in the capture).
//      Compared quads overlapping a stale page are EXCLUDED and counted (wire side invalid,
//      not a local-decode verdict).
function noClearMap(src) { const m = src ? new Map(src) : new Map(); m.clear = () => { }; return m; }
{
    const D0 = new FrameDecoder();
    D0._vcache = noClearMap();
    for (const r of recs) {
        if (r.sock !== 0) continue;
        const b = r.b;
        const magic = b.length >= 4 ? b.readUInt32LE(0) : 0;
        if (!isM(b, 'ZCST') && magic !== 0x434E5953 && magic !== 0x4E595346) continue;
        try { D0.applyFrame(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength)); } catch (e) { }
    }
    var _vcachePrewarm = D0._vcache;
    console.log(`VCACHE pre-warm: harvested ${_vcachePrewarm.size} content pages (pass-1 residual misses ${D0.vcacheMisses})`);
}
const D = new FrameDecoder();
D._vcache = noClearMap(_vcachePrewarm);
const stalePages = new Set();             // region-1 pageIdx whose last shipped content is unknown
// Walk one DELTA frame's dirty-page section (post-applyFrame; mirrors frame-decoder.mjs
// applyFrame offsets) and update stalePages: ref with unknown hash -> stale; applied -> fresh.
function walkStale(rawB) {
    let d;
    try { d = D._decompress(rawB); } catch (e) { return; }
    if (d.length < 80) return;
    const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    const m0 = dv.getUint32(0, true);
    if (m0 === 0x434E5953 || m0 === 0x4E595346) { stalePages.clear(); return; }   // SYNC/FSYN: all fresh
    let off = 72;                                     // frameSize(4)+frameNum(4)+pvr(64)
    const taSize = dv.getUint32(off, true); off += 4;
    const deltaPayloadSize = dv.getUint32(off, true); off += 4;
    off += deltaPayloadSize;                          // keyframe: deltaPayloadSize===taSize, same skip
    off += 4;                                         // checksum
    if (off + 4 > d.length) return;
    let n = dv.getUint32(off, true); off += 4;
    if (n === 0xFFFFFFFF) {                           // VCACHE section
        n = dv.getUint32(off, true); off += 4;
        for (let i = 0; i < n && off + 14 <= d.length; i++) {
            const regionId = d[off]; off += 1;
            const pageIdx = dv.getUint32(off, true); off += 4;
            const hLo = dv.getUint32(off, true), hHi = dv.getUint32(off + 4, true); off += 8;
            const hasData = d[off]; off += 1;
            const key = hLo.toString(16) + ':' + hHi.toString(16);
            if (hasData) off += 4096;
            if (regionId !== 1) continue;
            if (hasData || D._vcache.has(key)) stalePages.delete(pageIdx);
            else stalePages.add(pageIdx);
        }
    } else {                                          // raw dirty pages: all applied = fresh
        for (let i = 0; i < n && off + 5 <= d.length; i++) {
            const regionId = d[off]; off += 1;
            const pageIdx = dv.getUint32(off, true); off += 4;
            if (regionId === 1) stalePages.delete(pageIdx);
            off += 4096;
        }
    }
}
const TM = new TextureManager(null);      // decode-only (no GPU) — the EXACT wire _pal4
const rgbaA = new Uint8Array(4096), rgbaB = new Uint8Array(4096);
const _fdv = new DataView(new ArrayBuffer(4));
const gridA = new Uint8Array(1024), gridB = new Uint8Array(1024);
// PAL4_TW 512B chunk -> 32x32 linear nibble grid (PVR twiddle: y-bit then x-bit per level)
const _TW32 = (() => { const t = new Int32Array(1024); for (let y = 0; y < 32; y++) for (let x = 0; x < 32; x++) {
    let ti = 0; for (let b = 0; b < 5; b++) { ti |= ((y >> b) & 1) << (2 * b); ti |= ((x >> b) & 1) << (2 * b + 1); }
    t[y * 32 + x] = ti; } return t; })();
function detwGrid(src, base, out) { for (let i = 0; i < 1024; i++) { const ti = _TW32[i]; const b = src[base + (ti >> 1)];
    out[i] = (ti & 1) ? (b >> 4) & 0xF : b & 0xF; } return out; }
let framesPaired = 0, framesCompared = 0, pairIdx = 0, framesTaEq = 0;
const qDelta = new Map(); const alignAt = new Map();
const splitQ = { eq: [0, 0], df: [0, 0] };   // [sampExact, total] by TA-eq/diff
const splitPx = { eq: [0, 0], df: [0, 0] };
let quadsCompared = 0, quadsRawExact = 0, quadsBothZero = 0, quadsUnstaged = 0, quadsNon32 = 0, quadsAltExact = 0, quadsWireStale = 0, quadsSampExact = 0;
let pxTotal = 0, pxExact = 0;
const chDelta = [0, 0, 0, 0];             // sum |d| per channel r,g,b,a
const mmByKey = new Map();                // "bank:palSel" -> {quads, worstPx, exFrame}
let mmQuads = 0, mmPxExact = 0, mmPxTotal = 0;

for (const r of recs) {
    if (r.sock === 1) {
        let u8 = r.b;
        if (isM(u8, 'ZCST')) u8 = Buffer.from(decompress(u8.subarray(8)));
        try { applyReplica(u8); } catch (e) { /* tolerate torn feed records */ }
        continue;
    }
    const b = r.b;
    const magic = b.length >= 4 ? b.readUInt32LE(0) : 0;
    if (!isM(b, 'ZCST') && magic !== 0x434E5953 && magic !== 0x4E595346) continue;   // legacy chain only
    let fr = null;
    try { fr = D.applyFrame(b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength)); } catch (e) { continue; }
    try { walkStale(b); } catch (e) { }
    if (!fr) continue;
    // --sweep: score RAW tile match at ring offsets around the assumed pairing (K-1).
    if (SWEEP) {
        for (let off = -4; off <= 4; off++) {
            const e2 = ring.find(e => e.vf === D.frameNum + K - 1 + off);
            if (!e2 || !e2.nq || !e2.mask) continue;
            let eq = 0, tot = 0;
            const ta2 = e2.ta;
            for (let i = 0; i < e2.nq; i++) {
                if (!e2.mask[i]) continue;
                const tcw = (ta2[i * 96 + 12] | ta2[i * 96 + 13] << 8 | ta2[i * 96 + 14] << 16 | ta2[i * 96 + 15] << 24) >>> 0;
                const addr = ((tcw & 0x1FFFFF) << 3) >>> 0;
                const loc = e2.tiles.subarray(i * 512, i * 512 + 512);
                let same = true;
                for (let j = 0; j < 512; j++) if (loc[j] !== D.vram[addr + j]) { same = false; break; }
                tot++; if (same) eq++;
            }
            if (tot) { const s = sweep.get(off) || { eq: 0, tot: 0 }; s.eq += eq; s.tot += tot; sweep.set(off, s); }
        }
        if (++sweepN >= (MAXF || 400)) break;
        continue;
    }
    const ent = ring.find(e => e.vf === D.frameNum + K - 1);
    if (!ent || !ent.nq || !ent.mask) continue;
    framesPaired++;
    if ((pairIdx++ % SAMPLE) !== 0) continue;
    if (MAXF && framesCompared >= MAXF) break;
    framesCompared++;
    TM.updatePalette(D.pvrRegs);
    const ta = ent.ta;
    // SLOT-ALIGNMENT split (see below): same char-quad count == same staging TCW cursor.
    let taEq = false;
    {   // quad-count delta histogram (engine char quads minus ours): the slot-shift lead.
        // taEq (the texel split key) = SLOT-ALIGNED frame: same char-quad COUNT -> same
        // staging-cursor TCW assignment (full byte-equality fails on a param color word
        // that is texel-irrelevant; the texel question is only whether the slots align).
        const eng = extractCharTA(D.prevTA, D.prevTASize);
        const dq = (eng.length - ta.length) / 96;
        qDelta.set(dq, (qDelta.get(dq) || 0) + 1);
        taEq = dq === 0;
        if (taEq) framesTaEq++;
        // is misalignment PAIRING skew? tally which ring offset (if any) aligns this frame
        for (const off of [-2, -1, 0, 1, 2]) {
            const e2 = ring.find(e3 => e3.vf === D.frameNum + K - 1 + off);
            if (e2 && e2.ta.length === eng.length) { alignAt.set(off, (alignAt.get(off) || 0) + 1); }
        }
    }
    for (let i = 0; i < ent.nq; i++) {
        const o = i * 96;
        const tcw = (ta[o + 12] | ta[o + 13] << 8 | ta[o + 14] << 16 | ta[o + 15] << 24) >>> 0;
        const tsp = (ta[o + 8] | ta[o + 9] << 8 | ta[o + 10] << 16 | ta[o + 11] << 24) >>> 0;
        if (!ent.mask[i]) { quadsUnstaged++; continue; }                       // live: wire fallback
        const fmt = (tcw >> 27) & 7, tw = 8 << ((tsp >> 3) & 7), th = 8 << (tsp & 7);
        if (fmt !== 5 || tw !== 32 || th !== 32) { quadsNon32++; continue; }   // live: wire fallback
        const addr = ((tcw & 0x1FFFFF) << 3) >>> 0, palSel = (tcw >> 21) & 0x3F;
        if (stalePages.has(addr >> 12) || stalePages.has((addr + 511) >> 12)) { quadsWireStale++; continue; }
        const loc = ent.tiles.subarray(i * 512, i * 512 + 512);
        quadsCompared++;
        let rawEq = true, allZero = true;
        for (let j = 0; j < 512; j++) { const wv = D.vram[addr + j]; if (loc[j] !== wv) rawEq = false; if (loc[j] | wv) allZero = false; }
        if (rawEq) { quadsRawExact++; if (allZero) quadsBothZero++; }
        else {
            // DISCRIMINATOR: does our tile match the wire's OTHER staging bank (the engine
            // double-buffer partner, 82xxx<->88xxx = byte +/-0x30000)? If yes, the LOCAL
            // decode is right and the capture's wire VRAM/TA pairing is bank-skewed.
            const alt = addr ^ 0x30000;
            let altEq = true;
            for (let j = 0; j < 512; j++) if (loc[j] !== D.vram[alt + j]) { altEq = false; break; }
            if (altEq) quadsAltExact++;
        }
        // THE DRAW-CORRECTNESS COMPARE: only the texels the quad actually SAMPLES matter.
        // The walker declares a 32x32 texture but UV-clamps to the used sub-rect (u,v <=
        // m/32); the rest of the engine's 512B staging chunk is NEIGHBORING-PART bytes the
        // draw never reads (measured: every --dump mismatch had the sampled sub-rect equal
        // and only the unsampled padding different). Read the quad's OWN engine UVs (A/B/C
        // f16 pairs in the 96B block; D's UV lies inside their bbox for the rect) and
        // compare texel indices inside that bbox. Same palSel+palette both sides => index
        // equality == RGBA equality; deltas for differing indices come from the live palette.
        const f16 = (bits) => { _fdv.setUint32(0, (bits << 16) >>> 0, true); return _fdv.getFloat32(0, true); };
        const dvq = new DataView(ta.buffer, ta.byteOffset + o, 96);
        const Av = f16(dvq.getUint16(84, true)), Au = f16(dvq.getUint16(86, true));
        const Bv = f16(dvq.getUint16(88, true)), Bu = f16(dvq.getUint16(90, true));
        const Cv = f16(dvq.getUint16(92, true)), Cu = f16(dvq.getUint16(94, true));
        const umin = Math.min(Au, Bu, Cu), umax = Math.max(Au, Bu, Cu);
        const vmin = Math.min(Av, Bv, Cv), vmax = Math.max(Av, Bv, Cv);
        let x0 = Math.max(0, Math.floor(umin * 32)), x1 = Math.min(32, Math.ceil(umax * 32));
        let y0 = Math.max(0, Math.floor(vmin * 32)), y1 = Math.min(32, Math.ceil(vmax * 32));
        if (x1 <= x0) x1 = x0 + 1; if (y1 <= y0) y1 = y0 + 1;
        const gl = detwGrid(loc, 0, gridA), gw = detwGrid(D.vram, addr, gridB);
        let qex = 0, qtot = 0, sampEq = true;
        const pb = palSel << 4;
        for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) {
            const a = gl[y * 32 + x], b2 = gw[y * 32 + x];
            qtot++;
            if (a === b2) { pxExact++; qex++; continue; }
            sampEq = false;
            const pa = (pb + a) * 4, pbx = (pb + b2) * 4, P = TM._pal;
            chDelta[0] += Math.abs(P[pa] - P[pbx]); chDelta[1] += Math.abs(P[pa + 1] - P[pbx + 1]);
            chDelta[2] += Math.abs(P[pa + 2] - P[pbx + 2]); chDelta[3] += Math.abs(P[pa + 3] - P[pbx + 3]);
        }
        pxTotal += qtot;
        if (sampEq) quadsSampExact++;
        const sq = taEq ? splitQ.eq : splitQ.df, sp2 = taEq ? splitPx.eq : splitPx.df;
        sq[1]++; if (sampEq) sq[0]++;
        sp2[0] += qex; sp2[1] += qtot;
        // --hunt: for a total (0-exact) sampled mismatch, search the WHOLE GFX1 part space for
        // the art the WIRE actually holds at this TCW. A hit on a DIFFERENT sel proves the
        // C++ certification's "input skew" claim (engine drew sel X where the shipped cell
        // says sel Y — torn state-vs-staging in the wire), i.e. NOT a carve/decode defect.
        if (HUNT && hunted < HUNT && !sampEq && qex === 0 && ent.meta && taEq) {
            hunted++;
            const gfx1q = ent.meta.kG[i] >>> 0, selq = ent.meta.kS[i];
            const mI = x1 - x0;                                  // sampled square size
            console.log(`\n-- HUNT #${hunted}: frame ${D.frameNum} quad ${i} addr 0x${addr.toString(16)} sel ${selq} gfx1 0x${gfx1q.toString(16)} sampled ${x1 - x0}x${y1 - y0}`);
            const hits = huntWireArt(gfx1q, gw, x0, x1, y0, y1);
            console.log(hits.length ? `   wire art matches: ${hits.slice(0, 8).map(h => `sel ${h.sel}@(${h.cx},${h.ry})`).join('  ')}${hits.length > 8 ? ` +${hits.length - 8} more` : ''}`
                                    : '   wire art matches NO sel in this GFX1 (foreign object / non-GFX1 content)');
        }
        if (!sampEq && DUMP && dumped < DUMP && taEq) {   // dump ALIGNED-frame residuals (lead #3)
            dumped++;
            // meta for this quad (re-derive from the ring entry arrays is not stored; print TA-level info)
            console.log(`\n-- DUMP mismatch #${dumped}: frame ${D.frameNum} vf ${ent.vf} quad ${i}/${ent.nq} addr 0x${addr.toString(16)} palSel ${palSel} tsp 0x${tsp.toString(16)} tcw 0x${tcw.toString(16)}`);
            if (ent.meta) {
                const mI = ent.meta;
                console.log(`   sel ${mI.kS[i]} gfx1 0x${(mI.kG[i] >>> 0).toString(16)} colrow (${mI.kCR[2 * i]},${mI.kCR[2 * i + 1]}) mirror ${mI.kM[i]}` +
                    (mI.kSD ? ` srcdesc [m=${mI.kSD[4 * i]},cx=${mI.kSD[4 * i + 1]},ry=${mI.kSD[4 * i + 2]},fl=${mI.kSD[4 * i + 3]}]` : ' srcdesc -'));
            }
            // detwiddle both 512B PAL4_TW tiles to 32x32 nibble grids for eyeballing
            const detw = (src, base) => { const g = []; for (let y = 0; y < 32; y++) { let s = ''; for (let x = 0; x < 32; x++) {
                let ti = 0; for (let bit = 0; bit < 5; bit++) { ti |= ((y >> bit) & 1) << (2 * bit); ti |= ((x >> bit) & 1) << (2 * bit + 1); }
                const b = src[base + (ti >> 1)]; s += ((ti & 1) ? (b >> 4) & 0xF : b & 0xF).toString(16); } g.push(s); } return g; };
            const gl = detw(loc, 0), gw = detw(D.vram, addr);
            for (let y = 0; y < 32; y++) console.log(`   ${gl[y]}  |  ${gw[y]}  ${gl[y] === gw[y] ? '' : '<'}`);
        }
        if (!sampEq) {
            mmQuads++; mmPxExact += qex; mmPxTotal += qtot;
            const key = (addr >>> 15).toString(16) + ':' + palSel;             // 32KB bank : palette row
            let e = mmByKey.get(key);
            if (!e) { e = { quads: 0, worstFrac: 1, exFrame: D.frameNum, exAddr: addr }; mmByKey.set(key, e); }
            e.quads++; const frac = qex / qtot;
            if (frac < e.worstFrac) { e.worstFrac = frac; e.exFrame = D.frameNum; e.exAddr = addr; }
        }
    }
}

if (SWEEP) {
    console.log(`\n== PAIRING SWEEP (raw 512B tile match vs wire VRAM, ${sweepN} wire frames)`);
    [...sweep.entries()].sort((a, b) => a[0] - b[0])
        .forEach(([off, s]) => console.log(`  ring vf = frameNum+K-1${off >= 0 ? '+' + off : off}: ${(100 * s.eq / s.tot).toFixed(2)}% (${s.eq}/${s.tot})`));
    process.exit(0);
}
console.log(`\n== BODYTEX GATE (${dir})`);
console.log(`paired fight frames: ${framesPaired} | compared: ${framesCompared} (sample=${SAMPLE})`);
console.log(`quads compared: ${quadsCompared} | skipped: unstaged(wire-fallback)=${quadsUnstaged} non-32x32/pal4=${quadsNon32} wire-stale(excluded)=${quadsWireStale}`);
if (quadsCompared) {
    console.log(`SAMPLED-REGION quad match (the draw-correctness gate): ${quadsSampExact}/${quadsCompared} (${(100 * quadsSampExact / quadsCompared).toFixed(3)}%)`);
    console.log(`SAMPLED exact pixels: ${pxExact}/${pxTotal} (${(100 * pxExact / pxTotal).toFixed(4)}%)`);
    console.log(`SLOT-ALIGNED SPLIT: aligned frames ${framesTaEq}/${framesCompared} | ALIGNED quads ${splitQ.eq[0]}/${splitQ.eq[1]} (${splitQ.eq[1] ? (100 * splitQ.eq[0] / splitQ.eq[1]).toFixed(3) : '-'}%) px ${splitPx.eq[1] ? (100 * splitPx.eq[0] / splitPx.eq[1]).toFixed(4) : '-'}% | MISALIGNED quads ${splitQ.df[0]}/${splitQ.df[1]} (${splitQ.df[1] ? (100 * splitQ.df[0] / splitQ.df[1]).toFixed(3) : '-'}%) px ${splitPx.df[1] ? (100 * splitPx.df[0] / splitPx.df[1]).toFixed(4) : '-'}%`);
    console.log(`mean |delta| /chan  : R ${(chDelta[0] / pxTotal).toFixed(4)}  G ${(chDelta[1] / pxTotal).toFixed(4)}  B ${(chDelta[2] / pxTotal).toFixed(4)}  A ${(chDelta[3] / pxTotal).toFixed(4)}  (over sampled px)`);
    console.log(`[info] whole-512B raw match: ${quadsRawExact}/${quadsCompared} (${(100 * quadsRawExact / quadsCompared).toFixed(2)}%) [both-zero ${quadsBothZero}; unsampled padding differs by design]  alt-bank ${quadsAltExact}`);
    if (mmQuads) {
        console.log(`SAMPLED-mismatched quads: ${mmQuads} (exact px within them: ${(100 * mmPxExact / mmPxTotal).toFixed(1)}%) — TOP LEADS by 32KB-bank:palSel:`);
        [...mmByKey.entries()].sort((a, b) => b[1].quads - a[1].quads).slice(0, 10)
            .forEach(([k, e]) => console.log(`  bank ${k}  quads=${e.quads}  worst exact-frac=${(100 * e.worstFrac).toFixed(0)}%  ex: frame ${e.exFrame} addr 0x${e.exAddr.toString(16)}`));
    }
}
console.log(`frames whose count ALIGNS at ring offset (K-1+d): ${[-2, -1, 0, 1, 2].map(d => d + ':' + (alignAt.get(d) || 0)).join('  ')} (of ${framesCompared})`);
console.log(`engine-minus-ours char-quad count delta histogram: ${[...qDelta.entries()].sort((a, b) => a[0] - b[0]).map(([d, n]) => d + ':' + n).join('  ')}`);
console.log(`wire VCACHE ref misses (stale-wire hazard): ${D.vcacheMisses}`);
console.log(`local decode+stage  : avg ${(decMsSum / Math.max(1, decFrames)).toFixed(2)} ms/frame  max ${decMsMax.toFixed(2)} ms  (${decFrames} fight frames)`);
const pass = quadsCompared > 0 && quadsSampExact === quadsCompared;
console.log(pass ? 'VERDICT: TEXEL PASS (byte-exact)' : (quadsCompared ? 'VERDICT: MEASURED SHORTFALL — rank the leads above' : 'VERDICT: NO DATA'));
