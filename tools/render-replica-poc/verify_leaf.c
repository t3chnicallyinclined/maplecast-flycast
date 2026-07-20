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
static u8 truth[RAM_SIZE];

static int load(const char *path, u8 *dst) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 0; }
    size_t n = fread(dst, 1, RAM_SIZE, f);
    fclose(f);
    if (n != RAM_SIZE) { printf("short read %zu from %s\n", n, path); return 0; }
    return 1;
}

int main(int argc, char **argv) {
    const char *snap = (argc > 1) ? argv[1] : "_ram_f90.bin";
    /* optional next-frame snapshot = flycast ground truth for once-per-tick leaves */
    const char *truthpath = (argc > 2) ? argv[2] : NULL;
    if (!load(snap, ram)) return 2;
    if (truthpath && !load(truthpath, truth)) return 2;
    memcpy(before, ram, RAM_SIZE);

    Sh4Ctx c;
    memset(&c, 0, sizeof c);
    c.ram = ram;
    c.r[15] = 0x8CFF0000u;   /* scratch SP */

    /* entry-register overrides for reg-arg leaves: rN=0xHEX / frN=0xHEX (args 3+).
     * MUST match the oracle's --setr/--setfr for a valid comparison. */
    for (int ai = (truthpath ? 3 : 2); ai < argc; ai++) {
        int idx; unsigned v;
        if (sscanf(argv[ai], "fr%d=%x", &idx, &v) == 2)      memcpy(&c.fr[idx & 15], &v, 4);
        else if (sscanf(argv[ai], "r%d=%x", &idx, &v) == 2)  c.r[idx & 15] = v;
    }

    printf("=== verify leaf %s on %s ===\n", STR(LEAF_FN), snap);
    LEAF_FN(&c);

    /* enumerate the write bitmap -> merged ranges */
    long game_bytes = 0, stack_bytes = 0, ranges = 0, matched = 0, mismatched = 0;
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
        if (truthpath) {
            int ok = (memcmp(ram + start, truth + start, len) == 0);
            printf("  truth ");
            for (u32 k = 0; k < len && k < 16; k++) printf("%02X", truth[start + k]);
            printf("  %s", ok ? "MATCH" : "*** DIFF ***");
            if (ok) matched++; else mismatched++;
        }
        printf("\n");
    }
    printf("--- %ld game-state byte(s) in %ld range(s); %ld stack-scratch byte(s) ---\n",
           game_bytes, ranges, stack_bytes);
    if (truthpath) {
        printf("--- vs %s: %ld range(s) MATCH, %ld DIFF -> %s ---\n", truthpath,
               matched, mismatched, mismatched ? "NOT byte-exact" : "write-set BYTE-EXACT vs flycast");
        /* STRONG check: full game-region diff catches MISSING writes too (not just wrong
         * ones). Exclude the stack scratch region and the 3 known TCNT0 timing values
         * (0x8C2D5748 / 0x8C32DBAC / 0x8C268250) — proven non-cascading, masked by design. */
        long fdiff = 0; u32 first = 0;
        for (u32 off = 0; off < 0x01000000u; off++) {
            if (off >= 0x00FE0000u) continue;                 /* stack scratch */
            if (off >= 0x002D5748u && off <= 0x002D574Bu) continue;
            if (off >= 0x0032DBACu && off <= 0x0032DBAFu) continue;
            if (off == 0x00268250u) continue;
            if (ram[off] != truth[off]) { if (!fdiff) first = 0x8C000000u | off; fdiff++; }
        }
        printf("--- full game-region diff (excl stack+timers): %ld byte(s) differ -> %s ---\n",
               fdiff, fdiff ? "MISMATCH" : "FULLY BYTE-EXACT vs flycast");
        if (fdiff) printf("    first diff @0x%08X\n", first);
    }
    printf("r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X macl=0x%08X\n",
           c.r[0], c.r[1], c.r[2], c.r[3], c.macl);
    return 0;
}
