/*
	MapleCast Predict — client-side prediction + rollback, STAGE 1: the
	no-render re-sim PRIMITIVE.

	The rollback client needs to, on a misprediction, restore an earlier frame
	and RE-SIMULATE K (~10) frames forward FAST and INVISIBLY, inside a single
	60fps tick of the existing single-threaded render-every-frame lockstep loop.
	The requirement is NOT threaded rendering — it is a fast NO-RENDER
	frame-advance: run the SH4 to each vblank WITHOUT present()/GL render.

	Mechanism (verified against the code):
	  * GGPO already advances frames with no render: ggpo::endOfFrame()
	    (ggpo.cpp:1070) sets a flag and getSh4Executor()->Stop() at the non-RTT
	    display STARTRENDER (Renderer_if.cpp:631), so Run() returns after exactly
	    one game-frame with no present. We replicate that with a headless flag.
	  * rend_enable_renderer(false) makes QueueRender skip+recycle the context
	    (ta_ctx.cpp:53) => zero GPU work for the re-sim frames.
	  * scheduleRenderDone(ctx) still fires (Renderer_if.cpp:600, BEFORE the
	    skip) so the RENDER_DONE interrupt timing is unchanged => the SH4 executes
	    identically => game-state stays byte-deterministic (render never writes
	    guest RAM the SH4 reads).
	  * Save/restore reuse the rollback ring (maplecast_rollback capture/restore,
	    already validated byte-perfect by the F.1 gate + determinism proof).

	This stage builds+proves ONLY the primitive. The predict/ring/reconcile loop
	is the next stage, straightforward on top of a proven headless re-sim.

	Everything is gated (default OFF) so the proven lockstep client is untouched:
	  MAPLECAST_PREDICT=1            enable the predict subsystem
	  MAPLECAST_PREDICT_TEST=W,K     run the standalone primitive gate once:
	                                 warm up W frames, then capture, run K frames
	                                 NORMALLY (recording inputs), restore, run the
	                                 same K frames HEADLESS, and assert the
	                                 resulting game-state hash is BYTE-IDENTICAL
	                                 (and headless K takes <1ms). Logs GO/NO-GO.
*/
#pragma once
#include <cstdint>

namespace maplecast_predict
{

// True iff MAPLECAST_PREDICT is set (cached).
bool active();

// Read by rend_start_render on the render/emu thread: when true, the display
// (non-RTT) STARTRENDER stops the SH4 for a headless one-frame advance instead
// of queueing a render. Cheap atomic-ish bool. Always false unless a headless
// advance is in progress on the same (emu) thread.
bool headlessAdvanceActive();

// Advance the SH4 exactly `frames` game-frames with NO render/present. Must be
// called from the emu thread at a frame boundary (SH4 paused, between frames).
// Returns wall-clock microseconds spent. Render is disabled for the duration
// and restored after.
int64_t advanceHeadless(int frames);

// Per-frame hook, called from maplecast_player::frameGate() at the top of each
// frame BEFORE the SH4 runs (SH4 paused). Drives the MAPLECAST_PREDICT_TEST
// primitive gate and the MAPLECAST_PREDICT_GATE0 restore-into-live gate. No-op
// unless armed.
void onFrameBoundary(uint64_t frame);

// Record the authoritative input applied for `frame` (both slots already
// written to kcode[]/lt[]/rt[]). Called from frameGate right AFTER applyEntry,
// BEFORE runInternal, so the recorded snapshot is exactly what the SH4 sees for
// that frame. Feeds the GATE 0 / reconcile re-sim (which replays the same
// inputs). No-op unless predict is active.
void recordAppliedInput(uint64_t frame);

// Gate result accessors (meaningful after the gate completes).
bool testDone();
bool testPassed();

// STAGE c — frame-stamped local input. The frame the client is currently
// predicting/displaying; the client stamps its forwarded input with
// predictedFrame() + INPUT_DELAY so the server applies it at the SAME frame the
// client did, eliminating self-mispredict. 0 until the predict loop is running.
uint64_t predictedFrame();
static constexpr uint64_t INPUT_DELAY = 2;   // frames of input delay (GGPO-style)

}
