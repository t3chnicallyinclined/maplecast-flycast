/*
	MapleCast Lockstep Mirror — game-state checksum / resync layer.

	This is the ONE genuinely new piece of the lockstep-mirror client (the
	endgame architecture). Everything else it needs already exists and is
	reused as-is:
	  * JOIN snapshot   — maplecast_state_sync (STAT envelope, TCP 7102):
	                      buildFullSaveState -> emu.loadstate + seedLocalFrame.
	  * INPUT stream    — maplecast_input's frame-keyed tape (UDP 7101),
	                      applied by maplecast_player::frameGate (Hard stall).
	  * checksum region — maplecast_rollback::gameStateRegionHash() (the exact
	                      RAM ranges the determinism gate proved byte-identical).

	The lockstep client is a FULL flycast running MVC2 (SH4 ON, NATIVE render)
	whose inputs come from the server's tape, joined by a periodic dc_serialize
	snapshot, and kept honest by this checksum. Because it runs the real game
	and renders natively, decode/palette/DMA/HUD/effects are coherent by
	construction — no reconstruction, no texture/palette whack-a-mole.

	What THIS module adds:
	  SERVER: every N frames, ship the game-state-region hash to subscribed
	          lockstep clients over a tiny dedicated UDP channel (port 7103).
	  CLIENT: subscribe to that channel; each frame compute the same hash from
	          local guest RAM and compare against the server's hash for the
	          SAME absolute frame. On mismatch, request a fresh JOIN snapshot
	          (the state-sync reconnect path) and reseed.

	CRITICAL (measurement-forced): we hash the RAW mem_b RANGES via
	gameStateRegionHash(), NOT the dc_serialize blob. The blob carries the two
	execution-invariant scheduler-epoch bytes; a blob checksum would false-
	mismatch EVERY frame and drive perpetual resync. The gate proved the raw
	ranges are byte-identical between continuous-run server and load-then-run
	client, so the raw-range hash is the correct equality test.

	Wire (UDP datagram, little-endian, 24 bytes):
	    "GSHA"(4) + version(1)=1 + reserved(3) + frame(u64) + hash(u64)
	Client subscribes with a 4-byte "HELO" datagram (re-sent ~1s; server ages
	subscribers out after 5s of silence). Same self-healing subscriber model as
	the input tape.

	Env gate (default OFF — this module does NOT touch existing paths unless set):
	    MAPLECAST_LOCKSTEP=1            enable lockstep checksum layer
	    MAPLECAST_LOCKSTEP_INTERVAL=N   frames between hashes (default 60; set 1
	                                    during bring-up to hash EVERY frame)
	    MAPLECAST_LOCKSTEP_DEBUG=1      log every comparison + offset probe

	Role disambiguation: serverStart() is only called from the authoritative
	server's mirror init; clientInit() only from maplecast_player::init (the
	native client). MAPLECAST_LOCKSTEP alone => server; +MAPLECAST_PLAYER_CLIENT
	=> client.
*/
#pragma once
#include <cstdint>

namespace maplecast_lockstep
{

// Dedicated UDP port for the game-state-hash channel.
static constexpr int HASH_PORT = 7103;

// "GSHA" as a little-endian u32 (bytes 'G','S','H','A').
static constexpr uint32_t GSHA_MAGIC_LE = 0x41485347u;
static constexpr uint8_t  GSHA_VERSION  = 1;

// On-wire datagram size.
static constexpr int GSHA_DATAGRAM_LEN = 24;

// ── Common ────────────────────────────────────────────────────────────
// True iff MAPLECAST_LOCKSTEP is set. Cheap; cached after first call.
bool active();

// Frames between hashes. MAPLECAST_LOCKSTEP_INTERVAL, default 60. Read once.
uint32_t hashInterval();

// ── Server ────────────────────────────────────────────────────────────
// Bind UDP 7103, spawn the HELO-receive thread. Idempotent. No-op if
// !active(). Called from maplecast_mirror::initServer().
bool serverStart();
void serverStop();

// Called on the emu/publish thread once per committed server frame (from
// serverPublish, alongside publishFrameTick). Every hashInterval() frames it
// computes gameStateRegionHash() and ships a GSHA datagram to all subscribers.
// No-op if !active() or no subscribers.
void onServerFrame(uint64_t frame);

struct ServerStats {
	bool     running;
	uint32_t subscribers;
	uint64_t hashesSent;
	uint64_t datagramsSent;
	uint64_t bytesSent;
};
ServerStats getServerStats();

// ── Client ────────────────────────────────────────────────────────────
// Start the hash subscriber pointed at `host`. Spawns a receive thread that
// stores incoming server hashes keyed by absolute frame. Idempotent. No-op if
// !active(). Called from maplecast_player::init() with the tape server host.
bool clientInit(const char* host);
void clientShutdown();

// Called from maplecast_player::frameGate() each time the client is about to
// run frame `frameToRun` — i.e. resident guest RAM currently reflects the END
// of frame (frameToRun-1). Computes the local game-state hash, records it, and
// compares against any server hash for the matching frame. On a confirmed
// mismatch it triggers a resync (maplecast_player::requestResync()).
void clientVerify(uint64_t frameToRun);

struct ClientStats {
	bool     running;
	uint64_t hashesReceived;
	uint64_t compared;     // frames where both local+server hash were present
	uint64_t matched;
	uint64_t mismatched;
	uint64_t resyncs;
	uint64_t lastServerFrame;
	uint64_t bytesReceived;
};
ClientStats getClientStats();

// Server's most recent frame (from the hash channel). 0 until the first hash.
uint64_t lastServerFrame();

// Offset-aware: server's authoritative hash for a CLIENT frame (predict-live
// confirmed-hash gate). Returns false if offset unlocked or hash not yet received.
bool serverHashForClientFrame(uint64_t clientFrame, uint64_t* out);

}
