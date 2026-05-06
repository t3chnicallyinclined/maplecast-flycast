// frame-decoder.mjs — ZCST decompress, SYNC/FSYN handling, delta frame apply
// Mirrors wasm_bridge.cpp exactly including all 5 documented bugs.
// Pivot A (May 2026): TA-buffer dedup ring. taSize = 0xFFFFFFFE means REUSE;
// look up the next 4 bytes (reuseHash) in our local ring instead of decoding
// a delta. Server clears its ring on every SYNC; we mirror that by clearing
// ours in applySync().

import { decompress } from './fzstd.mjs';

const VRAM_SIZE = 8 * 1024 * 1024;
const PVR_REG_SIZE = 32 * 1024;
const PAGE_SIZE = 4096;
const MAGIC_ZCST = 0x5453435A;
const MAGIC_SYNC = 0x434E5953;
const MAGIC_FSYN = 0x4E595346;
const MAGIC_SAVE = 0x45564153;
const TA_REUSE_FLAG = 0xFFFFFFFE;
const TA_DEDUP_RING_SIZE = 256;

// xxhash32 — minimal implementation matching the C XXH32 we use server-side.
// 32-bit hash of a Uint8Array. No state object; one-shot mode is enough.
const PRIME32_1 = 0x9E3779B1 | 0;
const PRIME32_2 = 0x85EBCA77 | 0;
const PRIME32_3 = 0xC2B2AE3D | 0;
const PRIME32_4 = 0x27D4EB2F | 0;
const PRIME32_5 = 0x165667B1 | 0;
function rotl32(x, r) { return ((x << r) | (x >>> (32 - r))) | 0; }
function imul32(a, b) { return Math.imul(a, b) | 0; }
function xxhash32(buf, seed = 0) {
    const len = buf.length;
    let h32 = 0;
    let i = 0;
    if (len >= 16) {
        let v1 = (seed + PRIME32_1 + PRIME32_2) | 0;
        let v2 = (seed + PRIME32_2) | 0;
        let v3 = (seed + 0) | 0;
        let v4 = (seed - PRIME32_1) | 0;
        const limit = len - 16;
        const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
        while (i <= limit) {
            v1 = imul32(rotl32((v1 + imul32(dv.getUint32(i, true), PRIME32_2)) | 0, 13), PRIME32_1); i += 4;
            v2 = imul32(rotl32((v2 + imul32(dv.getUint32(i, true), PRIME32_2)) | 0, 13), PRIME32_1); i += 4;
            v3 = imul32(rotl32((v3 + imul32(dv.getUint32(i, true), PRIME32_2)) | 0, 13), PRIME32_1); i += 4;
            v4 = imul32(rotl32((v4 + imul32(dv.getUint32(i, true), PRIME32_2)) | 0, 13), PRIME32_1); i += 4;
        }
        h32 = (rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18)) | 0;
    } else {
        h32 = (seed + PRIME32_5) | 0;
    }
    h32 = (h32 + len) | 0;
    while (i + 4 <= len) {
        const dv = new DataView(buf.buffer, buf.byteOffset + i, 4);
        h32 = (h32 + imul32(dv.getUint32(0, true), PRIME32_3)) | 0;
        h32 = imul32(rotl32(h32, 17), PRIME32_4);
        i += 4;
    }
    while (i < len) {
        h32 = (h32 + imul32(buf[i], PRIME32_5)) | 0;
        h32 = imul32(rotl32(h32, 11), PRIME32_1);
        i++;
    }
    h32 ^= h32 >>> 15; h32 = imul32(h32, PRIME32_2);
    h32 ^= h32 >>> 13; h32 = imul32(h32, PRIME32_3);
    h32 ^= h32 >>> 16;
    return h32 >>> 0;
}

export class FrameDecoder {
    constructor() {
        this.vram = new Uint8Array(VRAM_SIZE);
        this.pvrRegs = new Uint8Array(PVR_REG_SIZE);
        this.prevTA = new Uint8Array(0);
        this.prevTASize = 0;
        this.hasPrevTA = false;
        this.frameNum = 0;
        this.stats = { syncs: 0, keyframes: 0, deltas: 0, dropped: 0,
                       reuseHits: 0, reuseMisses: 0 };
        // Pivot A: TA-buffer dedup ring. Map hash -> Uint8Array. FIFO eviction.
        this._taDedupMap = new Map();
        this._taDedupOrder = [];
    }
    _taDedupReset() {
        this._taDedupMap.clear();
        this._taDedupOrder.length = 0;
    }
    _taDedupAdd(hash, bytes) {
        if (this._taDedupMap.has(hash)) return;
        // bytes is a Uint8Array view; copy so subsequent mutations don't bleed in
        const copy = new Uint8Array(bytes.length);
        copy.set(bytes);
        this._taDedupMap.set(hash, copy);
        this._taDedupOrder.push(hash);
        while (this._taDedupOrder.length > TA_DEDUP_RING_SIZE) {
            const drop = this._taDedupOrder.shift();
            this._taDedupMap.delete(drop);
        }
    }

    _decompress(data) {
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        if (data.length >= 8 && view.getUint32(0, true) === MAGIC_ZCST) {
            return decompress(data.subarray(8));
        }
        return data;
    }

    applySync(rawData) {
        const data = this._decompress(new Uint8Array(rawData));
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        if (data.length < 8) return false;
        const magic = view.getUint32(0, true);

        if (magic === MAGIC_FSYN) {
            let off = 4;
            const recordCount = view.getUint16(off + 2, true); off += 4;
            for (let r = 0; r < recordCount && off + 8 <= data.length; r++) {
                const tag = String.fromCharCode(data[off], data[off+1], data[off+2], data[off+3]); off += 4;
                const recSize = view.getUint32(off, true); off += 4;
                if (off + recSize > data.length) break;
                if (tag === 'VRAM') this.vram.set(data.subarray(off, off + Math.min(recSize, VRAM_SIZE)));
                else if (tag === 'PREG') this.pvrRegs.set(data.subarray(off, off + Math.min(recSize, PVR_REG_SIZE)));
                off += recSize;
            }
            this.prevTA = new Uint8Array(0); this.prevTASize = 0; this.hasPrevTA = false;
            this._taDedupReset();   // Pivot A: server clears ring on FSYN, we mirror
            this.stats.syncs++;
            return true;
        }
        if (magic === MAGIC_SAVE) return true;
        if (magic !== MAGIC_SYNC) return false;

        let off = 4;
        const vramSize = Math.min(view.getUint32(off, true), VRAM_SIZE); off += 4;
        this.vram.set(data.subarray(off, off + vramSize)); off += vramSize;
        const pvrSize = Math.min(view.getUint32(off, true), PVR_REG_SIZE); off += 4;
        this.pvrRegs.set(data.subarray(off, off + pvrSize));
        this.prevTA = new Uint8Array(0); this.prevTASize = 0; this.hasPrevTA = false;
        this._taDedupReset();    // Pivot A: server clears ring on SYNC, we mirror
        this.stats.syncs++;
        return true;
    }

    applyFrame(rawData) {
        const data = this._decompress(new Uint8Array(rawData));
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        if (data.length < 80) return null;

        const maybeMagic = view.getUint32(0, true);
        if (maybeMagic === MAGIC_SYNC || maybeMagic === MAGIC_FSYN || maybeMagic === MAGIC_SAVE) {
            this.applySync(rawData);
            return null;
        }

        let off = 0;
        const frameSize = view.getUint32(off, true); off += 4;
        const frameNum = view.getUint32(off, true); off += 4;
        this.frameNum = frameNum;

        const pvrSnapshot = new Uint32Array(16);
        for (let i = 0; i < 16; i++) { pvrSnapshot[i] = view.getUint32(off, true); off += 4; }

        const taSize = view.getUint32(off, true); off += 4;
        let skipRender = false;

        if (taSize === TA_REUSE_FLAG) {
            // Pivot A REUSE: 4-byte reuseHash + 4-byte taChecksum (unused on this path).
            const reuseHash = view.getUint32(off, true); off += 4;
            off += 4;  // skip checksum field
            const cached = this._taDedupMap.get(reuseHash);
            if (!cached) {
                // Server's reset-on-SYNC discipline should make this impossible.
                // If it happens, drop the frame and wait for the next non-REUSE.
                this.stats.reuseMisses++;
                if (this.stats.reuseMisses < 6)
                    console.warn(`[frame-decoder] REUSE hash ${reuseHash.toString(16)} not in ring`);
                skipRender = true;
            } else {
                if (this.prevTA.length < cached.length) { const n = new Uint8Array(cached.length); this.prevTA = n; }
                this.prevTA.set(cached);
                this.prevTASize = cached.length;
                this.hasPrevTA = true;
                this.stats.reuseHits++;
            }
        } else {
            const deltaPayloadSize = view.getUint32(off, true); off += 4;

            if (deltaPayloadSize === taSize) {
                if (this.prevTA.length < taSize) { const n = new Uint8Array(taSize); n.set(this.prevTA); this.prevTA = n; }
                this.prevTA.set(data.subarray(off, off + taSize));
                this.prevTASize = taSize; this.hasPrevTA = true;
                off += taSize; this.stats.keyframes++;
            } else if (!this.hasPrevTA) {
                off += deltaPayloadSize; skipRender = true; this.stats.dropped++;
            } else {
                if (this.prevTA.length < taSize) { const n = new Uint8Array(taSize); n.set(this.prevTA); this.prevTA = n; }
                this.prevTASize = taSize;
                const deltaEnd = off + deltaPayloadSize;
                while (off + 4 <= deltaEnd) {
                    const doff = view.getUint32(off, true); off += 4;
                    if (doff === 0xFFFFFFFF) break;
                    const runLen = view.getUint16(off, true); off += 2;
                    if (doff + runLen <= taSize && off + runLen <= deltaEnd) this.prevTA.set(data.subarray(off, off + runLen), doff);
                    off += runLen;
                }
                off = deltaEnd - deltaPayloadSize + deltaPayloadSize;  // ensure we're past delta
                this.stats.deltas++;
            }

            // Pivot A: read on-wire taHash (formerly placeholder checksum) and
            // ring-add the decoded TA so future REUSE references resolve. We
            // trust the server's hash rather than recomputing — saves ~1ms/frame.
            const taHash = view.getUint32(off, true); off += 4;
            if (!skipRender && this.prevTASize > 0)
                this._taDedupAdd(taHash, this.prevTA.subarray(0, this.prevTASize));
        }

        const dirtyPages = view.getUint32(off, true); off += 4;
        let vramDirty = false, pvrDirty = false;
        const dirtyPageList = [];
        for (let d = 0; d < dirtyPages; d++) {
            const regionId = data[off]; off += 1;
            const pageIdx = view.getUint32(off, true); off += 4;
            const pageOff = pageIdx * PAGE_SIZE;
            if (regionId === 1 && pageOff + PAGE_SIZE <= VRAM_SIZE) {
                this.vram.set(data.subarray(off, off + PAGE_SIZE), pageOff);
                vramDirty = true;
                dirtyPageList.push(pageIdx);
            } else if (regionId === 3 && pageOff + PAGE_SIZE <= PVR_REG_SIZE) {
                this.pvrRegs.set(data.subarray(off, off + PAGE_SIZE), pageOff);
                pvrDirty = true;
            }
            off += PAGE_SIZE;
        }

        if (skipRender) return null;
        return { frameNum, pvrSnapshot, taBuffer: this.prevTA.subarray(0, this.prevTASize), taSize: this.prevTASize, vramDirty, pvrDirty, dirtyPageList };
    }
}
