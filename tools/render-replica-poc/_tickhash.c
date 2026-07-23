/* MSVC-vs-zig byte-exactness gate: run N neutral ticks over a seed, hash the game-state
 * region, and (optionally) dump the full 16MB RAM. If the MSVC build and the zig build
 * produce the SAME hash + identical RAM, the executor is byte-exact under MSVC and the
 * flycast headless rebuild (which uses cl.exe) is safe. Usage: _tickhash <seed> <N> [out.bin] */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
void tick_entry(Sh4Ctx *c);
void call_addr(Sh4Ctx *c, u32 a);
static long g_calls = 0;
int  mc_call_guard(void){ return (++g_calls > 50000000L) ? 1 : 0; }
void mc_unknown_call(u32 a){ (void)a; }
void mc_unk_regs(u32 *r){ (void)r; }
u32  mc_curfn = 0; void mc_push(u32 a){ (void)a; } void mc_pop(void){}
void mc_note_read(u32 a, u32 n){ (void)a; (void)n; }   /* MC_RTRAP hook (no value effect) */
static u8 ram[16u * 1024u * 1024u];
int main(int argc, char **argv){
    if (argc < 2){ printf("usage: _tickhash <seed> [N] [out.bin]\n"); return 2; }
    const char *rp = argv[1]; int N = argc > 2 ? atoi(argv[2]) : 300; const char *out = argc > 3 ? argv[3] : 0;
    FILE *f = fopen(rp, "rb"); if (!f){ printf("no %s\n", rp); return 2; } fread(ram, 1, RAM_SIZE, f); fclose(f);
    Sh4Ctx c; memset(&c, 0, sizeof c); c.ram = ram; c.r[15] = 0x8CFF0000u;
    for (int i = 0; i < N; i++){
        memset(ram + 0x2681DC, 0, 0x28);                 /* neutral Input_DEC, both slots */
        *(u32*)(ram + 0x1F9D98) = 0; *(u32*)(ram + 0x1F9D94) = 16;   /* arena frame-setup */
        tick_entry(&c);
        ram[0x289F80] = ram[0x289F81] = ram[0x289F82] = ram[0x289F83] = 0;  /* counter reset */
    }
    unsigned long long h = 1469598103934665603ULL;       /* FNV-1a over the game-state regions */
    for (unsigned o = 0x268340; o < 0x268340 + 6u * 0x5A4u; o++){ h ^= ram[o]; h *= 1099511628211ULL; }
    for (unsigned o = 0x289000; o < 0x28A000u; o++){ h ^= ram[o]; h *= 1099511628211ULL; }
    for (unsigned o = 0x26A520; o < 0x26A580u; o++){ h ^= ram[o]; h *= 1099511628211ULL; }
    printf("N=%d gamestate_hash=%016llX\n", N, h);
    if (out){ FILE *o2 = fopen(out, "wb"); if (o2){ fwrite(ram, 1, RAM_SIZE, o2); fclose(o2); } }
    return 0;
}
