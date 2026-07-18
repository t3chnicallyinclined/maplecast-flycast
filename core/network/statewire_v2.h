#pragma once
// Semantic-state wire v2 — keyframe/delta dirty-diff of the replica-live DYNAMIC
// payload (docs/RENDER-REPLICA-RECORDING-FORMAT.md "v2"). Ships only the bytes
// that changed vs the last KEYFRAME, so a dropped delta never desyncs later ones
// (they rebase on the reliably-kept keyframe). Proven byte-exact + 10-29x thinner
// by _bwlab/statewire_v2_gate.py and this header's own test (_bwlab/statewire_v2_test.cpp);
// this is the C++ port dropped into captureFrame (server), mirrored by the JS
// decoder (web/render-replica/replay.html).
//
// Wire — the DYNAMIC block only. The frame record uses magic "FRM2" (vs v1 "FRMx")
// so v1 clients are byte-identical; the GFX/pal/HUD tails are unchanged and appended
// AFTER this block (their offset is computed once the variable-length block is parsed):
//   keyframe: u8 flag=1 | u32 rawLen | rawLen bytes
//   delta   : u8 flag=0 | u32 keyId  | u32 nRuns | nRuns x (u32 off, u32 len, len bytes)
// Runs = maximal spans of bytes differing from the keyframe, gap-merged: up to
// MERGE stable bytes are folded into a run (a run header costs 8 B, so short
// stable gaps are cheaper kept inside the run).
#include <cstdint>
#include <cstring>
#include <vector>

namespace statewire_v2 {

static const uint32_t MERGE = 8;

inline void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)(v));       o.push_back((uint8_t)(v >> 8));
    o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 24));
}
inline uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Encode `cur` (n bytes) as a keyframe (isKey) or a delta vs `ref` (n bytes, the
// last keyframe). Result is written to `out` (cleared first). Matches the Python
// reference byte-for-byte.
inline void encode(const uint8_t* cur, uint32_t n, const uint8_t* ref,
                   uint32_t keyId, bool isKey, std::vector<uint8_t>& out) {
    out.clear();
    if (isKey) {
        out.push_back(1);
        put_u32(out, n);
        out.insert(out.end(), cur, cur + n);
        return;
    }
    out.push_back(0);
    put_u32(out, keyId);
    std::vector<uint8_t> body;         // (off,len,bytes) runs, counted as we go
    uint32_t nRuns = 0, i = 0;
    while (i < n) {
        if (cur[i] == ref[i]) { i++; continue; }
        uint32_t start = i, lastDiff = i, j = i + 1;
        while (j < n && (j - lastDiff) <= MERGE) {   // fold <=MERGE stable bytes in
            if (cur[j] != ref[j]) lastDiff = j;
            j++;
        }
        uint32_t len = lastDiff - start + 1;
        put_u32(body, start);
        put_u32(body, len);
        body.insert(body.end(), cur + start, cur + start + len);
        nRuns++;
        i = lastDiff + 1;
    }
    put_u32(out, nRuns);
    out.insert(out.end(), body.begin(), body.end());
}

// Decode `enc` into `blob` (n bytes) given `ref` (the last keyframe, n bytes).
// Returns false on malformed input. For a keyframe, `blob` becomes the new key.
inline bool decode(const uint8_t* enc, size_t encLen, const uint8_t* ref,
                   uint32_t n, std::vector<uint8_t>& blob) {
    if (encLen < 1) return false;
    if (enc[0] == 1) {
        if (encLen < 5) return false;
        uint32_t rawLen = get_u32(enc + 1);
        if (rawLen != n || encLen < 5 + (size_t)rawLen) return false;
        blob.assign(enc + 5, enc + 5 + rawLen);
        return true;
    }
    if (encLen < 9) return false;      // flag + keyId + nRuns
    uint32_t nRuns = get_u32(enc + 5);
    blob.assign(ref, ref + n);
    size_t o = 9;
    for (uint32_t r = 0; r < nRuns; r++) {
        if (o + 8 > encLen) return false;
        uint32_t off = get_u32(enc + o); o += 4;
        uint32_t len = get_u32(enc + o); o += 4;
        if (o + len > encLen || (size_t)off + len > n) return false;
        memcpy(&blob[off], enc + o, len);
        o += len;
    }
    return true;
}

} // namespace statewire_v2
