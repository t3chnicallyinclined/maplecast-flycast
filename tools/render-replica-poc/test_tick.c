/* EXECUTOR Test A harness. Runs the transpiled game-logic tick (tick_entry ->
 * scene handler sub_8c0358be) from a RAM snapshot with the same self-contained
 * entry state the flycast --leaf run used (minimal ctx, scratch stack), then diffs
 * the result vs the flycast scene-handler ground truth (oracle_ram_out.bin). Target:
 * 0 game-region bytes differ (executor reproduces flycast's tick). Unknown dispatch
 * targets (not-yet-transpiled functions) are counted, not fatal. */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void tick_entry(Sh4Ctx *c);

static u8 ram[RAM_SIZE];
static u8 truth[RAM_SIZE];
static long g_unknown = 0, g_unknown_distinct = 0;
static u32 g_first_unknown = 0;
static u32 g_unkset[4096]; static int g_nunk = 0;

static long g_calls = 0;
static const long CALL_LIMIT = 5000000L;   /* watchdog: a real tick is ~thousands of calls */
int mc_call_guard(void) { return (++g_calls > CALL_LIMIT) ? 1 : 0; }

void mc_unknown_call(u32 a) {
    if (!g_unknown) g_first_unknown = a;
    g_unknown++;
    int seen = 0; for (int i = 0; i < g_nunk; i++) if (g_unkset[i] == a) { seen = 1; break; }
    if (!seen && g_nunk < 4096) { g_unkset[g_nunk++] = a; g_unknown_distinct++; }
}

static int load(const char *p, u8 *d) {
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    size_t n = fread(d, 1, RAM_SIZE, f); fclose(f); return n == RAM_SIZE;
}

int main(int argc, char **argv) {
    const char *snap = argc > 1 ? argv[1] : "_ram_f90.bin";
    const char *tp   = argc > 2 ? argv[2] : NULL;
    if (!load(snap, ram)) { printf("cannot load %s\n", snap); return 2; }
    if (tp && !load(tp, truth)) { printf("cannot load %s\n", tp); return 2; }

    Sh4Ctx c; memset(&c, 0, sizeof c); c.ram = ram; c.r[15] = 0x8CFF0000u;
    tick_entry(&c);

    printf("tick %s. calls=%ld unknown_calls=%ld (distinct=%ld, first=0x%08X)\n",
           g_calls > CALL_LIMIT ? "HIT WATCHDOG (looping — missing fn breaks a loop)" : "ran to completion",
           g_calls, g_unknown, g_unknown_distinct, g_first_unknown);
    for (int i = 0; i < g_nunk && i < 24; i++) printf("   unknown target 0x%08X\n", g_unkset[i]);

    if (tp) {
        long diff = 0; u32 first = 0;
        for (u32 off = 0; off < 0x00FE0000u; off++) {
            if (off >= 0x002D5748u && off <= 0x002D574Bu) continue;
            if (off >= 0x0032DBACu && off <= 0x0032DBAFu) continue;
            if (off == 0x00268250u) continue;
            if (ram[off] != truth[off]) { if (!diff) first = 0x8C000000u | off; diff++; }
        }
        printf("game-region diff vs %s: %ld bytes%s (first@0x%08X)\n", tp, diff,
               diff ? "" : "  -> EXECUTOR BYTE-EXACT vs flycast tick", first);
    }
    return 0;
}
