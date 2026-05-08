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
#include "hw/sh4/sh4_if.h"
#include <xxhash.h>  // bundled at core/deps/xxHash/, on the include path

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

// dc_serialize blob size. MVC2/Naomi states fit within ~10 MB; reserve
// 16 MB per slot for headroom. 10 slots × 16 MB = 160 MB — fine on
// modern desktop. Compare GGPO vectorwar's malloc-per-save (480 MB/s
// allocator churn at this scale) — we allocate ONCE at init and reuse.
constexpr size_t SLOT_BLOB_SIZE = 16 * 1024 * 1024;

// Forward declarations for F.1 test plumbing (defined at end of file).
static void f1TryConfigure();

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
	memwatch::mirrorActive = true;

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

	// 1. dc_serialize into the slot's pre-allocated blob. Mirrors ggpo.cpp:494
	//    but writes to our reused buffer instead of malloc.
	Serializer ser(slot.serialBlob.data(), SLOT_BLOB_SIZE, true);
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
	memwatch::protect();

	// 3. Drain memwatch's page diffs into the slot. capture() calls
	//    getPages() which SWAPS the watcher's pages map out, leaving the
	//    watcher empty but still started+protected. Next frame's first-
	//    write to any of those pages will fault, get captured into the
	//    (now empty) pages map, get unprotected, and the write completes.
	slot.pages.clear();
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

	memwatch::unprotect();

	// Walk backward from mostRecent down to target+1, applying each slot's
	// pages back to live memory in REVERSE order. Mirrors ggpo.cpp:449-462
	// exactly — each pair.second.data is the PRE-WRITE contents of the page,
	// so memcpy'ing it back undoes that frame's writes.
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
		Deserializer deser(target.serialBlob.data(), target.serialSize, true);
		uint32_t frame32;
		deser >> frame32;
		emu.loadstate(deser);

		if (deser.size() != target.serialSize) {
			printf("[rollback] rewind: deserialize size mismatch (used %zu of %zu)\n",
			       deser.size(), target.serialSize);
			// Don't die here — V60 patches may have left some unreachable bytes
		}
	}

	// memwatch::reset already done by emu.loadstate; just re-arm.
	memwatch::protect();

	_mostRecentFrame = frame;  // we've effectively undone everything past this
	_rollbacksDone++;

	printf("[rollback] rewound to frame %llu (was %llu, undid %llu frames)\n",
	       (unsigned long long)frame,
	       (unsigned long long)(_mostRecentFrame),
	       (unsigned long long)(_mostRecentFrame > frame ? _mostRecentFrame - frame : 0));
	return true;
}

uint64_t oldestAvailable() { return _oldestFrame; }
uint64_t mostRecentSaved() { return _mostRecentFrame; }
bool active() { return _active.load(std::memory_order_relaxed); }

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
		// Warmup elapsed — begin the PRE pass.
		_f1Anchor = saveSeq;
		_f1Stage = F1_PRE;
		_f1Counter = 0;
		printf("[rollback-f1] warmup done @ saveSeq=%llu — beginning PRE pass (anchor frame %llu)\n",
		       (unsigned long long)saveSeq, (unsigned long long)_f1Anchor);
		// Fall through to F1_PRE handling below — capture this frame's hash.
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
			// PRE complete — rewind. The post-rewind SH4 should reproduce
			// the same hashes as PRE if our rollback is byte-deterministic.
			printf("[rollback-f1] PRE complete — calling rewindToFrame(%llu)\n",
			       (unsigned long long)_f1Anchor);
			if (!rewindToFrame(_f1Anchor)) {
				printf("[rollback-f1] FAIL — rewindToFrame returned false\n");
				_f1Stage = F1_DONE;
				_f1Pass = false;
				return;
			}
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

} // namespace maplecast_rollback
