/*
	SHELVED 2026-04-09 — superseded by GGPO peer mode. See SHELVED
	block at top of maplecast_player.cpp for full rationale. Do not
	add features. New work goes into core/network/ggpo.cpp.

	MapleCast Player Client — lockstep tape subscriber.

	Phase 2 of the lockstep-player-client branch.

	WHAT IT IS
	  A native flycast running full SH4 locally, whose input does NOT come
	  from the attached gamepad. Instead it subscribes to a MapleCast
	  server's input tape (UDP port 7101) and writes the authoritative,
	  frame-stamped tape entries into its own kcode[]/lt[]/rt[] globals at
	  the matching frame boundary. Because flycast's SH4 is deterministic
	  given identical inputs, the player client reproduces the server's
	  game state byte-for-byte with no rollback.

	  Local gamepad events still flow — but OUT, to the server's MapleCast
	  input UDP port, exactly the same path a NOBD stick or browser WS
	  client would use. They come back through the tape, authoritatively
	  frame-stamped, and only THEN land in the local SH4. This is how we
	  avoid divergence without rollback.

	WHAT IT IS NOT
	  Not a mirror client. Mirror clients don't run the SH4 — they render
	  TA deltas from the server. The player client does the opposite:
	  it runs full SH4 and uses its own PVR to render pixels; the tape is
	  a replacement for the local gamepad, not for the emulator.

	ENV VARS
	  MAPLECAST_PLAYER_CLIENT=<host:port>   — enable player-client mode
	                                          pointing at a tape server.
	                                          Default port = 7101.
	  MAPLECAST_PLAYER_STALL_POLICY=hard|speculate
	                                          Default 'hard'. See design
	                                          notes in the plan file.
*/
#pragma once
#include <atomic>
#include <cstdint>

namespace maplecast_player
{

enum class StallPolicy : uint8_t {
	Hard      = 0,   // block SH4 advance until tape reaches localFrame
	Speculate = 1,   // advance assuming last-known buttons held
};

// Initialize the player client. Called from Emulator::start() when
// MAPLECAST_PLAYER_CLIENT is set in the environment. Parses the server
// endpoint, opens the tape subscriber UDP socket, sends an initial HELO,
// starts the receive thread. Safe to call multiple times (second and
// later calls are no-ops). Returns true on success.
bool init();

// Shutdown — stop the receive thread, close the socket. Idempotent.
void shutdown();

// Is player client mode active?
bool active();

// Called at the top of each emulator frame boundary (right before the
// SH4 runs its next slice). Drains tape entries with frame <= the local
// frame counter into kcode[]/lt[]/rt[] and, depending on stall policy,
// either blocks until the tape reaches the local frame (Hard) or
// returns immediately even if the tape is behind (Speculate — last-
// applied values remain in kcode[]).
//
// The local frame counter is maintained internally and incremented
// each time this function returns true. It starts at 0 and advances in
// lockstep with the emu loop. When Phase 3 adds savestate sync, the
// seed will come from the savestate's frame number so both sides align.
//
// Returns true if the emulator may advance this frame, false if the
// caller should spin (Hard policy and tape not ready).
bool frameGate();

// Force the internal local frame counter to `frame`. Called after a
// savestate load (Phase 3) so the client restarts in lockstep with the
// server's frame number. Safe to call before init() — the value will
// take effect on the first frameGate() call after init().
void seedLocalFrame(uint64_t frame);

// Current stall policy — set from MAPLECAST_PLAYER_STALL_POLICY env var
// at init time, settable at runtime for testing.
StallPolicy getStallPolicy();
void        setStallPolicy(StallPolicy p);

// Telemetry — snapshot of the subscriber's health for debug UIs.
struct Stats {
	bool     active;
	bool     connected;            // have we ever received a tape packet?
	uint64_t packetsReceived;
	uint64_t entriesReceived;
	uint64_t entriesApplied;       // entries actually written to kcode[]
	uint64_t entriesDroppedStale;  // entries whose frame was already past
	uint64_t framesStalled;        // total frames we blocked waiting for tape
	uint64_t framesSpeculated;     // total frames we advanced without fresh tape
	uint64_t lastAppliedFrame;     // highest frame ever applied
	uint64_t serverLatestFrame;    // highest frame we've SEEN in any entry
	int64_t  lastPacketArrivalUs;  // monotonic us of last packet
};
Stats getStats();

}
