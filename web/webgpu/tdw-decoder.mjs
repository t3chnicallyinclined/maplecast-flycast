// tdw-decoder.mjs — browser TDW dict-wire decoder.
//
// Wraps tdw.wasm (the shipping native-client tdw.rs compiled to wasm, zstd and
// all — byte-exact vs native, gated 481/481 on 2026-07-18). It turns TDW1/TDWS
// messages into {frameNum, ta, pages, pvr}, and `synthLegacyFrame` repackages
// that as an uncompressed legacy delta frame so the UNCHANGED FrameDecoder /
// TAParser / PVR2Renderer path renders it — no renderer changes, and applyFrame's
// own VCACHE (this._vcache) handles the dirty-page hash cache.
//
//   const dec = await TdwDecoder.create('/webgpu/tdw.wasm');
//   // per message:
//   if (magic === 'TDWS') dec.feedSnapshot(msg);
//   else if (magic === 'TDW1') { const f = dec.feed(msg);
//       if (f) applyFrame(synthLegacyFrame(f)); }   // fall through the normal path

export class TdwDecoder {
  static async create(wasmSource) {
    let bytes;
    if (wasmSource instanceof Uint8Array) bytes = wasmSource;
    else if (wasmSource instanceof ArrayBuffer) bytes = new Uint8Array(wasmSource);
    else {
      const r = await fetch(wasmSource);
      if (!r.ok) throw new Error(`tdw.wasm fetch ${r.status}`);
      bytes = new Uint8Array(await r.arrayBuffer());
    }
    const { instance } = await WebAssembly.instantiate(bytes, {});
    return new TdwDecoder(instance);
  }

  constructor(instance) {
    this.ex = instance.exports;
    this.dec = this.ex.tdw_new();
  }

  // Copy the message into the wasm input buffer (memory may grow → re-view).
  _write(msg) {
    const p = this.ex.tdw_inbuf(this.dec, msg.length);
    new Uint8Array(this.ex.memory.buffer).set(msg, p);
  }

  // Feed a 'TDWS' dictionary snapshot. Returns true on success.
  feedSnapshot(msg) {
    this._write(msg);
    return this.ex.tdw_snapshot(this.dec, msg.length) === 0;
  }

  // Feed a 'TDW1' frame. Returns {frameNum, ta, pages, pvr} or null (no frame
  // decodable yet — waiting for TDWS/streamStart, or a desync). ta/pages are
  // COPIES (safe to keep past the next feed); pvr is a Uint32Array(16) or null.
  feed(msg) {
    this._write(msg);
    const h = this.ex.tdw_feed(this.dec, msg.length);
    if (h === 0) return null;
    const dv = new DataView(this.ex.memory.buffer);
    if (!dv.getUint32(h, true)) return null;
    const frameNum = dv.getUint32(h + 4, true);
    const taPtr = dv.getUint32(h + 8, true), taLen = dv.getUint32(h + 12, true);
    const pagesPtr = dv.getUint32(h + 16, true), pagesLen = dv.getUint32(h + 20, true);
    const hasPvr = dv.getUint32(h + 24, true), pvrPtr = dv.getUint32(h + 28, true);
    const buf = this.ex.memory.buffer;
    const ta = new Uint8Array(buf, taPtr, taLen).slice();
    const pages = pagesLen ? new Uint8Array(buf, pagesPtr, pagesLen).slice() : null;
    const pvr = hasPvr ? new Uint32Array(buf, pvrPtr, 16).slice() : null;
    return { frameNum, ta, pages, pvr };
  }

  dictLen() { return this.ex.tdw_dict_len(this.dec); }
  isSynced() { return this.ex.tdw_synced(this.dec) !== 0; }
}

// Repackage a decoded TDW frame as an uncompressed legacy delta frame:
//   frameSize(4) frameNum(4) pvrSnapshot(64) taSize(4) deltaPayloadSize(4)
//   TA(taSize) checksum(4) dirtyPageSection(pages verbatim)
// deltaPayloadSize == taSize marks a keyframe, so applyFrame copies the full TA
// (TDW always reassembles the whole TA). `pages` already begins with the
// [count-or-0xFFFFFFFF] the dirty-page loop expects.
const EMPTY_PAGES = new Uint8Array([0, 0, 0, 0]); // count = 0

export function synthLegacyFrame(f) {
  const taLen = f.ta.length;
  const pages = f.pages || EMPTY_PAGES;
  const total = 84 + taLen + pages.length;
  const buf = new Uint8Array(total);
  const dv = new DataView(buf.buffer);
  dv.setUint32(0, total, true);                 // frameSize
  dv.setUint32(4, f.frameNum, true);            // frameNum
  if (f.pvr) for (let i = 0; i < 16; i++) dv.setUint32(8 + 4 * i, f.pvr[i], true); // pvrSnapshot
  dv.setUint32(72, taLen, true);                // taSize
  dv.setUint32(76, taLen, true);                // deltaPayloadSize == taSize → keyframe
  buf.set(f.ta, 80);                            // TA bytes
  dv.setUint32(80 + taLen, 0, true);            // checksum (skipped by applyFrame)
  buf.set(pages, 84 + taLen);                   // dirty-page section verbatim
  return buf;
}
