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
#include "hw/mem/mem_watch.h"
#include "hw/sh4/sh4_if.h"

#include <atomic>
#include <cstdio>
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

	// Restore the SH4 + PVR scalar state (registers, scheduler, etc) from
	// the dc_serialize blob at the target frame.
	Deserializer deser(target.serialBlob.data(), target.serialSize, true);
	uint32_t frame32;
	deser >> frame32;
	dc_deserialize(deser);

	if (deser.size() != target.serialSize) {
		printf("[rollback] rewind: deserialize size mismatch (used %zu of %zu)\n",
		       deser.size(), target.serialSize);
		// Don't die here — V59 patches may have left some unreachable bytes
	}

	memwatch::reset();
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

} // namespace maplecast_rollback
