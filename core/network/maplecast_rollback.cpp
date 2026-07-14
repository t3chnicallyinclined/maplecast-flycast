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
#include <chrono>
#include "maplecast_rollback.h"
#include "types.h"

// Global active-low controller state (defined in gamepad_device.cpp). Written by
// the input server on UDP; used by the lag probe to drive P1 deterministically.
extern u32 kcode[4];
#include "serialize.h"
#include "emulator.h"
#include "hw/mem/mem_watch.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/spg.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_mem.h"   // mem_b (guest RAM) for game-state-region hashing
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
	int                   spgJitter = 0;          // jitter passed to
	                                              // spg_line_sched at this
	                                              // saveFrame; used on rewind
	                                              // to reconstruct the post-
	                                              // callback vblank_schid
	                                              // reschedule (closes 2 bytes
	                                              // of byte-diff drift).
};

static RingSlot          _ring[RING_DEPTH];
static std::atomic<bool> _active{false};
static uint64_t          _mostRecentFrame = UINT64_MAX;
static uint64_t          _oldestFrame     = UINT64_MAX;
static uint64_t          _framesSaved     = 0;
static uint64_t          _rollbacksDone   = 0;

// ── A2 dirty-page rewind determinism gate scratch ────────────────────
// FULL (rollback=false) blob of the most-recently-saved frame, stashed only
// under MAPLECAST_RA_DIRTY_GATE so the rewind can do an authoritative
// full-deser rewind and byte-compare it against the dirty-page result.
// Run-ahead only ever rewinds to the just-saved frame, so ONE buffer suffices.
static std::vector<uint8_t> _gateFullBlob;
static size_t               _gateFullSize  = 0;
static uint64_t             _gateFullFrame = UINT64_MAX;

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
	// A2 dirty-page rewind: under MAPLECAST_RA_DIRTY_REWIND / _GATE the ring blob
	// is SCALARS-ONLY (rollback=true skips ONLY mem_b + vram + aica_ram — VERIFIED
	// the only rollback()-guarded arrays: sh4_mmr.cpp:688, pvr.cpp:96, aica_if.cpp:537;
	// elan.cpp:1832 is Naomi2-only; the maple_cfg.cpp:464/499 guards change device
	// object lifecycle but write/read the SAME blob bytes). The 3 arrays are restored
	// on rewind by the memwatch live-map pagewalk. SAVE and RESTORE MUST use the SAME
	// rollback flag (both gated on the same env) or the deserializer desyncs.
	static const bool _raDirty     = [](){ const char* e = std::getenv("MAPLECAST_RA_DIRTY_REWIND"); return !(e && e[0] == '0'); }(); // A2: default ON — 1.9ms dirty rewind vs ~13ms full deser; disable with =0
	static const bool _raDirtyGate = std::getenv("MAPLECAST_RA_DIRTY_GATE") != nullptr;
	const bool _blobRollback = _raDirty || _raDirtyGate;
	Serializer ser(slot.serialBlob.data(), SLOT_BLOB_SIZE, _blobRollback);
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

	// Gate: also stash a FULL (rollback=false) blob of THIS frame for the
	// dirty-vs-full determinism compare in rewindToFrame. One scratch buffer is
	// enough — run-ahead only ever rewinds to the just-saved frame.
	if (_raDirtyGate) {
		try {
			if (_gateFullBlob.size() != SLOT_BLOB_SIZE) _gateFullBlob.resize(SLOT_BLOB_SIZE);
			Serializer fser(_gateFullBlob.data(), SLOT_BLOB_SIZE, false);
			uint32_t f32 = (uint32_t)(frame & 0xFFFFFFFFu);
			fser << f32;
			dc_serialize(fser);
			_gateFullSize  = fser.size();
			_gateFullFrame = frame;
		} catch (const Serializer::Exception& e) {
			printf("[ra-gate] saveFrame(%llu): full dc_serialize failed: %s\n",
			       (unsigned long long)frame, e.what());
			_gateFullSize = 0;
			_gateFullFrame = UINT64_MAX;
		}
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
	// Capture spg_line_sched's most-recent jitter so rewindToFrame can
	// reconstruct LIVE's post-callback vblank_schid reschedule
	// (handle_cb does sh4_sched_request with re_sch - jitter; we need
	// to mirror that on rewind to avoid a 1-byte ffb / interrupt_pend drift).
	slot.spgJitter = spg_last_jitter;

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

// A2 dirty-page determinism gate. On entry the guest state holds the DIRTY-PAGE
// rewind result of `frame` (fixups already applied by the caller). We serialize
// it FULL (blobA), then do the authoritative full-deser rewind of the same frame
// from _gateFullBlob applying the SAME post-loadstate fixups, serialize FULL
// (blobB), and byte-compare. First diff is localized to a subsystem via dcs_mark.
// Leaves the full-path (authoritative) state so a dirty-path bug can't corrupt
// the running sim. PASS = 0 diff sustained over thousands of rewinds.
static void raDirtyGateCompare(uint64_t frame, int spgJitter, bool lightweight)
{
	if (_gateFullFrame != frame || _gateFullSize == 0) {
		static uint64_t _w = 0;
		if ((_w++ % 600) == 0)
			printf("[ra-gate] no full blob for frame %llu (have %llu) — skip compare\n",
			       (unsigned long long)frame, (unsigned long long)_gateFullFrame);
		return;
	}
	static std::vector<uint8_t>     _blobA, _blobB;
	static std::vector<DcAuditMark> _marksA, _marksB;
	if (_blobA.size() != SLOT_BLOB_SIZE) _blobA.resize(SLOT_BLOB_SIZE);
	if (_blobB.size() != SLOT_BLOB_SIZE) _blobB.resize(SLOT_BLOB_SIZE);

	auto serFull = [](std::vector<uint8_t>& buf, std::vector<DcAuditMark>& marks) -> size_t {
		marks.clear();
		dc_audit_marks = &marks;
		size_t sz = 0;
		try { Serializer s(buf.data(), buf.size(), false); dc_serialize(s); sz = s.size(); }
		catch (const Serializer::Exception& e) { printf("[ra-gate] serialize overflow: %s\n", e.what()); sz = 0; }
		dc_audit_marks = nullptr;
		return sz;
	};

	// blobA = dirty-page-rewound state (fixups already applied by caller).
	const size_t sizeA = serFull(_blobA, _marksA);

	// Authoritative full-deser rewind of the same frame + IDENTICAL fixups so the
	// only variable is the state-restore method (dirty pagewalk vs full deser).
	try {
		Deserializer d(_gateFullBlob.data(), _gateFullSize, false);
		uint32_t f32; d >> f32;
		emu.loadstate(d, lightweight);
	} catch (const Deserializer::Exception& e) {
		printf("[ra-gate] full deser failed: %s\n", e.what());
		return;
	}
	rend_resync_after_rollback();
	{
		const int re_sch = spg_getNextInterrupt();
		sh4_sched_request(vblank_schid, std::max(0, re_sch - spgJitter));
	}

	// blobB = full-deser-rewound state.
	const size_t sizeB = serFull(_blobB, _marksB);

	// Compare.
	static uint64_t _n = 0, _diffRewinds = 0;
	_n++;
	const size_t common = std::min(sizeA, sizeB);
	uint64_t diff = 0;
	size_t firstDiff = SIZE_MAX;
	for (size_t i = 0; i < common; i++)
		if (_blobA[i] != _blobB[i]) { if (firstDiff == SIZE_MAX) firstDiff = i; diff++; }
	if (sizeA != sizeB)
		diff += (sizeA > sizeB) ? (sizeA - sizeB) : (sizeB - sizeA);
	if (diff != 0) _diffRewinds++;

	if (diff != 0) {
		const char* region = "(size-only)";
		size_t regOff = 0;
		if (firstDiff != SIZE_MAX) {
			for (size_t mi = 0; mi < _marksA.size(); mi++) {
				const size_t st = _marksA[mi].offset;
				const size_t en = (mi + 1 < _marksA.size()) ? _marksA[mi + 1].offset : common;
				if (firstDiff >= st && firstDiff < en) { region = _marksA[mi].name; regOff = firstDiff - st; break; }
			}
		}
		printf("[ra-gate] DIFF frame %llu: %llu bytes (sizeA=%zu sizeB=%zu) first@+%zu region=%s +%zu (A=%02x B=%02x)\n",
		       (unsigned long long)frame, (unsigned long long)diff, sizeA, sizeB,
		       firstDiff == SIZE_MAX ? (size_t)0 : firstDiff, region, regOff,
		       firstDiff == SIZE_MAX ? 0 : (unsigned)_blobA[firstDiff],
		       firstDiff == SIZE_MAX ? 0 : (unsigned)_blobB[firstDiff]);
		fflush(stdout);
	}
	if ((_n % 600) == 0) {
		printf("[ra-gate] over %llu rewinds: %llu had diffs => %s\n",
		       (unsigned long long)_n, (unsigned long long)_diffRewinds,
		       _diffRewinds == 0 ? "PASS (dirty==full, byte-identical)"
		                         : "FAIL (dirty-page state diverges — see DIFF lines)");
		fflush(stdout);
	}
}

bool rewindToFrame(uint64_t frame, bool lightweight)
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
	static const bool _rwProf = std::getenv("MAPLECAST_STATEVF") != nullptr;
	auto _rwt0 = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point _rwt1{}, _rwt2{};
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

	// ── A2 dirty-page tracking-completeness audit (MAPLECAST_RA_REWIND_AUDIT=1) ──
	// NON-DESTRUCTIVE to the real rewind. We snapshot the LIVE region bytes (the
	// leg-2/3 = frame N+2 state; the pagewalk above is empty for run-ahead so live
	// is untouched) and DRAIN the memwatch TRACKED page-set here, BEFORE
	// emu.loadstate restores TRUTH (frame N) below. Then (post-loadstate) we diff
	// LIVE vs TRUTH per page and cross it against TRACKED to find LEAK pages
	// (changed-but-untracked = the hybrid-state source a dirty-page restore misses).
	//
	// Why the drain (getPages) is harmless: it empties the watcher's live map, but
	// emu.loadstate calls memwatch::reset() (emulator.cpp:1157) which clears it
	// regardless, and our memwatch::protect() at the end of this function re-arms
	// with started=false -> full protectMem(0,0xffffffff) re-protect. So the real
	// rewind is byte-for-byte unchanged. Reads are safe: memwatch::unprotect() ran
	// at the top of rewindToFrame (all pages RW), and region_lock is PAGE_READONLY
	// anyway (win_vmem.cpp:19) so reads never fault even after re-arm.
	// A2 mode flags. Tracking audit and dirty rewind BOTH drain the memwatch live
	// map, so they are mutually exclusive; dirty wins (it is the ships path) and the
	// tracking audit yields when dirty is on. _dirtyPath MUST equal saveFrame's
	// _blobRollback exactly (both = _raDirty||_raDirtyGate) or the deser desyncs.
	static const bool _raAudit     = std::getenv("MAPLECAST_RA_REWIND_AUDIT") != nullptr;
	static const bool _raDirty     = [](){ const char* e = std::getenv("MAPLECAST_RA_DIRTY_REWIND"); return !(e && e[0] == '0'); }(); // A2: default ON — 1.9ms dirty rewind vs ~13ms full deser; disable with =0
	static const bool _raDirtyGate = std::getenv("MAPLECAST_RA_DIRTY_GATE") != nullptr;
	const bool _dirtyPath  = _raDirty || _raDirtyGate;   // == saveFrame _blobRollback
	const bool _trackAudit = _raAudit && !_dirtyPath;    // tracking audit yields to dirty
	if (_raAudit && _dirtyPath) {
		static bool _warned = false;
		if (!_warned) { _warned = true;
			printf("[ra] NOTE: MAPLECAST_RA_REWIND_AUDIT ignored — RA_DIRTY_REWIND/_GATE owns the live map.\n");
			fflush(stdout);
		}
	}
	static std::vector<uint8_t> _auRam, _auVram, _auAram;   // LIVE snapshots (reused)
	memwatch::PageMap _auTrkRam, _auTrkVram, _auTrkAram;     // drained TRACKED sets
	if (_trackAudit) {
		const size_t rs = RAM_SIZE, vs = VRAM_SIZE, as = ARAM_SIZE;
		if (_auRam.size()  != rs) _auRam.resize(rs);
		if (_auVram.size() != vs) _auVram.resize(vs);
		if (_auAram.size() != as) _auAram.resize(as);
		// getMemPage(0) = &mem_b[0] / &vram[0] / &aica::aica_ram[0] (mem_watch.h
		// 145/117/161) — the primary (protected) mirror; now holds LIVE (N+2).
		memcpy(_auRam.data(),  memwatch::ramWatcher.getMemPage(0),  rs);
		memcpy(_auVram.data(), memwatch::vramWatcher.getMemPage(0), vs);
		memcpy(_auAram.data(), memwatch::aramWatcher.getMemPage(0), as);
		// TRACKED = pages memwatch captured during legs 2-3 (undrained live map;
		// saveFrame(N) already drained leg1's pages into slot.pages). This is
		// exactly the page-set a dirty-page rewind would restore.
		memwatch::ramWatcher.getPages(_auTrkRam);
		memwatch::vramWatcher.getPages(_auTrkVram);
		memwatch::aramWatcher.getPages(_auTrkAram);
	}

	// ── A2 DIRTY-PAGE PAGEWALK (MAPLECAST_RA_DIRTY_REWIND / _GATE) ──
	// Replaces the (empty-for-run-ahead) ring pagewalk above: drain memwatch's
	// LIVE map (legs-2/3 captures; pair.second.data = PRE-WRITE = frame-N bytes,
	// proven byte-complete by the LEAK=0 tracking audit) and memcpy each page back
	// to undo legs 2-3 on mem_b/vram/aica_ram. The scalars-only blob (rollback=true)
	// restores everything else via emu.loadstate below. MUST run BEFORE loadstate:
	// loadstate calls memwatch::reset() which clears the live map.
	//
	// Consumes the live map (mutually exclusive with the tracking audit above,
	// which yields to us). _dirtyPath / _raDirtyGate were declared with the mode
	// flags at the top of the tracking-audit block above.
	if (_dirtyPath) {
		memwatch::PageMap dpRam, dpVram, dpAram, dpElan;
		memwatch::ramWatcher.getPages(dpRam);
		memwatch::vramWatcher.getPages(dpVram);
		memwatch::aramWatcher.getPages(dpAram);
		memwatch::elanWatcher.getPages(dpElan);
		for (const auto& pr : dpRam)  memcpy(memwatch::ramWatcher.getMemPage(pr.first),  &pr.second.data[0], PAGE_SIZE);
		for (const auto& pr : dpVram) memcpy(memwatch::vramWatcher.getMemPage(pr.first), &pr.second.data[0], PAGE_SIZE);
		for (const auto& pr : dpAram) memcpy(memwatch::aramWatcher.getMemPage(pr.first), &pr.second.data[0], PAGE_SIZE);
		for (const auto& pr : dpElan) memcpy(memwatch::elanWatcher.getMemPage(pr.first), &pr.second.data[0], PAGE_SIZE);
		// mem_b/vram/aica_ram now hold frame N; loadstate(rollback=true) restores scalars.
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
		// Deserializer rollback flag MUST match how saveFrame wrote this slot:
		// _dirtyPath ? rollback=true (scalars-only; mem_b/vram/aica_ram already
		// restored by the dirty pagewalk above) : rollback=false (full blob).
		// emu.loadstate does the load-bearing wrappers (bm_Reset, ResetCache,
		// aica recompiler flush, mmu_flush_table, custom_texture init,
		// EventManager LoadState broadcast) in BOTH modes — that coherence pass
		// is the other half of why the prior page-delta attempt deadlocked, so
		// we keep emu.loadstate rather than a bare dc_deserialize.
		Deserializer deser(target.serialBlob.data(), target.serialSize, _dirtyPath);
		uint32_t frame32;
		deser >> frame32;
		_rwt1 = std::chrono::steady_clock::now();
		emu.loadstate(deser, lightweight);   // A2: lightweight skips the per-tick dynarec flush
		_rwt2 = std::chrono::steady_clock::now();
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
		// Reconstruct LIVE's post-callback vblank_schid reschedule.
		// In LIVE, handle_cb computed: sh4_sched_request(vblank_schid,
		//   max(0, re_sch - jitter)) where jitter = anchor's spg_last_jitter.
		// rescheduleSPG() alone uses re_sch with no jitter subtract,
		// leaving sched.end exactly `jitter` cycles late vs LIVE.
		// Mirror LIVE's exact math here using the jitter we captured
		// alongside the anchor blob in saveFrame.
		const int re_sch = spg_getNextInterrupt();
		const int adj_cycles = std::max(0, re_sch - target.spgJitter);
		sh4_sched_request(vblank_schid, adj_cycles);

		if (deser.size() != target.serialSize) {
			printf("[rollback] rewind: deserialize size mismatch (used %zu of %zu)\n",
			       deser.size(), target.serialSize);
			// Don't die here — V60 patches may have left some unreachable bytes
		}
	}

	// A2 dirty-page determinism GATE: with the state now holding the dirty-page
	// rewind result (+fixups), re-serialize it, then do the authoritative
	// full-deser rewind of the same frame and byte-compare. Leaves the full-path
	// (authoritative) state. Its second emu.loadstate re-does unprotect()+reset(),
	// so the re-arm below still fires started=false -> full re-protect. No-op
	// unless MAPLECAST_RA_DIRTY_GATE=1.
	if (_raDirtyGate)
		raDirtyGateCompare(frame, target.spgJitter, lightweight);

	// memwatch::reset already done by emu.loadstate; just re-arm.
	if (!_memwatchDisabledRewind)
		memwatch::protect();
	if (_rwProf) {
		auto _rwt3 = std::chrono::steady_clock::now();
		auto us=[](auto a,auto b){ return (long long)std::chrono::duration_cast<std::chrono::microseconds>(b-a).count(); };
		static long long _pw=0,_ds=0,_pr=0,_n=0;
		_pw+=us(_rwt0,_rwt1); _ds+=us(_rwt1,_rwt2); _pr+=us(_rwt2,_rwt3); _n++;
		if (_n%600==0){ printf("[REWIND-PROF] pagewalk=%lldus deserialize=%lldus protect=%lldus (n=%lld)\n",_pw/600,_ds/600,_pr/600,_n); fflush(stdout); _pw=_ds=_pr=0; }
	}

	// ── A2 dirty-page audit, part 2: post-loadstate LIVE-vs-TRUTH diff ──
	// getMemPage(0) now holds TRUTH (frame N) restored by emu.loadstate above.
	// Per region: ACTUAL_CHANGED = { page : LIVE[page] != TRUTH[page] };
	//   LEAK     = ACTUAL_CHANGED \ TRACKED  (changed but memwatch missed it)
	//   SPURIOUS = TRACKED \ ACTUAL_CHANGED  (tracked but net-unchanged; harmless)
	// PASS for a region = LEAK stays 0 over thousands of rewinds -> a dirty-page
	// restore of that region would be byte-exact. Any LEAK>0 -> region must stay
	// full-restore; per-rewind lines print the leaking page offsets to trace them.
	if (_trackAudit) {
		struct RegionAcc { uint64_t changed=0, tracked=0, leak=0, spurious=0, rewinds=0; };
		static RegionAcc _acRam, _acVram, _acAram;
		auto audit1 = [](const char* name, RegionAcc& acc,
		                 const std::vector<uint8_t>& live, const void* truthBase,
		                 const memwatch::PageMap& tracked, size_t regionSize) {
			const uint8_t* truth = (const uint8_t*)truthBase;
			size_t changed = 0, leak = 0, spurious = 0;
			std::vector<uint32_t> leakPages;
			for (size_t off = 0; off < regionSize; off += PAGE_SIZE) {
				const size_t n = std::min((size_t)PAGE_SIZE, regionSize - off);
				const bool chg = memcmp(&live[off], truth + off, n) != 0;
				const bool trk = tracked.find((u32)off) != tracked.end();
				if (chg) changed++;
				if (chg && !trk) { leak++; if (leakPages.size() < 32) leakPages.push_back((u32)off); }
				if (trk && !chg) spurious++;
			}
			acc.changed += changed; acc.tracked += tracked.size();
			acc.leak += leak; acc.spurious += spurious; acc.rewinds++;
			// Per-rewind LEAK detail (only when nonzero) to trace the pages.
			if (leak > 0) {
				printf("[RA-AUDIT] %-4s LEAK=%zu changed=%zu tracked=%zu  leak-page-offsets:",
				       name, leak, changed, tracked.size());
				for (uint32_t p : leakPages) printf(" 0x%x", (unsigned)p);
				if (leak > leakPages.size()) printf(" ...(+%zu more)", leak - leakPages.size());
				printf("\n"); fflush(stdout);
			}
			// Rollup every 600 audited rewinds.
			if ((acc.rewinds % 600) == 0) {
				printf("[RA-AUDIT] %-4s over %llu rewinds: avg changed=%.1f tracked=%.1f  "
				       "LEAK-total=%llu SPURIOUS-total=%llu  => %s\n",
				       name, (unsigned long long)acc.rewinds,
				       (double)acc.changed / (double)acc.rewinds,
				       (double)acc.tracked / (double)acc.rewinds,
				       (unsigned long long)acc.leak, (unsigned long long)acc.spurious,
				       acc.leak == 0 ? "PASS (dirty-page-safe)" : "FAIL (must full-restore)");
				fflush(stdout);
			}
		};
		audit1("RAM",  _acRam,  _auRam,  memwatch::ramWatcher.getMemPage(0),  _auTrkRam,  _auRam.size());
		audit1("VRAM", _acVram, _auVram, memwatch::vramWatcher.getMemPage(0), _auTrkVram, _auVram.size());
		audit1("ARAM", _acAram, _auAram, memwatch::aramWatcher.getMemPage(0), _auTrkAram, _auAram.size());
	}

	_mostRecentFrame = frame;  // we've effectively undone everything past this
	_rollbacksDone++;

	static uint64_t _rwLogN = 0;   // A2: runahead rewinds EVERY tick — 60 printf/s of journald I/O is itself a perf suspect; log 1-in-600
	if ((_rwLogN++ % 600) == 0)
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

// ── External-blob capture/restore (used by .mcrec replay) ────────────
//
// Mirrors saveFrame/rewindToFrame minus the ring storage so the same
// byte-perfect capture/restore logic can produce/consume .mcrec blobs.
// No init()/active() check — these are stateless and safe to call
// without MAPLECAST_ROLLBACK_RING being enabled.

size_t captureFrameToBlob(uint8_t* outBuf, size_t bufSize, int32_t& outSpgJitter)
{
	// Same dc_serialize call saveFrame() uses (rollback=false → full
	// state, includes VRAM + mem_b). Caller must invoke from SH4 emu
	// thread at a frame boundary so handle_cb has finished its post-
	// callback re-schedule for every fired schid.
	try {
		Serializer ser(outBuf, bufSize, false);
		dc_serialize(ser);
		outSpgJitter = (int32_t)spg_last_jitter;
		return ser.size();
	} catch (const Serializer::Exception& e) {
		printf("[rollback] captureFrameToBlob: dc_serialize failed: %s\n", e.what());
		outSpgJitter = 0;
		return 0;
	}
}

bool restoreFromBlob(const uint8_t* blob, size_t size, int32_t spgJitter)
{
	const uint64_t prerewindNow = sh4_sched_now64();
	const uint32_t prerewindPc  = (uint32_t)Sh4cntx.pc;

	// rewindToFrame's flow, minus the page-delta walk (we don't have
	// prior-frame deltas in the .mcrec — the embedded blob is a full
	// dc_serialize). emu.loadstate handles the full restore including
	// memwatch::unprotect/reset, bm_Reset, ResetCache, custom_texture
	// init, mmu flush, and EventManager LoadState broadcast.
	try {
		Deserializer deser(blob, size, false);
		emu.loadstate(deser);
	} catch (const Deserializer::Exception& e) {
		printf("[rollback] restoreFromBlob: deserialize failed: %s\n", e.what());
		return false;
	}

	// Drain stale Render queue entries the recording's live-forward
	// path may have left behind. Without this, post-restore SH4 blocks
	// in pvrQueue::enqueue on a "duplicate Render".
	rend_resync_after_rollback();

	// Reconstruct LIVE's post-callback vblank_schid reschedule using the
	// captured jitter. handle_cb's math: sh4_sched_request(vblank_schid,
	// max(0, re_sch - jitter)). Doing this with no jitter (or wrong
	// jitter) leaves sched.end exactly `jitter` cycles late, which over
	// many frames cascades into a frame-level input misalignment and
	// state desync (a hit gets missed, etc.).
	const int re_sch = spg_getNextInterrupt();
	const int adj_cycles = std::max(0, re_sch - spgJitter);
	sh4_sched_request(vblank_schid, adj_cycles);

	// Re-arm memwatch protection (emu.loadstate did unprotect+reset; we
	// need to re-arm before SH4 resumes so dirty-page tracking restarts).
	static const bool _memwatchDisabledRestore = std::getenv("MAPLECAST_DISABLE_MEMWATCH") != nullptr;
	if (!_memwatchDisabledRestore)
		memwatch::protect();

	printf("[rollback] restoreFromBlob: %zu bytes, jitter=%d, sched_now %llu→%llu, pc 0x%08x→0x%08x\n",
	       size, spgJitter,
	       (unsigned long long)prerewindNow,
	       (unsigned long long)sh4_sched_now64(),
	       (unsigned)prerewindPc,
	       (unsigned)Sh4cntx.pc);
	fflush(stdout);
	return true;
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
// GATE ADD: game-state-region hash (char structs + global game-state page +
// frame ctr + fight tick) and full-RAM hash (16MB minus known-nondeterministic
// render-interp/stage-anim scratch), read from LIVE guest RAM each frame.
// Classifies F.1 blob mismatch: if these MATCH while blob differs, the diff is
// scheduler-epoch-only (harmless for a native-render lockstep client).
static std::vector<uint64_t> _f1PreGs,  _f1PostGs;
static std::vector<uint64_t> _f1PreRam, _f1PostRam;
static bool                 _f1Pass     = false;

// Hash the coordinator-specified deterministic game-state region directly from
// guest RAM (mem_b indexed by physical offset = guestAddr - 0x8C000000).
// Exposed (declared in maplecast_rollback.h) so the lockstep-mirror client and
// server share ONE definition of the checksum region — same bytes, same order.
uint64_t gameStateRegionHash()
{
	XXH3_state_t* st = XXH3_createState();
	XXH3_64bits_reset(st);
	XXH3_64bits_update(st, &mem_b[0x268340], 6u * 0x5A4u); // 6 char structs
	XXH3_64bits_update(st, &mem_b[0x289000], 0x1000u);     // global game-state page
	XXH3_64bits_update(st, &mem_b[0x3496B0], 4u);          // frame counter
	XXH3_64bits_update(st, &mem_b[0x268250], 1u);          // fight tick
	uint64_t h = XXH3_64bits_digest(st);
	XXH3_freeState(st);
	return h;
}

// Per-region hashes for the predict-live confirmed-vs-server divergence diagnostic.
// out[0]=char structs  out[1]=gs-page(0x289000)  out[2]=frame-ctr  out[3]=fight-tick
// out[4]=raw input latch (0x8C200BA8, 8 bytes) — same region set on client+server so
// their per-frame logs can be diffed to localize which region diverges under input.
void gameStateSubHashes(uint64_t out[5])
{
	out[0] = XXH3_64bits(&mem_b[0x268340], 6u * 0x5A4u);
	out[1] = XXH3_64bits(&mem_b[0x289000], 0x1000u);
	out[2] = XXH3_64bits(&mem_b[0x3496B0], 4u);
	out[3] = XXH3_64bits(&mem_b[0x268250], 1u);
	out[4] = XXH3_64bits(&mem_b[0x200BA8], 8u);   // raw controller latch
}

// Hash full 16MB main RAM EXCLUDING the known frame-deterministic render scratch
// (render-interp/phase 0x8C1F9D8C-98 + stage-anim 0x8C1F9D80 → phys 0x1F9D80..0x1F9DA0).
static uint64_t fullRamHashExclScratch()
{
	const size_t rs = RAM_SIZE;
	const size_t exStart = 0x1F9D80, exEnd = 0x1F9DA0;
	XXH3_state_t* st = XXH3_createState();
	XXH3_64bits_reset(st);
	XXH3_64bits_update(st, &mem_b[0], exStart);
	if (rs > exEnd)
		XXH3_64bits_update(st, &mem_b[exEnd], rs - exEnd);
	uint64_t h = XXH3_64bits_digest(st);
	XXH3_freeState(st);
	return h;
}

// GATE ADD (Test B): continuous per-frame game-state logger. Independent of
// the rollback ring — called every vblank when MAPLECAST_GSHASH_LOG is set.
// Logs the in-RAM game frame counter (0x8C3496B0) + game-state-region hash +
// full-RAM(excl scratch) hash. Two independent frame-keyed input-replay runs
// producing byte-identical logs = forward + input determinism over a full match.
void gshashLogTick(const char* path)
{
	static FILE* f = nullptr;
	static bool  tried = false;
	if (!f) {
		if (tried) return;
		tried = true;
		f = fopen(path, "w");
		if (!f) { printf("[gshash] cannot open %s\n", path); return; }
		printf("[gshash] logging game-state hashes to %s\n", path);
	}
	uint32_t gframe;
	memcpy(&gframe, &mem_b[0x3496B0], 4);
	// Per-subregion localization: 6 char structs (0x5A4 each), game-state page,
	// then 16 × 1MB buckets over full RAM. Lets an offline A/B diff name exactly
	// which region (and thus subsystem/addr range) diverges on a given frame.
	static const uint32_t charBase[6] = {0x268340,0x2688E4,0x268E88,0x26942C,0x2699D0,0x269F74};
	fprintf(f, "%u", gframe);
	for (int c = 0; c < 6; c++)
		fprintf(f, " %016llx",
		        (unsigned long long)XXH3_64bits(&mem_b[charBase[c]], 0x5A4));
	fprintf(f, " %016llx", (unsigned long long)XXH3_64bits(&mem_b[0x289000], 0x1000));
	const size_t bucket = (size_t)RAM_SIZE / 16;
	for (int b = 0; b < 16; b++) {
		size_t off = (size_t)b * bucket;
		size_t len = bucket;
		// zero the render-scratch window inside its bucket by hashing around it
		if (0x1F9D80 >= off && 0x1F9D80 < off + len) {
			uint64_t h1 = XXH3_64bits(&mem_b[off], 0x1F9D80 - off);
			uint64_t h2 = XXH3_64bits(&mem_b[0x1F9DA0], off + len - 0x1F9DA0);
			fprintf(f, " %016llx", (unsigned long long)(h1 ^ h2));
		} else {
			fprintf(f, " %016llx", (unsigned long long)XXH3_64bits(&mem_b[off], len));
		}
	}
	fprintf(f, "\n");
	fflush(f);
}

// ── INPUT-LAG PROBE (MAPLECAST_LAGPROBE=path) ────────────────────────────────
// Deterministic per-frame input driver + raw-field logger for measuring MVC2's
// internal input pipeline lag. Runs every vblank (end-of-frame, after this frame's
// game logic ran). Anchored to the in-RAM game frame counter (0x8C3496B0) so the
// input edge lands on the same guest frame every run.
//
//   MAPLECAST_LAGPROBE       output log path (enables the probe)
//   MAPLECAST_LAGPROBE_TRIG  guest-frames after baseline to begin applying input
//   MAPLECAST_LAGPROBE_MASK  DC key bits to PRESS (active-low: cleared in kcode),
//                            hex. 0 = neutral baseline run (never presses).
//
// kcode[] is only otherwise written by the input server on a UDP packet; with no
// input client connected it stays at our value, so this write is authoritative.
void lagProbeTick(const char* path)
{
	static FILE* f = nullptr;
	static bool  tried = false;
	static bool  haveBase = false;
	static uint32_t baseFrame = 0;
	static uint32_t trig = 0;
	static uint32_t mask = 0;
	if (!f) {
		if (tried) return;
		tried = true;
		const char* te = std::getenv("MAPLECAST_LAGPROBE_TRIG");
		const char* me = std::getenv("MAPLECAST_LAGPROBE_MASK");
		trig = te ? (uint32_t)strtoul(te, nullptr, 0) : 30;
		mask = me ? (uint32_t)strtoul(me, nullptr, 16) : 0;
		f = fopen(path, "w");
		if (!f) { printf("[lagprobe] cannot open %s\n", path); return; }
		printf("[lagprobe] logging to %s  trig=%u mask=0x%x\n", path, trig, mask);
		fprintf(f, "# rel gframe kcode16 posx velx vely fac110 walk1d3 st1d0 xflip1d2 fc142 sid144 in340 in344 in348 in34c structhash\n");
	}
	// P1C1 base = phys 0x268340 (guest 0x8C268340).
	const uint32_t B = 0x268340;
	uint32_t gframe; memcpy(&gframe, &mem_b[0x3496B0], 4);
	if (!haveBase) { haveBase = true; baseFrame = gframe; }
	uint32_t rel = gframe - baseFrame;

	// Apply the scripted input for the NEXT frame's maple poll.
	if (rel >= trig && mask != 0)
		kcode[0] = ((~mask) & 0xFFFFu) | 0xFFFF0000u;
	else
		kcode[0] = 0xFFFFFFFFu;

	float posx, velx, vely;
	memcpy(&posx, &mem_b[B + 0x034], 4);
	memcpy(&velx, &mem_b[B + 0x05c], 4);
	memcpy(&vely, &mem_b[B + 0x060], 4);
	uint8_t  fac110 = mem_b[B + 0x110];
	uint8_t  st1d0  = mem_b[B + 0x1d0];
	uint8_t  xflip1d2 = mem_b[B + 0x1d2];
	uint8_t  walk1d3  = mem_b[B + 0x1d3];
	uint16_t fc142, sid144;
	memcpy(&fc142,  &mem_b[B + 0x142], 2);
	memcpy(&sid144, &mem_b[B + 0x144], 2);
	uint32_t in340, in344, in348, in34c;
	memcpy(&in340, &mem_b[B + 0x340], 4);
	memcpy(&in344, &mem_b[B + 0x344], 4);
	memcpy(&in348, &mem_b[B + 0x348], 4);
	memcpy(&in34c, &mem_b[B + 0x34c], 4);
	uint64_t sh = XXH3_64bits(&mem_b[B], 0x5A4);

	// Raw-input hunt: dump the 4KB page 0x200000..0x201000 as hex so a
	// baseline-vs-pressed diff pins the exact byte the controller poll latches.
	static char gh[0x1000*2+1];
	for (int i = 0; i < 0x1000; i++)
		snprintf(gh + i*2, 3, "%02x", (unsigned)mem_b[0x200000 + i]);

	fprintf(f, "%u %u %04x %.6f %.6f %.6f %u %u %u %u %u %u %08x %08x %08x %08x %016llx %s\n",
	        rel, gframe, (kcode[0] & 0xFFFFu),
	        posx, velx, vely, fac110, walk1d3, st1d0, xflip1d2, fc142, sid144,
	        in340, in344, in348, in34c, (unsigned long long)sh, gh);
	fflush(f);
}

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
	_f1PreGs.assign(depth, 0);   _f1PostGs.assign(depth, 0);
	_f1PreRam.assign(depth, 0);  _f1PostRam.assign(depth, 0);
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
			_f1PreGs[_f1Counter]     = gameStateRegionHash();
			_f1PreRam[_f1Counter]    = fullRamHashExclScratch();
			printf("[rollback-f1] PRE  [%u/%u] saveSeq=%llu blob=%016llx gs=%016llx ram=%016llx\n",
			       _f1Counter + 1, _f1Depth,
			       (unsigned long long)saveSeq,
			       (unsigned long long)_f1PreHashes[_f1Counter],
			       (unsigned long long)_f1PreGs[_f1Counter],
			       (unsigned long long)_f1PreRam[_f1Counter]);
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
			_f1PostGs[_f1Counter]     = gameStateRegionHash();
			_f1PostRam[_f1Counter]    = fullRamHashExclScratch();
			printf("[rollback-f1] POST [%u/%u] saveSeq=%llu blob=%016llx gs=%016llx ram=%016llx\n",
			       _f1Counter + 1, _f1Depth,
			       (unsigned long long)saveSeq,
			       (unsigned long long)_f1PostHashes[_f1Counter],
			       (unsigned long long)_f1PostGs[_f1Counter],
			       (unsigned long long)_f1PostRam[_f1Counter]);
			_f1Counter++;
		}
		if (_f1Counter >= _f1Depth) {
			// POST complete — compare hashes. Track blob / game-state /
			// full-RAM separately so we can classify WHERE any divergence
			// lives (the whole make-or-break for the lockstep pivot).
			bool blobMatch = true, gsMatch = true, ramMatch = true;
			for (uint32_t i = 0; i < _f1Depth; i++) {
				if (_f1PreHashes[i] != _f1PostHashes[i]) {
					printf("[rollback-f1] BLOB  MISMATCH frame %u: pre=%016llx post=%016llx\n",
					       i + 1, (unsigned long long)_f1PreHashes[i],
					       (unsigned long long)_f1PostHashes[i]);
					blobMatch = false;
				}
				if (_f1PreGs[i] != _f1PostGs[i]) {
					printf("[rollback-f1] GSTATE MISMATCH frame %u: pre=%016llx post=%016llx\n",
					       i + 1, (unsigned long long)_f1PreGs[i],
					       (unsigned long long)_f1PostGs[i]);
					gsMatch = false;
				}
				if (_f1PreRam[i] != _f1PostRam[i]) {
					printf("[rollback-f1] RAM    MISMATCH frame %u: pre=%016llx post=%016llx\n",
					       i + 1, (unsigned long long)_f1PreRam[i],
					       (unsigned long long)_f1PostRam[i]);
					ramMatch = false;
				}
			}
			// The lockstep gate passes on GAME-STATE (+full RAM) equality, NOT
			// on full-blob equality (blob includes scheduler-epoch bytes that
			// are execution-invariant).
			_f1Pass = gsMatch && ramMatch;
			_f1Stage = F1_DONE;
			printf("[rollback-f1] === CLASSIFICATION === blob:%s  game-state:%s  full-RAM(excl scratch):%s\n",
			       blobMatch ? "MATCH" : "DIFFER",
			       gsMatch   ? "MATCH" : "DIFFER",
			       ramMatch  ? "MATCH" : "DIFFER");
			printf("[rollback-f1] === LOCKSTEP GATE %s === (game-state+RAM %u/%u frames byte-identical)\n",
			       _f1Pass ? "GO" : "NO-GO", _f1Depth, _f1Depth);
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
