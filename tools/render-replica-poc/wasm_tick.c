/* WHOLE-TICK EXECUTOR -> WASM. The browser-rollback bridge: the same transpiled game-tick
 * (gen_tick_all.c) that is byte-exact on the server, compiled to wasm32-freestanding so a
 * browser can run it for client-side prediction/rollback (no SH4, no emulator).
 *
 * Exposes a flat 16MB guest-RAM buffer to JS: write a frame snapshot into ram_ptr(), call
 * run_tick() (mutates it in place = one game-logic frame), read the result back. Byte-exact
 * vs flycast means the browser can advance the game state locally from the last server frame
 * plus the local input, then reconcile when the authoritative frame arrives.
 *
 *   zig cc -target wasm32-freestanding -O2 -I. gen_tick_all.c shadow_exec_runner.c wasm_tick.c
 *          -nostdlib -Wl,--no-entry -Wl,--export-dynamic -Wl,--initial-memory=33554432 -o exec_tick.wasm
 */
#include "sh4ctx.h"

extern long mc_shadow_run_tick(unsigned char *ram);
extern long mc_shadow_last_nonram_reads(void);

static unsigned char g_ram[16u * 1024u * 1024u];   /* the guest RAM the browser fills + reads */

/* sin/cos: instrumented so we can tell whether the game-tick even reaches them. If the
 * counter stays 0 across the verification frames, they're in an unhit branch and the
 * placeholder body is irrelevant to byte-exactness. If it fires, we'll need a
 * flycast-libm-matching implementation before trusting FP-heavy frames. */
static long g_trig_calls = 0;
float sinf(float x){ g_trig_calls++; return x; }   /* placeholder — validated unused below */
float cosf(float x){ g_trig_calls++; return x; }

__attribute__((export_name("ram_ptr")))   unsigned char *ram_ptr(void)   { return g_ram; }
__attribute__((export_name("ram_size")))  unsigned        ram_size(void)  { return (unsigned)sizeof g_ram; }
__attribute__((export_name("run_tick")))  long            run_tick(void)  { g_trig_calls=0; return mc_shadow_run_tick(g_ram); }
__attribute__((export_name("nonram")))    long            nonram(void)    { return mc_shadow_last_nonram_reads(); }
__attribute__((export_name("trig_calls")))long            trig_calls(void){ return g_trig_calls; }
