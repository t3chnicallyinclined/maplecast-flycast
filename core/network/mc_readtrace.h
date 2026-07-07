// mc_readtrace.h — STEP 2 read-set delta trace (adversarial RE adjudication).
//
// Measures the TRANSITIVE data-read closure of the MVC2 render driver
// loc_8c030858 ("Render Characters?", 0x8C030858) over ONE frame, to settle
// whether that closure is bounded to resident/shippable state (char structs,
// GFX art, camera, game-state, render scratch) or fans out into unshippable
// game-loop state (input pad / AI / physics / prior-frame globals).
//
// MECHANISM (fastmem-proof): boot in DYNAREC (fast — reaches a match frame in
// seconds via autoload), then at a trigger frame FLIP config::DynarecEnabled to
// false at runtime (Stop -> ResetCache -> Start, the proven Oracle-probe-reload
// pattern) so ONE frame renders under the interpreter. In interpreter mode EVERY
// guest data read — interpreter opcodes AND the SH4RECOMP_BLOCKS static blocks
// (ReadMem32_nommu) — funnels through addrspace::readt<T>. We gate readt with
// g_armed and log the DISTINCT address. Arm on the driver's block entry; disarm
// when control returns to the caller (pc==retPc). Reads are scoped to the
// driver's own call subtree via the SH4 stack pointer (r15 < spEntry), excluding
// the sibling HUD pass. (Interpreter-from-BOOT was ~10-50x too slow to reach a
// match frame — this flips only for the traced frame.)
//
// READ-ONLY, gated OFF by default (MAPLECAST_READTRACE unset) — zero effect on
// prod. Same determinism discipline as the Oracle hooks.
#pragma once
#include "types.h"

namespace mc_readtrace {

extern bool g_enabled;   // MAPLECAST_READTRACE set at boot
extern bool g_armed;     // currently inside the driver subtree (hot-path gate)

// Parse env (MAPLECAST_READTRACE, MAPLECAST_READTRACE_FRAME). Boot stays DYNAREC.
void init();

// Per-frame from Emulator::vblank() (emu thread). Counts frames; at the trigger
// frame requests the dynarec->interpreter flip. No-op when disabled.
void onFrame();

// vblank() checks this: true once the trigger frame is reached and the flip has
// not yet been applied -> vblank Stop()s the SH4 so Run() returns to the emu-loop
// boundary where applyFlip() can safely ResetCache().
bool flipStopPending();

// Emu-loop boundary (SH4 paused): if a flip is pending, set DynarecEnabled=false
// + arm the trace, return true so the caller ResetCache()+Start()s into the
// interpreter for the traced frame. Returns false otherwise.
bool applyFlip();

// Interpreter Run loop, once per dispatch, BEFORE the block/op executes, with the
// block-start PC. Arm (at 0x8C030858) / disarm (return to caller). No-op unless
// enabled + flipped.
void onPc(u32 pc);

// addrspace::readt<T> when g_armed. Logs the distinct guest address if the read
// is within the driver's call subtree (r15 < spEntry).
void onRead(u32 addr);

// addrspace::writet<T> when g_armed. Records writes within the driver closure so
// the dump distinguishes build-then-read scratch (written earlier this closure)
// from a genuine external read dependency.
void onWrite(u32 addr);

}
