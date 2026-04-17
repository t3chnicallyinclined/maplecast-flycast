/*
	MapleCast Input Server — single source of truth for all player input.

	Architecture (inspired by NOBD firmware):
	  NOBD: GPIO → cmd9ReadyW3 (always fresh, ISR just reads it)
	  This: UDP thread → kcode[] atomics (always fresh, CMD9 just reads them)

	All input paths converge here:
	  - NOBD stick UDP → recvfrom thread → updateSlot()
	  - Browser gamepad → WebSocket → injectInput() → updateSlot()
	  - XDP (future) → ring buffer → updateSlot()

	One registry. One set of globals. One latency tracker.
*/
#include "types.h"
#include "maplecast_input_server.h"
#include "maplecast_mirror.h"
#include "maplecast_telemetry.h"
#include "replay_writer.h"
#include "replay_reader.h"
#include "input/gamepad_device.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <climits>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <sched.h>
#include <pwd.h>
#include <time.h>
#include <strings.h>
#include <fstream>
#include <sstream>

// Gamepad globals — CMD9 reads these via ggpo::getLocalInput()
extern u32 kcode[4];
extern u16 rt[4], lt[4];

namespace maplecast_input
{

static std::atomic<bool> _active{false};
static std::thread _udpThread;
static int _udpSock = -1;

// ---------------------------------------------------------------------
// Input tape — Phase 1 (lockstep-player-client branch)
// ---------------------------------------------------------------------
//
// Per-slot bounded ring buffer. Single producer (the UDP/XDP input
// thread writing via updateSlot → pushTapeEntry) and single consumer
// (the tape publisher thread draining). Power-of-two capacity so the
// head/tail wrap is a cheap mask.
//
// Sizing: 1024 entries per slot → 16 KB per slot → 32 KB total. At a
// 1 kHz NOBD input rate that's one second of headroom before the
// publisher falls behind, which is far more than we'd ever want to
// buffer (a wedged publisher is better detected than papered over).
static constexpr size_t kTapeRingSize = 1024;
static constexpr size_t kTapeRingMask = kTapeRingSize - 1;
static_assert((kTapeRingSize & kTapeRingMask) == 0, "kTapeRingSize must be power of two");

struct TapeRing {
	TapeEntry                slots[kTapeRingSize];
	std::atomic<uint64_t>    head{0};   // next write index (producer)
	std::atomic<uint64_t>    tail{0};   // next read index  (consumer)
};
static TapeRing _tapeRings[2];

// Publisher state
static std::thread        _tapePubThread;
static int                _tapeSock = -1;
static constexpr int      kTapePort = 7101;
static constexpr int64_t  kSubscriberTtlUs = 5 * 1000000LL;   // 5 s
static constexpr size_t   kMaxEntriesPerPacket = 72;          // 72 * 16 + 8 = 1160 bytes

struct TapeSubscriber {
	uint32_t ip;          // network byte order
	uint16_t port;        // network byte order
	int64_t  lastSeenUs;
};
static std::vector<TapeSubscriber> _tapeSubs;
static std::mutex                  _tapeSubsMutex;

// Telemetry
static std::atomic<uint64_t> _tapeEntriesPushed{0};
static std::atomic<uint64_t> _tapeEntriesDropped{0};
static std::atomic<uint64_t> _tapePacketsSent{0};
static std::atomic<uint64_t> _tapeBytesSent{0};
static std::atomic<uint64_t> _tapeLastPublishedFrame{0};

// Player registry — THE single source of truth
static PlayerInfo _players[2] = {};
static std::mutex _registryMutex;

// Phase A — tear-free packed input slot for the CMD9 latch path. Storage
// definition for the extern declared in the header. Initialized to "neutral
// active-low buttons, zero triggers, seq 0" so a read before the first
// write produces a sane default rather than UB.
std::atomic<uint64_t> _slotInputAtomic[2] = {
	std::atomic<uint64_t>(packSlotInput(0xFFFF, 0, 0, 0)),
	std::atomic<uint64_t>(packSlotInput(0xFFFF, 0, 0, 0)),
};

// Phase B — per-slot input accumulator for ConsistencyFirst policy.
// Initialized to "neutral active-low (0xFFFF), no edges accumulated, zero
// triggers". Storage for the extern in the header.
static inline uint64_t _initialAccum() {
	AccumPacked a{};
	a.any_pressed = 0;
	a.any_released = 0;
	a.current = 0xFFFF;     // active-low neutral — no buttons pressed
	a.lt = 0;
	a.rt = 0;
	return packAccum(a);
}
std::atomic<uint64_t> _slotAccum[2] = {
	std::atomic<uint64_t>(_initialAccum()),
	std::atomic<uint64_t>(_initialAccum()),
};

// Phase B — per-slot latch policy. Default LatencyFirst. Set globally by
// the MAPLECAST_LATCH_POLICY env var at module init; per-slot overrides at
// runtime via setLatchPolicy() (called from the WS control message handler
// in B.7). Stored as atomic so the read in getLocalInput() doesn't need a
// lock.
static std::atomic<LatchPolicy> _latchPolicy[2] = {
	std::atomic<LatchPolicy>(LatchPolicy::LatencyFirst),
	std::atomic<LatchPolicy>(LatchPolicy::LatencyFirst),
};

// Phase B — guard window for ConsistencyFirst policy. Default 500 us.
// MAPLECAST_GUARD_US env var overrides at startup. The latch path reads
// this once per call via std::memory_order_relaxed — it's effectively
// constant after init.
static std::atomic<int64_t> _guardUs{500};

int64_t getGuardUs() {
	return _guardUs.load(std::memory_order_relaxed);
}

// Runtime setter for the guard window. Used by the ImGui debug overlay on
// native mirror clients so an operator can sweep the value while watching
// the latch-timing graph. Atomic store; safe to call from any thread. The
// latch path's getGuardUs() picks up the new value on its next read.
void setGuardUs(int64_t us) {
	if (us < 0) us = 0;
	if (us > 5000) us = 5000;
	_guardUs.store(us, std::memory_order_relaxed);
	printf("[input-server] guard window → %lld us\n", (long long)us);
}

LatchPolicy getLatchPolicy(int slot) {
	if (slot < 0 || slot > 1) return LatchPolicy::LatencyFirst;
	return _latchPolicy[slot].load(std::memory_order_acquire);
}

void setLatchPolicy(int slot, LatchPolicy policy) {
	if (slot < 0 || slot > 1) return;
	_latchPolicy[slot].store(policy, std::memory_order_release);
	const char* name = (policy == LatchPolicy::ConsistencyFirst) ? "consistency" : "latency";
	printf("[input-server] slot %d latch policy → %s\n", slot, name);
}

AccumPacked drainAccumulator(int slot) {
	AccumPacked snap{};
	if (slot < 0 || slot > 1) return snap;
	uint64_t old = _slotAccum[slot].load(std::memory_order_acquire);
	for (;;) {
		AccumPacked a = unpackAccum(old);
		AccumPacked cleared = a;
		cleared.any_pressed = 0;
		cleared.any_released = 0;
		uint64_t target = packAccum(cleared);
		if (_slotAccum[slot].compare_exchange_weak(
		        old, target,
		        std::memory_order_acq_rel, std::memory_order_acquire)) {
			snap = a;
			break;
		}
		// CAS failed: `old` was reloaded by compare_exchange_weak. Loop.
	}
	return snap;
}

// Phase B — full ConsistencyFirst latch computation for one slot.
// Called from the latch path (ggpo::getLocalInput) when the slot's policy
// is ConsistencyFirst. Drains the accumulator, applies edge preservation,
// blip-press deferred releases, and the guard window. Mutates per-slot
// state in _players[slot].
ConsistencyLatchResult consistencyFirstLatch(int slot, int64_t tLatchUs)
{
	ConsistencyLatchResult r{};
	if (slot < 0 || slot > 1) {
		r.buttons = 0xFFFF;
		return r;
	}

	PlayerInfo& p = _players[slot];

	// Drain accumulator FIRST. This atomically reads-and-clears the
	// any_pressed/any_released bits while keeping current/lt/rt. Even when
	// the guard fires, we still drain — the guard only freezes what THIS
	// latch reports; the next latch needs a fresh accumulator state. If we
	// didn't drain, the guarded events would re-fire on the next latch as
	// "still pressed" instead of "newly pressed".
	AccumPacked a = drainAccumulator(slot);
	r.lt = a.lt;
	r.rt = a.rt;
	r.packetSeq = p.lastPacketSeq;

	// Compute the latched buttons under edge preservation:
	//   start from a.current (the most recent button state),
	//   clear any bit that was pressed at any moment during the interval
	//   (active-low: pressed = bit cleared) — this catches the case where
	//   a button was pressed AND released within the interval.
	uint16_t latched = (uint16_t)(a.current & ~a.any_pressed);

	// Apply the deferred releases from the previous latch. Bits that were
	// pressed-and-released last frame need to actually go back to released
	// (active-low: bit set) NOW, on the very next frame. Without this,
	// blip-presses would stay held forever because nothing else clears
	// the press-bit in `latched`.
	latched = (uint16_t)(latched | p.deferredReleaseMask);

	// Compute the new deferred mask for the NEXT latch: bits that were
	// both pressed AND released during this interval are blip-presses.
	// They appear pressed THIS frame (cleared by `& ~a.any_pressed` above)
	// and need to be released NEXT frame.
	uint16_t newDeferred = (uint16_t)(a.any_pressed & a.any_released);

	// Guard window: if the network thread touched this slot within the
	// last guardUs and that touch is "fresh" (packet seq advanced since the
	// previous latch on this slot), classify it as a near-boundary arrival
	// and freeze the latched buttons at last frame's value. This converts
	// boundary jitter into a deterministic +1-frame mapping for the affected
	// inputs. Telemetry counts each guard fire so we can see the rate.
	//
	// Note: the accumulator itself was already drained above, so when the
	// guard fires the next-frame latch will see ALL the events (the ones
	// that landed inside the guard window plus any new ones from the next
	// vblank interval) accumulated correctly.
	const int64_t guard = getGuardUs();
	const int64_t deltaUs = tLatchUs - p.lastPacketUs;
	bool guardFired = false;
	if (guard > 0 && p.lastPacketUs > 0 && deltaUs >= 0 && deltaUs < guard) {
		// Only freeze if we actually saw activity worth deferring (i.e. the
		// drained accumulator had at least one press or release event). If
		// nothing happened this interval, freezing == passing through last
		// frame's state, which is the same thing — but skipping the freeze
		// keeps the telemetry counter honest about "real" guard fires.
		if (a.any_pressed != 0 || a.any_released != 0) {
			latched = p.lastLatchedButtons;
			// Don't update deferredReleaseMask either — we're frozen.
			newDeferred = p.deferredReleaseMask;
			p.guardHits++;
			guardFired = true;
		}
	}

	// Persist for the next call.
	p.lastLatchedButtons = latched;
	p.deferredReleaseMask = newDeferred;

	r.buttons = latched;
	r.guardFired = guardFired;
	return r;
}

// Phase A — latch telemetry. Per-slot ring buffer of recent (delta_us)
// samples + counters. The maple thread (single producer per slot) writes
// via recordLatchSample(); the WS thread reads aggregate stats once per
// second via getLatchStats(). The seq mismatch trick gives "did the
// network thread touch this slot since the last latch" without locking.
//
// Ring size: 256 samples ≈ 4.3 seconds at 60 Hz — a meaningful
// "recent input quality" window. 256 × 8 bytes × 2 slots = 4 KB total,
// fits in L1. p99 is computed by copy + sort on the read path; ~10 µs
// per call, called once per second from the WS status broadcaster.
constexpr int LATCH_RING_SIZE = 256;

struct LatchStatsAccum {
	std::atomic<uint64_t> totalLatches{0};
	std::atomic<uint64_t> latchesWithData{0};
	std::atomic<uint32_t> lastPacketSeq{0};
	std::atomic<uint64_t> lastFrameNum{0};
	std::atomic<uint32_t> lastSeenSeq{0};        // "did packet seq advance since last latch?"

	// Ring buffer of recent delta_us samples, in arrival order. Single
	// producer (the maple thread) so a plain index + memory_order_release
	// store is sufficient. Reader copies the whole ring under a snapshot.
	int64_t  ring[LATCH_RING_SIZE] = {};
	std::atomic<uint32_t> ringWriteIdx{0};       // monotonic — modulo gives ring slot
};
static LatchStatsAccum _latchStats[2];

void recordLatchSample(int slot, int64_t deltaUs, uint32_t packetSeq, uint64_t frameNum)
{
	if (slot < 0 || slot > 1) return;
	LatchStatsAccum& s = _latchStats[slot];

	// Single producer (the maple thread on the headless server, or the SH4
	// vblank handler in dev builds — same thread either way for a given run).
	// Multiple producer would require a CAS loop here; we don't need one.
	s.totalLatches.fetch_add(1, std::memory_order_relaxed);

	// Always record the most recent frame number — even on slots with no
	// connected input, this is a useful liveness signal ("the input system
	// is ticking once per vblank").
	s.lastFrameNum.store(frameNum, std::memory_order_relaxed);

	// If the seq is 0, the network thread has never written to this slot
	// since boot. The deltaUs the caller computed is meaningless in that
	// case (it's t_latch - 0 = uptime since boot). Skip the ring write,
	// don't bump latches_with_data, and don't update lastPacketSeq. The
	// telemetry histogram should only contain real input timing data.
	if (packetSeq == 0)
		return;

	// Did the network thread write to this slot since the previous latch?
	// Counts as "fresh input arrived this frame interval".
	uint32_t prev = s.lastSeenSeq.exchange(packetSeq, std::memory_order_relaxed);
	const bool freshArrival = (packetSeq != prev);
	if (freshArrival)
		s.latchesWithData.fetch_add(1, std::memory_order_relaxed);

	// Only write to the ring on vblanks where a FRESH packet arrived this
	// interval. The metric we want to track is "when a packet arrives, how
	// stale is it by latch time?" — meaningful only for active vblanks.
	// Idle vblanks (packetSeq == prev, player hasn't moved their stick) would
	// otherwise pollute the histogram with "time since last packet" values
	// that grow without bound during idle periods. The latches_with_data
	// counter still ticks on every fresh vblank so the "fresh%" stat works
	// regardless of histogram state.
	if (freshArrival) {
		uint32_t idx = s.ringWriteIdx.fetch_add(1, std::memory_order_relaxed);
		s.ring[idx % LATCH_RING_SIZE] = deltaUs;
	}

	s.lastPacketSeq.store(packetSeq, std::memory_order_relaxed);
}

LatchStats getLatchStats(int slot)
{
	LatchStats out{};
	if (slot < 0 || slot > 1) return out;
	LatchStatsAccum& s = _latchStats[slot];

	out.totalLatches    = s.totalLatches.load(std::memory_order_relaxed);
	out.latchesWithData = s.latchesWithData.load(std::memory_order_relaxed);
	out.lastPacketSeq   = s.lastPacketSeq.load(std::memory_order_relaxed);
	out.lastFrameNum    = s.lastFrameNum.load(std::memory_order_relaxed);

	// Snapshot the ring. There's a benign race here: the maple thread might
	// write a new sample mid-copy, in which case our snapshot includes one
	// "in progress" frame. That's fine — we're computing aggregates over a
	// 256-sample window, one extra sample doesn't move the percentile.
	uint32_t writeIdx = s.ringWriteIdx.load(std::memory_order_relaxed);
	uint32_t validCount = (writeIdx < LATCH_RING_SIZE) ? writeIdx : LATCH_RING_SIZE;
	if (validCount == 0) return out;

	int64_t snap[LATCH_RING_SIZE];
	for (uint32_t i = 0; i < validCount; ++i) snap[i] = s.ring[i];
	std::sort(snap, snap + validCount);

	// avg, min, max, p99 over the ring window (last ~4 seconds of latches)
	int64_t sum = 0;
	for (uint32_t i = 0; i < validCount; ++i) sum += snap[i];
	out.avgDeltaUs = sum / (int64_t)validCount;
	out.minDeltaUs = snap[0];
	out.maxDeltaUs = snap[validCount - 1];
	uint32_t p99Idx = (validCount * 99) / 100;
	if (p99Idx >= validCount) p99Idx = validCount - 1;
	out.p99DeltaUs = snap[p99Idx];
	return out;
}

// Telemetry
static std::atomic<uint64_t> _totalPackets{0};

// Stick registration — username-based.
// Persistence: WS server drains _pendingStickEvents and forwards to the
// collector for SurrealDB writes; saveStickCache() also mirrors current
// state to ~/.maplecast/sticks.json so flycast survives a restart even
// when the collector is briefly unreachable.
struct StickBinding {
	uint32_t srcIP;
	uint16_t srcPort;
	char username[16];       // 4-12 chars, [a-zA-Z0-9_]
	char browserId[64];      // legacy compat (same as username for new registrations)
	int64_t lastInputUs;     // for online detection
	bool wasOnline;          // last reported online state — for edge detection
};
static std::vector<StickBinding> _stickBindings;
static std::mutex _stickMutex;
static std::vector<StickEvent> _pendingStickEvents;

// Rhythm registration in progress (legacy)
static bool _registering = false;
static char _registerBrowserId[64] = {};

// Web registration — simpler "any press" mode
static bool _webRegistering = false;
static char _webRegisterUsername[16] = {};
static int64_t _webRegisterStartUs = 0;

struct RhythmTracker {
	uint32_t srcIP;
	uint16_t srcPort;
	uint16_t prevButtons;
	int tapCount;        // taps in current burst
	int burstCount;      // completed bursts (need 2)
	int64_t lastTapUs;   // time of last tap
	int64_t burstEndUs;  // when first burst ended
};
static std::vector<RhythmTracker> _rhythmTrackers;

// Forensics: every (srcIP, srcPort) we've ever received a UDP packet from,
// hashed to a single uint64. First-appearance is logged so we can identify
// rogue clients sending input. Lifetime = process lifetime; cleared on
// flycast restart. Memory cost is ~24 bytes per unique source — bounded by
// the number of distinct IP:port pairs that ever talk to us.
static std::unordered_set<uint64_t> _seenUdpSources;
static std::mutex _seenUdpSourcesMutex;
static inline uint64_t makeSourceKey(uint32_t ip, uint16_t port)
{
	return ((uint64_t)ip << 16) | (uint64_t)port;
}

// Phase 2 input redundancy: dedup by sequence number per source-key.
// 11-byte packets carry a u32 seq; client sends each packet twice (T+0
// + T+1ms) for loss tolerance. Server skips packets whose seq <= the
// last we saw for that source. Map grows with unique sources but each
// entry is ~16 bytes — bounded by player population.
static std::unordered_map<uint64_t, uint32_t> _lastSeenSeq;
static std::mutex _dedupMutex;
static std::atomic<uint64_t> _dedupCount{0};

// Per-slot input timing — written by UDP thread, read at vblank latch.
// Server-side CLOCK_MONOTONIC µs when the most recent input packet arrived.
static std::atomic<uint64_t> _lastInputArrivalUs[2] = {0, 0};
// Client-side CLOCK_MONOTONIC µs from the 19-byte packet (different clock domain).
static std::atomic<uint64_t> _lastInputClientTs[2] = {0, 0};
// Sequence number of the last arrival (so latch can detect "new since last check")
static std::atomic<uint32_t> _lastInputArrivalSeq[2] = {0, 0};
// Input age at vblank: how many µs old the input was when the game latched it.
// Written at vblank, read by telemetry/HUD. EMA with α=1/16.
static std::atomic<int64_t> _inputAgeAtLatchUs[2] = {0, 0};
static std::atomic<int64_t> _inputAgeEmaUs[2] = {0, 0};
// Last seq seen by measureInputAge, to detect new packets
static uint32_t _latchLastSeq[2] = {0, 0};

static inline int64_t nowUs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------------
// Input tape — push / publish / stats
// ---------------------------------------------------------------------

// Internal worker: push a single entry with an explicit frame stamp.
// Used by both pushTapeEntry (live, currentFrame()) and publishFrameTick
// (called once per server emu frame from serverPublish, with the new frame
// number explicitly passed in).
static void pushTapeEntryAtFrame(int slot, uint64_t frame, uint16_t buttons,
                                 uint8_t lt_, uint8_t rt_, uint32_t seq)
{
	if (slot < 0 || slot > 1) return;

	TapeRing& ring = _tapeRings[slot];
	const uint64_t head = ring.head.load(std::memory_order_relaxed);
	const uint64_t tail = ring.tail.load(std::memory_order_acquire);

	// Ring full? Drop the OLDEST entry by advancing tail. We are the sole
	// producer; the publisher is the sole consumer. Advancing tail from the
	// producer is safe only because a dropped old entry is strictly
	// preferable to losing the newest one — clients can re-sync from later
	// entries since each entry is absolute state, not a delta.
	if (head - tail >= kTapeRingSize) {
		ring.tail.store(tail + 1, std::memory_order_release);
		_tapeEntriesDropped.fetch_add(1, std::memory_order_relaxed);
	}

	TapeEntry& e = ring.slots[head & kTapeRingMask];
	e.frame      = frame;
	e.seqAndSlot = packSeqSlot(seq, (uint8_t)slot);
	e.buttons    = buttons;
	e.lt         = lt_;
	e.rt         = rt_;

	ring.head.store(head + 1, std::memory_order_release);
	_tapeEntriesPushed.fetch_add(1, std::memory_order_relaxed);

	// Phase 4: replay recording. No-op when not active (single atomic
	// load returns early). Captures EVERY input event the game sees,
	// which is exactly what we need for deterministic playback.
	maplecast_replay::append(frame, e.seqAndSlot, buttons, lt_, rt_);
}

void pushTapeEntry(int slot, uint16_t buttons, uint8_t lt_, uint8_t rt_, uint32_t seq)
{
	pushTapeEntryAtFrame(slot, maplecast_mirror::currentFrame(), buttons, lt_, rt_, seq);
}

// GGPO-style dense tape: called once per server emu frame from
// maplecast_mirror::serverPublish, RIGHT BEFORE the server frame counter
// is bumped, with the frame number that's about to become current. We
// snapshot the live packed-atomic for both slots and push one entry per
// slot stamped with `frame`. This is what makes the client's blocking
// frameGate work — every frame number from the server's perspective has
// at least one entry in the tape, even if no input changed.
//
// Bandwidth: 2 slots * 16 bytes * 60 Hz = ~2 KB/sec. Trivial.
void publishFrameTick(uint64_t frame)
{
	for (int slot = 0; slot < 2; slot++) {
		const uint64_t packed = _slotInputAtomic[slot].load(std::memory_order_acquire);
		uint16_t buttons;
		uint8_t  ltVal;
		uint8_t  rtVal;
		uint32_t seq;
		unpackSlotInput(packed, buttons, ltVal, rtVal, seq);
		pushTapeEntryAtFrame(slot, frame, buttons, ltVal, rtVal, seq);
	}
}

TapeStats getTapeStats()
{
	TapeStats s{};
	s.entriesPushed      = _tapeEntriesPushed.load(std::memory_order_relaxed);
	s.entriesDropped     = _tapeEntriesDropped.load(std::memory_order_relaxed);
	s.packetsSent        = _tapePacketsSent.load(std::memory_order_relaxed);
	s.bytesSent          = _tapeBytesSent.load(std::memory_order_relaxed);
	s.lastPublishedFrame = _tapeLastPublishedFrame.load(std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(_tapeSubsMutex);
		s.subscribers = (uint32_t)_tapeSubs.size();
	}
	return s;
}

// Drain one slot's ring up to `maxEntries` into out[]. Returns how many
// were copied. Consumer side — only called from _tapePubThread.
static size_t drainTapeRing(int slot, TapeEntry* out, size_t maxEntries)
{
	TapeRing& ring = _tapeRings[slot];
	const uint64_t tail = ring.tail.load(std::memory_order_relaxed);
	const uint64_t head = ring.head.load(std::memory_order_acquire);
	uint64_t avail = head - tail;
	if (avail == 0) return 0;
	if (avail > maxEntries) avail = maxEntries;
	for (uint64_t i = 0; i < avail; i++)
		out[i] = ring.slots[(tail + i) & kTapeRingMask];
	ring.tail.store(tail + avail, std::memory_order_release);
	return (size_t)avail;
}

static void tapePublisherLoop()
{
	printf("[input-server] tape publisher thread started on port %d\n", kTapePort);

	// Poll both the tape socket (for HELO subscriptions) and the drain ring
	// in a tight loop. A 1 ms recv timeout gives us ~1 kHz publish cadence
	// which is well below the tape ring's drop threshold under typical
	// load (NOBD sticks are ~1 kHz *input*, tape publishing is whatever
	// rate the publisher chooses — we batch up to kMaxEntriesPerPacket per
	// datagram).
	struct timeval tv = { .tv_sec = 0, .tv_usec = 1000 };
	setsockopt(_tapeSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	uint8_t rxBuf[64];
	struct sockaddr_in from;
	socklen_t fromLen;

	// Packet assembly buffer: 8-byte header + up to kMaxEntriesPerPacket entries
	uint8_t pktBuf[8 + kMaxEntriesPerPacket * sizeof(TapeEntry)];

	while (_active.load(std::memory_order_relaxed))
	{
		// --- 1. handle one HELO if pending (non-blocking-ish with 1ms timeout) ---
		fromLen = sizeof(from);
		int n = recvfrom(_tapeSock, rxBuf, sizeof(rxBuf), 0,
		                 (struct sockaddr *)&from, &fromLen);
		if (n >= 4 && memcmp(rxBuf, "HELO", 4) == 0)
		{
			std::lock_guard<std::mutex> lock(_tapeSubsMutex);
			int64_t now = nowUs();
			bool found = false;
			for (auto& s : _tapeSubs) {
				if (s.ip == from.sin_addr.s_addr && s.port == from.sin_port) {
					s.lastSeenUs = now;
					found = true;
					break;
				}
			}
			if (!found) {
				TapeSubscriber s{};
				s.ip         = from.sin_addr.s_addr;
				s.port       = from.sin_port;
				s.lastSeenUs = now;
				_tapeSubs.push_back(s);
				char ipstr[INET_ADDRSTRLEN];
				inet_ntop(AF_INET, &from.sin_addr, ipstr, sizeof(ipstr));
				printf("[input-server] tape subscriber joined: %s:%u (total=%zu)\n",
				       ipstr, ntohs(from.sin_port), _tapeSubs.size());
			}
		}

		// --- 2. age out stale subscribers ---
		{
			std::lock_guard<std::mutex> lock(_tapeSubsMutex);
			int64_t now = nowUs();
			auto it = _tapeSubs.begin();
			while (it != _tapeSubs.end()) {
				if (now - it->lastSeenUs > kSubscriberTtlUs) {
					char ipstr[INET_ADDRSTRLEN];
					struct in_addr a; a.s_addr = it->ip;
					inet_ntop(AF_INET, &a, ipstr, sizeof(ipstr));
					printf("[input-server] tape subscriber timed out: %s:%u\n",
					       ipstr, ntohs(it->port));
					it = _tapeSubs.erase(it);
				} else {
					++it;
				}
			}
		}

		// --- 3. drain both slots and build a single packet ---
		TapeEntry entries[kMaxEntriesPerPacket];
		size_t count = 0;

		// Interleave slot drains so neither slot starves the other under load.
		// drainTapeRing returns up to (max - count) entries per call.
		for (int pass = 0; pass < 2 && count < kMaxEntriesPerPacket; pass++) {
			for (int slot = 0; slot < 2 && count < kMaxEntriesPerPacket; slot++) {
				size_t budget = kMaxEntriesPerPacket - count;
				// First pass: give each slot half the budget so a busy slot
				// can't starve the other. Second pass mops up whatever's left.
				if (pass == 0) budget = std::min(budget, kMaxEntriesPerPacket / 2);
				size_t got = drainTapeRing(slot, entries + count, budget);
				count += got;
			}
		}

		if (count == 0) continue;

		// Track highest frame drained for telemetry
		uint64_t highestFrame = _tapeLastPublishedFrame.load(std::memory_order_relaxed);
		for (size_t i = 0; i < count; i++)
			if (entries[i].frame > highestFrame)
				highestFrame = entries[i].frame;
		_tapeLastPublishedFrame.store(highestFrame, std::memory_order_relaxed);

		// --- 4. assemble datagram ---
		pktBuf[0] = 'I'; pktBuf[1] = 'N'; pktBuf[2] = 'P'; pktBuf[3] = 'T';
		pktBuf[4] = 1;                        // version
		pktBuf[5] = (uint8_t)count;           // entry count
		pktBuf[6] = 0; pktBuf[7] = 0;         // reserved
		memcpy(pktBuf + 8, entries, count * sizeof(TapeEntry));
		const size_t pktLen = 8 + count * sizeof(TapeEntry);

		// --- 5. fan out to subscribers ---
		std::vector<TapeSubscriber> snap;
		{
			std::lock_guard<std::mutex> lock(_tapeSubsMutex);
			snap = _tapeSubs;  // copy so we don't hold the lock during sendto
		}
		for (const auto& s : snap) {
			struct sockaddr_in to = {};
			to.sin_family      = AF_INET;
			to.sin_addr.s_addr = s.ip;
			to.sin_port        = s.port;
			ssize_t sent = sendto(_tapeSock, pktBuf, pktLen, 0,
			                      (struct sockaddr *)&to, sizeof(to));
			if (sent > 0) {
				_tapePacketsSent.fetch_add(1, std::memory_order_relaxed);
				_tapeBytesSent.fetch_add((uint64_t)sent, std::memory_order_relaxed);
			}
		}
	}

	printf("[input-server] tape publisher thread stopped\n");
}

// Write button state to kcode[]/lt[]/rt[] globals AND update player stats
static void updateSlot(int slot, uint8_t ltVal, uint8_t rtVal, uint16_t buttons)
{
	if (slot < 0 || slot > 1) return;

	// Phase A — tear-free packed slot atomic FIRST. The CMD9 latch in
	// ggpo::getLocalInput() reads from this single 64-bit word, so writers
	// and readers can never see a torn buttons/lt/rt triple. Sequence number
	// is incremented monotonically; the network thread is the only writer
	// per slot, so a non-atomic ++ on lastPacketSeq is fine.
	PlayerInfo& p = _players[slot];
	uint32_t seq = ++p.lastPacketSeq;
	_slotInputAtomic[slot].store(packSlotInput(buttons, ltVal, rtVal, seq),
	                             std::memory_order_release);

	// NOTE: tape entries used to be pushed from here on every input
	// change. They are now pushed exclusively by publishFrameTick(),
	// called once per server emu frame from maplecast_mirror::serverPublish.
	// This is what makes the lockstep-player-client tape "dense" — every
	// server frame produces exactly one entry per slot, so the client's
	// blocking frameGate can use queue-has-entry-at-frame-N as a
	// definitive signal. Pushing from updateSlot would race with the
	// per-frame snapshot and create out-of-order entries.

	// Phase B — input accumulator CAS loop. Always update, regardless of
	// the per-slot LatchPolicy. The accumulator must be live so a runtime
	// switch from LatencyFirst → ConsistencyFirst doesn't see stale data.
	// The latch path (getLocalInput → drainAccumulator) reads it only when
	// the policy is ConsistencyFirst, so it's free for LatencyFirst slots.
	//
	// Edge detection (active-low semantics):
	//   newly_pressed  = bits that were 1 (released) and are now 0 (pressed)
	//   newly_released = bits that were 0 (pressed)  and are now 1 (released)
	// any_pressed/any_released are sticky-OR until the next drain — every
	// transition during a vblank interval is preserved.
	{
		uint64_t old = _slotAccum[slot].load(std::memory_order_acquire);
		for (;;) {
			AccumPacked a = unpackAccum(old);
			uint16_t newly_pressed  = (uint16_t)((~buttons) & a.current);
			uint16_t newly_released = (uint16_t)(buttons & (~a.current));
			AccumPacked next = a;
			next.any_pressed  = (uint16_t)(a.any_pressed  | newly_pressed);
			next.any_released = (uint16_t)(a.any_released | newly_released);
			next.current = buttons;
			next.lt = ltVal;
			next.rt = rtVal;
			uint64_t target = packAccum(next);
			if (_slotAccum[slot].compare_exchange_weak(
			        old, target,
			        std::memory_order_acq_rel, std::memory_order_acquire))
				break;
			// CAS failed: `old` was reloaded by compare_exchange_weak. Loop.
			// In practice this almost never fails — single producer in the
			// hot path (UDP/XDP thread for a given slot), and the consumer
			// (drainAccumulator) is a tiny CAS too.
		}
	}

	// Legacy plain globals — kept in sync for non-MapleCast paths (SDL local
	// gamepads on the dev box still write these directly, and ggpo's
	// getLocalInput() falls back to them for slots 2/3 and other input fields).
	// CMD9 still reads these on slots that don't go through the maplecast atomic.
	kcode[slot] = buttons | 0xFFFF0000;  // active-low, upper 16 bits set
	lt[slot] = (uint16_t)ltVal << 8;
	rt[slot] = (uint16_t)rtVal << 8;

	// Update player stats
	int64_t now = nowUs();
	p.lastPacketUs = now;
	p.lt = ltVal;
	p.rt = rtVal;

	// Track state changes (also drives idle-kick — see ws_server checkIdleKick)
	if (buttons != p._prevButtons)
	{
		p._chgAccum++;
		p._prevButtons = buttons;
		p.lastChangeUs = now;
	}
	p.buttons = buttons;
	p._pktAccum++;

	// Update per-second rates every second
	if (now - p._lastRateTime >= 1000000)
	{
		int64_t elapsed = now - p._lastRateTime;
		p.packetsPerSec = (uint32_t)(p._pktAccum * 1000000LL / elapsed);
		p.changesPerSec = (uint32_t)(p._chgAccum * 1000000LL / elapsed);
		p._pktAccum = 0;
		p._chgAccum = 0;
		p._lastRateTime = now;
	}

	_totalPackets.fetch_add(1, std::memory_order_relaxed);
}

// Find which slot a source IP belongs to (-1 if unknown)
static int findSlotByIP(uint32_t srcIP)
{
	for (int i = 0; i < 2; i++)
		if (_players[i].connected && _players[i].srcIP == srcIP)
			return i;
	return -1;
}

// autoAssignUDP removed — NOBD sticks must register via browser first

// UDP listener thread — receives NOBD stick and WebSocket-forwarded packets
static void udpThreadLoop(int port)
{
	printf("[input-server] UDP thread started on port %d\n", port);

	// ── Kernel tuning for 12KHz input ingress ─────────────────────────
	//
	// SO_BUSY_POLL: spin-poll NIC for 10µs before sleeping — avoids the
	// ~50µs interrupt-to-userspace wakeup on most VPS kernels.
	int busy_poll = 10;
	setsockopt(_udpSock, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));

	// SO_RCVBUF: 4MB receive buffer. At 12KHz × 11 bytes = 132 KB/s,
	// this gives ~30s of buffering against scheduling jitter.
	int rcvbuf = 4 * 1024 * 1024;
	setsockopt(_udpSock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

	// IP_TOS: mark input traffic as EF (Expedited Forwarding, DSCP 46)
	// so kernel QoS and NIC hardware prioritize these packets.
	int tos = 0xB8;  // DSCP EF = 46 << 2
	setsockopt(_udpSock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

	struct timeval tv = { .tv_sec = 0, .tv_usec = 1000 };  // 1ms recv timeout
	setsockopt(_udpSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	// SCHED_FIFO: real-time priority for the input thread. Ensures we
	// drain the UDP socket before the kernel drops packets. Graceful
	// fallback if CAP_SYS_NICE is missing.
	{
		struct sched_param sp{};
		sp.sched_priority = 55;  // above input-sink (50), below audio (70+)
		if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0)
			printf("[input-server] UDP thread → SCHED_FIFO priority 55\n");
		else
			printf("[input-server] SCHED_FIFO not granted, staying SCHED_OTHER\n");
	}

	// mlockall: prevent page faults during gameplay. At 12KHz, a single
	// page fault (~10-100µs) can drop 1-10 input packets.
	if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
		printf("[input-server] mlockall() — memory locked\n");
	else
		printf("[input-server] mlockall() failed (need CAP_IPC_LOCK)\n");

	uint8_t buf[64];
	struct sockaddr_in from;
	socklen_t fromLen;

	while (_active.load(std::memory_order_relaxed))
	{
		fromLen = sizeof(from);
		int n = recvfrom(_udpSock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromLen);
		if (n < 4) continue;

		// PROBE-ACK responder for native client hub-discovery RTT measurement.
		// Wire format: client sends [0xFF, seq:u8, 0, 0, 0, 0, 0] (7 bytes).
		// We echo back [0xFE, seq:u8, ts_lo:u32_LE, ts_hi:u16_LE] (8 bytes)
		// where ts is server CLOCK_MONOTONIC microseconds (truncated low 48
		// bits split LE). Client measures RTT as (now - send_time). Doesn't
		// touch any input state — pure echo, takes ~2µs.
		if (n >= 4 && buf[0] == 0xFF) {
			uint64_t ts = nowUs();
			uint8_t reply[8];
			reply[0] = 0xFE;
			reply[1] = buf[1];                       // echo seq
			reply[2] = (uint8_t)(ts);
			reply[3] = (uint8_t)(ts >> 8);
			reply[4] = (uint8_t)(ts >> 16);
			reply[5] = (uint8_t)(ts >> 24);
			reply[6] = (uint8_t)(ts >> 32);
			reply[7] = (uint8_t)(ts >> 40);
			sendto(_udpSock, reply, sizeof(reply), 0,
			       (struct sockaddr*)&from, fromLen);
			continue;
		}

		const uint8_t *w3 = buf;
		int slot = -1;

		// 5-byte tagged from WebSocket: [slot][LT][RT][btn_hi][btn_lo]
		if (n >= 5 && buf[0] <= 1 && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
		{
			slot = buf[0];
			w3 = buf + 1;
		}

		// 11 or 19-byte player-client packet:
		//   "PC"[slot][seq:u32_LE][LT][RT][btn_hi][btn_lo]           (11 bytes)
		//   "PC"[slot][seq:u32_LE][LT][RT][btn_hi][btn_lo][ts:u64_LE] (19 bytes)
		// The 19-byte variant includes a client CLOCK_MONOTONIC timestamp
		// (microseconds) for input-age measurement at vblank latch time.
		// Backward compatible: server accepts both sizes.
		else if ((n == 11 || n == 19) && buf[0] == 'P' && buf[1] == 'C' && buf[2] <= 1)
		{
			int claimedSlot = buf[2];
			uint32_t seq = (uint32_t)buf[3]
			             | ((uint32_t)buf[4] << 8)
			             | ((uint32_t)buf[5] << 16)
			             | ((uint32_t)buf[6] << 24);
			w3 = buf + 7;  // [LT][RT][btn_hi][btn_lo]

			// Extract client timestamp if present (19-byte format).
			// Store the SERVER arrival time alongside it for vblank-age calc.
			{
				uint64_t serverArrival = nowUs();
				_lastInputArrivalUs[claimedSlot].store(serverArrival, std::memory_order_relaxed);
				_lastInputArrivalSeq[claimedSlot].store(seq, std::memory_order_relaxed);

				if (n == 19) {
					uint64_t clientTs = (uint64_t)buf[11]
					                  | ((uint64_t)buf[12] << 8)
					                  | ((uint64_t)buf[13] << 16)
					                  | ((uint64_t)buf[14] << 24)
					                  | ((uint64_t)buf[15] << 32)
					                  | ((uint64_t)buf[16] << 40)
					                  | ((uint64_t)buf[17] << 48)
					                  | ((uint64_t)buf[18] << 56);
					_lastInputClientTs[claimedSlot].store(clientTs, std::memory_order_relaxed);
				}
			}

			// Per-source-IP dedup. Map: source-key → last seq seen.
			// Skip if seq <= last (handles redundant T+1ms copies and
			// stray reorderings). 32-bit seq wraps every ~4 billion packets
			// — at 12 KHz that's >9 days of continuous play, plenty.
			uint64_t srcKey = makeSourceKey(from.sin_addr.s_addr, from.sin_port);
			{
				std::lock_guard<std::mutex> lock(_dedupMutex);
				auto it = _lastSeenSeq.find(srcKey);
				if (it != _lastSeenSeq.end()) {
					uint32_t lastSeq = it->second;
					// Handle wraparound: treat (lastSeq - seq) > 2^31 as new
					int32_t diff = (int32_t)(seq - lastSeq);
					if (diff <= 0) {
						_dedupCount.fetch_add(1, std::memory_order_relaxed);
						// Still ACK the duplicate so client failover detection
						// gets a heartbeat from us
						{
							uint64_t ts = nowUs();
							uint8_t reply[8];
							reply[0] = 0xFE; reply[1] = (uint8_t)seq;
							reply[2]=(uint8_t)ts; reply[3]=(uint8_t)(ts>>8);
							reply[4]=(uint8_t)(ts>>16); reply[5]=(uint8_t)(ts>>24);
							reply[6]=(uint8_t)(ts>>32); reply[7]=(uint8_t)(ts>>40);
							sendto(_udpSock, reply, sizeof(reply), 0,
							       (struct sockaddr*)&from, fromLen);
						}
						continue;
					}
				}
				_lastSeenSeq[srcKey] = seq;
			}

			// ACK the input so the client can heartbeat-detect us
			{
				uint64_t ts = nowUs();
				uint8_t reply[8];
				reply[0] = 0xFE; reply[1] = (uint8_t)seq;
				reply[2]=(uint8_t)ts; reply[3]=(uint8_t)(ts>>8);
				reply[4]=(uint8_t)(ts>>16); reply[5]=(uint8_t)(ts>>24);
				reply[6]=(uint8_t)(ts>>32); reply[7]=(uint8_t)(ts>>40);
				sendto(_udpSock, reply, sizeof(reply), 0,
				       (struct sockaddr*)&from, fromLen);
			}

			// Same auto-bind logic as 7-byte path
			std::lock_guard<std::mutex> lock(_registryMutex);
			PlayerInfo& p = _players[claimedSlot];
			bool newBinding = !p.connected
			               || p.srcIP != from.sin_addr.s_addr
			               || p.srcPort != from.sin_port;
			if (newBinding) {
				p.connected = true;
				p.type      = InputType::NobdUDP;
				p.srcIP     = from.sin_addr.s_addr;
				p.srcPort   = from.sin_port;
				snprintf(p.id,     sizeof(p.id),     "player-client");
				snprintf(p.name,   sizeof(p.name),   "PlayerClient%d", claimedSlot + 1);
				snprintf(p.device, sizeof(p.device), "MapleCast PC");
				p._prevButtons   = 0xFFFF;
				p._lastRateTime  = nowUs();
				p.lastChangeUs   = nowUs();
				uint32_t ip = ntohl(from.sin_addr.s_addr);
				printf("[input-server] PC auto-bind (11-byte): slot %d <- %u.%u.%u.%u:%u (seq=%u)\n",
				       claimedSlot,
				       (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
				       ntohs(from.sin_port), seq);
			}
			slot = claimedSlot;
		}

		// 7-byte player-client packet: "PC"[slot][LT][RT][btn_hi][btn_lo]
		// Used by the lockstep-player-client branch to let a remote native
		// flycast forward its local gamepad to the server without going
		// through browser-WS registration. Auto-binds the source IP to the
		// declared slot on the first packet. Intended for private/trusted
		// deployments — there is no auth. Add AUTH before exposing to the
		// public internet.
		else if (n == 7 && buf[0] == 'P' && buf[1] == 'C' && buf[2] <= 1)
		{
			int claimedSlot = buf[2];
			w3 = buf + 3;

			// Auto-bind source IP to the claimed slot. Overrides any
			// prior binding for that slot (last-writer-wins). The slot
			// must be "connected" first — normally that happens via a
			// browser WS join. For the Phase 4-lite test, we force it
			// connected here on first packet.
			std::lock_guard<std::mutex> lock(_registryMutex);
			PlayerInfo& p = _players[claimedSlot];
			bool newBinding = !p.connected
			               || p.srcIP != from.sin_addr.s_addr
			               || p.srcPort != from.sin_port;
			if (newBinding) {
				p.connected = true;
				p.type      = InputType::NobdUDP;   // closest fit
				p.srcIP     = from.sin_addr.s_addr;
				p.srcPort   = from.sin_port;
				snprintf(p.id,     sizeof(p.id),     "player-client");
				snprintf(p.name,   sizeof(p.name),   "PlayerClient%d", claimedSlot + 1);
				snprintf(p.device, sizeof(p.device), "MapleCast PC");
				p._prevButtons   = 0xFFFF;
				p._lastRateTime  = nowUs();
				p.lastChangeUs   = nowUs();
				uint32_t ip = ntohl(from.sin_addr.s_addr);
				printf("[input-server] PC auto-bind: slot %d <- %u.%u.%u.%u:%u\n",
				       claimedSlot,
				       (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
				       ntohs(from.sin_port));
			}
			slot = claimedSlot;
		}

		// Forensics: first packet ever from this (ip, port) gets logged with
		// full context. Subsequent packets from the same source are silent.
		// Loopback (WS-forwarder) packets are skipped because they're our own
		// traffic and the WS server already logs join/leave for those.
		if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
		{
			uint64_t key = makeSourceKey(from.sin_addr.s_addr, from.sin_port);
			bool firstTime = false;
			{
				std::lock_guard<std::mutex> lock(_seenUdpSourcesMutex);
				firstTime = _seenUdpSources.insert(key).second;
			}
			if (firstTime)
			{
				uint32_t ip = ntohl(from.sin_addr.s_addr);
				uint16_t btnState = ((uint16_t)w3[2] << 8) | w3[3];
				printf("[input-server] FORENSIC: new UDP source %u.%u.%u.%u:%u — first packet (buttons=0x%04X len=%d)\n",
					(ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
					ntohs(from.sin_port), btnState, n);
			}
		}

		// Web registration — any press from unregistered stick binds it
		if (_webRegistering && from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
		{
			uint16_t buttons = ((uint16_t)w3[2] << 8) | w3[3];
			// Any button pressed (active-low: not 0xFFFF)
			if (buttons != 0xFFFF)
			{
				// Only register if this IP isn't already registered
				if (!getRegisteredBrowserId(from.sin_addr.s_addr, from.sin_port))
				{
					registerStick(from.sin_addr.s_addr, from.sin_port, _webRegisterUsername);
					printf("[input-server] WEB REGISTER: stick %08X bound to '%s'\n",
						from.sin_addr.s_addr, _webRegisterUsername);
					_webRegistering = false;
				}
			}
		}

		// Update last input time for online tracking
		{
			std::lock_guard<std::mutex> lock(_stickMutex);
			for (auto& b : _stickBindings)
			{
				if (b.srcIP == from.sin_addr.s_addr) {
					b.lastInputUs = nowUs();
					break;
				}
			}
		}

		// Check for stick registration rhythm (only from real NOBD sticks, not loopback)
		if (_registering && from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
		{
			uint16_t buttons = ((uint16_t)w3[2] << 8) | w3[3];
			int64_t now = nowUs();

			// Find or create tracker for this source
			RhythmTracker* rt = nullptr;
			for (auto& t : _rhythmTrackers)
				if (t.srcIP == from.sin_addr.s_addr) { rt = &t; break; }
			if (!rt) {
				_rhythmTrackers.push_back({from.sin_addr.s_addr, from.sin_port, 0xFFFF, 0, 0, 0, 0});
				rt = &_rhythmTrackers.back();
			}

			// Detect button press: any bit went from 1→0 (active-low)
			uint16_t newPresses = rt->prevButtons & ~buttons;
			rt->prevButtons = buttons;

			if (newPresses)
			{
				int64_t sinceLast = now - rt->lastTapUs;
				rt->lastTapUs = now;

				if (rt->burstCount == 0)
				{
					// First burst
					if (sinceLast > 2000000) { rt->tapCount = 1; } // reset if >2s gap
					else { rt->tapCount++; }

					if (rt->tapCount >= 5) {
						rt->burstCount = 1;
						rt->burstEndUs = now;
						rt->tapCount = 0;
						printf("[input-server] Registration: burst 1 detected from %08X\n", rt->srcIP);
					}
				}
				else if (rt->burstCount == 1)
				{
					int64_t sinceBurst = now - rt->burstEndUs;
					if (sinceBurst < 500000) {
						// Too fast after first burst — still part of burst 1, ignore
					} else if (sinceBurst > 3000000) {
						// Too slow — reset
						rt->burstCount = 0;
						rt->tapCount = 1;
					} else {
						// In the pause window — count second burst
						rt->tapCount++;
						if (rt->tapCount >= 5) {
							// Pattern complete!
							registerStick(rt->srcIP, rt->srcPort, _registerBrowserId);
							_registering = false;
							_rhythmTrackers.clear();
							printf("[input-server] STICK REGISTERED to %s via rhythm!\n", _registerBrowserId);
						}
					}
				}
			}
		}

		// NOBD stick input routing — only registered sticks with active slots
		if (slot < 0 && from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
		{
			// First check if already assigned to a slot
			slot = findSlotByIP(from.sin_addr.s_addr);

			// If not assigned, check if this stick is registered to a browser user
			// who has an active slot — if so, bind this stick to that slot
			if (slot < 0)
			{
				const char* browserId = getRegisteredBrowserId(from.sin_addr.s_addr, from.sin_port);
				if (browserId)
				{
					// Find which slot this browser user is in
					for (int i = 0; i < 2; i++)
					{
						if (_players[i].connected && strncmp(_players[i].id, browserId, 8) == 0)
						{
							// Bind this stick's IP to the slot
							_players[i].srcIP = from.sin_addr.s_addr;
							_players[i].srcPort = from.sin_port;
							_players[i].type = InputType::NobdUDP;
							snprintf(_players[i].device, sizeof(_players[i].device), "NOBD Stick");
							slot = i;
							printf("[input-server] P%d NOBD stick bound to %s\n", i + 1, browserId);
							break;
						}
					}
				}
				// Unregistered sticks: silently ignore (no auto-assign)
			}
		}

		// Tagged packets from WebSocket still need slot lookup
		if (slot < 0 && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
		{
			// Already handled above via tagged[0]
		}

		if (slot < 0) continue;

		uint16_t buttons = ((uint16_t)w3[2] << 8) | w3[3];
		updateSlot(slot, w3[0], w3[1], buttons);
	}

	printf("[input-server] UDP thread stopped (%lu total packets)\n",
		_totalPackets.load());
}

// === Public API ===

bool init(int udpPort)
{
	if (_active) return true;

	memset(_players, 0, sizeof(_players));
	for (int i = 0; i < 2; i++)
	{
		_players[i].buttons = 0xFFFF;
		_players[i]._prevButtons = 0xFFFF;
		_players[i]._lastRateTime = nowUs();
	}

	// Phase B — parse MAPLECAST_LATCH_POLICY env var. "consistency" enables
	// the accumulator + edge preservation + guard window for both slots;
	// any other value (or unset) leaves the LatencyFirst default. Per-slot
	// overrides at runtime via setLatchPolicy() — set globally here as
	// the boot-time baseline.
	if (const char* lp = std::getenv("MAPLECAST_LATCH_POLICY")) {
		if (strcasecmp(lp, "consistency") == 0) {
			setLatchPolicy(0, LatchPolicy::ConsistencyFirst);
			setLatchPolicy(1, LatchPolicy::ConsistencyFirst);
		} else if (strcasecmp(lp, "latency") == 0) {
			setLatchPolicy(0, LatchPolicy::LatencyFirst);
			setLatchPolicy(1, LatchPolicy::LatencyFirst);
		} else {
			printf("[input-server] ignoring MAPLECAST_LATCH_POLICY='%s' "
			       "(expected 'latency' or 'consistency')\n", lp);
		}
	}

	// Phase B — parse MAPLECAST_GUARD_US env var. Sane bounds: 0..5000 us.
	// 0 disables the guard window without disabling the accumulator path.
	if (const char* g = std::getenv("MAPLECAST_GUARD_US")) {
		long val = strtol(g, nullptr, 10);
		if (val < 0)    val = 0;
		if (val > 5000) val = 5000;
		_guardUs.store(val, std::memory_order_relaxed);
		printf("[input-server] MAPLECAST_GUARD_US = %ld us\n", val);
	}

	// Create UDP socket
	_udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (_udpSock < 0)
	{
		printf("[input-server] socket failed: %s\n", strerror(errno));
		return false;
	}

	int reuse = 1;
	setsockopt(_udpSock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(udpPort);

	if (bind(_udpSock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		printf("[input-server] bind port %d failed: %s\n", udpPort, strerror(errno));
		close(_udpSock);
		_udpSock = -1;
		return false;
	}

	// Rehydrate stick bindings from local hot-cache (~/.maplecast/sticks.json)
	// before threads start so the very first UDP packet from a previously-
	// registered stick routes correctly. Collector will overwrite this with
	// authoritative DB state when it next connects.
	loadStickCache();

	// --- Input tape publisher socket (Phase 1 of lockstep-player-client) ---
	// Separate UDP socket on kTapePort (7101). Failure to bind is non-fatal:
	// the main input path still works, only the tape fan-out is lost.
	_tapeSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (_tapeSock >= 0) {
		int reuse1 = 1;
		setsockopt(_tapeSock, SOL_SOCKET, SO_REUSEPORT, &reuse1, sizeof(reuse1));
		struct sockaddr_in taddr = {};
		taddr.sin_family      = AF_INET;
		taddr.sin_addr.s_addr = INADDR_ANY;
		taddr.sin_port        = htons(kTapePort);
		if (bind(_tapeSock, (struct sockaddr *)&taddr, sizeof(taddr)) < 0) {
			printf("[input-server] tape bind port %d failed: %s (tape disabled)\n",
			       kTapePort, strerror(errno));
			close(_tapeSock);
			_tapeSock = -1;
		}
	} else {
		printf("[input-server] tape socket failed: %s (tape disabled)\n", strerror(errno));
	}

	_active = true;
	_udpThread = std::thread(udpThreadLoop, udpPort);
	if (_tapeSock >= 0)
		_tapePubThread = std::thread(tapePublisherLoop);

	printf("[input-server] === READY === port %d\n", udpPort);
	if (_tapeSock >= 0)
		printf("[input-server] tape publisher ready on port %d\n", kTapePort);
	printf("[input-server] waiting for players (NOBD UDP or browser WebSocket)\n");
	maplecast_telemetry::send("[input-server] ready on port %d", udpPort);

	// Phase 4: env-var-driven replay recording. Set MAPLECAST_REPLAY_OUT
	// to a file path and we'll auto-record everything (savestate at boot
	// + every input event) until the process shuts down. Useful for
	// tournament archive mode + dev testing the replay format.
	if (const char* outPath = std::getenv("MAPLECAST_REPLAY_OUT")) {
		maplecast_replay::StartParams sp;
		sp.out_path = outPath;
		if (const char* p1 = std::getenv("MAPLECAST_REPLAY_P1_NAME")) sp.p1_name = p1;
		if (const char* p2 = std::getenv("MAPLECAST_REPLAY_P2_NAME")) sp.p2_name = p2;
		if (const char* sid = std::getenv("MAPLECAST_REPLAY_SERVER_ID")) sp.server_id = sid;
		if (const char* rh = std::getenv("MAPLECAST_REPLAY_ROM_HASH")) sp.rom_hash_hex = rh;
		maplecast_replay::start(sp);
	}

	// Phase 4b: env-var-driven replay PLAYBACK. Set MAPLECAST_REPLAY_IN to
	// a .mcrec file and we'll open it, restore its savestate, and inject
	// the recorded inputs at their original frame numbers. Optional speed
	// override via MAPLECAST_REPLAY_SPEED (float, default 1.0).
	if (const char* inPath = std::getenv("MAPLECAST_REPLAY_IN")) {
		if (maplecast_replay::openReplay(inPath)) {
			if (maplecast_replay::loadStartSavestate()) {
				double speed = 1.0;
				if (const char* s = std::getenv("MAPLECAST_REPLAY_SPEED"))
					speed = atof(s);
				maplecast_replay::startPlayback(speed);
			}
		}
	}

	return true;
}

void shutdown()
{
	if (!_active) return;
	_active = false;

	if (_udpSock >= 0) { close(_udpSock); _udpSock = -1; }
	if (_tapeSock >= 0) { close(_tapeSock); _tapeSock = -1; }
	if (_udpThread.joinable()) _udpThread.join();
	if (_tapePubThread.joinable()) _tapePubThread.join();

	printf("[input-server] shutdown\n");
}

int registerPlayer(const char* id, const char* name, const char* device, InputType type)
{
	std::lock_guard<std::mutex> lock(_registryMutex);

	// Check if already registered (reconnect)
	for (int i = 0; i < 2; i++)
	{
		if (_players[i].connected && strncmp(_players[i].id, id, sizeof(_players[i].id)) == 0)
		{
			// Reconnect — update info, keep slot
			strncpy(_players[i].name, name, sizeof(_players[i].name) - 1);
			strncpy(_players[i].device, device, sizeof(_players[i].device) - 1);
			_players[i].type = type;
			// Reset the idle-kick clock so a reconnect always gets a fresh
			// grace period (otherwise a long-disconnected player would be
			// kicked the moment they come back).
			_players[i].lastChangeUs = nowUs();
			printf("[input-server] P%d RECONNECTED: %s (%s)\n", i + 1, name, device);
			return i;
		}
	}

	// Find first empty slot
	for (int i = 0; i < 2; i++)
	{
		if (!_players[i].connected)
		{
			_players[i].connected = true;
			_players[i].type = type;
			_players[i].buttons = 0xFFFF;
			_players[i]._prevButtons = 0xFFFF;
			_players[i]._lastRateTime = nowUs();
			// Seed lastChangeUs so the idle-kick clock starts from join time,
			// not from whatever stale value the slot had previously.
			_players[i].lastChangeUs = nowUs();
			// Clear any leftover source binding from a prior tenant — the
			// new player must explicitly bind a stick (via NOBD rhythm or
			// web-register) to get hardware input routed to this slot. This
			// is the second half of the disconnect-clears-srcIP fix: a fresh
			// join must NOT inherit a ghost stick binding from whoever held
			// the slot before us.
			_players[i].srcIP = 0;
			_players[i].srcPort = 0;
			strncpy(_players[i].id, id, sizeof(_players[i].id) - 1);
			strncpy(_players[i].name, name, sizeof(_players[i].name) - 1);
			strncpy(_players[i].device, device, sizeof(_players[i].device) - 1);

			printf("[input-server] P%d JOINED: %s (%s) via %s\n", i + 1, name, device,
				type == InputType::BrowserWS ? "WebSocket" : "UDP");
			maplecast_telemetry::send("[input-server] P%d JOINED: %s (%s)", i + 1, name, device);
			return i;
		}
	}

	printf("[input-server] FULL — rejected %s (%s)\n", name, device);
	return -1;
}

void disconnectPlayer(int slot)
{
	if (slot < 0 || slot > 1) return;
	std::lock_guard<std::mutex> lock(_registryMutex);

	if (_players[slot].connected)
	{
		printf("[input-server] P%d DISCONNECTED: %s (was %u.%u.%u.%u:%u)\n",
			slot + 1, _players[slot].name,
			(ntohl(_players[slot].srcIP) >> 24) & 0xFF,
			(ntohl(_players[slot].srcIP) >> 16) & 0xFF,
			(ntohl(_players[slot].srcIP) >>  8) & 0xFF,
			(ntohl(_players[slot].srcIP)      ) & 0xFF,
			ntohs(_players[slot].srcPort));

		_players[slot].connected = false;
		_players[slot].type = InputType::None;

		// CRITICAL: clear the source binding so a NOBD stick that was routing
		// to this slot via findSlotByIP() stops doing so the moment the
		// owning WebSocket disconnects. Without this, a stale srcIP from a
		// previous session keeps the stick → slot binding alive forever and
		// any browser that re-joins under a matching id silently inherits
		// the orphan. This is the root of the "drifter ghost" class of bugs.
		_players[slot].srcIP = 0;
		_players[slot].srcPort = 0;

		// Also clear the id so a stale getRegisteredBrowserId() match against
		// the now-empty slot can't re-bind a stick by accident. The id is
		// rewritten on the next registerPlayer() call from a real join.
		_players[slot].id[0] = '\0';

		// Reset gamepad state to neutral
		kcode[slot] = 0xFFFFFFFF;
		lt[slot] = 0;
		rt[slot] = 0;
	}
}

void injectInput(int slot, uint8_t ltVal, uint8_t rtVal, uint16_t buttons)
{
	updateSlot(slot, ltVal, rtVal, buttons);
}

void setPlayerRtt(int slot, int rttMs)
{
	if (slot < 0 || slot > 1) return;
	if (rttMs < 0) rttMs = 0;
	if (rttMs > 9999) rttMs = 9999;   // sanity clamp; HUD shows 4 digits max
	_players[slot].rttMs = rttMs;
}

const PlayerInfo& getPlayer(int slot)
{
	static PlayerInfo empty = {};
	if (slot < 0 || slot > 1) return empty;
	return _players[slot];
}

int connectedCount()
{
	int count = 0;
	for (int i = 0; i < 2; i++)
		if (_players[i].connected) count++;
	return count;
}

int findIdlePlayer(int64_t thresholdUs)
{
	int64_t now = nowUs();
	std::lock_guard<std::mutex> lock(_registryMutex);
	for (int i = 0; i < 2; i++)
	{
		if (!_players[i].connected) continue;
		// Skip slots that haven't established a baseline yet (lastChangeUs==0
		// would otherwise look like decades-stale).
		if (_players[i].lastChangeUs == 0) continue;
		if (now - _players[i].lastChangeUs > thresholdUs)
			return i;
	}
	return -1;
}

bool active()
{
	return _active.load(std::memory_order_relaxed);
}

int64_t measureInputAge(int slot)
{
	if (slot < 0 || slot > 1) return 0;

	// Only measure when a NEW packet arrived since our last check.
	// Otherwise we'd report stale age that grows unbounded during idle.
	uint32_t curSeq = _lastInputArrivalSeq[slot].load(std::memory_order_relaxed);
	if (curSeq == 0 || curSeq == _latchLastSeq[slot])
		return _inputAgeAtLatchUs[slot].load(std::memory_order_relaxed);
	_latchLastSeq[slot] = curSeq;

	uint64_t arrival = _lastInputArrivalUs[slot].load(std::memory_order_relaxed);
	uint64_t now = nowUs();
	int64_t age = (int64_t)(now - arrival);

	_inputAgeAtLatchUs[slot].store(age, std::memory_order_relaxed);

	// EMA with α = 1/16
	int64_t prev = _inputAgeEmaUs[slot].load(std::memory_order_relaxed);
	int64_t ema = prev + ((age - prev) >> 4);
	_inputAgeEmaUs[slot].store(ema, std::memory_order_relaxed);

	// Log every 300 NEW samples (~5 seconds of active play)
	static uint32_t _logCounter[2] = {0, 0};
	if (++_logCounter[slot] >= 300) {
		_logCounter[slot] = 0;
		printf("[input-age] P%d: last=%.2fms ema=%.2fms (packet→latch)\n",
		       slot + 1, age / 1000.0, ema / 1000.0);
		fflush(stdout);
	}

	return age;
}

int64_t getInputAgeEmaUs(int slot)
{
	if (slot < 0 || slot > 1) return 0;
	return _inputAgeEmaUs[slot].load(std::memory_order_relaxed);
}

void startStickRegistration(const char* browserId)
{
	strncpy(_registerBrowserId, browserId, sizeof(_registerBrowserId) - 1);
	_rhythmTrackers.clear();
	_registering = true;
	printf("[input-server] Registration started for %s — tap any button 5x, pause, 5x again\n", browserId);
}

void cancelStickRegistration()
{
	_registering = false;
	_rhythmTrackers.clear();
	printf("[input-server] Registration cancelled\n");
}

bool isRegistering()
{
	return _registering;
}

const char* getRegisteredBrowserId(uint32_t srcIP, uint16_t srcPort)
{
	// Called from the UDP thread which is the only writer, so the
	// pointer it returns is stable for the duration of one packet's
	// processing. The lock guards against concurrent installs from
	// the WS thread (collector pushes / cache loads).
	std::lock_guard<std::mutex> lock(_stickMutex);
	for (const auto& b : _stickBindings)
		if (b.srcIP == srcIP)  // match by IP only, port can change
			return b.browserId;
	return nullptr;
}

const char* getRegisteredUsername(uint32_t srcIP, uint16_t srcPort)
{
	std::lock_guard<std::mutex> lock(_stickMutex);
	for (const auto& b : _stickBindings)
		if (b.srcIP == srcIP)
			return b.username[0] ? b.username : b.browserId;
	return nullptr;
}

// Append a StickEvent to the pending queue. Caller must hold _stickMutex.
static void enqueueStickEventLocked(StickEventKind kind, const char* username,
                                    uint32_t srcIP, uint16_t srcPort)
{
	StickEvent ev = {};
	ev.kind = kind;
	strncpy(ev.username, username, sizeof(ev.username) - 1);
	ev.srcIP = srcIP;
	ev.srcPort = srcPort;
	ev.ts = (int64_t)time(nullptr);
	_pendingStickEvents.push_back(ev);
}

void registerStick(uint32_t srcIP, uint16_t srcPort, const char* identifier)
{
	bool created = false;
	{
		std::lock_guard<std::mutex> lock(_stickMutex);
		bool updated = false;
		// Update existing binding for this IP, or existing binding for this username
		for (auto& b : _stickBindings)
		{
			if (b.srcIP == srcIP || strcmp(b.username, identifier) == 0 ||
				strcmp(b.browserId, identifier) == 0) {
				b.srcIP = srcIP;
				b.srcPort = srcPort;
				strncpy(b.username, identifier, sizeof(b.username) - 1);
				strncpy(b.browserId, identifier, sizeof(b.browserId) - 1);
				b.lastInputUs = nowUs();
				b.wasOnline = true;
				updated = true;
				break;
			}
		}
		if (!updated) {
			StickBinding b = {};
			b.srcIP = srcIP;
			b.srcPort = srcPort;
			strncpy(b.username, identifier, sizeof(b.username) - 1);
			strncpy(b.browserId, identifier, sizeof(b.browserId) - 1);
			b.lastInputUs = nowUs();
			b.wasOnline = true;
			_stickBindings.push_back(b);
			created = true;
		}
		enqueueStickEventLocked(StickEventKind::Register, identifier, srcIP, srcPort);
	}
	(void)created;
	saveStickCache();   // hot-cache survives flycast restart even if collector is down
}

void unregisterStick(const char* identifier)
{
	bool removed = false;
	{
		std::lock_guard<std::mutex> lock(_stickMutex);
		auto it = std::remove_if(_stickBindings.begin(), _stickBindings.end(),
			[identifier](const StickBinding& b) {
				return strcmp(b.browserId, identifier) == 0 ||
				       strcmp(b.username, identifier) == 0;
			});
		if (it != _stickBindings.end()) {
			removed = true;
			_stickBindings.erase(it, _stickBindings.end());
			enqueueStickEventLocked(StickEventKind::Unregister, identifier, 0, 0);
		}
	}
	if (removed) {
		printf("[input-server] Stick unregistered for %s\n", identifier);
		saveStickCache();
	}
}

bool isStickOnline(const char* username)
{
	std::lock_guard<std::mutex> lock(_stickMutex);
	int64_t now = nowUs();
	for (const auto& b : _stickBindings)
		if ((strcmp(b.username, username) == 0 || strcmp(b.browserId, username) == 0)
			&& (now - b.lastInputUs) < 10000000)  // 10 seconds
			return true;
	return false;
}

StickInfo getStickInfo(const char* username)
{
	std::lock_guard<std::mutex> lock(_stickMutex);
	StickInfo info = {};
	int64_t now = nowUs();
	for (const auto& b : _stickBindings)
	{
		if (strcmp(b.username, username) == 0 || strcmp(b.browserId, username) == 0)
		{
			info.registered = true;
			info.online = (now - b.lastInputUs) < 10000000;
			strncpy(info.username, b.username, sizeof(info.username) - 1);
			info.srcIP = b.srcIP;
			info.srcPort = b.srcPort;
			info.lastInputUs = b.lastInputUs;
			return info;
		}
	}
	return info;
}

int registeredStickCount()
{
	std::lock_guard<std::mutex> lock(_stickMutex);
	return (int)_stickBindings.size();
}

// ==================== Stick persistence — public API ====================

std::vector<StickEvent> drainStickEvents()
{
	std::lock_guard<std::mutex> lock(_stickMutex);

	// Synthesize Online/Offline edge events from lastInputUs vs wasOnline.
	// Cheap: at most 2 entries on the typical 2-stick install.
	int64_t now = nowUs();
	for (auto& b : _stickBindings)
	{
		bool onlineNow = (now - b.lastInputUs) < 10000000;
		if (onlineNow != b.wasOnline)
		{
			enqueueStickEventLocked(
				onlineNow ? StickEventKind::Online : StickEventKind::Offline,
				b.username, b.srcIP, b.srcPort);
			b.wasOnline = onlineNow;
		}
	}

	std::vector<StickEvent> out;
	out.swap(_pendingStickEvents);
	return out;
}

std::vector<StickSnapshot> snapshotStickBindings()
{
	std::lock_guard<std::mutex> lock(_stickMutex);
	std::vector<StickSnapshot> out;
	out.reserve(_stickBindings.size());
	for (const auto& b : _stickBindings) {
		StickSnapshot s = {};
		strncpy(s.username, b.username, sizeof(s.username) - 1);
		s.srcIP = b.srcIP;
		s.srcPort = b.srcPort;
		s.lastInputUs = b.lastInputUs;
		out.push_back(s);
	}
	return out;
}

void installStickBindings(const std::vector<StickSnapshot>& snapshots)
{
	{
		std::lock_guard<std::mutex> lock(_stickMutex);
		for (const auto& s : snapshots) {
			if (!s.username[0]) continue;
			bool found = false;
			for (auto& b : _stickBindings) {
				if (strcmp(b.username, s.username) == 0) {
					b.srcIP = s.srcIP;
					b.srcPort = s.srcPort;
					if (s.lastInputUs > b.lastInputUs)
						b.lastInputUs = s.lastInputUs;
					found = true;
					break;
				}
			}
			if (!found) {
				StickBinding b = {};
				strncpy(b.username, s.username, sizeof(b.username) - 1);
				strncpy(b.browserId, s.username, sizeof(b.browserId) - 1);
				b.srcIP = s.srcIP;
				b.srcPort = s.srcPort;
				b.lastInputUs = s.lastInputUs;
				b.wasOnline = false;   // remote install — wait for first packet to flip
				_stickBindings.push_back(b);
			}
		}
	}
	printf("[input-server] Installed %zu stick binding(s) from cache/collector\n",
		snapshots.size());
}

// --- Local hot-cache (~/.maplecast/sticks.json) ---

static std::string stickCachePath()
{
	const char* home = getenv("HOME");
	if (!home || !*home) {
		struct passwd* pw = getpwuid(getuid());
		if (pw) home = pw->pw_dir;
	}
	if (!home || !*home) return std::string();
	std::string dir = std::string(home) + "/.maplecast";
	mkdir(dir.c_str(), 0700);   // EEXIST ignored
	return dir + "/sticks.json";
}

bool saveStickCache()
{
	std::string path = stickCachePath();
	if (path.empty()) return false;

	auto snaps = snapshotStickBindings();   // takes its own lock

	// Atomic write: temp + rename so a crash mid-write can't corrupt the cache.
	std::string tmp = path + ".tmp";
	std::ofstream f(tmp, std::ios::trunc);
	if (!f) {
		printf("[input-server] saveStickCache: cannot open %s\n", tmp.c_str());
		return false;
	}
	f << "[\n";
	for (size_t i = 0; i < snaps.size(); i++) {
		const auto& s = snaps[i];
		struct in_addr ia;
		ia.s_addr = s.srcIP;
		f << "  {\"username\":\"" << s.username
		  << "\",\"ip\":\"" << inet_ntoa(ia)
		  << "\",\"port\":" << ntohs(s.srcPort)
		  << ",\"last_input_us\":" << s.lastInputUs
		  << "}" << (i + 1 < snaps.size() ? "," : "") << "\n";
	}
	f << "]\n";
	f.close();
	if (rename(tmp.c_str(), path.c_str()) != 0) {
		printf("[input-server] saveStickCache: rename failed: %s\n", strerror(errno));
		return false;
	}
	return true;
}

bool loadStickCache()
{
	std::string path = stickCachePath();
	if (path.empty()) return false;

	std::ifstream f(path);
	if (!f) return false;   // no cache yet — first run

	std::stringstream ss;
	ss << f.rdbuf();
	std::string text = ss.str();

	// Tiny hand-rolled parser to avoid pulling in nlohmann::json here. The
	// file we just wrote ourselves is well-formed; if a user hand-edits it
	// and breaks it, we silently fall through and start clean.
	std::vector<StickSnapshot> loaded;
	size_t pos = 0;
	while ((pos = text.find('{', pos)) != std::string::npos) {
		size_t end = text.find('}', pos);
		if (end == std::string::npos) break;
		std::string obj = text.substr(pos, end - pos + 1);
		pos = end + 1;

		auto extractStr = [&](const char* key, std::string& out) {
			std::string needle = std::string("\"") + key + "\":\"";
			size_t k = obj.find(needle);
			if (k == std::string::npos) return false;
			k += needle.size();
			size_t e = obj.find('"', k);
			if (e == std::string::npos) return false;
			out = obj.substr(k, e - k);
			return true;
		};
		auto extractNum = [&](const char* key, long long& out) {
			std::string needle = std::string("\"") + key + "\":";
			size_t k = obj.find(needle);
			if (k == std::string::npos) return false;
			k += needle.size();
			out = strtoll(obj.c_str() + k, nullptr, 10);
			return true;
		};

		std::string username, ip;
		long long port = 0, lastInputUs = 0;
		if (!extractStr("username", username) || !extractStr("ip", ip)) continue;
		extractNum("port", port);
		extractNum("last_input_us", lastInputUs);

		StickSnapshot s = {};
		strncpy(s.username, username.c_str(), sizeof(s.username) - 1);
		s.srcIP = inet_addr(ip.c_str());
		s.srcPort = htons((uint16_t)port);
		s.lastInputUs = (int64_t)lastInputUs;
		loaded.push_back(s);
	}

	if (!loaded.empty())
		installStickBindings(loaded);
	printf("[input-server] Loaded %zu stick binding(s) from %s\n",
		loaded.size(), path.c_str());
	return true;
}

// --- Web registration ---

void startWebRegistration(const char* username)
{
	strncpy(_webRegisterUsername, username, sizeof(_webRegisterUsername) - 1);
	_webRegisterStartUs = nowUs();
	_webRegistering = true;
	printf("[input-server] Web registration started for '%s' — press any button\n", username);
}

void cancelWebRegistration()
{
	_webRegistering = false;
	_webRegisterUsername[0] = 0;
	printf("[input-server] Web registration cancelled\n");
}

bool isWebRegistering()
{
	// Auto-cancel after 30 seconds
	if (_webRegistering && (nowUs() - _webRegisterStartUs) > 30000000) {
		printf("[input-server] Web registration timed out for '%s'\n", _webRegisterUsername);
		_webRegistering = false;
	}
	return _webRegistering;
}

const char* webRegisteringUsername()
{
	return _webRegisterUsername;
}

bool isValidUsername(const char* name)
{
	if (!name) return false;
	int len = 0;
	for (const char* p = name; *p; p++, len++)
	{
		char c = *p;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	return len >= 4 && len <= 12;
}

} // namespace maplecast_input
