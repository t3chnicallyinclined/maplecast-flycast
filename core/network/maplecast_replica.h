/*
	MapleCast Replica Client — deterministic SH4 replica of the headless server.

	WHAT IT IS
	  A native flycast that runs the full SH4 + GPU locally, but its input
	  comes from an authoritative tape stream broadcast by the headless
	  MapleCast server. The local SH4 is a byte-perfect deterministic
	  replica of the server's SH4 — same ROM, same starting state (bootstrapped
	  via state-sync), same per-frame inputs (received over UDP). The local
	  GPU renders the result.

	  This is the native "flycast client" tier from the product vision —
	  the thing a player downloads to play with sub-RTT input feel rather
	  than streamed pixels. The browser path (king.html, packages/renderer/)
	  is unaffected and continues to be the spectator tier.

	WHAT IT IS NOT
	  Not GGPO. We borrow the *idea* of GGPO spectator mode (frame-stamped
	  authoritative inputs + a fast-forward catchup loop with rendering
	  disabled) but implement it directly against MapleCast's existing
	  network plumbing. No GGPOSession, no UdpProto, no ggpo_idle. The
	  vendored GGPO library at core/deps/ggpo/ is left strictly alone for
	  the existing peer-to-peer netplay path.

	  Not maplecast_player. That file (now SHELVED) had the right transport
	  but the wrong replay loop — its frameGate fast-forwarded a counter
	  without running the SH4, which desynced the local state from the
	  server's state immediately on any catchup. This module replaces the
	  replay loop with a GGPO-style "step the SH4 with rendering disabled"
	  catchup, while keeping the same UDP tape wire format and the same
	  state-sync STAT bootstrap.

	HOW IT WORKS
	  1. init() reads MAPLECAST_REPLICA=<host[:port]> from the environment.
	     Default port 7101 (the existing tape publisher port on the server).
	  2. Opens a UDP socket, sends HELO every ~900ms to subscribe to the
	     server's per-frame input tape.
	  3. Starts a TCP state-sync client (reuses maplecast_state_sync) to
	     receive a one-shot dc_serialize savestate snapshot. The snapshot
	     header carries the server's current frame number X.
	  4. frameGate() is called from the emu loop before every SH4 frame.
	     - If we haven't received the bootstrap STAT yet, stall.
	     - When STAT arrives, dc_deserialize it into the live machine and
	       seed _localFrame = X.
	     - Drain the tape ring for entries at frame == _localFrame, write
	       them to kcode[]/lt[]/rt[], let the SH4 run one frame, increment
	       _localFrame.
	     - If the tape is ahead (server has moved on while we were stalled),
	       enter CATCHUP MODE: disable rendering, mute audio, run the SH4
	       through the missing frames as fast as possible, applying inputs
	       as we go. Re-enable rendering when caught up. This is the
	       GGPO-spectator fast-forward semantics, lifted from ggpo.cpp:345-361.
	     - If the tape is behind (no entry for the current frame yet), stall.

	ENV VARS
	  MAPLECAST_REPLICA=<host[:port]>
	    Enable replica mode pointing at a MapleCast server. Default port = 7101.
	    Mutually exclusive with MAPLECAST_PLAYER_CLIENT (the SHELVED path).
	    If both are set, MAPLECAST_REPLICA wins and player client is skipped.
*/
#pragma once
#include <atomic>
#include <cstdint>

namespace maplecast_replica
{

// Initialize the replica client. Called from Emulator::start() when
// MAPLECAST_REPLICA is set in the environment. Parses the server endpoint,
// opens the tape subscriber UDP socket, starts the receive thread, starts
// the state-sync TCP client. Idempotent. Returns true on success.
bool init();

// Shutdown — stop receive threads, close sockets. Idempotent.
void shutdown();

// Is replica mode active?
bool active();

// Called at the top of each emulator frame boundary, before the SH4 runs
// its next slice. Returns true if the emu may advance one frame this tick,
// false if the caller should spin waiting for tape data.
//
// In normal mode this is a one-frame advance per call. In catchup mode it
// internally steps the SH4 multiple frames with rendering disabled before
// returning true for the final frame (which is rendered).
bool frameGate();

// Telemetry snapshot for debug UIs.
struct Stats {
	bool     active;
	bool     bootstrapped;          // STAT envelope received and applied
	bool     tapeConnected;         // at least one INPT packet received
	uint64_t localFrame;            // current local frame counter
	uint64_t serverLatestFrame;     // highest frame seen in any tape entry
	uint64_t framesAdvanced;        // total frames the SH4 has stepped
	uint64_t framesCatchupSkipped;  // frames stepped during catchup (no render)
	uint64_t framesStalled;         // total frames the gate blocked on missing tape
	uint64_t tapePacketsReceived;
	uint64_t tapeEntriesReceived;
	uint64_t tapeEntriesApplied;
};
Stats getStats();

}
