/*
	MapleCast RENDER-REPLICA — PHASE 0 validation experiment.
	See maplecast_replica_phase0.h + docs/RENDER-REPLICA-PLAN.md §4 for the contract.

	This is the cheap go/no-go gate. It re-runs MVC2's own render entry over a frozen
	snapshot with only the GSTA-shipped fields patched, then diffs the re-emitted RAM
	display list against the one the authoritative frame just produced — proving (or
	killing) the claim that the per-frame render read-set ⊆ {GSTA} ∪ {static snapshot}.

	DESIGN (why it is determinism-safe):
	  The experiment is a FULL save → patch → re-render → diff → RESTORE cycle that
	  leaves the authoritative SH4 context AND every touched RAM byte bit-identical.
	  It runs ONLY at the SH4-paused emu-loop boundary (Emulator run loop, right after
	  runInternal() returns) — the same context the rollback deferred-rewind + the
	  Oracle probe-reload use. We drive the render subtree with a SELF-CONTAINED
	  op-step loop over the shared Sh4cntx using the public OpPtr[]/OpDesc[] tables
	  (exactly what Sh4Interpreter::Step does, minus the scheduler / cycle bookkeeping /
	  interrupts), so we never disturb the dynarec block cache and never run the
	  scheduler. A guest exception during the re-render (the expected Tier-2 failure
	  mode) is caught and logged as crashed=1 — never propagated.
*/
#include "maplecast_replica_phase0.h"
#include "hw/sh4/sh4_if.h"          // Sh4cntx, Sh4Context
#include "hw/sh4/sh4_mem.h"         // addrspace::read*/write*, GetMemPtr
#include "hw/sh4/sh4_core.h"        // SH4ThrownException
#include "hw/sh4/sh4_opcode_list.h" // OpPtr[], OpDesc[]
#include "serialize.h"              // Serializer/Deserializer, dc_serialize/dc_deserialize
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cmath>
#include <algorithm>
#include <vector>

namespace maplecast_replica_phase0
{

// ===========================================================================
// Gate. active() is the master env switch; every public entry point checks it
// first and is a literal no-op when off -> prod is byte-stock.
static const bool s_active = (std::getenv("MAPLECAST_REPLICA_PHASE0") != nullptr);
// Tier 2 (change sprite_id, leave the +0x154..+0x184 cluster stale) is the risky
// path — guard it separately and best-effort. When unset, only Tier 1 runs.
static const bool s_tier2  = (std::getenv("MAPLECAST_REPLICA_PHASE0_TIER2") != nullptr);

// THROTTLE. The experiment is expensive on prod (full-machine save/restore + 2-3
// re-renders), so fire it on ~1 in-match frame per N. Default 30. Env override
// MAPLECAST_REPLICA_PHASE0_EVERY (>=1). Clamped to >=1 so 0/garbage means "every
// frame" never happens by accident.
static int parseEvery() {
	const char* e = std::getenv("MAPLECAST_REPLICA_PHASE0_EVERY");
	int v = e ? atoi(e) : 30;
	return v < 1 ? 1 : v;
}
static const int s_every = parseEvery();
// In-match frame counter (incremented every in-match boundary; the experiment
// fires when (counter % s_every) == 0).
static u32 s_inMatchTick = 0;

// SELF-VERIFY the outer determinism guard. When MAPLECAST_REPLICA_PHASE0_VERIFY is
// set, after the full-machine restore we re-serialize the machine and compare it to
// the pre-experiment serialized snapshot, asserting byte-identical, and log
// guard_ok=1/0. Proves the guard holds before any Tier-1 result is trusted.
static const bool s_verify  = (std::getenv("MAPLECAST_REPLICA_PHASE0_VERIFY") != nullptr);

bool active() { return s_active; }

// ===========================================================================
// CONFIRMED addresses (marvelous2 + memory/work.asm + CLAUDE.md). Cited inline.
static const u32 IN_MATCH        = 0x8C289624;   // u8 in_match (CHARQ/Oracle gate)
static const u32 VFRAME          = 0x8C3496B0;   // u32 global frame counter (work.asm)

// GameGlobalPointer (memory/work.asm:1  #symbol GameGlobalPointer 0x8c26823c).
// +0x24 = the per-frame display-list write cursor (bank03 loc_8c030a74:
//   mov.l @(0x24,r2),r1; add r4,r1; mov.l r1,@(0x24,r2)  — read, advance, write).
static const u32 GAME_GLOBAL_PTR = 0x8C26823C;
static const u32 DLIST_CURSOR_OFF = 0x24;        // u32 display-list write cursor

// The render entry. TWO options, build-time selectable:
//   (default) loc_8c0308c2 "Render_sprites" (bank03:1200) — the slot-table walk
//     itself: loads 0x8C2895E0 (slot table) + 0x8C287DE0 (node-ptr array), reads
//     category@node+0x3, dispatches loc_8c03093c (body) / loc_8c030af8 (effect).
//     Smallest hermetic subtree that emits the per-object display lists. Entered
//     via bsr -> ends in rts to pr, so the SENTINEL-pr driver works directly.
//   (RENDER_ENTRY_MASTER) loc_8c0305d8 (bank03:733) — the master render that runs
//     the per-layer setup (loc_8c030410 with layer args 0x0B/05/07/01) and the
//     cursor/template init itself before the walk. More faithful (it does the
//     setup loc_8c0308c2's callers normally do) but a larger subtree (more ops,
//     touches more state). Use this if Render_sprites alone diverges (see §4.4 /
//     the BIGGEST RISK note in the handoff).
#ifdef MAPLECAST_REPLICA_RENDER_ENTRY_MASTER
static const u32 RENDER_ENTRY = 0x8C0305D8;   // loc_8c0305d8 (master render)
#else
static const u32 RENDER_ENTRY = 0x8C0308C2;   // loc_8c0308c2 (Render_sprites)
#endif

// A sentinel return address that is NOT a real code PC and that our driver loop
// stops on. The render entry's terminal `rts` loads pr into pc; when pc reaches
// the sentinel we stop. Must be even (SH4 PCs are halfword-aligned) and outside
// any region the render subtree could legitimately branch to. 0x00000002 is in
// the boot/IP.BIN non-code region (< 0x8C010000) so the subtree never targets it.
static const u32 SENTINEL_PR  = 0x00000002;

// anim-load loc_8c034e8c (bank03:11669): R4=player base, R5=group id, R6=anim id —
// rebuilds current_cell@+0x154 from animations@+0x168 and calls the bank12 cell
// processor loc_8c1294c8. Tier 2 re-runs this after patching sprite_id.
static const u32 ANIM_LOAD     = 0x8C034E8C;

// ---- Per-character struct (memory/pl_mem.asm / CLAUDE.md) ----
static const u32 CHAR_BASE0    = 0x8C268340;
static const u32 CHAR_STRIDE   = 0x5A4;
static const u32 OFF_ACTIVE    = 0x000;   // u8
static const u32 OFF_CHAR_ID   = 0x001;   // u8
static const u32 OFF_POS_X     = 0x034;   // f32  (GSTA)
static const u32 OFF_POS_Y     = 0x038;   // f32  (GSTA)
static const u32 OFF_SCALE_X   = 0x050;   // f32  (GSTA)
static const u32 OFF_SCALE_Y   = 0x054;   // f32  (GSTA)
static const u32 OFF_FACING    = 0x110;   // u8   (GSTA)
static const u32 OFF_FLIP      = 0x130;   // u8   (GSTA xflip copy)
static const u32 OFF_SPRITE_ID = 0x144;   // u16  (GSTA — Tier 2 only)
// anim-load loc_8c034e8c stores GROUP id @ +0x9F (r0=159) and ANIM id @ +0x9E
// (r0-1=158); these are the authoritative current group/anim. We read them back as
// the r5/r6 inputs to re-run loc_8c034e8c in Tier 2b. (NOTE: +0x158 is the
// anim_id/anim_group BYTE the cell-step routine uses per the Oracle struct map; the
// AUTHORITATIVE setter for the anim-load inputs is +0x9F/+0x9E — confirm with an
// Oracle probe before trusting Tier 2b's re-derivation.)
static const u32 OFF_ANIM_GRP  = 0x09F;   // u8   anim-group id (loc_8c034e8c r5 input)
static const u32 OFF_ANIM_ID   = 0x09E;   // u8   anim id        (loc_8c034e8c r6 input)

// ===========================================================================
// SNAPSHOT REGIONS. We snapshot+restore a SUPERSET of everything the render
// subtree reads or writes, so the restore is wholesale (simpler + strictly safer
// than surgical). Each {base,size} is an area-3 RAM window; GetMemPtr returns a
// direct host pointer (&mem_b[addr & RAM_MASK]) for bulk memcpy.
//
// CRITICAL: the wholesale restore of REG_STRUCTS already covers the two
// non-idempotent accumulators the plan names — 0x8C26A974 (the loc_8c030a74
// `fmac` target, bank03 loc_8c030ad4 #data 0x8c26a974) and its sibling 0x8C26A518
// (loc_8c030ad0) — because both fall inside 0x8C268000..0x8C26B000. So restoring
// the region restores them; no surgical accumulator handling is needed.
struct Region { u32 base; u32 size; const char* tag; };
static const Region REGIONS[] = {
	// Char/object structs page: 6 char structs (base 0x8C268340 stride 0x5A4),
	// object pool, the quad-count/ptr tables 0x8C26AA24/0x8C26AA34, the
	// non-idempotent accumulators 0x8C26A974 / 0x8C26A518. Also the slot table's
	// node-ptr array 0x8C287DE0 is FURTHER up — covered by SLOT_TABLE below.
	{ 0x8C268000, 0x3000, "char_obj" },        // 0x8C268000..0x8C26B000
	// Slot table (16 x 0x180) + the x4 node-ptr array.
	{ 0x8C2895E0, 0x1800, "slot_table" },       // covers 0x8C2895E0 + a margin
	{ 0x8C287DE0, 0x0400, "node_ptr_arr" },     // 0x8C287DE0 x4 node-ptr array
	// GameGlobalPointer block (reads +0x24 cursor, +0x2E, +0x8E, +0x98, ...).
	{ 0x8C26823C, 0x2000, "game_global" },
	// Template scratch tables (bank03/bank12 cell-output/vertex templates).
	{ 0x8C1F9D80, 0x0400, "tmpl_9d80" },        // 0x8C1F9D80..0x8C1FA000 + 0x8C1F9F9C
};
static const int NUM_REGIONS = (int)(sizeof(REGIONS) / sizeof(REGIONS[0]));

// The display-list output buffer to capture for the diff. The draw routines write
// a RAM display list starting at the cursor base; we reset the cursor to a known
// base before the re-render and capture [base, base+len) afterwards. The base is
// the cursor value at the snapshot point (the value the authoritative frame's
// finalize left it at the START of its own walk is what we want to mimic — see the
// cursor-reset note below). DL_CAP bounds the capture (the body display list is
// well under this).
static const u32 DL_CAP = 256 * 1024;   // 256 KiB capture window

// The DISPLAY-LIST OUTPUT window is a DYNAMIC region: the draw routines write the
// per-frame display list to the address the cursor (GameGlobalPointer+0x24) holds,
// which is OUTSIDE the static regions above (KB: ~0x0C56xxxx). It must be
// snapshotted+restored too, or the re-render's writes there would NOT be reverted →
// determinism break. We resolve its base at the boundary (the cursor value) and
// treat [base, base+DL_CAP) as one extra region appended to the snapshot set. The
// area-mask normalizes the P0/P1 alias to area-3 RAM for GetMemPtr.
static u32 s_dlBaseRaw  = 0;          // cursor value as read (may be P0 0x0C..)
static u32 s_dlBaseRam  = 0;          // area-3 normalized (0x8C..) for GetMemPtr/rd/wr
static std::vector<uint8_t> s_dlStatic;   // frozen display-list window (frame N)
static std::vector<uint8_t> s_dlLive;     // this frame's display-list window

// ===========================================================================
// Side buffers (persistent across calls; sized once).
//   s_staticSnap : the FROZEN static regions, captured ONCE at the first in-match
//                  frame (frame N). The replica re-projects this frozen pose to
//                  each later frame's GSTA — that is the actual hypothesis under
//                  test (read-set ⊆ {GSTA} ∪ {static}). NOT re-taken per frame.
//   s_liveSnap   : the CURRENT frame's regions, captured every boundary so we can
//                  (a) re-render them to get THIS frame's authoritative baseline
//                  list and (b) restore the authoritative guest bit-for-bit.
static std::vector<uint8_t> s_staticSnap[NUM_REGIONS];
static std::vector<uint8_t> s_liveSnap[NUM_REGIONS];
static bool                 s_staticReady = false;   // frame-N snapshot taken?
static Sh4Context           s_ctxSnap;               // full authoritative ctx snapshot
static bool                 s_buffersReady = false;

// ---- OUTER FULL-MACHINE determinism guard (prod-safety requirement) ----
// Independent of the per-region snapshot above. Before the FIRST re-render of a
// boundary we serialize the ENTIRE machine (dc_serialize: SH4 ctx + ALL of RAM +
// VRAM + PVR regs + every device — including the un-enumerated bank12 transform/
// matrix scratch at 0x8C2D6xxx that the REGIONS[] set deliberately does NOT cover)
// into s_fullSnap, and after the LAST re-render we deserialize it back, guaranteeing
// the authoritative guest is bit-identical no matter what the render subtree wrote.
// 40 MiB matches the MAPLECAST_SELFTEST_DESERIALIZE buffers in Emulator::vblank()
// (emulator.cpp:1773) — a full dc_serialize blob is ~10-12 MiB; 40 MiB is headroom.
static const size_t         FULL_SNAP_CAP = 40 * 1024 * 1024;
static std::vector<uint8_t> s_fullSnap;          // pre-experiment full-machine blob
static size_t               s_fullSnapSize = 0;  // valid bytes in s_fullSnap
static std::vector<uint8_t> s_verifySnap;        // post-restore re-serialize (VERIFY only)

// The latched GSTA field values for the just-completed frame (read on the render
// thread in onServerPublish, applied on the SH4 thread in runAtBoundary).
struct GstaFields {
	bool  active;
	u8    character_id;
	float pos_x, pos_y, scale_x, scale_y;
	u8    facing, flip;
	u16   sprite_id;
	u8    anim_grp, anim_id;
};
static GstaFields s_gsta[6];
static u32        s_pendingFrame = 0;
static volatile bool s_runPending = false;

// ===========================================================================
// Small read/write helpers (area-aware, same path the engine uses).
static inline u8  rd8 (u32 a) { return addrspace::read8(a); }
static inline u16 rd16(u32 a) { return addrspace::read16(a); }
static inline u32 rd32(u32 a) { return addrspace::read32(a); }
static inline void wr8 (u32 a, u8  v) { addrspace::write8(a, v); }
static inline void wr16(u32 a, u16 v) { addrspace::write16(a, v); }
static inline void wr32(u32 a, u32 v) { addrspace::write32(a, v); }
static inline float rdF(u32 a) { u32 r = addrspace::read32(a); float f; memcpy(&f, &r, 4); return f; }
static inline void  wrF(u32 a, float f) { u32 r; memcpy(&r, &f, 4); addrspace::write32(a, r); }

// ===========================================================================
// JSONL output (append + fflush + rolling-tail, same discipline as mc_probe.log).
static FILE*  s_log = nullptr;
static bool   s_logInit = false;
static long   s_logBytes = 0;
static const long LOG_CAP = 16L * 1024 * 1024;
static const char* logPath() {
	const char* p = std::getenv("MAPLECAST_REPLICA_PHASE0_LOG");
	return p ? p : "/dev/shm/mc_replica_phase0.jsonl";
}
static void logLine(const char* fmt, ...) {
	if (!s_logInit) {
		s_logInit = true;
		s_log = fopen(logPath(), "w");
		s_logBytes = 0;
	}
	if (!s_log) return;
	if (s_logBytes > LOG_CAP) { fseek(s_log, 0, SEEK_SET); s_logBytes = 0; }
	va_list ap; va_start(ap, fmt);
	int n = vfprintf(s_log, fmt, ap);
	va_end(ap);
	if (n > 0) s_logBytes += n;
	fflush(s_log);
}

// ===========================================================================
// SELF-CONTAINED render driver. Mirrors Sh4Interpreter::Step()'s op fetch+execute
// (sh4_interpreter.cpp:21-39,95-110) WITHOUT the scheduler / cycle bookkeeping /
// interrupt poll, so it runs only the render subtree. Returns the EXPEVT code on a
// guest exception (>0), 0 on clean rts-to-sentinel, or 0xFFFFFFFF on op-cap.
// *outOps = SH4 ops executed (for the Phase-5 perf estimate). READ side effects on
// Sh4cntx are fully reverted by the caller's ctx restore.
static u32 driveRender(u32 entry, u32 sentinel, u64 opCap, u64* outOps) {
	Sh4Context* c = &Sh4cntx;
	c->pc = entry;
	c->pr = sentinel;
	u64 ops = 0;
	u32 result = 0xFFFFFFFF;   // assume op-cap unless we hit the sentinel / fault
	try {
		while (ops < opCap) {
			u32 pc = c->pc;
			if (pc == sentinel) { result = 0; break; }    // clean return
			// Halfword-aligned fetch (address error mirrors ReadNexOp()).
			if (pc & 1) { result = (u32)Sh4Ex_AddressErrorRead; break; }
			u16 op = IReadMem16(pc);
			c->pc = pc + 2;
			// FPU-disabled guard (Sh4Interpreter::ExecuteOpcode). MVC2 render is
			// FP-heavy; if SR.FD is set this would fault — same as the interpreter.
			if (c->sr.FD == 1 && OpDesc[op]->IsFloatingPoint()) { result = (u32)Sh4Ex_FpuDisabled; break; }
			OpPtr[op](c, op);
			ops++;
		}
	} catch (const SH4ThrownException& ex) {
		result = (u32)ex.expEvn;
	} catch (...) {
		result = 0xFFFFFFFE;   // non-SH4 host exception (should not happen)
	}
	*outOps = ops;
	return result;
}

// ===========================================================================
// Snapshot / restore the RAM regions + the full ctx. Bulk memcpy via GetMemPtr.
static void ensureBuffers() {
	if (s_buffersReady) return;
	for (int i = 0; i < NUM_REGIONS; i++) {
		s_staticSnap[i].resize(REGIONS[i].size);
		s_liveSnap[i].resize(REGIONS[i].size);
	}
	s_dlStatic.resize(DL_CAP);
	s_dlLive.resize(DL_CAP);
	s_fullSnap.resize(FULL_SNAP_CAP);
	if (s_verify) s_verifySnap.resize(FULL_SNAP_CAP);
	s_buffersReady = true;
}

// ---- OUTER FULL-MACHINE save/restore (the prod-safety guard). Wraps the whole
// re-render experiment. Uses the codebase's existing full serialize entry points
// (core/serialize.h dc_serialize/dc_deserialize), the SAME round-trip the
// MAPLECAST_SELFTEST_DESERIALIZE audit in Emulator::vblank() exercises
// (emulator.cpp:1775-1779). Save runs ONCE before the first re-render; restore runs
// ONCE after the last. Both run at the SH4-paused boundary (no scheduler/SH4 race).
static void fullMachineSave() {
	Serializer ser(s_fullSnap.data(), s_fullSnap.size(), false);
	dc_serialize(ser);                 // SH4 ctx + RAM + VRAM + PVR + all devices
	s_fullSnapSize = ser.size();
}
static void fullMachineRestore() {
	Deserializer deser(s_fullSnap.data(), s_fullSnapSize, false);
	dc_deserialize(deser);             // exact inverse — bit-identical machine
}
// Copy the live regions (the NUM_REGIONS static set + the dynamic display-list
// window at s_dlBaseRam) INTO the destination buffers (snapshot). `dlDst` is the
// matching display-list buffer.
static void snapshotInto(std::vector<uint8_t>* dst, std::vector<uint8_t>& dlDst) {
	for (int i = 0; i < NUM_REGIONS; i++) {
		u8* p = GetMemPtr(REGIONS[i].base, REGIONS[i].size);
		if (p) memcpy(dst[i].data(), p, REGIONS[i].size);
		else   memset(dst[i].data(), 0, REGIONS[i].size);
	}
	u8* dp = s_dlBaseRam ? GetMemPtr(s_dlBaseRam, DL_CAP) : nullptr;
	if (dp) memcpy(dlDst.data(), dp, DL_CAP);
	else    memset(dlDst.data(), 0, DL_CAP);
}
// Copy the source buffers back into the live regions + the display-list window
// (restore).
static void restoreFrom(const std::vector<uint8_t>* src, const std::vector<uint8_t>& dlSrc) {
	for (int i = 0; i < NUM_REGIONS; i++) {
		u8* p = GetMemPtr(REGIONS[i].base, REGIONS[i].size);
		if (p) memcpy(p, src[i].data(), REGIONS[i].size);
	}
	u8* dp = s_dlBaseRam ? GetMemPtr(s_dlBaseRam, DL_CAP) : nullptr;
	if (dp) memcpy(dp, dlSrc.data(), DL_CAP);
}

// ===========================================================================
// Latch the GSTA-shipped fields from the 6 live char structs (read-only). Called
// from the render thread in onServerPublish — must NOT drive the SH4.
static void latchGsta() {
	for (int i = 0; i < 6; i++) {
		u32 base = CHAR_BASE0 + (u32)i * CHAR_STRIDE;
		GstaFields& g = s_gsta[i];
		g.active       = rd8(base + OFF_ACTIVE) != 0;
		g.character_id = rd8(base + OFF_CHAR_ID);
		g.pos_x        = rdF(base + OFF_POS_X);
		g.pos_y        = rdF(base + OFF_POS_Y);
		g.scale_x      = rdF(base + OFF_SCALE_X);
		g.scale_y      = rdF(base + OFF_SCALE_Y);
		g.facing       = rd8(base + OFF_FACING);
		g.flip         = rd8(base + OFF_FLIP);
		g.sprite_id    = rd16(base + OFF_SPRITE_ID);
		g.anim_grp     = rd8(base + OFF_ANIM_GRP);
		g.anim_id      = rd8(base + OFF_ANIM_ID);
	}
}

// Apply the latched GSTA fields onto the (already snapshot-restored) char structs.
// `withSprite` controls Tier 1 (false: leave sprite_id at snapshot) vs Tier 2
// (true: patch sprite_id, leaving the +0x154..+0x184 cluster stale).
static void applyGsta(bool withSprite) {
	for (int i = 0; i < 6; i++) {
		if (!s_gsta[i].active) continue;
		u32 base = CHAR_BASE0 + (u32)i * CHAR_STRIDE;
		wrF(base + OFF_POS_X,   s_gsta[i].pos_x);
		wrF(base + OFF_POS_Y,   s_gsta[i].pos_y);
		wrF(base + OFF_SCALE_X, s_gsta[i].scale_x);
		wrF(base + OFF_SCALE_Y, s_gsta[i].scale_y);
		wr8(base + OFF_FACING,  s_gsta[i].facing);
		wr8(base + OFF_FLIP,    s_gsta[i].flip);
		if (withSprite) wr16(base + OFF_SPRITE_ID, s_gsta[i].sprite_id);
	}
}

// Capture the RAM display list the re-render just emitted: [base, postCursor),
// where `base` is the cursor value we seeded before the render (s_dlBaseRaw) and
// postCursor is the cursor value after it (the routine advances +0x24 per object).
// len is clamped to DL_CAP. Reads via the raw (possibly-P0) base — addrspace
// normalizes the alias.
static void captureDList(u32 base, std::vector<uint8_t>& out) {
	u32 endCur = rd32(GAME_GLOBAL_PTR + DLIST_CURSOR_OFF);
	u32 len = 0;
	if (endCur > base && (endCur - base) <= DL_CAP) len = endCur - base;
	else if (endCur > base) len = DL_CAP;          // overflow guard
	out.resize(len);
	for (u32 i = 0; i < len; i++) out[i] = rd8(base + i);
}

// One full re-render pass (snapshot already taken, regions already restored to
// snapshot). Returns the EXPEVT result of driveRender; fills the diff stats.
struct PassResult { u32 expevt; u64 ops; u32 dlistLen; u32 mismatchBytes; u32 maxByteDelta; };
static PassResult onePass(bool withSprite, const std::vector<uint8_t>& liveDList, u32 dlistBase, u64 opCap) {
	PassResult r{}; r.expevt = 0; r.ops = 0; r.dlistLen = 0; r.mismatchBytes = 0; r.maxByteDelta = 0;
	// Restore the FROZEN static snapshot (frame N) + the frozen display-list window,
	// then patch this frame's GSTA.
	restoreFrom(s_staticSnap, s_dlStatic);
	applyGsta(withSprite);
	// Reset the display-list write cursor to the captured base so the re-render
	// writes from the same origin (bank03 loc_8c030a74 advances GameGlobalPointer
	// +0x24 per object; we seed it to the frame's start-of-walk value).
	wr32(GAME_GLOBAL_PTR + DLIST_CURSOR_OFF, dlistBase);
	u64 ops = 0;
	r.expevt = driveRender(RENDER_ENTRY, SENTINEL_PR, opCap, &ops);
	r.ops = ops;
	std::vector<uint8_t> reDList;
	captureDList(dlistBase, reDList);
	r.dlistLen = (u32)reDList.size();
	// Byte-for-byte diff vs the live display list.
	u32 n = (u32)std::min(reDList.size(), liveDList.size());
	for (u32 i = 0; i < n; i++) {
		if (reDList[i] != liveDList[i]) {
			r.mismatchBytes++;
			u32 d = (u32)abs((int)reDList[i] - (int)liveDList[i]);
			if (d > r.maxByteDelta) r.maxByteDelta = d;
		}
	}
	r.mismatchBytes += (u32)(std::max(reDList.size(), liveDList.size()) - n); // length diff
	return r;
}

// ===========================================================================
// PUBLIC ENTRY POINTS
// ---------------------------------------------------------------------------
void onServerPublish(void* /*liveCtx*/, u32 frame) {
	if (!s_active) return;
	if (s_runPending) return;                 // a run is already queued
	if (rd8(IN_MATCH) == 0) return;           // gate on in-match (CHARQ/Oracle)
	latchGsta();                              // read-only snapshot of GSTA fields
	s_pendingFrame = frame;
	s_runPending = true;                      // emu-loop boundary will pick it up
}

bool runPending() {
	if (!s_active) return false;
	return s_runPending;
}

bool runAtBoundary() {
	if (!s_active || !s_runPending) return false;
	s_runPending = false;                     // consume

	// THROTTLE: fire the experiment on ~1 in-match frame per s_every. We still
	// reached the boundary (the Stop()/runInternal()/boundary dance already paid),
	// but we skip the expensive full save/restore + re-renders on the off-frames.
	// Cheap early-out: increment the in-match tick and bail unless it's our turn.
	if ((s_inMatchTick++ % (u32)s_every) != 0)
		return false;

	ensureBuffers();

	// --- Resolve the display-list output window FIRST (the snapshots include it).
	// The cursor (GameGlobalPointer+0x24) holds the absolute write address the draw
	// routines emit the per-frame list to (KB: ~0x0C56xxxx, OUTSIDE the static
	// regions). dlistBase is the origin BOTH the baseline render and the patched
	// renders reset to. (BIGGEST-RISK note: if the authoritative per-frame list
	// origin differs from this boundary value, switch to the MASTER entry which seeds
	// the cursor itself.) ---
	u32 dlistBase = rd32(GAME_GLOBAL_PTR + DLIST_CURSOR_OFF);
	s_dlBaseRaw = dlistBase;
	s_dlBaseRam = (dlistBase & 0x1FFFFFFF) | 0x80000000;   // area-3 normalized for GetMemPtr

	// --- SAVE the authoritative state so the whole experiment is reversible: the
	// full ctx + a LIVE snapshot of THIS frame's RAM regions + the display-list window
	// (s_liveSnap / s_dlLive). This is what we restore the authoritative guest from at
	// the end, and what we re-render to obtain THIS frame's authoritative baseline. ---
	memcpy(&s_ctxSnap, &Sh4cntx, sizeof(Sh4Context));
	snapshotInto(s_liveSnap, s_dlLive);

	// --- OUTER GUARD (prod-safety): full-machine save BEFORE the first re-render.
	// The per-region snapshot above drives the experiment's own diff + restore, but
	// it CANNOT cover state the render subtree touches outside REGIONS[] (the
	// un-enumerated bank12 transform/matrix scratch at 0x8C2D6xxx, VRAM, PVR regs).
	// This serializes the WHOLE machine so the restore at the end is guaranteed
	// bit-identical regardless of what any re-render wrote. ---
	fullMachineSave();

	// --- Take the FROZEN static snapshot ONCE, at the first in-match frame (frame N).
	// Every later frame re-projects this frozen pose to its own GSTA — which is the
	// actual hypothesis (read-set ⊆ {GSTA} ∪ {static}). If we re-snapshotted each
	// frame, Tier 1 would be a trivial no-op patch (the snapshot already holds the
	// frame's own pos/scale/facing) and prove nothing. ---
	if (!s_staticReady) {
		snapshotInto(s_staticSnap, s_dlStatic);
		s_staticReady = true;
		logLine("{\"frame\":%u,\"event\":\"static_snapshot_taken\",\"dlist_base\":\"0x%08X\"}\n",
		        s_pendingFrame, s_dlBaseRaw);
	}

	// --- BASELINE: re-render THIS frame's LIVE RAM (already in place) with no patch,
	// to get the authoritative-equivalent display list. The real frame's list may be
	// consumed/overwritten by downstream passes by the time we reach this boundary,
	// so we reproduce it from the same inputs (same RAM) — bit-identical to what the
	// authoritative SH4 produced this frame. This is the (a) Authoritative reference
	// (§4.2 step 2). ---
	std::vector<uint8_t> liveDList;
	{
		wr32(GAME_GLOBAL_PTR + DLIST_CURSOR_OFF, dlistBase);
		u64 ops = 0;
		u32 expevt = driveRender(RENDER_ENTRY, SENTINEL_PR, 4 * 1024 * 1024, &ops);
		captureDList(dlistBase, liveDList);
		// If the baseline itself faults, the frame is inconclusive — log + bail
		// (after restoring ctx + live RAM).
		if (expevt != 0) {
			logLine("{\"frame\":%u,\"tier\":0,\"baseline_fault\":\"0x%03X\",\"ops\":%llu}\n",
			        s_pendingFrame, expevt, (unsigned long long)ops);
			// OUTER GUARD restore (covers everything, incl. whatever the faulting
			// baseline re-render touched outside REGIONS[]). The per-region restore
			// below is now redundant for correctness but kept harmless/cheap.
			fullMachineRestore();
			memcpy(&Sh4cntx, &s_ctxSnap, sizeof(Sh4Context));
			restoreFrom(s_liveSnap, s_dlLive);
			return false;
		}
	}

	// --- TIER 1: restore the FROZEN static snapshot (onePass does this), patch ONLY
	// pos/scale/facing/flip from THIS frame's GSTA (sprite_id at the snapshot pose),
	// re-render, diff vs the baseline. EXPECT byte-identical IF the read-set is closed
	// over GSTA position/scale/facing for a re-projection. ---
	{
		PassResult t1 = onePass(/*withSprite=*/false, liveDList, dlistBase, 4 * 1024 * 1024);
		bool crashed = (t1.expevt != 0);
		logLine("{\"frame\":%u,\"tier\":1,\"matched\":%s,\"mismatched_bytes\":%u,"
		        "\"max_byte_delta\":%u,\"dlist_len\":%u,\"baseline_len\":%u,"
		        "\"sh4_ops\":%llu,\"crashed\":%d,\"expevt\":\"0x%03X\"}\n",
		        s_pendingFrame,
		        (!crashed && t1.mismatchBytes == 0) ? "true" : "false",
		        t1.mismatchBytes, t1.maxByteDelta, t1.dlistLen, (u32)liveDList.size(),
		        (unsigned long long)t1.ops, crashed ? 1 : 0, t1.expevt);
	}

	// --- TIER 2 (best-effort, guarded): also patch sprite_id, leave the
	// +0x154..+0x184 cluster stale. EXPECT mismatch/crash (proves the cluster is on
	// the per-frame read path). Then re-run loc_8c034e8c per active char to rebuild
	// +0x154/+0x160 and re-test → EXPECT match. A guest fault is caught inside
	// driveRender (logged crashed=1), never fatal to prod. ---
	if (s_tier2) {
		// 2a: stale-cluster pose change (onePass restores the static snapshot first).
		PassResult t2a = onePass(/*withSprite=*/true, liveDList, dlistBase, 4 * 1024 * 1024);
		bool crashed2a = (t2a.expevt != 0);
		logLine("{\"frame\":%u,\"tier\":2,\"phase\":\"stale\",\"matched\":%s,"
		        "\"mismatched_bytes\":%u,\"max_byte_delta\":%u,\"dlist_len\":%u,"
		        "\"sh4_ops\":%llu,\"crashed\":%d,\"expevt\":\"0x%03X\"}\n",
		        s_pendingFrame,
		        (!crashed2a && t2a.mismatchBytes == 0) ? "true" : "false",
		        t2a.mismatchBytes, t2a.maxByteDelta, t2a.dlistLen,
		        (unsigned long long)t2a.ops, crashed2a ? 1 : 0, t2a.expevt);

		// 2b: rebuild the cluster via the single anim-load call, then re-render.
		restoreFrom(s_staticSnap, s_dlStatic);
		applyGsta(/*withSprite=*/true);
		// Re-run loc_8c034e8c(R4=base, R5=anim_grp, R6=anim_id) for each active
		// char to recompute current_cell@+0x154 from the patched sprite_id/group.
		// Wrapped in driveRender's try/catch via a tiny per-call driver below.
		u32 animFault = 0; u64 animOps = 0;
		for (int i = 0; i < 6 && animFault == 0; i++) {
			if (!s_gsta[i].active) continue;
			u32 base = CHAR_BASE0 + (u32)i * CHAR_STRIDE;
			Sh4Context* c = &Sh4cntx;
			c->r[4] = base;
			c->r[5] = s_gsta[i].anim_grp;
			c->r[6] = s_gsta[i].anim_id;
			u64 ops = 0;
			animFault = driveRender(ANIM_LOAD, SENTINEL_PR, 1024 * 1024, &ops);
			animOps += ops;
		}
		PassResult t2b{}; t2b.expevt = animFault;
		if (animFault == 0) {
			wr32(GAME_GLOBAL_PTR + DLIST_CURSOR_OFF, dlistBase);
			u64 rops = 0;
			t2b.expevt = driveRender(RENDER_ENTRY, SENTINEL_PR, 4 * 1024 * 1024, &rops);
			t2b.ops = rops;
			std::vector<uint8_t> reD; captureDList(dlistBase, reD);
			t2b.dlistLen = (u32)reD.size();
			u32 n = (u32)std::min(reD.size(), liveDList.size());
			for (u32 k = 0; k < n; k++) if (reD[k] != liveDList[k]) {
				t2b.mismatchBytes++;
				u32 d = (u32)abs((int)reD[k] - (int)liveDList[k]);
				if (d > t2b.maxByteDelta) t2b.maxByteDelta = d;
			}
			t2b.mismatchBytes += (u32)(std::max(reD.size(), liveDList.size()) - n);
		}
		bool crashed2b = (t2b.expevt != 0);
		logLine("{\"frame\":%u,\"tier\":2,\"phase\":\"animload\",\"matched\":%s,"
		        "\"mismatched_bytes\":%u,\"max_byte_delta\":%u,\"dlist_len\":%u,"
		        "\"anim_ops\":%llu,\"render_ops\":%llu,\"crashed\":%d,\"expevt\":\"0x%03X\"}\n",
		        s_pendingFrame,
		        (!crashed2b && t2b.mismatchBytes == 0) ? "true" : "false",
		        t2b.mismatchBytes, t2b.maxByteDelta, t2b.dlistLen,
		        (unsigned long long)animOps, (unsigned long long)t2b.ops,
		        crashed2b ? 1 : 0, t2b.expevt);
	}

	// --- RESTORE (OUTER GUARD): deserialize the full-machine snapshot taken before
	// the first re-render. This is the AUTHORITATIVE revert — it reverts EVERY byte
	// any re-render wrote, including state outside REGIONS[] (bank12 0x8C2D6xxx
	// transform/matrix scratch, VRAM, PVR regs). After this the live game state is
	// byte-identical to before the experiment, satisfying the prod-safety contract.
	fullMachineRestore();
	// The per-region restore below is now strictly redundant (the full deserialize
	// already restored these bytes), but it is cheap and harmless — kept so the
	// experiment's own bookkeeping (s_liveSnap / s_dlLive / s_ctxSnap) stays self-
	// consistent even if dc_deserialize's coverage is ever narrowed.
	memcpy(&Sh4cntx, &s_ctxSnap, sizeof(Sh4Context));
	restoreFrom(s_liveSnap, s_dlLive);

	// --- SELF-VERIFY the guard (MAPLECAST_REPLICA_PHASE0_VERIFY). Re-serialize the
	// restored machine and compare to the pre-experiment blob byte-for-byte; assert
	// the guard held. Logged as guard_ok=1/0 so the FIRST captures prove determinism
	// before any Tier-1 result is trusted. Off by default (re-serialize is ~10 MiB). ---
	if (s_verify) {
		Serializer ser(s_verifySnap.data(), s_verifySnap.size(), false);
		dc_serialize(ser);
		size_t postSize = ser.size();
		uint64_t diffBytes = 0;
		size_t common = std::min(postSize, s_fullSnapSize);
		for (size_t i = 0; i < common; i++)
			if (s_verifySnap[i] != s_fullSnap[i]) diffBytes++;
		bool sizeMatch = (postSize == s_fullSnapSize);
		bool guardOk = (diffBytes == 0) && sizeMatch;
		logLine("{\"frame\":%u,\"event\":\"guard_verify\",\"guard_ok\":%d,"
		        "\"diff_bytes\":%llu,\"pre_size\":%zu,\"post_size\":%zu}\n",
		        s_pendingFrame, guardOk ? 1 : 0,
		        (unsigned long long)diffBytes, s_fullSnapSize, postSize);
	}

	// We drove the render on the self-contained interpreter loop over Sh4cntx and
	// compiled NO dynarec blocks, so the dynarec block cache is untouched -> no
	// ResetCache() needed (return false, matching mc_probeApplyReload's contract).
	return false;
}

} // namespace maplecast_replica_phase0
