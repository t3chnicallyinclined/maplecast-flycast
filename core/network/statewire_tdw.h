#pragma once
// Keyframe/delta codec for the VARIABLE-LENGTH TDW inner payload (loss-tolerant TDW
// wire, "TDW2"). Like statewire_v2.h but the blob length changes every frame
// (nBlocks varies), so the delta carries a target length, and BOTH keyframe and
// delta carry a keyId so a delta whose keyframe was lost is skipped, not misapplied.
// Proven byte-exact + loss-tolerant offline (_bwlab/statewire_tdw_kf.py). Server
// encodes (maplecast_mirror.cpp); the Rust client decodes (native-client-tdw tdw.rs);
// cross-validated by the G0 gate under injected loss.
//
// Block layout (rides the wire, then one-shot zstd'd):
//   keyframe: u8 flag=1 | u32 keyId | u32 rawLen | rawLen bytes
//   delta   : u8 flag=0 | u32 keyId | u32 targetLen | u32 nRuns |
//             nRuns x (u32 off, u32 len, len bytes)   -- runs vs keyframe `keyId`
#include <cstdint>
#include <cstring>
#include <vector>

namespace statewire_tdw {

static const uint32_t MERGE = 8;

inline void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8));
    o.push_back((uint8_t)(v >> 16)); o.push_back((uint8_t)(v >> 24));
}
inline uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint8_t  block_is_key(const uint8_t* enc) { return enc[0]; }          // 1 = keyframe
inline uint32_t block_keyid (const uint8_t* enc) { return get_u32(enc + 1); }

// Encode `cur` (n bytes) as keyframe (isKey) or delta vs `ref` (refLen bytes = the
// keyframe named `keyId`). Cleared then written to `out`.
inline void encode(const uint8_t* cur, uint32_t n, const uint8_t* ref, uint32_t refLen,
                   uint32_t keyId, bool isKey, std::vector<uint8_t>& out) {
    out.clear();
    out.push_back(isKey ? 1 : 0);
    put_u32(out, keyId);
    put_u32(out, n);                       // rawLen (keyframe) / targetLen (delta)
    if (isKey) { out.insert(out.end(), cur, cur + n); return; }
    std::vector<uint8_t> body;
    uint32_t nRuns = 0;
    uint32_t m = n < refLen ? n : refLen;  // compare over the overlap
    uint32_t i = 0;
    while (i < m) {
        if (cur[i] == ref[i]) { i++; continue; }
        uint32_t start = i, lastDiff = i, j = i + 1;
        while (j < m && (j - lastDiff) <= MERGE) { if (cur[j] != ref[j]) lastDiff = j; j++; }
        uint32_t len = lastDiff - start + 1;
        put_u32(body, start); put_u32(body, len);
        body.insert(body.end(), cur + start, cur + start + len);
        nRuns++;
        i = lastDiff + 1;
    }
    if (n > refLen) {                      // grew: tail beyond the keyframe is all-new
        put_u32(body, refLen); put_u32(body, n - refLen);
        body.insert(body.end(), cur + refLen, cur + n);
        nRuns++;
    }
    put_u32(out, nRuns);
    out.insert(out.end(), body.begin(), body.end());
}

// Decode `enc` into `blob` given `ref` (the keyframe named block_keyid(enc), refLen
// bytes; ignored for a keyframe). false on malformed input. Caller must have already
// checked that, for a delta, block_keyid matches the keyframe it holds.
inline bool decode(const uint8_t* enc, size_t encLen, const uint8_t* ref, uint32_t refLen,
                   std::vector<uint8_t>& blob) {
    if (encLen < 9) return false;
    uint32_t sz = get_u32(enc + 5);        // rawLen or targetLen
    if (enc[0] == 1) {
        if (encLen < 9 + (size_t)sz) return false;
        blob.assign(enc + 9, enc + 9 + sz);
        return true;
    }
    if (encLen < 13) return false;
    uint32_t nRuns = get_u32(enc + 9);
    blob.assign(sz, 0);
    uint32_t m = sz < refLen ? sz : refLen;
    if (m) memcpy(blob.data(), ref, m);
    size_t o = 13;
    for (uint32_t r = 0; r < nRuns; r++) {
        if (o + 8 > encLen) return false;
        uint32_t off = get_u32(enc + o); o += 4;
        uint32_t len = get_u32(enc + o); o += 4;
        if (o + len > encLen || (size_t)off + len > sz) return false;
        memcpy(blob.data() + off, enc + o, len);
        o += len;
    }
    return true;
}

} // namespace statewire_tdw
