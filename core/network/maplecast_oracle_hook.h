/*
	MapleCast Frame Oracle — LIVE compile-time block-entry hook (EXACT attribution).

	Unlike the position-correlation MAPLECAST_FRAME_ORACLE path (maplecast_mirror.cpp
	serverPublish, post-ta_parse, heuristic nearest-object), this captures the EXACT
	per-object quad attribution by reading the SH4 guest registers at two block
	entries the recompiler hooks:
	  0x8C03093C  loc_8c03093c  "Render Main Sprite"  — r4 = the object node (begin)
	  0x8C033E90  loc_8c033e90  "reading sprite data" — r8 = texptr, r12 = palptr,
	                                                     r14 = display-buffer cursor (one quad)
	Each 0x8C033E90 quad is attributed to the object opened by the most recent
	0x8C03093C — PERFECT per-quad attribution, no overlap heuristic.

	Mechanism: a compile-time GenCall injected by core/rec-x64/rec_x64.cpp
	BlockCompiler::compile() in the block prologue for the hooked PCs, AFTER
	sub(rsp, STACK_ALIGN) and BEFORE regalloc.DoAlloc(block) (all guest regs are
	coherent in Sh4cntx.r[] there). Read-only: reads Sh4cntx.r[] + guest RAM via
	addrspace::read*; never writes guest state, never raises an exception ->
	determinism-safe + perf-trivial.

	Gated default-OFF by env MAPLECAST_FRAME_ORACLE_HOOK. Output:
	/dev/shm/mc_oracle_hook.jsonl (FRAME-ORACLE-SPEC §Output schema, but exactly
	attributed). Reuses the readAllDrawn/readHotspot/GetMemPtr patterns.
*/
#pragma once
#include "types.h"

namespace maplecast_oracle_hook
{

// Set once at init from getenv("MAPLECAST_FRAME_ORACLE_HOOK"). The recompiler
// reads this before deciding to inject the GenCall, so when unset the prologue
// is byte-for-byte the stock prologue (prod unaffected).
extern bool mc_oracleHookEnabled;

// One-time init from env. Safe to call multiple times.
void mc_oracleInit();

// Fast membership test used by the recompiler at COMPILE time (once per block
// compile, not per execution) to decide whether to inject the hook call.
bool mc_isHookedPC(u32 pc);

// DYNACALL block-entry handler. Injected as the first call in the recompiled
// block for a hooked PC; `pc` is block->vaddr. Reads Sh4cntx.r[] + guest RAM.
void DYNACALL mc_oracle_blockEntry(u32 pc);

// Called once per frame from serverPublish() to flush the buffered per-object
// state to /dev/shm/mc_oracle_hook.jsonl and reset the buffer.
//
// `ctx` is the just-completed frame's TA_context (the same one serverPublish is
// publishing). frameFlush runs ta_parse(ctx) here to recover the per-frame SCREEN
// quads (verts with real screen x,y) and attributes each to the OBJ_BEGIN object
// nearest its on-screen position — the per-object SCREEN-quad anchor.
//
// Forward-declared as void* to avoid pulling ta_ctx.h into this header; the .cpp
// casts it back to TA_context*. No-op when the hook is disabled / ctx is null.
void mc_oracle_frameFlush(void* ctx, u32 frame);

}
