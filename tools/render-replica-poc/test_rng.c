/* Verify the AUTO-TRANSPILED RNG (gen_rng.c) is byte-exact vs the golden
 * reference (hand-Zig milestone-1 executor + flycast): seed -> 0x92EA332A,
 * ret 0x12EA. Proves the full lift.py/codegen.py pipeline transpiles a real
 * MVC2 game-tick function correctly. */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>

void rng_e730(Sh4Ctx *c);

static u8 ram[RAM_SIZE];

int main(void) {
    FILE *f = fopen("_ram_f90.bin", "rb");
    if (!f) { printf("no _ram_f90.bin in cwd\n"); return 2; }
    size_t n = fread(ram, 1, RAM_SIZE, f);
    fclose(f);
    if (n != RAM_SIZE) { printf("short read %zu\n", n); return 2; }

    Sh4Ctx c;
    memset(&c, 0, sizeof c);
    c.ram = ram;
    c.r[15] = 0x8CFF0000u;   /* scratch SP for sts.l/lds.l macl */

    u32 seed_before = r32(&c, 0x8C16BC2C);
    rng_e730(&c);
    u32 seed_after  = r32(&c, 0x8C16BC2C);
    u32 ret         = c.r[0];

    printf("seed 0x%08X -> 0x%08X  ret 0x%04X\n", seed_before, seed_after, ret);
    int ok = (seed_after == 0x92EA332Au && ret == 0x12EAu);
    printf("%s\n", ok ? "PIPELINE BYTE-EXACT — codegen.py transpiled the RNG correctly"
                      : "MISMATCH vs golden reference");
    return ok ? 0 : 1;
}
