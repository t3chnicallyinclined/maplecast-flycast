/* Reusable self-contained-leaf verifier for the SH4 game-tick executor.
 *
 * Runs one transpiled function (void FN(Sh4Ctx*), name via -DLEAF_FN=<fn>) against
 * a RAM snapshot and prints its EXACT write-set (merged address ranges, with
 * before/after bytes) using the MC_WRITELOG capture. That write-set is the
 * function's output I/O footprint — diff it byte-exact vs flycast / vs the frame
 * snapshot to verify the transpile.
 *
 *   zig cc -O2 -DMC_WRITELOG -DLEAF_FN=rng_e730 -I. gen_rng.c verify_leaf.c -o verify_leaf.exe
 *   ./verify_leaf.exe _ram_f90.bin
 *
 * Self-contained leaves take no register arguments (all inputs from pools + fixed
 * RAM), so a zeroed ctx + a scratch SP is a faithful entry state. The scratch SP
 * (0x8CFF0000) absorbs sts.l/lds.l macl bookkeeping; those stack writes are
 * flagged separately from game-state writes. */
#ifndef MC_WRITELOG
#define MC_WRITELOG
#endif
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

unsigned char mc_writebmp[0x1000000/8];   /* definition for the header's extern */

#ifndef LEAF_FN
#error "define -DLEAF_FN=<transpiled function name>"
#endif
void LEAF_FN(Sh4Ctx *c);

#define STR2(x) #x
#define STR(x)  STR2(x)

#define SCRATCH_SP_LO 0x8CFE0000u   /* [lo,hi) = stack scratch, writes here are bookkeeping */
#define SCRATCH_SP_HI 0x8D000000u

static u8 ram[RAM_SIZE];
static u8 before[RAM_SIZE];

int main(int argc, char **argv) {
    const char *snap = (argc > 1) ? argv[1] : "_ram_f90.bin";
    FILE *f = fopen(snap, "rb");
    if (!f) { printf("cannot open snapshot %s\n", snap); return 2; }
    size_t n = fread(ram, 1, RAM_SIZE, f);
    fclose(f);
    if (n != RAM_SIZE) { printf("short read %zu (want %u)\n", n, RAM_SIZE); return 2; }
    memcpy(before, ram, RAM_SIZE);

    Sh4Ctx c;
    memset(&c, 0, sizeof c);
    c.ram = ram;
    c.r[15] = 0x8CFF0000u;   /* scratch SP */

    printf("=== verify leaf %s on %s ===\n", STR(LEAF_FN), snap);
    LEAF_FN(&c);

    /* enumerate the write bitmap -> merged ranges */
    long game_bytes = 0, stack_bytes = 0, ranges = 0;
    u32 i = 0;
    while (i < 0x01000000u) {
        if (!(mc_writebmp[i>>3] & (1u << (i & 7)))) { i++; continue; }
        u32 start = i;
        while (i < 0x01000000u && (mc_writebmp[i>>3] & (1u << (i & 7)))) i++;
        u32 len = i - start;
        u32 gaddr = 0x8C000000u | start;
        int is_stack = (gaddr >= SCRATCH_SP_LO && gaddr < SCRATCH_SP_HI);
        if (is_stack) { stack_bytes += len; continue; }   /* bookkeeping — skip detail */
        ranges++;
        game_bytes += len;
        /* dump up to 16 bytes before/after for eyeballing */
        printf("  WROTE 0x%08X..+%u :", gaddr, len);
        printf("  before ");
        for (u32 k = 0; k < len && k < 16; k++) printf("%02X", before[start + k]);
        printf("  after ");
        for (u32 k = 0; k < len && k < 16; k++) printf("%02X", ram[start + k]);
        printf("\n");
    }
    printf("--- %ld game-state byte(s) in %ld range(s); %ld stack-scratch byte(s) ---\n",
           game_bytes, ranges, stack_bytes);
    printf("r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X macl=0x%08X\n",
           c.r[0], c.r[1], c.r[2], c.r[3], c.macl);
    return 0;
}
