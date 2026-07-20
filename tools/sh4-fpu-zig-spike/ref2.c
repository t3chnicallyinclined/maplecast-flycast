// Phase 0.4 — scalar FP ops. flycast exact (interpr/sh4_fpu.cpp, PR==0):
//   fadd a+b, fsub a-b, fmul a*b, fdiv a/b, fmac fma(a,b,c),
//   ftrc = (u32)(s32)a with the 0x7fffff80 clamp + x86 out-of-range fixup.
// Generates a corpus; the Zig reimpl (native + wasm) must match every bit.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint64_t st = 0x9E3779B97F4A7C15ULL;
static uint32_t nx(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (uint32_t)(st >> 32); }
static float rf(void) { float f; uint32_t u; do { u = nx(); memcpy(&f, &u, 4); } while (!isfinite(f)); return f; }

// flycast ftrc (sh4_fpu.cpp:513-530), x64 path
static uint32_t ftrc(float x) {
    if (isnan(x)) return 0x80000000u;
    uint32_t fpul = (uint32_t)(int32_t)x;        // x86: out-of-range -> 0x80000000
    if ((int32_t)fpul > 0x7fffff80) fpul = 0x7fffffffu;
    else if (fpul == 0x80000000u && x > 0) fpul--;
    return fpul;
}

int main(int argc, char **argv) {
    long N = argc > 1 ? atol(argv[1]) : 200000;
    const char *path = argc > 2 ? argv[2] : "corpus2.bin";
    FILE *out = fopen(path, "wb");
    if (!out) { perror("open"); return 1; }
    for (long i = 0; i < N; i++) {
        float a = rf(), b = rf(), c = rf();
        float add = a + b, sub = a - b, mul = a * b, dv = a / b, mac = fmaf(a, b, c);
        uint32_t ft = ftrc(a);
        fwrite(&a, 4, 1, out); fwrite(&b, 4, 1, out); fwrite(&c, 4, 1, out);
        fwrite(&add, 4, 1, out); fwrite(&sub, 4, 1, out); fwrite(&mul, 4, 1, out);
        fwrite(&dv, 4, 1, out); fwrite(&mac, 4, 1, out); fwrite(&ft, 4, 1, out);
    }
    fclose(out);
    fprintf(stderr, "ref2: wrote %ld records (9 words: a b c | add sub mul div fmac | ftrc) -> %s\n", N, path);
    return 0;
}
