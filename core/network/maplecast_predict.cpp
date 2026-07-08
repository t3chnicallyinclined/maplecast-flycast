/*
	MapleCast Predict — STAGE 1 implementation: the no-render re-sim primitive
	and its standalone byte-exact gate. See maplecast_predict.h.
*/
#include "types.h"
#include "maplecast_predict.h"
#include "maplecast_rollback.h"        // captureFrameToBlob/restoreFromBlob, gameStateRegionHash
#include "emulator.h"                  // emu, getSh4Executor
#include "hw/sh4/sh4_if.h"             // Sh4Executor Run/Start/Stop
#include "hw/pvr/Renderer_if.h"        // rend_enable_renderer / rend_is_enabled
#include "serialize.h"                 // dc_serialize + dc_audit_marks (region diff)

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>

#include "maplecast_compat.h"          // clock_gettime on Windows

extern u32 kcode[4];
extern u16 rt[4], lt[4];

namespace maplecast_predict
{

static inline int64_t nowUs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

bool active()
{
	static int cached = -1;
	if (cached < 0) {
		const char* e = std::getenv("MAPLECAST_PREDICT");
		cached = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
	}
	return cached == 1;
}

// ── the headless-advance flag ─────────────────────────────────────────
static std::atomic<bool> g_headless{false};
bool headlessAdvanceActive() { return g_headless.load(std::memory_order_relaxed); }

// Run exactly one game-frame. `headless`: no GPU render (QueueRender skips) and
// the SH4 stops at the non-RTT STARTRENDER via the headless hook in
// rend_start_render; else a normal rendered frame (present()->Stop returns).
// scheduleRenderDone fires in BOTH (Renderer_if.cpp:600, before the skip), so
// the RENDER_DONE interrupt timing — hence SH4 execution — is identical.
static void runOneFrame(bool headless)
{
	g_headless.store(headless, std::memory_order_relaxed);
	rend_enable_renderer(!headless);
	emu.getSh4Executor()->Start();   // re-arm CpuRunning (Run() clears it on Stop)
	emu.getSh4Executor()->Run();     // returns after one game-frame
}

int64_t advanceHeadless(int frames)
{
	const bool prevEnabled = rend_is_enabled();
	const int64_t t0 = nowUs();
	for (int i = 0; i < frames; i++)
		runOneFrame(/*headless=*/true);
	const int64_t dt = nowUs() - t0;
	g_headless.store(false, std::memory_order_relaxed);
	rend_enable_renderer(prevEnabled);
	return dt;
}

// ── standalone primitive gate (MAPLECAST_PREDICT_TEST=W,K) ────────────
//
// From a warmed-up live frame T: capture the state, run K frames HEADLESS
// (no render) -> hash_headless, restore T, run the SAME K frames NORMALLY
// (rendered) -> hash_normal, assert byte-identical. Held input is snapshotted
// and re-applied each frame in both runs so the two K-runs see identical input.

enum Stage { IDLE, WARMUP, DONE };
static Stage    s_stage = IDLE;
static bool     s_configured = false;
static uint32_t s_warmup = 0, s_K = 0;
static bool     s_pass = false;
static std::vector<uint8_t> s_blob;   // anchor state
static int32_t  s_jitter = 0;

static void configure()
{
	if (s_configured) return;
	s_configured = true;
	const char* e = std::getenv("MAPLECAST_PREDICT_TEST");
	if (!e || !*e) return;
	unsigned w = 0, k = 0;
	if (sscanf(e, "%u,%u", &w, &k) != 2 || k == 0) {
		printf("[predict-gate] malformed MAPLECAST_PREDICT_TEST='%s' (want W,K)\n", e);
		return;
	}
	s_warmup = w; s_K = k;
	try { s_blob.resize(40u * 1024u * 1024u); } catch (...) {
		printf("[predict-gate] blob alloc failed\n"); return;
	}
	s_stage = WARMUP;
	printf("[predict-gate] armed: warmup=%u K=%u\n", w, k);
}

struct HeldInput { u32 kc[4]; u16 lt_[4]; u16 rt_[4]; };
static HeldInput snapInput()
{
	HeldInput h;
	for (int i = 0; i < 4; i++) { h.kc[i] = kcode[i]; h.lt_[i] = lt[i]; h.rt_[i] = rt[i]; }
	return h;
}
static void applyInput(const HeldInput& h)
{
	for (int i = 0; i < 4; i++) { kcode[i] = h.kc[i]; lt[i] = h.lt_[i]; rt[i] = h.rt_[i]; }
}

static void runGate()
{
	using namespace maplecast_rollback;

	// Snapshot the held input so both K-runs see identical input every frame.
	const HeldInput held = snapInput();

	// Mute audio (fastForwardMode) for BOTH K-runs so AICA output/pacing is an
	// identical held variable — isolating the ONE thing we're testing (render
	// on vs off) and letting the SH4 run at full dynarec speed (no 60fps audio
	// block => the headless re-sim is sub-ms as required).
	const bool prevFF = settings.input.fastForwardMode;
	settings.input.fastForwardMode = true;

	// Anchor the current state (frame T).
	int32_t jitter = 0;
	const size_t n = captureFrameToBlob(s_blob.data(), s_blob.size(), jitter);
	if (n == 0) { printf("[predict-gate] capture failed\n"); s_stage = DONE; return; }
	s_jitter = jitter;
	const uint64_t hAnchor = gameStateRegionHash();

	// ── HEADLESS: K frames, no render ──
	g_headless.store(true, std::memory_order_relaxed);
	const bool prevEnabled = rend_is_enabled();
	const int64_t t0 = nowUs();
	for (uint32_t i = 0; i < s_K; i++) { applyInput(held); runOneFrame(true); }
	const int64_t headlessUs = nowUs() - t0;
	g_headless.store(false, std::memory_order_relaxed);
	rend_enable_renderer(prevEnabled);
	const uint64_t hHeadless = gameStateRegionHash();

	// ── restore the anchor ──
	if (!restoreFromBlob(s_blob.data(), n, s_jitter)) {
		printf("[predict-gate] restore failed\n"); s_stage = DONE; return;
	}
	const uint64_t hAnchor2 = gameStateRegionHash();
	const bool restoreOk = (hAnchor2 == hAnchor);

	// ── NORMAL: same K frames, rendered ──
	const int64_t t1 = nowUs();
	for (uint32_t i = 0; i < s_K; i++) { applyInput(held); runOneFrame(false); }
	const int64_t normalUs = nowUs() - t1;
	const uint64_t hNormal = gameStateRegionHash();

	// Restore the anchor a final time so the live client resumes from T (=
	// localFrame). The gate is then non-destructive to the lockstep timeline.
	restoreFromBlob(s_blob.data(), n, s_jitter);
	settings.input.fastForwardMode = prevFF;

	s_pass = restoreOk && (hHeadless == hNormal);
	printf("[predict-gate] anchor=%016llx restore=%016llx(%s)\n",
	       (unsigned long long)hAnchor, (unsigned long long)hAnchor2, restoreOk ? "OK" : "BAD");
	printf("[predict-gate] K=%u  headless=%016llx (%lld us, %.1f us/frame)  "
	       "normal=%016llx (%lld us)\n",
	       s_K, (unsigned long long)hHeadless, (long long)headlessUs,
	       s_K ? (double)headlessUs / s_K : 0.0,
	       (unsigned long long)hNormal, (long long)normalUs);
	printf("[predict-gate] === PRIMITIVE %s === headless==normal:%s  headless<1ms(K=10):%s\n",
	       s_pass ? "GO" : "NO-GO",
	       (hHeadless == hNormal) ? "YES" : "NO",
	       (headlessUs < 1000) ? "YES" : "NO");
	fflush(stdout);
	s_stage = DONE;
}

// ── input ring (authoritative input applied per frame) ───────────────
static std::map<uint64_t, HeldInput> g_inputs;
static constexpr size_t kInputRing = 4096;

void recordAppliedInput(uint64_t frame)
{
	if (!active()) return;
	g_inputs[frame] = snapInput();
	while (g_inputs.size() > kInputRing) g_inputs.erase(g_inputs.begin());
}

// ── GATE 0: restore-into-a-live-running client is CLEAN ──────────────
// From a warmed-up frame F, capture the anchor, run K frames NORMALLY (input
// recorded), capture F+K, then RESTORE the anchor into the LIVE client and
// re-sim K frames HEADLESS with the RECORDED inputs -> must byte-match F+K.
// Finally restore F+K so the client resumes exactly where it was — the reconcile
// does this every frame, so it must leave NO residual host state: after the op
// the client MUST stay 60fps / buffer flat / offset-locked (no mismatch storm).
// Env: MAPLECAST_PREDICT_GATE0=W,K.
enum G0Stage { G0_IDLE, G0_WARMUP, G0_ANCHOR, G0_RUN, G0_MONITOR, G0_DONE };
static G0Stage g0_stage = G0_IDLE;
static bool     g0_configured = false;
static uint32_t g0_warmup = 0, g0_K = 0;
static uint64_t g0_anchorFrame = 0;
static std::vector<uint8_t> g0_blobA, g0_blobB, g0_blobLive, g0_blobResim;
static int32_t  g0_jitA = 0, g0_jitB = 0;
static size_t   g0_nA = 0, g0_nB = 0;
static uint64_t g0_hashA = 0;
static bool     g0_pass = false;

static void g0Configure()
{
	if (g0_configured) return;
	g0_configured = true;
	const char* e = std::getenv("MAPLECAST_PREDICT_GATE0");
	if (!e || !*e) return;
	unsigned w = 0, k = 0;
	if (sscanf(e, "%u,%u", &w, &k) != 2 || k == 0) {
		printf("[predict-gate0] malformed MAPLECAST_PREDICT_GATE0='%s' (want W,K)\n", e);
		return;
	}
	g0_warmup = w; g0_K = k;
	try {
		g0_blobA.resize(40u*1024u*1024u); g0_blobB.resize(40u*1024u*1024u);
		g0_blobLive.resize(40u*1024u*1024u); g0_blobResim.resize(40u*1024u*1024u);
	} catch (...) { printf("[predict-gate0] blob alloc failed\n"); return; }
	g0_stage = G0_WARMUP;
	printf("[predict-gate0] armed: warmup=%u K=%u\n", w, k);
}

// Restore a full dc_serialize blob WITHOUT the vblank_schid re-arm that
// maplecast_rollback::restoreFromBlob does. That re-arm is correct only for
// captures taken INSIDE the vblank handler (where vblank_schid was saved
// end=-1); GATE 0 captures at a clean frame boundary where the deserialized
// vblank_schid is already VALID, so re-arming it CORRUPTS the scheduler and the
// re-sim diverges. Plain emu.loadstate keeps the deserialized scheduler as-is.
static bool g0Restore(const std::vector<uint8_t>& blob, size_t n)
{
	try {
		Deserializer deser(blob.data(), n, false);
		emu.loadstate(deser);
	} catch (...) { return false; }
	rend_resync_after_rollback();   // drain stale render-queue entries
	return true;
}

// Serialize the FULL machine state into `buf` recording per-region marks, so the
// GATE 0 diff can bucket divergent bytes by dc_serialize subsystem.
static size_t g0SerializeMarked(std::vector<uint8_t>& buf, std::vector<DcAuditMark>& marks)
{
	marks.clear();
	dc_audit_marks = &marks;
	size_t sz = 0;
	try {
		Serializer ser(buf.data(), buf.size(), false);   // rollback=false: full state
		dc_serialize(ser);
		sz = ser.size();
	} catch (...) { sz = 0; }
	dc_audit_marks = nullptr;
	return sz;
}

// Called at frame F+K (from onFrameBoundary), AFTER the client normally reached
// F+K and recorded inputs F..F+K-1. Runs the restore-into-live + re-sim check.
static void g0RunCheck(uint64_t nowFrame)
{
	using namespace maplecast_rollback;
	// Capture F+K (the live "current" state) so we can put the client back here.
	int32_t jitB = 0;
	g0_nB = captureFrameToBlob(g0_blobB.data(), g0_blobB.size(), jitB);
	g0_jitB = jitB;
	const uint64_t hashB = gameStateRegionHash();
	if (g0_nB == 0) { printf("[predict-gate0] captureB failed\n"); g0_stage = G0_DONE; return; }
	// FULL live-continuation blob (with region marks) for the byte-diff.
	std::vector<DcAuditMark> marksLive;
	const size_t nLive = g0SerializeMarked(g0_blobLive, marksLive);

	// Restore the anchor F INTO THE LIVE CLIENT (render loop + audio running).
	// Use g0Restore (plain emu.loadstate, NO vblank re-arm) — the capture is a
	// clean frame boundary so the deserialized scheduler is already valid.
	if (!g0Restore(g0_blobA, g0_nA)) {
		printf("[predict-gate0] restoreA failed\n");
		g0_stage = G0_DONE; return;
	}
	const bool restoreOk = (gameStateRegionHash() == g0_hashA);

	// ── ROUND-TRIP FIDELITY (K=0): serialize the JUST-restored state and diff it
	// against blobA. Any divergence here is a capture->restore round-trip LOSS
	// (state that dc_serialize writes but dc_deserialize doesn't restore identically,
	// or vice-versa) — independent of any re-sim. ──
	{
		std::vector<DcAuditMark> marksRT;
		const size_t nRT = g0SerializeMarked(g0_blobResim, marksRT);
		const size_t common = std::min(g0_nA, nRT);
		uint64_t total = 0;
		for (size_t i = 0; i < common; i++)
			if (g0_blobA[i] != g0_blobResim[i]) total++;
		printf("[predict-gate0] ROUNDTRIP(K=0): blobA=%zu reser=%zu; %llu differing. Regions:\n",
		       g0_nA, nRT, (unsigned long long)total);
		for (size_t mi = 0; mi < marksRT.size(); mi++) {
			const size_t start = marksRT[mi].offset;
			const size_t end = (mi + 1 < marksRT.size()) ? marksRT[mi+1].offset : common;
			if (end > common || start >= end) continue;
			uint64_t rdiff = 0; size_t firstOff = SIZE_MAX;
			for (size_t j = start; j < end; j++)
				if (g0_blobA[j] != g0_blobResim[j]) { if (firstOff==SIZE_MAX) firstOff=j-start; rdiff++; }
			if (rdiff)
				printf("[predict-gate0]   RT %-19s | %8llu / %8zu bytes | first@+%zu\n",
				       marksRT[mi].name, (unsigned long long)rdiff, end-start, firstOff);
		}
		// Re-restore so the re-sim starts from the clean anchor (the serialize above
		// is read-only, but re-restore defensively to guarantee identical start).
		g0Restore(g0_blobA, g0_nA);
	}

	// Re-sim K frames HEADLESS replaying the recorded authoritative inputs.
	g_headless.store(true, std::memory_order_relaxed);
	const bool prevEnabled = rend_is_enabled();
	const int64_t t0 = nowUs();
	int inpFound = 0, inpMissing = 0;
	for (uint32_t i = 0; i < g0_K; i++) {
		auto it = g_inputs.find(g0_anchorFrame + i);
		if (it != g_inputs.end()) { applyInput(it->second); inpFound++;
			printf("[predict-gate0]   resim f=%llu kc0=%08x lt0=%u rt0=%u\n",
			       (unsigned long long)(g0_anchorFrame + i), it->second.kc[0],
			       it->second.lt_[0], it->second.rt_[0]);
		} else { inpMissing++;
			printf("[predict-gate0]   resim f=%llu INPUT-MISSING (kcode live=%08x)\n",
			       (unsigned long long)(g0_anchorFrame + i), kcode[0]);
		}
		runOneFrame(true);
	}
	printf("[predict-gate0] inputs: found=%d missing=%d (ring=%zu)\n",
	       inpFound, inpMissing, g_inputs.size());
	const int64_t resimUs = nowUs() - t0;
	g_headless.store(false, std::memory_order_relaxed);
	rend_enable_renderer(prevEnabled);
	const uint64_t hashResim = gameStateRegionHash();
	// FULL re-sim blob for the byte-diff.
	std::vector<DcAuditMark> marksResim;
	const size_t nResim = g0SerializeMarked(g0_blobResim, marksResim);

	// DETERMINISM PROBE: restore the SAME anchor A again and re-sim the SAME K
	// frames with the SAME recorded inputs. If hashResim2 != hashResim, the
	// re-sim reads state NOT captured by the restore (host-timing / external) =>
	// the restore/mechanism is the bug, not a live-vs-resim rendering gap.
	g0Restore(g0_blobA, g0_nA);
	g_headless.store(true, std::memory_order_relaxed);
	for (uint32_t i = 0; i < g0_K; i++) {
		auto it = g_inputs.find(g0_anchorFrame + i);
		if (it != g_inputs.end()) applyInput(it->second);
		runOneFrame(true);
	}
	g_headless.store(false, std::memory_order_relaxed);
	rend_enable_renderer(prevEnabled);
	const uint64_t hashResim2 = gameStateRegionHash();
	printf("[predict-gate0] determinism: resim=%016llx resim2=%016llx  %s\n",
	       (unsigned long long)hashResim, (unsigned long long)hashResim2,
	       (hashResim == hashResim2) ? "DETERMINISTIC" : "NON-DETERMINISTIC(external-state)");

	// MECHANISM PROBE: from the SAME restore A, re-sim K frames with RENDER ON
	// (runOneFrame(false) — the primitive's "normal" path). If hashNormal == hashB
	// (live) the divergence is RENDER-OFF; if hashNormal == hashResim (== the
	// render-off run) but still != hashB, the divergence is the restore-vs-live-
	// forward MECHANISM, not render.
	g0Restore(g0_blobA, g0_nA);
	for (uint32_t i = 0; i < g0_K; i++) {
		auto it = g_inputs.find(g0_anchorFrame + i);
		if (it != g_inputs.end()) applyInput(it->second);
		runOneFrame(false);   // render ON, emu-loop-equivalent mechanism
	}
	const uint64_t hashNormal = gameStateRegionHash();
	printf("[predict-gate0] mechanism: renderOFF=%016llx renderON=%016llx live=%016llx  "
	       "[renderON==live:%s  renderON==renderOFF:%s]\n",
	       (unsigned long long)hashResim, (unsigned long long)hashNormal,
	       (unsigned long long)hashB,
	       (hashNormal == hashB) ? "YES" : "no",
	       (hashNormal == hashResim) ? "YES" : "no");

	// Put the client back at F+K so the LIVE timeline is undisturbed.
	g0Restore(g0_blobB, g0_nB);

	g0_pass = restoreOk && (hashResim == hashB);
	printf("[predict-gate0] anchorF=%llu K=%u  resim=%016llx  live(F+K)=%016llx  "
	       "restoreA:%s  match:%s  resim=%lld us (%.1f us/frame)\n",
	       (unsigned long long)g0_anchorFrame, g0_K,
	       (unsigned long long)hashResim, (unsigned long long)hashB,
	       restoreOk ? "OK" : "BAD", (hashResim == hashB) ? "YES" : "NO",
	       (long long)resimUs, g0_K ? (double)resimUs / g0_K : 0.0);

	// ── PRECISE DIVERGENCE DIAGNOSIS: byte-diff the two full blobs, bucketed by
	// dc_serialize region (dcs_mark), so we see EXACTLY what the re-sim didn't
	// reproduce (AICA? sh4_sched? TMU? a RAM region?). ──
	if (!g0_pass && nLive > 0 && nResim > 0) {
		const size_t common = std::min(nLive, nResim);
		uint64_t total = 0;
		for (size_t i = 0; i < common; i++)
			if (g0_blobLive[i] != g0_blobResim[i]) total++;
		printf("[predict-gate0] BLOB DIFF: live=%zu resim=%zu bytes; %llu differing "
		       "(%.4f%%). Divergent regions:\n", nLive, nResim,
		       (unsigned long long)total, common ? 100.0*(double)total/common : 0.0);
		for (size_t mi = 0; mi < marksLive.size(); mi++) {
			const size_t start = marksLive[mi].offset;
			const size_t end = (mi + 1 < marksLive.size()) ? marksLive[mi+1].offset : common;
			if (end > common || start >= end) continue;
			uint64_t rdiff = 0; size_t firstOff = SIZE_MAX;
			for (size_t j = start; j < end; j++)
				if (g0_blobLive[j] != g0_blobResim[j]) { if (firstOff==SIZE_MAX) firstOff=j-start; rdiff++; }
			if (rdiff)
				printf("[predict-gate0]   %-22s | %8llu / %8zu bytes | first@+%zu\n",
				       marksLive[mi].name, (unsigned long long)rdiff, end-start, firstOff);
		}
	}
	printf("[predict-gate0] === RESTORE-INTO-LIVE %s ===\n",
	       g0_pass ? "byte-exact GO" : "NO-GO (see BLOB DIFF regions above)");
	fflush(stdout);
}

void onFrameBoundary(uint64_t frame)
{
	if (!active()) return;

	// Primitive gate (MAPLECAST_PREDICT_TEST).
	configure();
	if (s_stage == WARMUP) {
		if (s_warmup > 0) s_warmup--; else runGate();
	}

	// GATE 0 (MAPLECAST_PREDICT_GATE0).
	g0Configure();
	switch (g0_stage) {
	case G0_WARMUP:
		if (g0_warmup > 0) { g0_warmup--; break; }
		// Anchor the current live frame F.
		g0_anchorFrame = frame;
		g0_nA = maplecast_rollback::captureFrameToBlob(g0_blobA.data(), g0_blobA.size(), g0_jitA);
		g0_hashA = maplecast_rollback::gameStateRegionHash();
		g0_stage = (g0_nA ? G0_RUN : G0_DONE);
		break;
	case G0_RUN:
		// Wait until the live client has normally advanced K frames past F,
		// recording inputs along the way, then run the restore-into-live check.
		if (frame >= g0_anchorFrame + g0_K) {
			g0RunCheck(frame);
			g0_stage = G0_MONITOR;
		}
		break;
	default: break;
	}
}

bool testDone()   { return s_stage == DONE; }
bool testPassed() { return s_pass; }

}
