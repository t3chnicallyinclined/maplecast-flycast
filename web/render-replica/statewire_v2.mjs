// Semantic-state wire v2 decoder (client). Mirrors core/network/statewire_v2.h
// decode() and _bwlab/statewire_v2_gate.py — proven byte-identical across all three
// (see _bwlab/statewire_v2_jstest.mjs). Reconstructs the replica-live DYNAMIC blob
// from a keyframe (flag=1) or a delta (flag=0) rebased on the last keyframe.
//
// The frame record is tagged FRM2 (vs v1 FRMx); this decodes just the DYNAMIC
// block. The caller keeps the last keyframe blob and passes it as `ref`; on a
// keyframe the returned blob becomes the new `ref`. The block length is
// self-describing (see decodeV2Len) so the GFX/pal/HUD tails that follow are
// found at start + decodeV2Len(enc).

// Length in bytes of the v2 block starting at enc[0..] — so the caller can locate
// the tails that follow it in the frame record.
export function decodeV2Len(enc) {
  const dv = new DataView(enc.buffer, enc.byteOffset, enc.byteLength);
  if (enc[0] === 1) return 5 + dv.getUint32(1, true);   // flag + rawLen + bytes
  const nRuns = dv.getUint32(5, true);                   // flag + keyId + nRuns
  let o = 9;
  for (let r = 0; r < nRuns; r++) {
    const len = dv.getUint32(o + 4, true);
    o += 8 + len;
  }
  return o;
}

// Reconstruct the n-byte DYNAMIC blob. `enc` = the v2 block (Uint8Array), `ref` =
// the last keyframe blob (Uint8Array, n bytes; ignored for a keyframe).
export function decodeV2(enc, ref, n) {
  const dv = new DataView(enc.buffer, enc.byteOffset, enc.byteLength);
  if (enc[0] === 1) {
    const rawLen = dv.getUint32(1, true);
    if (rawLen !== n) throw new Error(`v2 keyframe len ${rawLen} != ${n}`);
    // MUST be an owning copy — node Buffer.slice/subarray share memory, so a
    // returned view would be mutated when a later delta writes through it.
    return new Uint8Array(enc.subarray(5, 5 + rawLen));
  }
  // delta: enc[1..4]=keyId (informational — caller supplies the matching ref)
  const nRuns = dv.getUint32(5, true);
  const blob = new Uint8Array(n);               // fresh buffer, never aliases ref
  blob.set(ref.subarray(0, n));                 // copy of the keyframe
  let o = 9;
  for (let r = 0; r < nRuns; r++) {
    const off = dv.getUint32(o, true); o += 4;
    const len = dv.getUint32(o, true); o += 4;
    blob.set(enc.subarray(o, o + len), off);
    o += len;
  }
  return blob;
}
