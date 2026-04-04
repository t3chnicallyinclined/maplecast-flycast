/*
	MapleCast Rend Replay — record rend_context frames, play them back.

	Proof of concept: can we capture the finished TA display list and
	replay it through the renderer to produce identical output?

	MAPLECAST_REND_REPLAY=1 to enable.
	Records 600 frames (10 seconds), then replays in a loop.
*/
#pragma once

struct rend_context;

namespace maplecast_rend_replay
{
void init();
bool active();
bool replaying();  // true when in replay phase (skip Process, go straight to Render)

// Called to record or replay. During replay, swaps rend_context contents.
bool tick(rend_context& rc);
}
