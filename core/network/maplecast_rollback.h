/*
	MapleCast Rollback Ring — A.4 of the Phase 1 rollback prediction plan.

	Builds on flycast's existing page-delta + dc_serialize hybrid (the same
	pattern ggpo.cpp:439-561 uses for P2P netplay rollback) but exposes it
	as a non-GGPO API so MapleCast's predictor can use it without an active
	GGPO session.

	Architecture and rationale: docs/ROLLBACK-RING-DESIGN.md
	Audit prerequisites:        docs/DC-SERIALIZE-AUDIT.md (priority-1 done in V59)
	Phase plan:                  docs/ROLLBACK-PREDICTION.md

	Threading model:
	  * Single writer: the SH4 emu thread (saveFrame() called from
	    serverPublish, which runs per emu frame).
	  * Single reader during rewind: the SH4 emu thread itself
	    (rewindToFrame() interrupts forward progress to load).
	  * Lock-free SPSC ring — no mutex on the hot save path.

	Memory model:
	  * 10 slots (matches GGPO MAX_PREDICTION_FRAMES + 2). Bounded depth so
	    the arena size is fixed regardless of how long the match runs.
	  * Pre-allocated 40 MB arena (4 MB per slot) — never malloc on the
	    save path. Avoids the GGPO vectorwar-style allocator churn that
	    would be 480 MB/s at our state size.
*/
#pragma once
#include <cstdint>

namespace maplecast_rollback
{

// Ring depth. Matches GGPO's MAX_PREDICTION_FRAMES + 2 = 10. Keep aligned
// with the predictor's max prediction depth — A.6 will use this.
constexpr int RING_DEPTH = 10;

// Initialize the ring. Allocates the 40 MB arena, sets memwatch::mirrorActive
// so the page-fault watcher starts tracking writes. Idempotent. Returns
// false if allocation failed.
bool init();

// Shutdown. Frees the arena, clears memwatch::mirrorActive. Idempotent.
void shutdown();

// Capture the current SH4 + page-delta state into ring slot[frame % RING_DEPTH].
// Called from serverPublish() once per emu frame when MAPLECAST_ROLLBACK_RING=1.
// No-op if init() hasn't been called.
void saveFrame(uint64_t frame);

// Restore state to the given frame. Walks the ring backward from
// mostRecentSaved() down to frame, restoring page deltas + dc_deserialize.
// Returns false if frame is older than oldestAvailable() or newer than
// mostRecentSaved().
//
// SAFETY: this calls emu.loadstate() which does bm_Reset() / ResetCache()
// — destructive ops on the SH4 dynarec. MUST be called from a context
// where the SH4 is fully paused (executor->Stop() called and Run() has
// returned). For async-safe usage from inside SH4 execution (vblank
// handler), use requestRewindToFrame() instead.
bool rewindToFrame(uint64_t frame);

// Async-safe rewind request. Sets a pending flag; the actual loadstate
// happens later when the SH4 thread is at a safe pause point. Caller can
// fire this from inside vblank() — vblank will then signal the SH4
// executor to Stop(), Run() will return, and the emu loop will execute
// the queued rewind via executePendingRewind() before restarting the SH4.
void requestRewindToFrame(uint64_t targetFrame);

// True if a rewind has been requested but not yet executed.
bool pendingRollback();

// Execute the queued rewind. MUST be called from the emu thread loop
// AFTER getSh4Executor()->Run() has returned (i.e., SH4 is paused).
// Returns true on success, false if no rollback was pending or if the
// rewind itself failed.
bool executePendingRewind();

// Oldest frame currently in the ring. Frames older than this have been
// overwritten by newer saves. Returns UINT64_MAX if ring is empty.
uint64_t oldestAvailable();

// Most-recently-saved frame. Returns UINT64_MAX if ring is empty.
uint64_t mostRecentSaved();

// True if the ring is initialized and recording.
bool active();

// F.1 round-trip determinism test. Activated via MAPLECAST_ROLLBACK_F1_TEST=
// "warmup,depth" (e.g. "300,5" → wait 300 frames, then capture 5 frames'
// worth of state hashes, rewind, recapture 5 frames, compare). Logs PASS
// or FAIL with mismatching frame numbers. The test runs once and stops.
//
// Internals:
//   1. Stage IDLE: count frames until warmup elapsed
//   2. Stage PRE:  hash the dc_serialize blob each saveFrame, store [0..depth-1]
//   3. After PRE final hash: rewindToFrame(anchor)
//   4. Stage POST: hash on resumed frames, store [0..depth-1]
//   5. After POST final hash: memcmp(pre, post) → log result
void f1TickFromVblank(uint64_t saveSeq);

// True if F.1 test has run to completion (regardless of pass/fail).
bool f1Done();
// True if F.1 test passed (only meaningful after f1Done()).
bool f1Passed();

// F.2 DC-SERIALIZE byte-diff harness. Activated via MAPLECAST_DC_AUDIT=
// "warmup" (single integer, e.g. "300" → wait 300 frames, then run a
// single round-trip and byte-diff the live vs post-rewind serialized blob).
// Reuses the rollback ring's request/execute rewind plumbing. On
// completion, dumps a bucketed diff report grouping bytes by subsystem
// (named via dcs_mark() calls in dc_serialize). Test runs once and stops.
void dcAuditTickFromVblank(uint64_t saveSeq);

// True if F.2 audit completed (regardless of whether it found diffs).
bool dcAuditDone();
// Total bytes that differed between live and post-rewind blobs.
// Meaningful only after dcAuditDone(). 0 = byte-perfect round-trip.
uint64_t dcAuditDiffBytes();

// Telemetry snapshot for debug UIs.
struct Stats {
	uint64_t framesSaved;
	uint64_t bytesArenaUsed;
	uint64_t bytesArenaTotal;
	uint64_t rollbacksPerformed;
};
Stats getStats();

} // namespace maplecast_rollback
