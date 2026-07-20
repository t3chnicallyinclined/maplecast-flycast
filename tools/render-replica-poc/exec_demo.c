/* exec_demo.c — the thin MVC2 SH4 game-tick executor, compiled to WASM for the
 * browser. Bundles four transpiled + byte-exact-verified game-tick functions and
 * a minimal guest-RAM poke/peek interface so a web page can run real MVC2 SH4
 * game code (RNG, phase accumulators, normalized-random-float, combined
 * button-mask) live, with no ROM and no emulator. This is the portability endgame
 * of the executor: the same C that runs byte-exact vs flycast natively runs in the
 * browser as WASM (as the opcode-parity spike first proved). */
#include "sh4ctx.h"

static unsigned char ram[RAM_SIZE];
static Sh4Ctx C;

/* transpiled game-tick functions (auto-generated via lift.py/codegen.py) */
void rng_e730(Sh4Ctx *c);
void accum_3015c(Sh4Ctx *c);
void nrand_e750(Sh4Ctx *c);
void btnmask_2d1c0(Sh4Ctx *c);
void knockback_4f974(Sh4Ctx *c);
/* nrand_e750 bsr's the RNG -> link the sub_<hex> convention to it */
void sub_8c11e730(Sh4Ctx *c) { rng_e730(c); }

#define EXPORT(n) __attribute__((export_name(#n)))

EXPORT(boot) void boot(void) {
    for (u32 i = 0; i < RAM_SIZE; i++) ram[i] = 0;
    C.ram = ram;
    C.r[15] = 0x8CFF0000u;
}
EXPORT(poke8)  void poke8(u32 a, u32 v)  { w8(&C, a, v); }
EXPORT(poke16) void poke16(u32 a, u32 v) { w16(&C, a, v); }
EXPORT(poke32) void poke32(u32 a, u32 v) { w32(&C, a, v); }
EXPORT(peek8)  u32  peek8(u32 a)  { return r8u(&C, a); }
EXPORT(peek16) u32  peek16(u32 a) { return r16u(&C, a); }
EXPORT(peek32) u32  peek32(u32 a) { return r32(&C, a); }

/* zero the scratch GP regs before each call (leaves are self-contained) */
static void reset_regs(void) {
    for (int i = 0; i < 15; i++) C.r[i] = 0;
    C.r[15] = 0x8CFF0000u;
    for (int i = 0; i < 16; i++) C.fr[i] = 0.0f;
    C.macl = C.mach = 0;
}

EXPORT(run_rng)     u32 run_rng(void)     { reset_regs(); rng_e730(&C);     return C.r[0]; }
EXPORT(run_accum)   void run_accum(void)  { reset_regs(); accum_3015c(&C);              }
EXPORT(run_btnmask) u32 run_btnmask(void) { reset_regs(); btnmask_2d1c0(&C); return C.r[0]; }
EXPORT(run_nrand)   float run_nrand(void) { reset_regs(); nrand_e750(&C);   return C.fr[0]; }

/* symmetric pos_x knockback: r4=P1 char, r5=P2 char, r6=2-float vector, fr4=scalar.
 * The page pokes the two chars' pos_x/facing + the vector + 0x8c28963c beforehand. */
EXPORT(run_knockback) void run_knockback(float mag) {
    reset_regs();
    C.r[4] = 0x8C268340u; C.r[5] = 0x8C2688E4u; C.r[6] = 0x8CFD0000u;
    C.fr[4] = mag;
    knockback_4f974(&C);
}

EXPORT(get_r)  u32   get_r(int i)  { return C.r[i & 15]; }
EXPORT(get_fr) float get_fr(int i) { return C.fr[i & 15]; }
