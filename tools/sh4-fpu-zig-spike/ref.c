// Reference: flycast's EXACT ftrv/fipr (core/hw/sh4/dyna/shil_canonical.h:142-148
// and interpr/sh4_fpu.cpp:407-412) — accumulate in DOUBLE, left-to-right, round
// to f32. Generates a corpus of random (matrix, vector) -> (ftrv[4], fipr) and
// writes it. The Zig reimplementation must reproduce every output bit-for-bit.
//
// Build with fp-contraction OFF so `(double)a*b + acc` is never fused to fma
// (flycast's determinism relies on this).  zig cc -O2 -ffp-contract=off
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint64_t st = 0x9E3779B97F4A7C15ULL;
static uint32_t nx(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (uint32_t)(st >> 32); }
// random FINITE f32 from a raw bit pattern (stresses denormals/huge/tiny/sign)
static float rf(void) { float f; uint32_t u; do { u = nx(); memcpy(&f, &u, 4); } while (!isfinite(f)); return f; }

// flycast innerProduct<1> — the FIPR path
static float fipr(const float *fn, const float *fm) {
    double f = (double)fn[0]*fm[0] + (double)fn[1]*fm[1] + (double)fn[2]*fm[2] + (double)fn[3]*fm[3];
    return (float)f;
}
// flycast ftrv — four innerProduct<4>(fn, fm+k)
static void ftrv(float *fd, const float *fn, const float *fm) {
    for (int k = 0; k < 4; k++) {
        double f = (double)fn[0]*fm[k+0] + (double)fn[1]*fm[k+4] + (double)fn[2]*fm[k+8] + (double)fn[3]*fm[k+12];
        fd[k] = (float)f;
    }
}

int main(int argc, char **argv) {
    long N = argc > 1 ? atol(argv[1]) : 1000000;
    const char *path = argc > 2 ? argv[2] : "corpus.bin";
    FILE *out = fopen(path, "wb");
    if (!out) { perror("open"); return 1; }
    for (long i = 0; i < N; i++) {
        float m[16], v[4], ftr[4], fip;
        for (int j = 0; j < 16; j++) m[j] = rf();
        for (int j = 0; j < 4; j++)  v[j] = rf();
        ftrv(ftr, v, m);
        fip = fipr(v, m);
        fwrite(m, 4, 16, out); fwrite(v, 4, 4, out); fwrite(ftr, 4, 4, out); fwrite(&fip, 4, 1, out);
    }
    fclose(out);
    fprintf(stderr, "ref: wrote %ld records -> %s (record = 16+4+4+1 f32)\n", N, path);
    return 0;
}
