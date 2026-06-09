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

// === GENERIC PROBE v2 — no-restart live reload =============================
// mc_probeCheckReload(): RENDER-THREAD watcher. Call once/frame from a frame
// boundary (serverPublish). Cheap: throttled stat() of the probe config; on a
// changed mtime it sets an internal pending flag. NEVER parses the probe table
// and NEVER touches the block cache (so it is safe even when serverPublish runs
// synchronously inside an SH4 block in non-threaded mode). No-op when the probe
// is disabled.
void mc_probeCheckReload();
// mc_probeReloadPending(): peek the pending flag (does NOT consume it). Call from
// Emulator::vblank() (emu thread, per-frame): when true, Stop() the SH4 so Run()
// returns to the emu-loop boundary where mc_probeApplyReload()+ResetCache() run —
// the SAME Stop()->boundary->Start() dance the rollback deferred-rewind uses.
bool mc_probeReloadPending();
// mc_probeApplyReload(): SH4-THREAD reload. Call at the emu-loop boundary right
// AFTER runInternal() returns (SH4 fully paused — the same context the rollback
// deferred-rewind uses for bm_Reset/ResetCache). If a reload is pending it
// re-parses the config into the live probe table and returns TRUE, meaning the
// caller MUST flush the SH4 block cache (getSh4Executor()->ResetCache()) so the
// recompiler re-runs mc_isHookedPC against the new probe-PC set. Returns false
// (no flush needed) when nothing changed or the probe is disabled.
bool mc_probeApplyReload();

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

// CHARQ — the DEFINITIVE per-part body-quad capture point. Called from
// rend_start_render() (core/hw/pvr/Renderer_if.cpp, after the isRTT stamp, BEFORE
// QueueRender) for EVERY STARTRENDER context. MVC2 emits multiple STARTRENDER passes
// per video frame; flycast's single-slot QueueRender DROPS all but one (on MVC2 the
// surviving pass is HUD/composite — the per-part CHARACTER body quads live in a pass
// QueueRender recycles). This is the ONLY point those character quads exist, so we
// capture here UPSTREAM of the drop.
//
// READ-ONLY + determinism-safe: ta_parse(ctx,true) builds ctx->rend exactly like
// norend::Process — it never writes guest state, never enqueues, never touches rqueue.
// The real render path re-parses for the wire. Per STARTRENDER it logs the [CHARPASS]
// confirmation (isRTT/op/pt/tr/sprite/bodyBand/vframe), discriminates the character
// pass, and routes it into the Oracle capture path (collectScreenQuads +
// attributeScreenQuads + JSONL + the CHARQ accessor), per-vframe deduped.
//
// Gated MAPLECAST_CHARQ + in-match (0x8C289624). No-op when CHARQ disabled / ctx null.
// `ctx` is forward-declared void* (cast to TA_context* in the .cpp) — same as above.
void mc_oracle_charPassCapture(void* ctx);

// ===========================================================================
// CHARQ accessor (Phase 2 consumer — maplecast_mirror.cpp serverPublish).
//
// After mc_oracle_frameFlush() runs for a frame, the per-object identity table
// and the kept-sprite-quad -> object mapping for THAT frame are available via
// these accessors. CALL ORDERING: serverPublish() invokes mc_oracle_frameFlush()
// (which runs ta_parse + collectScreenQuads + attributeScreenQuads and populates
// the statics) BEFORE the CHARQ emit block, so the statics are ready when CHARQ
// reads them. The statics are reset at the END of frameFlush, so CHARQ must read
// them within the SAME serverPublish call, after the flush.
//
// The "kept-sprite-quad ordinal" is the index into the sprite-filtered quad list
// that collectScreenQuads builds — same isSprite predicate, same op->pt->tr walk
// order. CHARQ filters the live ta_parse PolyParam lists with the IDENTICAL
// predicate/order, so its Nth kept sprite poly == ordinal N here.

// Per-object identity (a flattened view of the internal Obj for the CHARQ wire).
struct CharqObj {
	u32   node;            // object node base
	int   sprite_id;       // node+0x144 & 0x7FFF (mask applied here)
	int   cid;             // character_id: body = node+0x1; satellite = ownerCid
	float screen_x;        // node+0xE0 (refreshed to current-frame value in frameFlush)
	float screen_y;        // node+0xE4
	bool  isSatellite;     // came through the effect/satellite path (loc_8c030af8)
	int   ownerSlot;       // 0..5 owning body slot; -1 = none/global
	int   ownerCid;        // owner's character_id; -1 = unknown
};

// Returns the per-frame object table (count via out param). Pointer is to the
// internal static — valid only until the next frameFlush. NULL/0 if hook inactive.
const CharqObj* mc_oracle_objects(int* outCount);

// Returns the kept-sprite-quad ordinal -> object-index map (count via out param).
// map[ordinal] = index into mc_oracle_objects()[] for that sprite quad, or -1 if
// the quad was not attributed to any object (unassigned bucket). The ordinal is
// the Nth quad passing collectScreenQuads' isSprite filter in op->pt->tr order.
const int* mc_oracle_quadObjMap(int* outCount);

}
