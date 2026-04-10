/*
	MapleCast Input Server — single source of truth for all player input.

	Every input path (NOBD UDP, browser WebSocket, XDP, SDL) feeds through
	this server. One registry, one set of kcode[] writes, one place to
	track latency, jitter, and buffer depth per player.

	The game reads kcode[]/lt[]/rt[] at CMD9 time — always fresh.
*/
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

namespace maplecast_input
{

enum class InputType { None, NobdUDP, BrowserWS, XDP, LocalUSB };

struct PlayerInfo {
	bool connected;
	InputType type;
	char id[32];          // NOBD code, browser UUID, etc.
	char name[32];        // display name
	char device[48];      // "NOBD-7X4K", "PS4 Controller", etc.
	uint32_t srcIP;       // source IP (network byte order)
	uint16_t srcPort;     // source port

	// Live button state
	uint16_t buttons;     // DC active-low
	uint8_t lt, rt;       // triggers 0-255

	// Latency tracking
	int64_t lastPacketUs; // timestamp of last received packet
	int64_t lastChangeUs; // timestamp of last button-state change (idle detection)
	int64_t avgE2eUs;     // rolling average E2E latency
	int64_t avgJitterUs;  // rolling average jitter
	uint32_t packetsPerSec;
	uint32_t changesPerSec;
	int      rttMs;       // browser-reported WS RTT (ms), 0 = unknown

	// Phase A — input-update sequence counter, increments every time updateSlot
	// is called for this slot regardless of whether buttons changed. Lets the
	// latch tell "did the network thread touch this slot since I last looked".
	uint32_t lastPacketSeq;

	// Phase B — ConsistencyFirst policy state (only used when getLatchPolicy(slot)
	// == ConsistencyFirst; LatencyFirst slots leave these untouched).
	//   lastLatchedButtons — what the SH4 saw last frame in active-low form.
	//                        Used as the freeze fallback when the guard window
	//                        defers a near-boundary network arrival.
	//   deferredReleaseMask — bits that were pressed AND released within the
	//                        same vblank interval the previous frame. The latch
	//                        showed them as PRESSED last frame; we need to flip
	//                        them back to RELEASED on the NEXT frame so blip-
	//                        presses don't stay held forever.
	//   guardHits           — telemetry: how many latches deferred a fresh
	//                        arrival because it landed inside the guard window.
	uint16_t lastLatchedButtons;
	uint16_t deferredReleaseMask;
	uint64_t guardHits;

	// Stats accumulators (reset every second)
	uint32_t _pktAccum;
	uint32_t _chgAccum;
	int64_t _lastRateTime;
	uint16_t _prevButtons;
};

// Phase A — tear-free packed input slot, written by updateSlot() and read by
// the CMD9 latch in ggpo::getLocalInput(). Layout (LSB→MSB):
//   bits  0..15  buttons (DC active-low, lower 16)
//   bits 16..23  lt (8-bit trigger)
//   bits 24..31  rt (8-bit trigger)
//   bits 32..63  packet sequence number (monotonic, increments per write)
// The whole 64-bit word is updated atomically so the latch never sees a torn
// read across the buttons/lt/rt boundary.
extern std::atomic<uint64_t> _slotInputAtomic[2];

inline uint64_t packSlotInput(uint16_t buttons, uint8_t ltVal, uint8_t rtVal, uint32_t seq) {
	return (uint64_t)buttons
	     | ((uint64_t)ltVal << 16)
	     | ((uint64_t)rtVal << 24)
	     | ((uint64_t)seq << 32);
}

inline void unpackSlotInput(uint64_t packed, uint16_t& buttons, uint8_t& ltVal, uint8_t& rtVal, uint32_t& seq) {
	buttons = (uint16_t)(packed & 0xFFFF);
	ltVal   = (uint8_t)((packed >> 16) & 0xFF);
	rtVal   = (uint8_t)((packed >> 24) & 0xFF);
	seq     = (uint32_t)((packed >> 32) & 0xFFFFFFFFu);
}

// Phase A — telemetry: every CMD9 latch (called from ggpo::getLocalInput once
// per vblank) stamps a sample. The ring buffer + accessor land in A.4; for now
// recordLatchSample() is the public hook so the latch site can be wired in
// A.3 without waiting on the storage layer.
//   slot       — 0 or 1 (only maplecast slots are stamped)
//   deltaUs    — t_latch_us - p.lastPacketUs (positive = stale, negative = packet
//                arrived "in the future" relative to publish time which means
//                the publish stamp lagged the input thread by a hair — happens
//                during cold start)
//   packetSeq  — last seq the network thread wrote to _slotInputAtomic[slot]
//   frameNum   — maplecast_mirror::currentFrame() at latch time
void recordLatchSample(int slot, int64_t deltaUs, uint32_t packetSeq, uint64_t frameNum);

// Phase A — per-slot latch statistics summary, served via the WebSocket
// status JSON every second. Backed by the ring buffer recordLatchSample()
// writes to. All fields are zero before the first sample on a slot.
struct LatchStats {
	uint64_t totalLatches;     // total samples seen on this slot
	uint64_t latchesWithData;  // samples where the network thread had touched the slot since the previous latch
	int64_t  avgDeltaUs;       // mean (t_latch - lastPacketUs) over the ring window
	int64_t  p99DeltaUs;       // 99th percentile
	int64_t  minDeltaUs;       // min observed in the window
	int64_t  maxDeltaUs;       // max observed in the window
	uint32_t lastPacketSeq;    // most recent packet seq seen
	uint64_t lastFrameNum;     // most recent frame number
};
LatchStats getLatchStats(int slot);

// =====================================================================
//                       PHASE B — DUAL-POLICY LATCH
// =====================================================================
//
// Two latch policies, selectable per-slot at runtime:
//
//   LatencyFirst (= today's behavior + Phase A's atomic-read fix)
//     The CMD9 latch reads the most recent _slotInputAtomic snapshot
//     directly. Whatever the network thread last wrote is what the SH4
//     sees. Lowest possible per-input latency. Zero behavior change vs
//     pre-Phase-B determinism baseline. Default policy.
//
//   ConsistencyFirst (= press-accumulation + edge preservation + guard)
//     The CMD9 latch drains a per-slot accumulator that records EVERY
//     button-press edge that occurred during the last vblank interval.
//     A 1ms tap-and-release that fits inside one vblank still appears
//     as a press in the next latch (it would otherwise be lost by the
//     instantaneous-snapshot path). Symmetric handling for releases via
//     a deferredReleaseMask so blip-presses unlatch on the NEXT frame.
//     A small guard window also defers near-boundary network arrivals
//     by exactly one frame for predictability. Trades up to 1 frame of
//     latency on near-boundary inputs for "every press the player
//     intended is in the game". Best for fighting games with tight
//     simultaneous-press requirements (dashes, super cancels, dual-button
//     supers).
//
// Inspired by GP2040-CE NOBD firmware's syncGpioGetAll() — the same
// problem solved at a different layer. See docs/ARCHITECTURE.md and the
// project_input_latch_architecture memory.

enum class LatchPolicy : uint8_t {
	LatencyFirst     = 0,  // default — instantaneous snapshot, today's behavior
	ConsistencyFirst = 1,  // accumulator + edge preservation + guard window
};

// Per-slot policy accessors. Default is LatencyFirst. The MAPLECAST_LATCH_POLICY
// env var sets the global default at startup ("latency" = LatencyFirst,
// "consistency" = ConsistencyFirst); per-slot setLatchPolicy() at runtime
// overrides for testing / live A/B / future per-player UI selection.
LatchPolicy getLatchPolicy(int slot);
void        setLatchPolicy(int slot, LatchPolicy policy);

// Guard window in microseconds, used by ConsistencyFirst policy. Inputs that
// arrived within (now - guard_us, now] are deferred to the NEXT vblank latch
// for predictability — converts boundary-arrival jitter into a deterministic
// +1-frame mapping for inputs that would otherwise straddle the latch
// instant. Default 500 us. MAPLECAST_GUARD_US env var overrides.
int64_t getGuardUs();

// Runtime setter used by the ImGui debug overlay on native mirror clients.
// Clamped to [0, 5000] us. Atomic store — safe from any thread.
void setGuardUs(int64_t us);

// Phase B — input accumulator for ConsistencyFirst policy. Single 64-bit
// word per slot, atomic, tear-free. Layout (LSB→MSB):
//   bits  0..15  any_pressed   — OR of every bit that went 1→0 (active-low pressed)
//                                since the last drain. Cleared on drain.
//   bits 16..31  any_released  — OR of every bit that went 0→1 (active-low released)
//                                since the last drain. Cleared on drain.
//   bits 32..47  current       — most-recent kcode lower 16 bits
//   bits 48..55  lt            — most-recent LT raw 8-bit
//   bits 56..63  rt            — most-recent RT raw 8-bit
//
// Producer (network thread) does a CAS loop in updateSlot() to atomically
// read the prior state, compute newly-pressed/newly-released bits relative
// to a.current, OR them into the accumulators, and store back.
//
// Consumer (the SH4 vblank latch in ggpo::getLocalInput) calls
// drainAccumulator(slot) to atomically read-and-clear the press/release
// flags while keeping current/lt/rt. Returns the snapshot.
struct AccumPacked {
	uint16_t any_pressed;
	uint16_t any_released;
	uint16_t current;
	uint8_t  lt;
	uint8_t  rt;
};
static_assert(sizeof(AccumPacked) == 8, "AccumPacked must fit in a uint64_t");

inline uint64_t packAccum(const AccumPacked& a) {
	uint64_t packed;
	__builtin_memcpy(&packed, &a, 8);
	return packed;
}
inline AccumPacked unpackAccum(uint64_t packed) {
	AccumPacked a;
	__builtin_memcpy(&a, &packed, 8);
	return a;
}

extern std::atomic<uint64_t> _slotAccum[2];

// Atomically read-and-clear the accumulator's press/release flags. Keeps
// current/lt/rt. Returns the AccumPacked observed before the clear. Called
// from the latch path (ggpo::getLocalInput) on the SH4 vblank thread.
AccumPacked drainAccumulator(int slot);

// Phase B — full ConsistencyFirst latch computation for one slot.
// Drains the accumulator, applies edge preservation (any_pressed → buttons
// stay pressed for one frame), applies the previous frame's deferred-release
// mask (blip-presses unlatch), updates the new deferred-release mask
// (any bit pressed-and-released this interval becomes deferred), and applies
// the guard window (if a fresh packet arrived inside guardUs of t_latch_us,
// freeze at lastLatchedButtons for one frame).
//
// All state mutation (PlayerInfo.lastLatchedButtons, deferredReleaseMask,
// guardHits) happens inside this call. Returns the active-low button mask
// the SH4 should see this frame in the lower 16 bits of a u32 (without the
// 0xFFFF0000 high mask — caller adds it for the kcode[] format).
//
// Out params:
//   ltOut, rtOut — most recent trigger values from the accumulator
//   packetSeqOut — most recent packet seq seen (for telemetry)
struct ConsistencyLatchResult {
	uint16_t buttons;       // active-low low 16, no high mask
	uint8_t  lt;
	uint8_t  rt;
	uint32_t packetSeq;
	bool     guardFired;    // true if the guard window deferred this frame
};
ConsistencyLatchResult consistencyFirstLatch(int slot, int64_t tLatchUs);

// Initialize the input server — starts UDP listener thread
bool init(int udpPort = 7100);

// Shutdown all input threads
void shutdown();

// Register a player from WebSocket join (returns assigned slot, -1 if full)
int registerPlayer(const char* id, const char* name, const char* device, InputType type);

// Unregister / disconnect a player
void disconnectPlayer(int slot);

// Get player info for slot (0=P1, 1=P2)
const PlayerInfo& getPlayer(int slot);

// Get number of connected players
int connectedCount();

// Idle detection — returns the lowest connected slot that hasn't sent a
// button-state CHANGE in `thresholdUs` microseconds, or -1 if everyone
// is fresh. Pps (gamepad poll rate) is ignored — we only care about
// changes, so a player holding nothing still counts as idle.
int findIdlePlayer(int64_t thresholdUs);

// Called by WebSocket message handler to inject browser gamepad input
void injectInput(int slot, uint8_t lt, uint8_t rt, uint16_t buttons);

// Browser-reported WebSocket round-trip in milliseconds. Used by the cabinet
// HUD to show each playing user's actual ping (not the spectator's local ping).
void setPlayerRtt(int slot, int rttMs);

// Is the input server running?
bool active();

// Stick registration — bind a NOBD stick to a username
// Rhythm mode: tap any button 5x, pause, 5x again
void startStickRegistration(const char* browserId);
void cancelStickRegistration();
bool isRegistering();

// Web registration — first unregistered stick to send any input gets bound
void startWebRegistration(const char* username);
void cancelWebRegistration();
bool isWebRegistering();
const char* webRegisteringUsername();  // who's waiting to register

// Lookup: is this NOBD source registered?
// Returns username if registered, nullptr if not
const char* getRegisteredUsername(uint32_t srcIP, uint16_t srcPort);

// Legacy: returns browser ID (same as username now)
const char* getRegisteredBrowserId(uint32_t srcIP, uint16_t srcPort);

// Bind a specific stick source to a username
void registerStick(uint32_t srcIP, uint16_t srcPort, const char* username);

// Unregister a stick by username
void unregisterStick(const char* username);

// Check if a registered stick is online (sent input recently)
bool isStickOnline(const char* username);

// Get stick info for a username
struct StickInfo {
    bool registered;
    bool online;
    char username[16];
    uint32_t srcIP;
    uint16_t srcPort;
    int64_t lastInputUs;
};
StickInfo getStickInfo(const char* username);

// How many registered sticks?
int registeredStickCount();

// Validate username: 4-12 chars, [a-zA-Z0-9_] only
bool isValidUsername(const char* name);

// ==================== Stick persistence ====================
// Bindings live in RAM in this process. Persistence flows out through
// the WebSocket layer to the Rust collector, which mirrors them to
// SurrealDB. On boot we also rehydrate from a local JSON hot-cache so
// flycast survives a restart even if the collector is briefly down.

enum class StickEventKind { Register, Unregister, Online, Offline };
struct StickEvent {
    StickEventKind kind;
    char username[16];
    uint32_t srcIP;       // network byte order
    uint16_t srcPort;     // network byte order
    int64_t  ts;          // unix seconds
};
// Returns and clears the pending event queue. Called by the ws server
// each time it has a place to push events out (immediately on registration
// since rare, or on the periodic status broadcast).
std::vector<StickEvent> drainStickEvents();

// Snapshot all current bindings (for hot-cache writer / debugging).
struct StickSnapshot {
    char username[16];
    uint32_t srcIP;
    uint16_t srcPort;
    int64_t lastInputUs;
};
std::vector<StickSnapshot> snapshotStickBindings();

// Bulk install bindings from local cache or from a `stick_load` push by
// the collector. Existing bindings for the same username are overwritten.
// Does NOT emit StickEvents — these are already persisted upstream.
void installStickBindings(const std::vector<StickSnapshot>& snapshots);

// Local hot-cache I/O. Path: ~/.maplecast/sticks.json. Rewritten on every
// register/unregister so a flycast crash loses at most the in-flight
// transition. loadStickCache() is safe to call before init().
bool loadStickCache();
bool saveStickCache();

// =====================================================================
//             INPUT TAPE — Phase 1 of lockstep-player-client
// =====================================================================
//
// The input tape is the authoritative record of what the server's SH4
// saw on each frame for slots 0 and 1. Every call to updateSlot() pushes
// a frame-stamped entry into a lock-free SPSC-per-slot ring buffer (the
// network thread is the only producer per slot); a dedicated publisher
// thread drains all slots into a UDP datagram stream on port 7101 so
// native player clients can replay the tape into their own SH4.
//
// Wire format is intentionally tiny — absolute state, not deltas, so
// packet loss is self-healing: any surviving packet re-syncs the slot.
//
//   struct TapeEntry {           // 16 bytes, little-endian, naturally aligned
//       uint64_t frame;          // maplecast_mirror::currentFrame() at write
//       uint32_t seq;            // lower 24 bits: monotonic packet seq for this slot
//                                //  upper  8 bits: slot (0 or 1)
//       uint16_t buttons;        // DC active-low lower 16 bits
//       uint8_t  lt;             // raw 8-bit trigger
//       uint8_t  rt;             //           "
//   };
//
// Slot is packed into the high byte of `seq` rather than eating its own
// word — 24 bits of seq is ~4 hours at 1 kHz which is fine (the client
// only uses seq for monotonicity checks inside a session, never as an
// absolute). Dropping `stampUs` from the wire: the receiver timestamps
// arrival locally, and server wall-clock adds nothing deterministic to
// the replay — the frame number is the only ordering that matters.
//
// UDP datagram format:
//   4 bytes  magic "INPT"
//   1 byte   version (=1)
//   1 byte   entry count
//   2 bytes  reserved
//   entries[count]           // TapeEntry repeated
//
// Subscriber model: a client sends a 4-byte "HELO" datagram to the tape
// port; the server remembers (srcIP, srcPort) as a subscriber and starts
// fanning tape drains to it. Subscribers age out after 5 s of silence —
// clients re-HELO every ~1 s to stay subscribed (cheap heartbeat).

struct TapeEntry {
	uint64_t frame;
	uint32_t seqAndSlot;   // [31:24] slot, [23:0] seq mod 2^24
	uint16_t buttons;
	uint8_t  lt;
	uint8_t  rt;
};
static_assert(sizeof(TapeEntry) == 16, "TapeEntry wire layout must be 16 bytes");

inline uint32_t packSeqSlot(uint32_t seq, uint8_t slot) {
	return (seq & 0x00FFFFFFu) | ((uint32_t)slot << 24);
}
inline void unpackSeqSlot(uint32_t v, uint32_t& seq, uint8_t& slot) {
	seq  = v & 0x00FFFFFFu;
	slot = (uint8_t)(v >> 24);
}

// Push a tape entry for slot. Called from updateSlot() on the UDP/XDP
// input thread. Lock-free, bounded ring per slot — if the ring is full
// (publisher is wedged), the oldest entry is dropped. Safe to call before
// the publisher thread has started; entries will sit in the ring until
// the publisher drains them.
void pushTapeEntry(int slot, uint16_t buttons, uint8_t lt, uint8_t rt, uint32_t seq);

// GGPO-style dense tape tick: push one tape entry per slot using the
// current `_slotInputAtomic` packed snapshot, stamped with the explicit
// frame number `frame`. Called from maplecast_mirror::serverPublish
// once per server emu frame. This is what makes the player-client
// frameGate work as a blocking-read GGPO equivalent — every server
// frame number is guaranteed to have an entry in the tape, even if no
// input changed since the previous frame.
void publishFrameTick(uint64_t frame);

// Telemetry snapshot for the input tape publisher. All counters are
// monotonic process-lifetime totals unless otherwise noted.
struct TapeStats {
	uint64_t entriesPushed;     // total pushTapeEntry calls across all slots
	uint64_t entriesDropped;    // ring-full drops (publisher couldn't keep up)
	uint64_t packetsSent;       // total UDP datagrams emitted
	uint64_t bytesSent;         // total UDP payload bytes
	uint32_t subscribers;       // current live subscriber count
	uint64_t lastPublishedFrame;// highest frame number ever drained from any slot
};
TapeStats getTapeStats();

}
