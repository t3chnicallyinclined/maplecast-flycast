/*
	MapleCast RENDER-REPLICA — PHASE 0 validation experiment.

	docs/RENDER-REPLICA-PLAN.md §4. The go/no-go gate for the render-only SH4
	replica: empirically prove (or kill) the claim that MVC2's per-frame render
	read-set is closed over {GSTA-shipped fields} ∪ {static snapshot}.

	WHAT IT DOES (server-side, in-process, env-gated, determinism-safe):
	  Once per in-match frame, at the SH4-PAUSED emu-loop boundary (right after
	  runInternal() returns — the SAME proven-safe context the rollback
	  deferred-rewind and the Oracle probe-reload use), it:
	    1. snapshots the static RAM regions the render code reads + the full SH4
	       Sh4Context to side buffers;
	    2. restores the snapshot, then patches ONLY the GSTA-shipped char-struct
	       fields (Tier 1: pos/scale/facing/flip; Tier 2 also sprite_id) from the
	       just-captured live values;
	    3. resets the display-list cursor (GameGlobalPointer+0x24) and restores the
	       non-idempotent accumulators (0x8C26A974/0x8C26A518) + template scratch;
	    4. re-invokes MVC2's own render entry on the INTERPRETER, driven op-by-op
	       (ctx.pc=ENTRY, ctx.pr=SENTINEL, Step() until pc==SENTINEL), counting ops,
	       capturing the re-emitted RAM display list;
	    5. diffs the re-run display list vs the live frame's (byte-for-byte);
	    6. RESTORES every touched RAM region + the full Sh4Context byte-for-byte, so
	       the authoritative guest is bit-identical before/after (READ-ONLY w.r.t.
	       the authoritative machine);
	    7. appends per-frame stats to /dev/shm/mc_replica_phase0.jsonl.

	DETERMINISM CONTRACT (docs/RENDER-REPLICA-PLAN.md §4.5):
	  * env-gated: a literal no-op when MAPLECAST_REPLICA_PHASE0 is unset → prod is
	    byte-stock; the MAPLECAST_DUMP_TA determinism rig must still pass with the
	    gate OFF.
	  * runs ONLY at the SH4-paused boundary on the emu thread; never inside a
	    compiled block, never racing the SH4 thread.
	  * full save→re-render→restore: the authoritative ctx + RAM are restored
	    byte-for-byte, so the experiment cannot perturb the deterministic mirror
	    stream / the next authoritative frame.

	Default OFF. Tier 2 is additionally guarded behind MAPLECAST_REPLICA_PHASE0_TIER2
	(best-effort; a crash in the re-render is caught and logged, never fatal).
*/
#pragma once
#include "types.h"

namespace maplecast_replica_phase0
{

// True iff MAPLECAST_REPLICA_PHASE0 is set. When false EVERY entry point below is
// a literal no-op (checked first thing) so prod is byte-stock.
bool active();

// RENDER-THREAD watcher half. Call once per frame from serverPublish() AFTER the
// live ta_parse/flush work. Cheap: if we're in-match and a run isn't already
// pending, it (a) latches the just-completed frame's GSTA field values from the 6
// char structs into a side buffer (read-only) and (b) sets an internal pending
// flag so the emu-loop boundary performs the re-render. It NEVER drives the SH4,
// never touches the block cache, never writes guest state — safe even when
// serverPublish runs synchronously inside an SH4 STARTRENDER write (non-threaded).
// No-op when the gate is off / not in-match. `liveCtx` is the just-published
// TA_context (forward-declared void*; cast to TA_context* in the .cpp) used only
// for the optional ta_parse cross-check; may be null.
void onServerPublish(void* liveCtx, u32 frame);

// Peek the pending flag (does NOT consume it). Call from Emulator::vblank() (emu
// thread): when true, Stop() the SH4 so runInternal() returns to the emu-loop
// boundary where runAtBoundary() executes with the SH4 fully paused — the SAME
// Stop()->boundary->Start() dance the rollback deferred-rewind / probe-reload use.
bool runPending();

// SH4-THREAD apply half. Call at the emu-loop boundary right AFTER runInternal()
// returns (SH4 fully paused). If a run is pending it performs the full
// snapshot→patch→re-render→diff→restore cycle and clears the pending flag.
// Returns TRUE iff it flushed/needs a block-cache reset (it does NOT — see below),
// matching the mc_probeApplyReload() contract so the caller can uniformly decide
// whether to ResetCache(). Phase 0 drives the re-render on the INTERPRETER, which
// shares Sh4cntx but compiles no dynarec blocks, so it returns FALSE (no flush
// needed) — the authoritative dynarec cache is untouched. No-op (returns false)
// when the gate is off / nothing pending.
bool runAtBoundary();

}
