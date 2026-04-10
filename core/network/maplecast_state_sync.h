/*
	SHELVED 2026-04-09 — superseded by GGPO peer mode. See SHELVED
	block at top of maplecast_player.cpp for full rationale. The
	GGPO replacement uses save_game_state / load_game_state callbacks
	in core/network/ggpo.cpp instead. Do not add features here.

	MapleCast State Sync — Phase 3 of lockstep-player-client.

	Wire format for server→client full state snapshots delivered over a
	dedicated TCP port (7102). The server builds a full dc_serialize blob
	every STATE_SYNC_INTERVAL frames using maplecast_mirror::buildFullSaveState(),
	pipes it through the existing MirrorCompressor (zstd level 1 with the
	ZCST envelope from maplecast_compress.h), and frames it on the state
	socket using the STAT envelope defined below. The player client
	receives it on a persistent TCP connection, parses the envelope, and
	hands the inner payload (which may itself be a ZCST envelope) to
	MirrorDecompressor → dc_deserialize to restore the full machine state.

	Rationale vs GGPO's save/load path
	  - GGPO allocates a fixed 10 MB / 20 MB buffer per save and keeps
	    per-frame delta caches around for rollback. We don't rollback, so
	    we don't need the deltas — we just need "here's the authoritative
	    state, slot it in".
	  - buildFullSaveState() uses a dry-run dc_serialize to get the exact
	    size and mallocs only that much (~2-5 MB typical on MVC2).
	  - The existing zstd level 1 compressor gets us another ~3-5x on top
	    for free — ~1 MB wire per snapshot on a healthy DC state, far
	    below the 1-second budget.
	  - TCP (not UDP) because states are much bigger than a datagram and
	    sync is not latency-critical — reliability/ordering matter more
	    than round-trip time here.

	Wire format (big-endian NOT used — all fields are little-endian/host)
	  Envelope header (24 bytes, naturally aligned on x86_64)
	    4 bytes   magic "STAT"           0x54415453 when loaded as le u32
	    1 byte    version (=1)
	    1 byte    flags                  bit0 = payload is ZCST-compressed
	                                     bit1 = initial sync (first blob
	                                            after TCP accept)
	    2 bytes   reserved0 (=0)
	    8 bytes   serverFrame            maplecast_mirror::currentFrame()
	                                     at the moment buildFullSaveState
	                                     was captured
	    4 bytes   payloadLen             length of payload bytes following
	    4 bytes   reserved1 (=0)         trailing pad — struct alignment
	                                     would add this implicitly, so we
	                                     make it explicit on the wire
	    N bytes   payload                raw dc_serialize bytes, OR a ZCST
	                                     envelope (see maplecast_compress.h)

	The payload field carries whatever MirrorCompressor::compress returned —
	if zstd shrunk the blob, it's a ZCST envelope and flags bit0 is set. If
	compression failed, the raw bytes pass through and flags bit0 is clear.
	The receiver always passes the payload to MirrorDecompressor::decompress
	which does the ZCST check transparently.
*/
#pragma once
#include <cstdint>

namespace maplecast_state_sync
{

// TCP port the server listens on for state-sync connections.
static constexpr int STATE_PORT = 7102;

// Cadence: rebuild + broadcast full state every N server frames. 60 ≈ 1s
// at the typical 60 fps MVC2 cadence. Also sent immediately after a new
// TCP client is accepted, so late-joining players get an initial sync.
static constexpr uint64_t STATE_SYNC_INTERVAL = 60;

// STAT envelope
static constexpr uint32_t STAT_MAGIC = 0x54415453; // "STAT" as le-u32
static constexpr uint8_t  STAT_VERSION = 1;

// Flag bits
static constexpr uint8_t  STAT_FLAG_COMPRESSED  = 0x01; // payload is ZCST wrapper
static constexpr uint8_t  STAT_FLAG_INITIAL     = 0x02; // first blob after accept

struct StatHeader {
	uint32_t magic;
	uint8_t  version;
	uint8_t  flags;
	uint16_t reserved0;
	uint64_t serverFrame;
	uint32_t payloadLen;
	uint32_t reserved1;
};
static_assert(sizeof(StatHeader) == 24, "StatHeader must be 24 bytes on the wire");

// =====================================================================
// Server side — implemented in maplecast_mirror.cpp
// =====================================================================

// Start the TCP state listener. Called from maplecast_mirror::initServer().
// Spawns an accept thread and a send thread. Idempotent.
bool serverStart();

// Stop the listener. Idempotent.
void serverStop();

// Called from maplecast_mirror::serverPublish() right after the frame
// counter atomic is bumped. On frame boundaries that are multiples of
// STATE_SYNC_INTERVAL, builds a fresh state + compresses + queues it to
// all connected clients. Runs on the emu thread — synchronous snapshot,
// async wire send (via the send thread).
void onServerFramePublished(uint64_t frame);

// Telemetry snapshot.
struct ServerStats {
	bool     running;
	uint32_t clientCount;
	uint64_t statesBuilt;
	uint64_t statesSent;           // (count * clients) — one per client-send
	uint64_t totalBytesSent;       // wire bytes after compression
	uint64_t totalBytesBeforeComp; // raw dc_serialize size
	uint64_t lastBuildUs;          // wall time for the last buildFullSaveState
	uint64_t lastCompressUs;       // wall time for the last zstd compress
	uint64_t lastCompressedSize;   // last compressed size in bytes
	uint64_t lastRawSize;          // last raw dc_serialize size
};
ServerStats getServerStats();

// =====================================================================
// Client side — implemented in maplecast_player.cpp
// =====================================================================

// Start the TCP state client. Called from maplecast_player::init() after
// the UDP tape subscriber is up. Connects to `host:STATE_PORT`, spawns a
// receive thread that parses STAT envelopes into the pending slot below.
// Returns true if the socket was created (connection may still be in
// progress). Idempotent.
bool clientStart(const char* host);

// Stop the client. Idempotent.
void clientStop();

// Called from maplecast_player::frameGate() on the emu thread, BEFORE
// tape inputs are applied. If a pending state is ready and the client
// either hasn't synced yet OR has caught up to the state's serverFrame,
// the state is dc_deserialize'd into the live machine and the local
// frame counter is reseeded to match. Returns true if a state was
// applied this call (useful so the caller can flush stale tape entries
// that predate the reseed).
bool clientApplyPending();

struct ClientStats {
	bool     running;
	bool     connected;
	uint64_t statesReceived;
	uint64_t statesApplied;
	uint64_t bytesReceived;
	uint64_t lastApplyUs;          // wall time for the last dc_deserialize
	uint64_t lastDecompressUs;     // wall time for the last ZSTD_decompress
	uint64_t lastAppliedFrame;     // server frame of the last applied state
	int64_t  lastReceiveUs;        // monotonic us of last byte received
};
ClientStats getClientStats();

}
