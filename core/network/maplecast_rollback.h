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
bool rewindToFrame(uint64_t frame);

// Oldest frame currently in the ring. Frames older than this have been
// overwritten by newer saves. Returns UINT64_MAX if ring is empty.
uint64_t oldestAvailable();

// Most-recently-saved frame. Returns UINT64_MAX if ring is empty.
uint64_t mostRecentSaved();

// True if the ring is initialized and recording.
bool active();

// Telemetry snapshot for debug UIs.
struct Stats {
	uint64_t framesSaved;
	uint64_t bytesArenaUsed;
	uint64_t bytesArenaTotal;
	uint64_t rollbacksPerformed;
};
Stats getStats();

} // namespace maplecast_rollback
