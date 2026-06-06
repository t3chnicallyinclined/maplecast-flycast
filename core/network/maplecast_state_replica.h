/*
	MapleCast State-Replica Client — server-authoritative state INJECTION.

	WHAT IT IS (and how it differs from maplecast_replica.cpp)
	  This is NOT lockstep and NOT the SHELVED tape-replay replica
	  (maplecast_replica.cpp). The concept here:

	    The server runs the game (authoritative) and broadcasts the per-frame
	    GSTA state (~261 bytes). This client runs its OWN flycast (same ROM +
	    same savestate) but treats the server's GSTA as the truth. Each frame,
	    BEFORE the game's SH4 draw code runs, it writes the server's GSTA into
	    its RAM (writeGameState). The game's own render code then builds the
	    TA list from the injected state and the local GPU renders it.

	  No video stream (~15 KB/s, not ~2 MB/s). No drift (the client renders
	  the server's truth every frame). Pixel-exact (the game's own render code
	  runs locally). Requires the ROM + the same savestate on the client.

	THE INJECTION TIMING (the crux)
	  flycast has no hook between the game's per-frame UPDATE and DRAW —
	  runInternal() runs a whole frame (game update -> TA list built at vblank
	  -> renderer presents). So we inject at the TOP of the frame, BEFORE
	  runInternal(). The deterministic local SH4 (same ROM + same savestate +
	  the same inputs carried in the GSTA) reproduces the server frame-for-
	  frame; the injection then snaps the visible fields (char pos / sprite_id
	  / health / camera / meter) to the server's authoritative values right
	  before the game's draw code reads them. This is the only emulator-level
	  point where the injected state reaches the draw code of the SAME frame.

	  MAPLECAST_STATE_REPLICA_FREEZE=1 additionally holds the local inputs at
	  neutral so the ONLY thing moving the characters is the injected GSTA —
	  this is the strict "no simulation, inject-only" test. Use it to measure
	  whether 261 bytes is sufficient: anything that drifts under FREEZE is a
	  region the GSTA does not capture.

	ENV VARS
	  MAPLECAST_STATE_REPLICA=<host[:port]>
	    Enable. host = MapleCast server, port = mirror/relay WS (default 7201).
	    The mirror WS already deserializes GSTA into maplecast_mirror's
	    _clientGameState; this module reads it via getClientGameState() and
	    injects it with writeGameState().
	  MAPLECAST_STATE_REPLICA_FREEZE=1
	    Strict inject-only: zero the local inputs so the local SH4 cannot move
	    the characters; only the injected GSTA does.
	  MAPLECAST_HEADLESS_AUTOLOAD=1 + the matching savestate slot
	    Load the same savestate the server autoloaded (set up by the operator).
*/
#pragma once
#include <cstdint>

namespace maplecast_state_replica
{

// Parse MAPLECAST_STATE_REPLICA, start the GSTA stream (mirror WS, GSTA-only).
// Returns true if state-replica mode is enabled. Idempotent.
bool init();

// Is state-replica mode active?
bool active();

// Called at the TOP of each emu frame, before runInternal(). Injects the
// latest server GSTA into local RAM via writeGameState(). Returns true when
// the emu may advance (always true once bootstrapped; stalls until the first
// GSTA arrives so the savestate isn't run forward with no authority).
bool frameInject();

// Shutdown. Idempotent.
void shutdown();

struct Stats {
	bool     active;
	bool     gotFirstState;
	bool     freeze;
	uint32_t lastFrameCounter;   // frame_counter from the last injected GSTA
	uint64_t framesInjected;
	uint64_t framesStalled;
	int      lastObjsSeen;       // pool objects the server shipped last frame
	int      lastObjsWritten;    // pool objects we found a local node for
	                             // (seen-written = the node-synthesis gap)
};
Stats getStats();

}
