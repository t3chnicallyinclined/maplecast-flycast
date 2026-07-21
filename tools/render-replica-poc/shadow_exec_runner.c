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

/* runtime deps referenced by gen_tick_all.c */
static long g_calls = 0;
int  mc_call_guard(void){ return (++g_calls > 5000000L) ? 1 : 0; }
void mc_unknown_call(u32 a){ (void)a; }       /* non-transpiled dispatch target -> no-op */
void mc_unk_regs(u32 *r){ (void)r; }
u32  mc_curfn = 0;
void mc_push(u32 a){ (void)a; }
void mc_pop(void){}

/* Run ONE game-logic tick on a 16 MB guest-RAM image, in place.
 * `ram` must be RAM_SIZE bytes, little-endian, the mem_b layout (offset = guestAddr&0xFFFFFF).
 * Returns the number of dispatches (a cheap liveness signal; ~2000 for a real in-match tick). */
long mc_shadow_run_tick(unsigned char *ram){
    g_calls = 0;
    Sh4Ctx c;
    for (unsigned i = 0; i < sizeof c; i++) ((unsigned char*)&c)[i] = 0;
    c.ram = ram;
    c.r[15] = 0x8CFF0000u;      /* scratch stack top, as in Test A / validate_multiframe */
    tick_entry(&c);
    return g_calls;
}
