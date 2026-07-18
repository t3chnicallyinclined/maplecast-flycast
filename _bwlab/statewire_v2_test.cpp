// Standalone validation of core/network/statewire_v2.h against a real .mcrr —
// the C++ analogue of statewire_v2_gate.py. Proves the C++ codec that goes into
// captureFrame is byte-exact and numerically identical to the proven Python
// reference BEFORE it touches the live server. No flycast deps.
//
// Build (x64 Native Tools):  cl /EHsc /O2 /std:c++17 statewire_v2_test.cpp
// Run:                       statewire_v2_test.exe <capture.mcrr> [keyN]
#include "../core/network/statewire_v2.h"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

static uint32_t rdu32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <capture.mcrr> [keyN]\n", argv[0]); return 2; }
    int KEY = argc >= 3 ? atoi(argv[2]) : 60;

    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz);
    if (fread(buf.data(), 1, sz, f) != (size_t)sz) { fprintf(stderr, "read failed\n"); return 2; }
    fclose(f);

    size_t p = 0;
    auto u32 = [&]() { uint32_t v = rdu32(&buf[p]); p += 4; return v; };
    if (u32() != 0x5252434D) { fprintf(stderr, "bad MCRR magic\n"); return 2; }
    /*ver*/ u32(); uint32_t nS = u32(), nD = u32(), nF = u32(), vram = u32(), pvr = u32(); u32();
    auto region = [&]() { uint32_t a = u32(), l = u32(); p += 8; return std::pair<uint32_t,uint32_t>(a, l); };
    std::vector<std::pair<uint32_t,uint32_t>> S, D;
    for (uint32_t i = 0; i < nS; i++) S.push_back(region());
    for (uint32_t i = 0; i < nD; i++) D.push_back(region());
    p += vram; p += pvr;
    for (auto& r : S) p += r.second;
    size_t frameStart = p;
    uint32_t dynbytes = 0; for (auto& r : D) dynbytes += r.second;

    // scan every "FRMx" occurrence from frameStart (variable-tail records => no fixed stride)
    const uint8_t MAG[4] = {0x46, 0x52, 0x4D, 0x78};
    std::vector<size_t> fpos;
    for (size_t i = frameStart; i + 4 <= buf.size(); i++)
        if (memcmp(&buf[i], MAG, 4) == 0) fpos.push_back(i);

    std::vector<uint8_t> keyblob, enc, dec;
    uint32_t keyId = 0;
    uint64_t nfr = 0, nkey = 0, ndelta = 0, corrupt = 0, v2total = 0;
    long long firstBad = -1;
    for (size_t fi = 0; fi < fpos.size(); fi++) {
        size_t pos = fpos[fi];
        if (pos + 12 + dynbytes > buf.size()) break;
        const uint8_t* cur = &buf[pos + 12];
        bool isKey = (fi % (size_t)KEY == 0);
        if (isKey) { keyblob.assign(cur, cur + dynbytes); keyId = (uint32_t)fi; nkey++; }
        else ndelta++;
        statewire_v2::encode(cur, dynbytes, keyblob.data(), keyId, isKey, enc);
        v2total += enc.size();
        bool ok = statewire_v2::decode(enc.data(), enc.size(), keyblob.data(), dynbytes, dec);
        if (!ok || dec.size() != dynbytes || memcmp(dec.data(), cur, dynbytes) != 0) {
            corrupt++; if (firstBad < 0) firstBad = (long long)fi;
        }
        nfr++;
    }

    printf("== statewire_v2.h C++ codec test ==\n");
    printf("capture : %s\n", argv[1]);
    printf("frames  : %zu FRMx (expected %u); dynbytes/frame = %u ; keyframe every %d\n",
           fpos.size(), nF, dynbytes, KEY);
    printf("encoded : %llu keyframes + %llu deltas\n", (unsigned long long)nkey, (unsigned long long)ndelta);
    printf("CORRECTNESS : %s%s\n",
           corrupt == 0 ? "PASS - C++ decode == raw, byte-exact all frames"
                        : "FAIL - byte mismatch",
           firstBad >= 0 ? (std::string("  (first bad frame ") + std::to_string(firstBad) + ")").c_str() : "");
    printf("v2 mean bytes/frame (pre-zstd) : %.0f  (%.1fx thinner than raw %u)\n",
           (double)v2total / (nfr ? nfr : 1),
           (double)dynbytes / ((double)v2total / (nfr ? nfr : 1)), dynbytes);
    printf("(compare to statewire_v2_gate.py keyframe-mode 'v2 delta' pre-zstd mean)\n");
    return corrupt == 0 ? 0 : 1;
}
