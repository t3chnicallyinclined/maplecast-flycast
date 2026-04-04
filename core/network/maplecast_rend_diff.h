/*
	MapleCast Rend Diff — discover hidden state by diffing rend_context against game state.

	Every frame:
	1. Read 253-byte game state (fingerprint)
	2. Hash rend_context vertex positions per character + global polygon stats
	3. If same game state fingerprint → different vertex hash = HIDDEN STATE FOUND
	4. Log the diff to CSV for analysis

	The frames where 253 bytes match but visuals differ reveal exactly which
	RAM addresses we're missing. Those addresses are the hidden variables
	(physics accumulators, RNG, animation blending, particle seeds).

	MAPLECAST_REND_DIFF=1 to enable.
*/
#pragma once

struct rend_context;

namespace maplecast_rend_diff
{
void init();
void tick(rend_context& rc);  // called every frame from render pipeline, before Render()
bool active();
void report(); // dump summary to stdout
}
