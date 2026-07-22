/* SHADOW-EXECUTOR RUNNER — the C bridge between the transpiled game-tick executor
 * (gen_tick_all.c) and the flycast server-side shim (maplecast_shadow_exec.cpp).
 *
 * This is a SEPARATE translation unit that includes ONLY sh4ctx.h (NOT flycast's
 * types.h) so the `RAM_SIZE` macro collision is isolated — the C++ shim sees flycast's
 * RAM_SIZE, this file sees the executor's. The shim hands us a raw 16 MB guest-RAM image
 * (a copy of mem_b); we run one game-logic tick on it in place, byte-exact vs flycast.
 *
 * Provides the executor's runtime deps (call guard + dispatch stubs) once, for the whole
 * transpiled unit — do NOT also link test_tick.c / validate_multiframe.c / chain_render.c
 * into the same target (they define these too). */
#include "sh4ctx.h"

void tick_entry(Sh4Ctx *c);
void call_addr(Sh4Ctx *c, u32 addr);   /* address dispatch, defined in gen_tick_all.c */

/* runtime deps referenced by gen_tick_all.c */
static long g_calls = 0;
int  mc_call_guard(void){ return (++g_calls > 5000000L) ? 1 : 0; }
void mc_unknown_call(u32 a){ (void)a; }       /* non-transpiled dispatch target -> no-op */
void mc_unk_regs(u32 *r){ (void)r; }
u32  mc_curfn = 0;
void mc_push(u32 a){ (void)a; }
void mc_pop(void){}

/* NON-RAM READ CENSUS — answers "does the in-match tick ever read ROM / hardware?" directly.
 * Active only when the shadow build compiles with -DMC_RTRAP (sh4ctx.h calls mc_note_read for
 * any read outside area-3 RAM). Defined unconditionally so link never fails; when MC_RTRAP is
 * off the hook is never called and the count stays 0. */
static long g_nonram = 0;
static u32  g_nonram_first = 0, g_nonram_fn = 0;
static u32  g_nonram_set[32]; static int g_nonram_ndistinct = 0;
void mc_note_read(u32 a, u32 n){ (void)n;
    if(!g_nonram) { g_nonram_first = a; g_nonram_fn = mc_curfn; }  /* which game fn read outside RAM */
    g_nonram++;
    for(int i=0;i<g_nonram_ndistinct;i++) if(g_nonram_set[i]==a) return;
    if(g_nonram_ndistinct<32) g_nonram_set[g_nonram_ndistinct++]=a;
}
long mc_shadow_last_nonram_reads(void){ return g_nonram; }
u32  mc_shadow_last_nonram_addr(void){ return g_nonram_first; }
u32  mc_shadow_last_nonram_fn(void){ return g_nonram_fn; }
int  mc_shadow_nonram_distinct(u32 *out, int max){
    int k = g_nonram_ndistinct < max ? g_nonram_ndistinct : max;
    for(int i=0;i<k;i++) out[i]=g_nonram_set[i]; return k;
}

/* Run ONE game-logic tick on a 16 MB guest-RAM image, in place.
 * `ram` must be RAM_SIZE bytes, little-endian, the mem_b layout (offset = guestAddr&0xFFFFFF).
 * Returns the number of dispatches (a cheap liveness signal; ~2000 for a real in-match tick). */
long mc_shadow_run_tick(unsigned char *ram){
    g_calls = 0;
    g_nonram = 0; g_nonram_first = 0;   /* reset the non-RAM read census for this tick */
    Sh4Ctx c;
    for (unsigned i = 0; i < sizeof c; i++) ((unsigned char*)&c)[i] = 0;
    c.ram = ram;
    c.r[15] = 0x8CFF0000u;      /* scratch stack top, as in Test A / validate_multiframe */
    tick_entry(&c);
    return g_calls;
}

/* Rebuild the render-walk projection @0x2D6AD8 from the LIVE matrix-stack descriptors, POST-tick
 * — the LEGIT render-state rebuild (chain_render "tickproj"). The game-logic tick transiently
 * zeroes the render matrices; predict carries the stale 64B proj (the "tickcam" hack), which is
 * fine 1-tick-off-a-fresh-base but COMPOUNDS over a continuous drive until render_frame follows a
 * bad transform and segfaults. Recomposing every frame keeps the render state valid. Returns the
 * composed proj[0] (float bits) as a liveness signal — 0 means the composer didn't run/deposit. */
u32 mc_shadow_compose_proj(unsigned char *ram){
    Sh4Ctx c;
    for (unsigned i = 0; i < sizeof c; i++) ((unsigned char*)&c)[i] = 0;
    c.ram = ram;
    c.r[15] = 0x8CFF0000u;
    call_addr(&c, 0x8c1216c0u);   /* compose proj -> 0x2D6AD8 */
    return r32(&c, 0x8C2D6AD8u);
}
