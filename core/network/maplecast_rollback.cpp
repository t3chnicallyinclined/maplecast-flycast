/*
	MapleCast Rollback Ring — implementation.

	Mirrors flycast's existing GGPO ring (core/network/ggpo.cpp:439-561) but
	extracted into a non-GGPO API. Same page-delta + dc_serialize hybrid
	pattern; same memwatch::Watcher<> infrastructure. Different consumer.

	Memory layout per slot:
	  * uint8_t* serialBlob    — pre-allocated 16 MB buffer, dc_serialize writes here
	  * size_t   serialSize    — actual bytes used (dc_serialize.size())
	  * MemPages pages         — page-delta map captured AT THIS FRAME (FROM the
	                              previous frame's save). On rewind, walk backward
	                              and reapply each frame's pages in reverse order.
	  * uint64_t frame         — frame number this slot represents (for cursor).

	Save path (saveFrame(N)):
	  1. dc_serialize → serialBlob (size goes in serialSize)
	  2. Drain memwatch into pages (all pages written between frame N-1 and N)
	  3. Re-arm memwatch (protect for next frame's diff)

	Rewind path (rewindToFrame(target)):
	  1. Walk backward from mostRecent down to target+1, applying each
	     slot's page diffs back into live memory (memcpy each saved page
	     to its current location)
	  2. dc_deserialize from target's serialBlob — restores SH4 + PVR scalars
	  3. Re-arm memwatch

	Threading: SPSC. saveFrame() called from the SH4 emu thread (via
	serverPublish hook). rewindToFrame() also called from emu thread on
	rollback events. No locks needed — single writer = single reader.

	Audit: docs/DC-SERIALIZE-AUDIT.md priority-1 patches landed in V59
	(commit 5919ea1b7). Without them, dc_serialize round-trip would not
	be byte-equal even on the same binary — F.1 test would fail for known
	reasons rather than discoverable ones.
*/
#include "maplecast_rollback.h"
#include "types.h"
#include "serialize.h"
#include "emulator.h"
#include "hw/mem/mem_watch.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/spg.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"
extern int vblank_schid; // defined in core/hw/pvr/spg.cpp
#include <xxhash.h>  // bundled at core/deps/xxHash/, on the include path

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace maplecast_rollback
{

// Same struct as the one in ggpo.cpp:281 — copying rather than #including
// it because we don't want to inherit GGPO's static state. Identical
// layout means rewind logic stays line-for-line equivalent.
struct MemPages
{
	void capture()
	{
		memwatch::ramWatcher.getPages(ram);
		memwatch::vramWatcher.getPages(vram);
		memwatch::aramWatcher.getPages(aram);
		memwatch::elanWatcher.getPages(elanram);
	}
	void clear()
	{
		ram.clear();
		vram.clear();
		aram.clear();
		elanram.clear();
	}
	memwatch::PageMap ram;
	memwatch::PageMap vram;
	memwatch::PageMap aram;
	memwatch::PageMap elanram;
};

struct RingSlot
{
	uint64_t              frame = UINT64_MAX;     // sentinel = empty
	std::vector<uint8_t>  serialBlob;             // pre-sized to 16 MB on init
	size_t                serialSize = 0;
	MemPages              pages;
};

static RingSlot          _ring[RING_DEPTH];
static std::atomic<bool> _active{false};
static uint64_t          _mostRecentFrame = UINT64_MAX;
static uint64_t          _oldestFrame     = UINT64_MAX;
static uint64_t          _framesSaved     = 0;
static uint64_t          _rollbacksDone   = 0;

// dc_serialize blob size. With rollback=false (full state including
// VRAM + 16MB mem_b), MVC2 states are ~28 MB. Reserve 40 MB per slot
// for headroom. 10 slots × 40 MB = 400 MB — fine on modern desktop.
//
// Why rollback=false: the page-delta replay path was leaving VRAM/mem_b
// in a hybrid post-rewind state (partly anchor, partly forward) that
// caused SH4 to hang in the next runInternal. Audit self-test confirmed
// dc_serialize(rollback=false) → dc_deserialize(rollback=false)
// round-trip works cleanly without page-deltas. Trade ~240 MB extra
// arena for correctness; we still keep the page-delta map for diff
// streaming to clients but no longer depend on it for state restore.
constexpr size_t SLOT_BLOB_SIZE = 40 * 1024 * 1024;

// Forward declarations for F.1 / F.2 test plumbing (defined at end of file).
static void f1TryConfigure();
static void dcAuditTryConfigure();

bool init()
{
	if (_active.load(std::memory_order_acquire)) return true;

	for (int i = 0; i < RING_DEPTH; i++)
	{
		try {
			_ring[i].serialBlob.resize(SLOT_BLOB_SIZE);
		} catch (const std::bad_alloc&) {
			printf("[rollback] init: failed to allocate ring slot %d (%zu MB)\n",
			       i, SLOT_BLOB_SIZE / (1024 * 1024));
			// Best-effort cleanup
			for (int j = 0; j < i; j++)
				_ring[j].serialBlob.clear();
			return false;
		}
		_ring[i].serialSize = 0;
		_ring[i].frame = UINT64_MAX;
	}

	// Activate the page-fault watcher. From here on, every write to RAM /
	// VRAM / ARAM / ElanRAM is recorded by memwatch. We drain it per-frame
	// in saveFrame().
	//
	// IMPORTANT: do NOT call memwatch::protect() here. GGPO's pattern is to
	// arm protection at the END of save_game_state, AFTER the first frame
	// has run unimpeded (ggpo.cpp:507). Calling protect() before the SH4
	// thread starts would fault on every page-write during early SH4 boot
	// — which works correctness-wise but stalls the emu thread for tens of
	// seconds before any forward progress is visible. saveFrame() handles
	// arming protection on subsequent frames after capturing each delta.
	if (std::getenv("MAPLECAST_DISABLE_MEMWATCH")) {
		printf("[rollback] memwatch DISABLED for audit (no page-protection)\n");
		memwatch::mirrorActive = false;
	} else {
		memwatch::mirrorActive = true;
	}

	_mostRecentFrame = UINT64_MAX;
	_oldestFrame     = UINT64_MAX;
	_framesSaved     = 0;
	_rollbacksDone   = 0;
	_active.store(true, std::memory_order_release);

	printf("[rollback] init: %d-slot ring, %zu MB total arena, memwatch armed\n",
	       RING_DEPTH, (size_t)RING_DEPTH * SLOT_BLOB_SIZE / (1024 * 1024));

	// Arm F.1 round-trip determinism test if MAPLECAST_ROLLBACK_F1_TEST is set.
	// No-op otherwise.
	f1TryConfigure();
	// Arm F.2 byte-diff audit if MAPLECAST_DC_AUDIT is set. No-op otherwise.
	dcAuditTryConfigure();
	return true;
}

void shutdown()
{
	if (!_active.load(std::memory_order_acquire)) return;
	_active.store(false, std::memory_order_release);

	memwatch::mirrorActive = false;
	memwatch::unprotect();
	memwatch::reset();

	for (int i = 0; i < RING_DEPTH; i++)
	{
		_ring[i].serialBlob.clear();
		_ring[i].serialBlob.shrink_to_fit();
		_ring[i].pages.clear();
		_ring[i].frame = UINT64_MAX;
	}
	printf("[rollback] shutdown: ring released, memwatch disarmed\n");
}

void saveFrame(uint64_t frame)
{
	if (!_active.load(std::memory_order_relaxed)) return;

	const int idx = (int)(frame % RING_DEPTH);
	RingSlot& slot = _ring[idx];

	// 1. dc_serialize into the slot's pre-allocated blob.
	//    Use rollback=false (full state, includes VRAM + mem_b) — the
	//    page-delta-only path was leaving post-rewind state hybrid and
	//    deadlocking SH4. Audit self-test (vblank → dc_serialize →
	//    dc_deserialize → continue) confirmed full-state round-trip is
	//    clean. We still record page-deltas in slot.pages for diff
	//    streaming to mirror clients (separate consumer).
	Serializer ser(slot.serialBlob.data(), SLOT_BLOB_SIZE, false);
	try {
		uint32_t frame32 = (uint32_t)(frame & 0xFFFFFFFFu);
		ser << frame32;
		dc_serialize(ser);
		slot.serialSize = ser.size();
	} catch (const Serializer::Exception& e) {
		printf("[rollback] saveFrame(%llu): dc_serialize failed: %s\n",
		       (unsigned long long)frame, e.what());
		slot.serialSize = 0;
		return;
	}

	// 2. Re-arm protection on pages-just-touched, BEFORE draining them.
	//    Mirrors ggpo.cpp:507 ordering exactly. With started=true and the
	//    watcher's pages map populated by THIS frame's writes, protect()
	//    re-protects only those specific pages — not all of memory. Doing
	//    reset() here would set started=false, causing the next protect()
	//    to fault on every write to all 16+8+2 MB of guest RAM, which
	//    stalls the SH4 thread out of forward progress.
	//
	// DIAG: gate on env var so we can disable memwatch during the audit
	// to test the "page-fault timing causes 448-cycle drift" hypothesis.
	static const bool _memwatchDisabled = std::getenv("MAPLECAST_DISABLE_MEMWATCH") != nullptr;
	if (!_memwatchDisabled)
		memwatch::protect();

	// 3. Drain memwatch's page diffs into the slot. capture() calls
	//    getPages() which SWAPS the watcher's pages map out, leaving the
	//    watcher empty but still started+protected. Next frame's first-
	//    write to any of those pages will fault, get captured into the
	//    (now empty) pages map, get unprotected, and the write completes.
	slot.pages.clear();
	if (!_memwatchDisabled)
		slot.pages.capture();

	slot.frame = frame;

	// Update cursor — mostRecent advances, oldest tracks the ring's tail.
	_mostRecentFrame = frame;
	if (_framesSaved < (uint64_t)RING_DEPTH) {
		_oldestFrame = (_oldestFrame == UINT64_MAX) ? frame : _oldestFrame;
	} else {
		_oldestFrame = frame >= (uint64_t)RING_DEPTH ? frame - (RING_DEPTH - 1) : 0;
	}
	_framesSaved++;

	// Heartbeat log every 600 frames (~10s @ 60Hz) so we can see the ring
	// is firing without needing a connected client. Keeps the same cadence
	// as the existing [MIRROR] Server frame N log so they line up.
	if ((_framesSaved % 600) == 0) {
		size_t totalPages = slot.pages.ram.size() + slot.pages.vram.size()
			+ slot.pages.aram.size() + slot.pages.elanram.size();
		printf("[rollback] frames saved: %llu (latest @ frame %llu, %zu pages, %zu B serialized)\n",
		       (unsigned long long)_framesSaved,
		       (unsigned long long)frame,
		       totalPages,
		       slot.serialSize);
		fflush(stdout);
	}

	// F.1 round-trip determinism test orchestration. No-op unless
	// MAPLECAST_ROLLBACK_F1_TEST is set. Runs once and stops.
	f1TickFromVblank(frame);

	// F.2 byte-diff audit orchestration. No-op unless MAPLECAST_DC_AUDIT
	// is set. Runs once and stops.
	dcAuditTickFromVblank(frame);
}

bool rewindToFrame(uint64_t frame)
{
	if (!_active.load(std::memory_order_relaxed)) return false;
	if (_mostRecentFrame == UINT64_MAX) return false;
	if (frame > _mostRecentFrame) return false;
	if (frame < _oldestFrame) {
		printf("[rollback] rewind: target %llu older than ring tail %llu\n",
		       (unsigned long long)frame, (unsigned long long)_oldestFrame);
		return false;
	}

	const int targetIdx = (int)(frame % RING_DEPTH);
	const RingSlot& target = _ring[targetIdx];
	if (target.frame != frame || target.serialSize == 0) {
		printf("[rollback] rewind: slot[%d].frame=%llu mismatch (expected %llu)\n",
		       targetIdx, (unsigned long long)target.frame, (unsigned long long)frame);
		return false;
	}

	const uint64_t prerewindNow = sh4_sched_now64();
	const uint32_t prerewindPc  = (uint32_t)Sh4cntx.pc;
	static const bool _memwatchDisabledRewind = std::getenv("MAPLECAST_DISABLE_MEMWATCH") != nullptr;
	if (!_memwatchDisabledRewind)
		memwatch::unprotect();

	// Walk backward from mostRecent down to target+1, applying each slot's
	// pages back to live memory in REVERSE order. Mirrors ggpo.cpp:449-462
	// exactly — each pair.second.data is the PRE-WRITE contents of the page,
	// so memcpy'ing it back undoes that frame's writes. With rollback=false
	// blobs (full state), this is now belt-and-suspenders: the dc_deserialize
	// also restores RAM/VRAM. Kept for safety + cheap.
	for (uint64_t f = _mostRecentFrame; f > frame; f--)
	{
		const int idx = (int)(f % RING_DEPTH);
		const MemPages& pg = _ring[idx].pages;
		for (const auto& pair : pg.ram)
			memcpy(memwatch::ramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pg.vram)
			memcpy(memwatch::vramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pg.aram)
			memcpy(memwatch::aramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pg.elanram)
			memcpy(memwatch::elanWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
	}

	// Restore the SH4 + PVR scalar state from the dc_serialize blob at the
	// target frame. CRITICAL: use emu.loadstate(deser) instead of calling
	// dc_deserialize directly. loadstate does 8 additional critical things
	// that dc_deserialize alone misses:
	//   custom_texture.terminate()/init() — texture cache reset
	//   aica::arm::recompiler::flush()   — AICA ARM recompiler flush
	//   mmu_flush_table() / mmu_set_state — MMU page table flush
	//   bm_Reset()                       — SH4 dynarec block manager reset
	//   getSh4Executor()->ResetCache()    — SH4 executor cache reset
	//   EventManager::event(Event::LoadState) — broadcast load-state event
	// Without these, rolling-back into a state restored by dc_deserialize
	// alone leaves stale dynarec blocks dispatching to wrong code, and the
	// "frame" we resume at runs against subtly mismatched runtime state.
	// Same wrappers V2 .mcrec replay relies on via the on-disk dc_loadstate
	// path — same lesson, in-memory.
	//
	// Note: emu.loadstate also does memwatch::unprotect()+reset(). We
	// already did unprotect at the top of rewindToFrame; the redundant
	// calls inside loadstate are no-ops. The reset() call inside loadstate
	// IS load-bearing — clears watcher state so our re-protect below
	// arms correctly for the next frame.
	{
		// rollback=false: blob includes VRAM + mem_b. emu.loadstate does the
		// load-bearing wrappers (bm_Reset, ResetCache, aica recompiler flush,
		// mmu_flush_table, custom_texture init, EventManager LoadState
		// broadcast) that plain dc_deserialize alone misses. Tested both
		// paths — drift is identical (~155 bytes / 448-cycle skew), so
		// bm_Reset/ResetCache aren't the cycle-drift source. We use
		// emu.loadstate for the safer dynarec-aware path.
		Deserializer deser(target.serialBlob.data(), target.serialSize, false);
		uint32_t frame32;
		deser >> frame32;
		emu.loadstate(deser);
		rend_resync_after_rollback();

		// CRITICAL: vblank_schid was saved with end=-1 (inactive) because
		// saveFrame is called INSIDE vblank_schid's callback (handle_cb
		// sets sched.end=-1 BEFORE calling the callback). In the SYNCHRONOUS
		// rewind path, the live forward path's handle_cb re-schedules
		// vblank_schid AFTER the callback returns, fixing it up. In the
		// DEFERRED rewind path, that re-schedule never happens because we
		// short-circuited via Stop()→executePendingRewind. So vblank_schid
		// stays inactive forever, no vblanks fire, SH4 dispatches blocks
		// indefinitely with no progress.
		//
		// Fix: re-schedule vblank_schid using rescheduleSPG() — same
		// helper used at PVR register-write paths (pvr_regs.cpp:211).
		// rescheduleSPG calls sh4_sched_request(vblank_schid,
		// getNextSpgInterrupt()) which uses the deserialized
		// clc_pvr_scanline / Line_Cycles to compute the EXACT next-
		// scanline-cycle count — matching what LIVE's natural
		// re-schedule would have done after the original anchor's
		// scanline 0 block continuation. This closes the last 2-byte
		// drift in interrupt_pend and sh4_sched_ffb low byte that
		// the +1-cycle reschedule produced.
		rescheduleSPG();

		if (deser.size() != target.serialSize) {
			printf("[rollback] rewind: deserialize size mismatch (used %zu of %zu)\n",
			       deser.size(), target.serialSize);
			// Don't die here — V60 patches may have left some unreachable bytes
		}
	}

	// memwatch::reset already done by emu.loadstate; just re-arm.
	if (!_memwatchDisabledRewind)
		memwatch::protect();

	_mostRecentFrame = frame;  // we've effectively undone everything past this
	_rollbacksDone++;

	printf("[rollback] rewound to frame %llu — sched_now %llu→%llu, pc 0x%08x→0x%08x\n",
	       (unsigned long long)frame,
	       (unsigned long long)prerewindNow,
	       (unsigned long long)sh4_sched_now64(),
	       (unsigned)prerewindPc,
	       (unsigned)Sh4cntx.pc);
	fflush(stdout);
	return true;
}

uint64_t oldestAvailable() { return _oldestFrame; }
uint64_t mostRecentSaved() { return _mostRecentFrame; }
bool active() { return _active.load(std::memory_order_relaxed); }

// ── Async-safe rewind request (stop-callback-restart pattern) ────────
//
// rewindToFrame() calls emu.loadstate() which does bm_Reset() and
// ResetCache() — destructive ops on the SH4 dynarec. Calling them from
// inside vblank() corrupts in-flight dispatch (the very block we're
// returning into is invalidated). To be safe, the rewind must happen
// AFTER the SH4 has fully paused.
//
// Pattern (matches GGPO's session loop semantics):
//   1. F.1 (or any caller in vblank context) calls requestRewindToFrame().
//   2. vblank() checks pendingRollback() and calls Stop() if set.
//   3. Stop() makes Run() return at the next safe point.
//   4. Emu thread loop sees runInternal() returned. Calls
//      executePendingRewind() — SH4 is fully paused here, safe.
//   5. Loop calls Start() to restart SH4 from the rolled-back state.

static std::atomic<bool>     _rollbackPending{false};
static std::atomic<uint64_t> _rollbackTarget{0};

void requestRewindToFrame(uint64_t targetFrame)
{
	_rollbackTarget.store(targetFrame, std::memory_order_release);
	_rollbackPending.store(true, std::memory_order_release);
}

bool pendingRollback()
{
	return _rollbackPending.load(std::memory_order_acquire);
}

bool executePendingRewind()
{
	if (!_rollbackPending.load(std::memory_order_acquire))
		return false;
	const uint64_t target = _rollbackTarget.load(std::memory_order_acquire);
	const bool ok = rewindToFrame(target);
	_rollbackPending.store(false, std::memory_order_release);
	return ok;
}

Stats getStats()
{
	Stats s{};
	s.framesSaved        = _framesSaved;
	s.bytesArenaTotal    = (uint64_t)RING_DEPTH * SLOT_BLOB_SIZE;
	uint64_t used = 0;
	for (int i = 0; i < RING_DEPTH; i++) used += _ring[i].serialSize;
	s.bytesArenaUsed     = used;
	s.rollbacksPerformed = _rollbacksDone;
	return s;
}

// ── F.1 round-trip determinism test ──────────────────────────────────

enum F1Stage { F1_IDLE, F1_WARMUP, F1_PRE, F1_POST, F1_DONE };
static F1Stage              _f1Stage    = F1_IDLE;
static bool                 _f1Configured = false;
static uint32_t             _f1Warmup   = 0;   // frames before we start the test
static uint32_t             _f1Depth    = 0;   // frames to compare on each side
static uint32_t             _f1Counter  = 0;   // sub-counter within current stage
static uint64_t             _f1Anchor   = 0;   // frame to rewind back to
static std::vector<uint64_t> _f1PreHashes;
static std::vector<uint64_t> _f1PostHashes;
static bool                 _f1Pass     = false;

static void f1TryConfigure()
{
	if (_f1Configured) return;
	_f1Configured = true;
	const char* env = std::getenv("MAPLECAST_ROLLBACK_F1_TEST");
	if (!env || !*env) return;
	uint32_t warmup = 0, depth = 0;
	if (sscanf(env, "%u,%u", &warmup, &depth) != 2 || depth == 0) {
		printf("[rollback-f1] env malformed (expected 'warmup,depth', got '%s') — test disabled\n", env);
		return;
	}
	if (depth > (uint32_t)RING_DEPTH - 2) {
		printf("[rollback-f1] depth=%u too deep for RING_DEPTH=%d — capping to %d\n",
		       depth, RING_DEPTH, RING_DEPTH - 2);
		depth = RING_DEPTH - 2;
	}
	_f1Warmup = warmup;
	_f1Depth  = depth;
	_f1PreHashes.assign(depth, 0);
	_f1PostHashes.assign(depth, 0);
	_f1Stage  = F1_WARMUP;
	printf("[rollback-f1] armed: warmup=%u frames, depth=%u (will rewind %u frames, replay, compare hashes)\n",
	       warmup, depth, depth);
}

void f1TickFromVblank(uint64_t saveSeq)
{
	if (_f1Stage == F1_IDLE)  return;
	if (_f1Stage == F1_DONE)  return;

	// Diagnostic — every 60 calls so we can see f1Tick is firing
	static int _f1DbgCounter = 0;
	if ((_f1DbgCounter++ % 60) == 0) {
		printf("[rollback-f1-dbg] tick saveSeq=%llu stage=%d warmupRem=%u counter=%u\n",
		       (unsigned long long)saveSeq, (int)_f1Stage, _f1Warmup, _f1Counter);
		fflush(stdout);
	}

	// Hash the just-saved slot's dc_serialize blob. xxh3 is the same hash
	// GGPO uses for sync-test mode (ggpo.cpp:505) — fast (~10 GB/s on
	// modern x86), zero collisions for our workload.
	const int idx = (int)(saveSeq % RING_DEPTH);
	const RingSlot& slot = _ring[idx];
	auto blobHash = [&]() -> uint64_t {
		return XXH3_64bits(slot.serialBlob.data(), slot.serialSize);
	};

	if (_f1Stage == F1_WARMUP) {
		if (_f1Warmup > 0) {
			_f1Warmup--;
			return;
		}
		// Warmup elapsed — record the anchor frame and ENTER PRE without
		// capturing this frame's hash. The next saveFrame call captures
		// pre[0] = state at end of frame anchor+1.
		//
		// Rationale: rewind to anchor restores "state at end of frame
		// anchor"; SH4 resumes execution from frame anchor+1's start.
		// First post-rewind hash captures "state at end of frame anchor+1"
		// — which is what pre[0] needs to be for the comparison to make
		// sense. If we captured pre[0] = "state at end of frame anchor"
		// (the same frame we rewind TO), post[0] would be "state at end
		// of frame anchor+1" and the comparison would always mismatch by
		// definition (different frames produce different state).
		_f1Anchor = saveSeq;
		_f1Stage = F1_PRE;
		_f1Counter = 0;
		printf("[rollback-f1] warmup done @ saveSeq=%llu — anchor recorded; pre[0] starts at saveSeq=%llu\n",
		       (unsigned long long)saveSeq, (unsigned long long)(saveSeq + 1));
		return;  // skip capture for the anchor frame
	}

	if (_f1Stage == F1_PRE) {
		if (_f1Counter < _f1Depth) {
			_f1PreHashes[_f1Counter] = blobHash();
			printf("[rollback-f1] PRE  [%u/%u] saveSeq=%llu hash=%016llx\n",
			       _f1Counter + 1, _f1Depth,
			       (unsigned long long)saveSeq,
			       (unsigned long long)_f1PreHashes[_f1Counter]);
			_f1Counter++;
		}
		if (_f1Counter >= _f1Depth) {
			// PRE complete — REQUEST rewind (don't run it synchronously).
			// We're inside vblank() right now → inside SH4 execution. The
			// emu loop will see pendingRollback after Run() returns and
			// execute the rewind from a safe context.
			printf("[rollback-f1] PRE complete — requesting deferred rewind to anchor saveSeq=%llu\n",
			       (unsigned long long)_f1Anchor);
			requestRewindToFrame(_f1Anchor);
			_f1Stage = F1_POST;
			_f1Counter = 0;
		}
		return;
	}

	if (_f1Stage == F1_POST) {
		if (_f1Counter < _f1Depth) {
			_f1PostHashes[_f1Counter] = blobHash();
			printf("[rollback-f1] POST [%u/%u] saveSeq=%llu hash=%016llx (pre was %016llx)\n",
			       _f1Counter + 1, _f1Depth,
			       (unsigned long long)saveSeq,
			       (unsigned long long)_f1PostHashes[_f1Counter],
			       (unsigned long long)_f1PreHashes[_f1Counter]);
			_f1Counter++;
		}
		if (_f1Counter >= _f1Depth) {
			// POST complete — compare hashes.
			bool allMatch = true;
			for (uint32_t i = 0; i < _f1Depth; i++) {
				if (_f1PreHashes[i] != _f1PostHashes[i]) {
					printf("[rollback-f1] MISMATCH frame %u: pre=%016llx post=%016llx\n",
					       i + 1,
					       (unsigned long long)_f1PreHashes[i],
					       (unsigned long long)_f1PostHashes[i]);
					allMatch = false;
				}
			}
			_f1Pass = allMatch;
			_f1Stage = F1_DONE;
			printf("[rollback-f1] === %s === (%u/%u frames matched)\n",
			       _f1Pass ? "PASS" : "FAIL",
			       _f1Pass ? _f1Depth : 0,
			       _f1Depth);
		}
		return;
	}
}

bool f1Done()   { return _f1Stage == F1_DONE; }
bool f1Passed() { return _f1Pass; }

// Hook into init() so the test arms at startup if env is set.
namespace { struct F1AutoArm { F1AutoArm() {} } _f1autoarm; }

// ── F.2 DC-SERIALIZE byte-diff audit ─────────────────────────────────
//
// Captures two FULL serialized blobs (rollback=false, includes VRAM +
// mem_b) at the same logical frame: once on the live forward path, once
// after rewind+re-emulate. Diffs them and buckets the diff bytes by
// subsystem name (via dcs_mark() in dc_serialize). Tells us exactly
// which subsystem owns the missing-field gap that drives the F.1 hash
// mismatch.
//
// Only runs once; arms via MAPLECAST_DC_AUDIT=warmup_frames env var.

enum F2Stage { F2_IDLE, F2_WARMUP, F2_LIVE_CAPTURE, F2_AFTER_REWIND, F2_DONE };

// Full-blob audit needs more headroom than rollback=true. mem_b alone is
// 16 MB, vram is 8 MB, plus all the rest. 40 MB per buffer is comfortable.
constexpr size_t AUDIT_BLOB_SIZE = 40 * 1024 * 1024;

static F2Stage                _f2Stage      = F2_IDLE;
static bool                   _f2Configured = false;
static uint32_t               _f2Warmup     = 0;
static uint64_t               _f2Anchor     = 0;
static std::vector<uint8_t>   _f2LiveBlob;
static size_t                 _f2LiveSize   = 0;
static std::vector<DcAuditMark> _f2LiveMarks;
static std::vector<uint8_t>   _f2RedoBlob;
static size_t                 _f2RedoSize   = 0;
static std::vector<DcAuditMark> _f2RedoMarks;
static uint64_t               _f2DiffBytes  = 0;

static void dcAuditTryConfigure()
{
	if (_f2Configured) return;
	_f2Configured = true;
	const char* env = std::getenv("MAPLECAST_DC_AUDIT");
	if (!env || !*env) return;
	uint32_t warmup = 0;
	if (sscanf(env, "%u", &warmup) != 1) {
		printf("[dc-audit] env malformed (expected 'warmup', got '%s') — audit disabled\n", env);
		return;
	}
	try {
		_f2LiveBlob.resize(AUDIT_BLOB_SIZE);
		_f2RedoBlob.resize(AUDIT_BLOB_SIZE);
	} catch (const std::bad_alloc&) {
		printf("[dc-audit] failed to allocate %zu MB audit buffers — audit disabled\n",
		       2 * AUDIT_BLOB_SIZE / (1024 * 1024));
		return;
	}
	_f2Warmup = warmup;
	_f2Stage  = F2_WARMUP;
	printf("[dc-audit] armed: warmup=%u frames; will rewind 1 frame and byte-diff full blobs\n",
	       warmup);
}

// Run dc_serialize into the given buffer with mark recording enabled.
// Returns the serialized size on success, 0 on overflow.
static size_t dcSerializeWithMarks(std::vector<uint8_t>& buf,
                                    std::vector<DcAuditMark>& marks)
{
	marks.clear();
	dc_audit_marks = &marks;
	size_t sz = 0;
	try {
		// rollback=false: include VRAM + mem_b. We want to see if missing
		// fields cause SH4-visible writes to those regions to diverge.
		Serializer ser(buf.data(), buf.size(), false);
		dc_serialize(ser);
		sz = ser.size();
	} catch (const Serializer::Exception& e) {
		printf("[dc-audit] dc_serialize overflowed audit buffer: %s\n", e.what());
		sz = 0;
	}
	dc_audit_marks = nullptr;
	return sz;
}

static void dcAuditCompareAndReport()
{
	const size_t commonSize = std::min(_f2LiveSize, _f2RedoSize);

	// First pass: count total diff bytes.
	uint64_t total = 0;
	for (size_t i = 0; i < commonSize; i++)
		if (_f2LiveBlob[i] != _f2RedoBlob[i])
			total++;
	if (_f2LiveSize != _f2RedoSize)
		total += (_f2LiveSize > _f2RedoSize)
			? (_f2LiveSize - _f2RedoSize)
			: (_f2RedoSize - _f2LiveSize);
	_f2DiffBytes = total;

	printf("[dc-audit] === RESULT ===\n");
	printf("[dc-audit] live blob: %zu bytes; redo blob: %zu bytes; common: %zu\n",
	       _f2LiveSize, _f2RedoSize, commonSize);
	printf("[dc-audit] total differing bytes: %llu (%.3f%%)\n",
	       (unsigned long long)total,
	       commonSize ? (100.0 * (double)total / (double)commonSize) : 0.0);

	if (total == 0) {
		printf("[dc-audit] BYTE-PERFECT round-trip. dc_serialize is complete.\n");
		return;
	}

	// Second pass: bucket diffs by region (using live blob's marks; redo
	// marks should match offsets if dc_serialize is structurally identical).
	const auto& marks = _f2LiveMarks;
	printf("[dc-audit] diffs by region (region | bytes_diff / region_size | first_diff_offset_within_region):\n");
	for (size_t mi = 0; mi < marks.size(); mi++) {
		const size_t start = marks[mi].offset;
		const size_t end = (mi + 1 < marks.size()) ? marks[mi + 1].offset : commonSize;
		if (end > commonSize) continue;
		if (start >= end) continue;
		uint64_t regionDiff = 0;
		size_t firstDiffOffset = SIZE_MAX;
		for (size_t j = start; j < end; j++) {
			if (_f2LiveBlob[j] != _f2RedoBlob[j]) {
				if (firstDiffOffset == SIZE_MAX) firstDiffOffset = j - start;
				regionDiff++;
			}
		}
		if (regionDiff > 0) {
			printf("[dc-audit]   %-22s | %8llu / %8zu | first diff @ +%zu\n",
			       marks[mi].name,
			       (unsigned long long)regionDiff,
			       end - start,
			       firstDiffOffset);
			// Dump up to first 16 differing offsets with their byte values
			// so we can correlate to specific fields. live-byte / redo-byte
			// at each offset (relative to region start).
			int dumpedCount = 0;
			printf("[dc-audit]     ");
			for (size_t j = start; j < end && dumpedCount < 16; j++) {
				if (_f2LiveBlob[j] != _f2RedoBlob[j]) {
					printf("+%zu(%02x→%02x) ",
					       j - start,
					       (unsigned)_f2LiveBlob[j],
					       (unsigned)_f2RedoBlob[j]);
					dumpedCount++;
				}
			}
			if (regionDiff > (uint64_t)dumpedCount)
				printf("... +%llu more", (unsigned long long)(regionDiff - dumpedCount));
			printf("\n");
		}
	}
	printf("[dc-audit] ─── end of report ───\n");
}

void dcAuditTickFromVblank(uint64_t saveSeq)
{
	if (_f2Stage == F2_IDLE) return;
	if (_f2Stage == F2_DONE) return;

	if (_f2Stage == F2_WARMUP) {
		if (_f2Warmup > 0) {
			_f2Warmup--;
			return;
		}
		_f2Anchor = saveSeq;
		_f2Stage = F2_LIVE_CAPTURE;
		printf("[dc-audit] warmup done @ saveSeq=%llu — anchor recorded; live capture next frame "
		       "(anchor sched_now=%llu, sh4_pc=0x%08x)\n",
		       (unsigned long long)saveSeq,
		       (unsigned long long)sh4_sched_now64(),
		       (unsigned)Sh4cntx.pc);
		return;
	}

	if (_f2Stage == F2_LIVE_CAPTURE) {
		// SYNCHRONOUS rewind — runs inside vblank's saveFrame call. Works
		// (no hang) but produces 155-byte / SH4_TIMESLICE drift because
		// the host stack stays inside spg_line_sched's continuation,
		// leaving stale local state.
		//
		// Deferred rewind (Stop → emu loop → executePendingRewind →
		// Start → re-enter Run()) WOULD give byte-perfect determinism
		// because it unwinds the host stack fully, but currently HANGS
		// after dc_deserialize-while-SH4-stopped. Hang is NOT JIT-
		// specific (interpreter also hangs), NOT renderer-specific
		// (rend_resync didn't help), NOT memwatch-specific (disabling
		// didn't help), NOT audio-pacing-specific (bypass didn't help).
		// Pure Stop/Start round-trip without dc_deserialize works fine.
		// Root cause appears to be in dc_deserialize's restoration of
		// some state that's incompatible with SH4-currently-stopped.
		_f2LiveSize = dcSerializeWithMarks(_f2LiveBlob, _f2LiveMarks);
		printf("[dc-audit] LIVE captured @ saveSeq=%llu — %zu bytes, %zu marks "
		       "(sched_now=%llu, sh4_pc=0x%08x)\n",
		       (unsigned long long)saveSeq, _f2LiveSize, _f2LiveMarks.size(),
		       (unsigned long long)sh4_sched_now64(),
		       (unsigned)Sh4cntx.pc);
		if (_f2LiveSize == 0) {
			printf("[dc-audit] live capture failed — audit aborted\n");
			_f2Stage = F2_DONE;
			return;
		}
		// Switch between sync rewind (works, 154-byte drift) and deferred
		// (target byte-perfect, currently hangs after dc_deserialize).
		// Deferred is the path we need for byte-perfect; sync is the
		// fallback. MAPLECAST_DC_AUDIT_DEFERRED=1 selects deferred.
		if (std::getenv("MAPLECAST_DC_AUDIT_DEFERRED")) {
			printf("[dc-audit] requesting DEFERRED rewind to anchor saveSeq=%llu\n",
			       (unsigned long long)_f2Anchor);
			requestRewindToFrame(_f2Anchor);
		} else {
			printf("[dc-audit] SYNCHRONOUS rewind to anchor saveSeq=%llu\n",
			       (unsigned long long)_f2Anchor);
			bool ok = rewindToFrame(_f2Anchor);
			printf("[dc-audit] synchronous rewindToFrame returned %s\n", ok ? "OK" : "FAILED");
			fflush(stdout);
		}
		_f2Stage = F2_AFTER_REWIND;
		return;
	}

	if (_f2Stage == F2_AFTER_REWIND) {
		// Post-rewind, post-re-execution. saveFrame fired again at the
		// logical-equivalent of anchor+1 (one frame past the rewind anchor).
		// Capture redo blob and compare.
		_f2RedoSize = dcSerializeWithMarks(_f2RedoBlob, _f2RedoMarks);
		printf("[dc-audit] REDO captured @ saveSeq=%llu — %zu bytes, %zu marks "
		       "(sched_now=%llu, sh4_pc=0x%08x)\n",
		       (unsigned long long)saveSeq, _f2RedoSize, _f2RedoMarks.size(),
		       (unsigned long long)sh4_sched_now64(),
		       (unsigned)Sh4cntx.pc);
		if (_f2RedoSize == 0) {
			printf("[dc-audit] redo capture failed — audit aborted\n");
			_f2Stage = F2_DONE;
			return;
		}
		dcAuditCompareAndReport();
		_f2Stage = F2_DONE;
		return;
	}
}

bool     dcAuditDone()      { return _f2Stage == F2_DONE; }
uint64_t dcAuditDiffBytes() { return _f2DiffBytes; }

} // namespace maplecast_rollback
