/*
	MapleCast Predictor — A.5 of the Phase 1 rollback prediction plan.

	*** SHELVED 2026-05-09 *** — see docs/ROLLBACK-SHELVED.md.

	Only init'd when MAPLECAST_ROLLBACK_RING=1. Code kept intact for
	future un-shelving but not on the production path.

	The predictor is the consumer of the rollback ring (A.4):
	  • recordPrediction(slot, frame, ...) — A.6 will call this when it
	    fills in opponent input speculatively. For now (A.5), the only
	    caller is the synthetic-mispredict test mode used to validate
	    that the rollback trigger actually fires end-to-end.
	  • onAuthoritativeInput(slot, frame, ...) — called from
	    maplecast_input_server::publishFrameTick when the real input
	    for `frame` becomes known. The comparator looks up the
	    prediction for this (slot, frame). If they match → discard
	    the prediction. If they mismatch → call
	    maplecast_rollback::requestRewindToFrame(frame).

	The actual rewind happens at the emu loop's safe-context point
	(executePendingRewind). After the rewind, SH4 re-emulates from
	`frame` forward with the corrected input — byte-perfect now that
	the dc_serialize round-trip has been audited to 0 bytes drift.

	Test mode (MAPLECAST_TEST_ROLLBACK=N): every Nth frame, the
	predictor injects a synthetic "guaranteed mismatch" prediction
	(buttons=0xFFFF, lt=rt=0xFF) for slot 1. The mismatch fires and
	the rewind triggers, validating the comparator + rewind path.

	KNOWN LIMITATION (A.6 will fix): the `frame` parameter passed to
	onAuthoritativeInput is the renderer's per-publish counter
	(_localFrameNum in maplecast_mirror::serverPublish), not the
	rollback ring's SH4 vblank counter (_rollbackFrameSeq in
	Emulator::vblank). They're parallel but not equal. Rewind targets
	from the predictor are relative to renderer frames, while the
	rollback ring's slot.frame uses vblank frames. For real netcode
	the two counters need to be unified — A.6 will introduce a single
	canonical "game frame" counter shared between predictor and ring.

	Threading:
	  • recordPrediction and onAuthoritativeInput are called from the
	    SH4 emu thread (publishFrameTick is on the emu thread).
	  • Single-threaded access — no internal locking needed.
*/
#pragma once
#include <cstdint>

namespace maplecast_predictor
{

// Initialize. Reads MAPLECAST_TEST_ROLLBACK env var if set. Idempotent.
void init();

// Record what the predictor thinks the input for (slot, frame) will be.
// Called by A.6 (opponent prediction) at the moment SH4 reads input from
// ggpo::getLocalInput for a slot whose input hasn't arrived yet.
void recordPrediction(int slot, uint64_t frame,
                       uint16_t buttons, uint8_t lt, uint8_t rt);

// Authoritative input arrived for (slot, frame). Compare to the
// recorded prediction, if any. On mismatch, fires
// maplecast_rollback::requestRewindToFrame(frame).
void onAuthoritativeInput(int slot, uint64_t frame,
                           uint16_t buttons, uint8_t lt, uint8_t rt);

struct Stats {
	uint64_t predictions;       // recordPrediction calls
	uint64_t matches;           // authoritative matched prediction
	uint64_t mismatches;        // authoritative differed → rollback fired
	uint64_t rollbacksTriggered;// requestRewindToFrame calls (== mismatches)
	uint64_t syntheticInjects;  // MAPLECAST_TEST_ROLLBACK injections
};
Stats getStats();

// True when init has run and the predictor is recording.
bool active();

} // namespace maplecast_predictor
