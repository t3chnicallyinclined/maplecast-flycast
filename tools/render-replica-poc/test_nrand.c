/* Verify the transpiled normalized-random-float leaf (loc_8C11E750) — which calls
 * the already-transpiled RNG via bsr — against the golden reference: for RNG result
 * 0x12EA (=4842, itself flycast-verified), fr0 = 4842 * 2^-15 = 0.14776611328125.
 * That value is exactly representable in float32 (numerator < 2^24, denominator a
 * power of two) so the check is bit-exact. Exercises the new bsr emission arm +
 * the mova/fmov.s/float/fmul FP path. */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>

void rng_e730(Sh4Ctx *c);
void nrand_e750(Sh4Ctx *c);
/* bsr loc_8c11e730 in nrand_e750 links to this shim -> the transpiled RNG */
void sub_8c11e730(Sh4Ctx *c) { rng_e730(c); }

static u8 ram[RAM_SIZE];

int main(void) {
    FILE *f = fopen("_ram_f90.bin", "rb");
    if (!f) { printf("no _ram_f90.bin\n"); return 2; }
    size_t n = fread(ram, 1, RAM_SIZE, f);
    fclose(f);
    if (n != RAM_SIZE) { printf("short read %zu\n", n); return 2; }

    Sh4Ctx c;
    memset(&c, 0, sizeof c);
    c.ram = ram;
    c.r[15] = 0x8CFF0000u;

    u32 seed_before = r32(&c, 0x8C16BC2C);
    nrand_e750(&c);
    float fr0 = c.fr[0];
    u32 bits; memcpy(&bits, &fr0, 4);

    float expect = 4842.0f / 32768.0f;
    u32 ebits; memcpy(&ebits, &expect, 4);

    printf("seed_before=0x%08X  fr0=%.17g bits=0x%08X\n", seed_before, (double)fr0, bits);
    printf("expect      fr0=%.17g bits=0x%08X\n", (double)expect, ebits);
    int ok = (bits == ebits);
    printf("%s\n", ok ? "BYTE-EXACT (bit-identical float32) — bsr arm + FP path correct"
                      : "MISMATCH");
    return ok ? 0 : 1;
}
