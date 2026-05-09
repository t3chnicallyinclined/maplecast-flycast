/*
	MapleCast Predictor — comparator + rollback trigger (A.5).
	See maplecast_predictor.h for the design rationale.
*/
#include "maplecast_predictor.h"
#include "maplecast_rollback.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace maplecast_predictor
{

// Per-slot ring of recorded predictions. We keep enough history to cover
// the rollback ring's depth (10 frames) plus a small margin.
constexpr int kPredRingDepth = 16;

struct PredEntry {
	uint64_t frame    = UINT64_MAX;   // sentinel = empty
	uint16_t buttons  = 0;
	uint8_t  lt       = 0;
	uint8_t  rt       = 0;
};

static PredEntry _predRing[2][kPredRingDepth];
static std::atomic<bool> _active{false};
static std::atomic<uint64_t> _statPredictions{0};
static std::atomic<uint64_t> _statMatches{0};
static std::atomic<uint64_t> _statMismatches{0};
static std::atomic<uint64_t> _statRollbacks{0};
static std::atomic<uint64_t> _statSyntheticInjects{0};

// Test mode: every N frames, inject a synthetic "predicted as zero"
// entry for slot 1 to verify the mismatch → rollback path. 0 = disabled.
static uint32_t _testEveryN = 0;

void init()
{
	if (_active.load(std::memory_order_acquire)) return;

	// Clear rings
	for (int s = 0; s < 2; s++)
		for (int i = 0; i < kPredRingDepth; i++)
			_predRing[s][i] = PredEntry{};

	_statPredictions.store(0);
	_statMatches.store(0);
	_statMismatches.store(0);
	_statRollbacks.store(0);
	_statSyntheticInjects.store(0);

	if (const char* e = std::getenv("MAPLECAST_TEST_ROLLBACK")) {
		_testEveryN = (uint32_t)std::strtoul(e, nullptr, 10);
		if (_testEveryN > 0)
			printf("[predictor] test mode armed: synthetic mispredict every %u frames\n",
			       _testEveryN);
	}

	_active.store(true, std::memory_order_release);
	printf("[predictor] init — ring depth %d, comparator armed\n", kPredRingDepth);
}

bool active() { return _active.load(std::memory_order_relaxed); }

void recordPrediction(int slot, uint64_t frame,
                       uint16_t buttons, uint8_t lt, uint8_t rt)
{
	if (!_active.load(std::memory_order_relaxed)) return;
	if (slot < 0 || slot > 1) return;

	const int idx = (int)(frame % kPredRingDepth);
	PredEntry& e = _predRing[slot][idx];
	e.frame   = frame;
	e.buttons = buttons;
	e.lt      = lt;
	e.rt      = rt;
	_statPredictions.fetch_add(1, std::memory_order_relaxed);
}

// Test-mode helper: inject a synthetic GUARANTEED-mismatch prediction for
// slot 1 every Nth frame. We predict 0xFFFF (all buttons pressed) which
// will never match real input at the boot/menu screen — and rarely match
// even in active gameplay — guaranteeing the mismatch path fires so we
// can verify the rollback trigger end-to-end.
static void maybeInjectSynthetic(uint64_t frame)
{
	if (_testEveryN == 0) return;
	if ((frame % _testEveryN) != 0) return;

	// Only inject for slot 1 (the canonical "opponent" slot).
	const int slot = 1;
	const int idx = (int)(frame % kPredRingDepth);
	PredEntry& e = _predRing[slot][idx];
	if (e.frame == frame) return;  // already have a prediction for this frame

	e.frame   = frame;
	e.buttons = 0xFFFF;   // guaranteed not to match real input
	e.lt      = 0xFF;
	e.rt      = 0xFF;
	_statSyntheticInjects.fetch_add(1, std::memory_order_relaxed);
}

void onAuthoritativeInput(int slot, uint64_t frame,
                           uint16_t buttons, uint8_t lt, uint8_t rt)
{
	if (!_active.load(std::memory_order_relaxed)) return;
	if (slot < 0 || slot > 1) return;

	maybeInjectSynthetic(frame);

	const int idx = (int)(frame % kPredRingDepth);
	PredEntry& e = _predRing[slot][idx];
	if (e.frame != frame) {
		// No prediction for this (slot, frame) — nothing to compare.
		return;
	}

	const bool match =
		e.buttons == buttons &&
		e.lt      == lt      &&
		e.rt      == rt;

	if (match) {
		_statMatches.fetch_add(1, std::memory_order_relaxed);
	} else {
		_statMismatches.fetch_add(1, std::memory_order_relaxed);
		printf("[predictor] MISMATCH slot=%d frame=%llu pred=%04x/%02x/%02x auth=%04x/%02x/%02x — rewinding\n",
		       slot, (unsigned long long)frame,
		       e.buttons, e.lt, e.rt,
		       buttons, lt, rt);
		// Fire the rewind. The actual rewindToFrame happens later at
		// the emu loop's safe-context point (executePendingRewind);
		// we just request it here.
		if (maplecast_rollback::active()) {
			maplecast_rollback::requestRewindToFrame(frame);
			_statRollbacks.fetch_add(1, std::memory_order_relaxed);
		} else {
			printf("[predictor] rollback ring not active — would have rewound to frame %llu\n",
			       (unsigned long long)frame);
		}
	}

	// Consume the prediction so we don't double-fire if the same frame
	// shows up twice (safety belt; shouldn't happen).
	e.frame = UINT64_MAX;
}

Stats getStats()
{
	Stats s{};
	s.predictions        = _statPredictions.load(std::memory_order_relaxed);
	s.matches            = _statMatches.load(std::memory_order_relaxed);
	s.mismatches         = _statMismatches.load(std::memory_order_relaxed);
	s.rollbacksTriggered = _statRollbacks.load(std::memory_order_relaxed);
	s.syntheticInjects   = _statSyntheticInjects.load(std::memory_order_relaxed);
	return s;
}

} // namespace maplecast_predictor
