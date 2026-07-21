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

/* write-trap + shadow call stack to find the memcpy's caller chain */
u32 mc_curfn = 0;
static u32 g_stack[256]; static int g_sp = 0;
static u32 g_disp[400000]; static int g_ndisp = 0;   /* every dispatch, for CF-divergence diff */
void mc_push(u32 a) {
    if (g_sp < 256) g_stack[g_sp] = a; g_sp++;
    if (g_ndisp < 400000) g_disp[g_ndisp++] = a;
}
void mc_pop(void) { if (g_sp > 0) g_sp--; }
static u32 g_snap[256]; static int g_snapn = -1; static u32 g_snapraw = 0;
static long g_trapcnt = 0;
void mc_wtrap(u32 a) {
    u32 off = a & 0x00FFFFFFu;
    if (off >= 0x2d6a00u && off < 0x2d6c80u) {
        g_trapcnt++;
        if (g_snapn < 0) {                            /* snapshot the call chain on first bad write */
            g_snapraw = a; g_snapn = g_sp < 256 ? g_sp : 256;
            for (int i = 0; i < g_snapn; i++) g_snap[i] = g_stack[i];
        }
    }
}

static u32 g_unkregs[16]; static int g_haveunkregs = 0;
void mc_unk_regs(u32 *r) { if (!g_haveunkregs) { for (int i = 0; i < 16; i++) g_unkregs[i] = r[i]; g_haveunkregs = 1; } }

/* capture xf[]/fr[]/r4/fpscr at EVERY call to the two matrix-save fns, so we can pin
 * which specific save (dest 0x2D6AD8 / 0x2152E0) holds the divergent bank content. */
typedef struct { u32 fn, r4, fpscr; float xf[16], fr[16]; } SaveCap;
static SaveCap g_caps[64]; static int g_ncaps = 0;
/* also log every (dropped) call to the back-bank LOADER 8c120220: r4 = source ptr,
 * plus the 16 source floats from RAM[r4] — one should be a zero matrix (-> 0x2D6AD8). */
typedef struct { u32 r4; float src[16]; int inram; } LoadCap;
static LoadCap g_lds[16]; static int g_nlds = 0;
static float _rf(Sh4Ctx *c, u32 a){ u32 off=a&0x00FFFFFFu; if(off+4>RAM_SIZE) return 0.f/0.f; u32 w=r32(c,a); return *(float*)&w; }
/* ordered interleave of loads(L)/saves(S) across the matrix subtree */
static char g_seq[256][40]; static int g_nseq = 0;
static void seqlog(const char*tag,u32 fn,u32 addr){ if(g_nseq<256){ snprintf(g_seq[g_nseq++],40,"%s %06X@%08X",tag,fn&0xffffff,addr); } }
void mc_dispatch_hook(u32 a, Sh4Ctx *c) {
    if (a==0x8c120220u) seqlog("L", a, c->r[4]);
    if (a==0x8c11fb80u||a==0x8c11fa80u) seqlog("S", a, c->r[4]);
    if ((a == 0x8c11fb80u || a == 0x8c11fa80u || a == 0x8c1204f0u) && g_ncaps < 64) {
        SaveCap *s = &g_caps[g_ncaps++];
        s->fn = a; s->r4 = c->r[4]; s->fpscr = c->fpscr;
        for (int i = 0; i < 16; i++) { s->xf[i] = c->xf[i]; s->fr[i] = c->fr[i]; }
    }
    if (a == 0x8c120220u && g_nlds < 16) {
        LoadCap *l = &g_lds[g_nlds++]; u32 r4 = c->r[4]; l->r4 = r4;
        l->inram = ((r4 & 0x1C000000u) == 0x0C000000u);
        for (int i = 0; i < 16; i++) l->src[i] = _rf(c, r4 + i*4);
    }
}
static u32 g_usnap[256]; static int g_usnapn = -1; static u32 g_usnaptgt = 0;
void mc_unknown_call(u32 a) {
    if (!g_unknown) {
        g_first_unknown = a;
        g_usnaptgt = a; g_usnapn = g_sp < 256 ? g_sp : 256;   /* chain to the first garbage call */
        for (int i = 0; i < g_usnapn; i++) g_usnap[i] = g_stack[i];
    }
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
    { FILE *of = fopen("executor_out.bin", "wb"); if (of) { fwrite(ram, 1, RAM_SIZE, of); fclose(of); } }
    { FILE *df = fopen("_exec_disp.bin", "wb"); if (df) { fwrite(g_disp, 4, g_ndisp, df); fclose(df);
        printf("wrote _exec_disp.bin (%d dispatches)\n", g_ndisp); } }

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

__attribute__((destructor)) static void _trapdump(void){
  if(g_trapcnt){
    printf("WTRAP 0x44xxxx: %ld bad writes, first raw addr=0x%08X\n", g_trapcnt, g_snapraw);
    printf("  call chain (outer->inner):");
    for(int i=0;i<g_snapn;i++) printf(" %08x", g_snap[i]);
    printf("\n");
  }
  if(g_usnapn>=0){
    printf("first UNKNOWN call target=0x%08X, chain (outer->inner):", g_usnaptgt);
    for(int i=0;i<g_usnapn;i++) printf(" %08x", g_usnap[i]);
    printf("\n");
  }
  if(g_haveunkregs){
    printf("  regs at 1st unknown: r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x\n",
      g_unkregs[0],g_unkregs[1],g_unkregs[2],g_unkregs[3],g_unkregs[4],g_unkregs[5],g_unkregs[6]);
  }
  for(int k=0;k<g_ncaps;k++){
    SaveCap *s=&g_caps[k];
    printf("SAVE#%d fn=%08X r4=0x%08X fpscr=0x%08X (FR=%d SZ=%d)\n",
      k, s->fn, s->r4, s->fpscr, (s->fpscr>>21)&1, (s->fpscr>>20)&1);
    printf("   xf:"); for(int i=0;i<16;i++) printf(" %.4g", s->xf[i]);
    printf("\n   fr:"); for(int i=0;i<16;i++) printf(" %.4g", s->fr[i]); printf("\n");
  }
  for(int k=0;k<g_nlds;k++){
    LoadCap *l=&g_lds[k];
    printf("LOAD8c120220 #%d r4=0x%08X inram=%d src:", k, l->r4, l->inram);
    for(int i=0;i<16;i++) printf(" %.4g", l->src[i]); printf("\n");
  }
  printf("SEQ(%d):\n",g_nseq); for(int k=0;k<g_nseq;k++) printf("  %02d %s\n",k,g_seq[k]);
}
