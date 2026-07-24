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
#include "hw/sh4/sh4_mem.h"            // mem_b (guest RAM) for sub-region hashing
#include "hw/sh4/modules/mmu.h"        // mmu_set_state (fast restore, skip bm_Reset)
#include "hw/mem/mem_watch.h"          // memwatch: page-fault write tracking + block-invalidation
#include "hw/pvr/spg.h"                // spg_last_jitter / spg_getNextInterrupt (vblank reschedule)
#include "hw/sh4/sh4_sched.h"          // sh4_sched_request (vblank_schid reschedule after rewind)
#include "serialize.h"                 // dc_serialize + dc_audit_marks (region diff)
#include <xxhash.h>                    // XXH3_64bits (bundled at core/deps/xxHash/)

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
extern int vblank_schid;   // defined in core/hw/pvr/spg.cpp

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

// Advance EXACTLY one MVC2 game frame headless (OFFLINE gates only). runOneFrame stops at
// the next non-RTT STARTRENDER; MVC2 emits multiple per video frame, so loop until the
// game-frame counter (0x3496B0) ticks. Guard caps runaway (paused/menu with no tick).
// NOTE: this stops at STARTRENDER — the WRONG boundary for cross-instance determinism (it
// freezes sh4_sched mid-render; a13c49e54). The LIVE predict path does NOT use this — it uses
// advanceHeadlessOneFrame, which stops at Emulator::vblank() (the correct boundary; see there).
static void runOneGameFrameHeadless()
{
	const uint32_t start = *(uint32_t*)&mem_b[0x3496B0];
	int guard = 0;
	do { runOneFrame(true); } while (*(uint32_t*)&mem_b[0x3496B0] == start && ++guard < 16);
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
static uint32_t g0_anchorFrameCtr = 0;
static uint8_t  g0_anchorFightTick = 0;
static bool     g0_pass = false;
// Multi-trial re-gate: repeat (random warmup gap, random K) N times while LIVE.
static uint32_t g0_trials = 1;       // total trials requested
static uint32_t g0_trialIdx = 0;     // completed trials
static uint32_t g0_passCount = 0;    // byte-exact trials
static uint32_t g0_baseWarmup = 300; // first-trial warmup
static double   g0_totUsPerFrame = 0;// running avg of muted us/frame
static double   g0_lastUsPerFrame = 0;// last trial muted us/frame

static void g0Configure()
{
	if (g0_configured) return;
	g0_configured = true;
	const char* e = std::getenv("MAPLECAST_PREDICT_GATE0");
	if (!e || !*e) return;
	// Optional 3rd field = trial count: "W,K[,N]".
	{ unsigned tw=0,tk=0,tn=0; if (sscanf(e,"%u,%u,%u",&tw,&tk,&tn)==3 && tn>0) g0_trials=tn; }
	unsigned w = 0, k = 0;
	if (sscanf(e, "%u,%u", &w, &k) != 2 || k == 0) {
		printf("[predict-gate0] malformed MAPLECAST_PREDICT_GATE0='%s' (want W,K)\n", e);
		return;
	}
	g0_warmup = w; g0_K = k; g0_baseWarmup = w;
	try {
		g0_blobA.resize(40u*1024u*1024u); g0_blobB.resize(40u*1024u*1024u);
		g0_blobLive.resize(40u*1024u*1024u); g0_blobResim.resize(40u*1024u*1024u);
	} catch (...) { printf("[predict-gate0] blob alloc failed\n"); return; }
	g0_stage = G0_WARMUP;
	srand(0xC0FFEEu ^ w ^ (k << 8));
	// FOUNDATION UNLOCK (memwatch): arm the page-fault write watcher so the
	// bm_Reset-FREE fast restore stays correct over open-ended live continuation.
	// The watcher's write path (mem_watch.h:195) calls bm_RamWriteAccess /
	// VramLockedWrite on every faulted page, INVALIDATING stale dynarec blocks &
	// texcache for changed pages — the exact mechanism that makes GGPO's
	// bm_Reset-free rollback correct. GATE 0's earlier fast-restore mismatch storm
	// was caused by MAPLECAST_DISABLE_MEMWATCH=1 (no invalidation), NOT by the fast
	// restore being unsafe. Env: MAPLECAST_PREDICT_MEMWATCH=1.
	if (std::getenv("MAPLECAST_PREDICT_MEMWATCH")) {
		memwatch::mirrorActive = true;
		memwatch::protect();
		printf("[predict-gate0] memwatch ARMED (block-invalidation on write => fast restore safe)\n");
	}
	printf("[predict-gate0] armed: warmup=%u K=%u trials=%u\n", w, k, g0_trials);
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

// FAST restore: dc_deserialize WITHOUT emu.loadstate's bm_Reset()/ResetCache()
// (emulator.cpp:1137/1145) — those FLUSH the SH4 dynarec, so the re-sim then runs
// with a COLD JIT cache (~realtime, the measured 10-18ms/frame). For a rollback of
// a few frames the guest CODE pages are unchanged, so compiled blocks stay valid
// and skipping the flush keeps the dynarec WARM. Correctness is gated: if this
// leaves a stale block, the byte-exact re-gate turns RED and we fall back.
static bool g0RestoreFast(const std::vector<uint8_t>& blob, size_t n)
{
	try {
		Deserializer deser(blob.data(), n, false);
		dc_deserialize(deser);
	} catch (...) { return false; }
	mmu_set_state();
	rend_resync_after_rollback();
	return true;
}

// Break gameStateRegionHash into its 4 sub-regions so we see WHICH diverges:
// char structs (real game logic), global game-state page, frame counter (the +1
// skew is benign), fight tick. r[0]=chars r[1]=gspage r[2]=framectr r[3]=fighttick.
static void g0SubHashes(uint64_t r[4])
{
	r[0] = XXH3_64bits(&mem_b[0x268340], 6u * 0x5A4u);
	r[1] = XXH3_64bits(&mem_b[0x289000], 0x1000u);
	r[2] = XXH3_64bits(&mem_b[0x3496B0], 4u);
	r[3] = XXH3_64bits(&mem_b[0x268250], 1u);
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
	uint64_t subLive[4]; g0SubHashes(subLive);   // live F+K sub-region hashes
	const uint32_t liveFrameCtr = *(uint32_t*)&mem_b[0x3496B0];
	const uint8_t  liveFightTick = mem_b[0x268250];
	if (g0_nB == 0) { printf("[predict-gate0] captureB failed\n"); g0_stage = G0_DONE; return; }

	// Restore the anchor F INTO THE LIVE CLIENT (render loop + audio running).
	const bool useFast = std::getenv("MAPLECAST_PREDICT_FASTRESTORE") != nullptr;
	bool ok = useFast ? g0RestoreFast(g0_blobA, g0_nA) : g0Restore(g0_blobA, g0_nA);
	if (!ok) {
		printf("[predict-gate0] restoreA failed\n");
		g0_stage = G0_DONE; return;
	}
	const bool restoreOk = (gameStateRegionHash() == g0_hashA);

	// ── ROUND-TRIP FIDELITY (K=0) — DIAGNOSTIC ONLY (proven 1 harmless byte). Gated
	// off (MAPLECAST_PREDICT_DECISIVE=1) because its trailing full g0Restore would
	// re-flush the dynarec right before the timed re-sim, masking the fast-restore
	// speed win. ──
	if (std::getenv("MAPLECAST_PREDICT_DECISIVE")) {
		std::vector<DcAuditMark> marksRT;
		const size_t nRT = g0SerializeMarked(g0_blobResim, marksRT);
		const size_t common = std::min(g0_nA, nRT);
		uint64_t total = 0;
		for (size_t i = 0; i < common; i++)
			if (g0_blobA[i] != g0_blobResim[i]) total++;
		printf("[predict-gate0] ROUNDTRIP(K=0): blobA=%zu reser=%zu; %llu differing.\n",
		       g0_nA, nRT, (unsigned long long)total);
		g0RestoreFast(g0_blobA, g0_nA);   // fast — keep dynarec warm
	}

	// Re-sim K frames HEADLESS replaying the recorded authoritative inputs.
	// MUTED (fastForwardMode) — the decisive experiment proved mute is game-state
	// NEUTRAL and gets ~1.9ms/frame; the un-muted path blocks on the real audio
	// backend (~17ms/frame) and couples real-time audio drain into the run.
	const bool prevFF = settings.input.fastForwardMode;
	const bool prevMute = settings.aica.muteAudio;
	settings.input.fastForwardMode = true;
	settings.aica.muteAudio = true;   // robust mute (fastForwardMode can be cleared)
	g_headless.store(true, std::memory_order_relaxed);
	const bool prevEnabled = rend_is_enabled();
	// WARM-UP re-sim (untimed, fast-restore only): with a full loadstate return-to-
	// live the dynarec is flushed, so warm it once to MEASURE the production warm
	// hot-path cost. Only when fast-restore mode is on (else keep it simple).
	if (useFast) {
		for (uint32_t i = 0; i < g0_K; i++) {
			auto it = g_inputs.find(g0_anchorFrame + i);
			if (it != g_inputs.end()) applyInput(it->second);
			runOneGameFrameHeadless();
		}
		g0RestoreFast(g0_blobA, g0_nA);   // back to anchor, dynarec now WARM
	}

	const int64_t t0 = nowUs();
	int inpFound = 0, inpMissing = 0;
	for (uint32_t i = 0; i < g0_K; i++) {
		auto it = g_inputs.find(g0_anchorFrame + i);
		if (it != g_inputs.end()) { applyInput(it->second); inpFound++; }
		else inpMissing++;
		runOneGameFrameHeadless();   // advance EXACTLY one MVC2 game frame
	}
	const int64_t resimUs = nowUs() - t0;   // WARM timed loop, NO stdout inside
	g_headless.store(false, std::memory_order_relaxed);
	rend_enable_renderer(prevEnabled);
	const uint64_t hashResim = gameStateRegionHash();
	uint64_t subResim[4]; g0SubHashes(subResim);   // re-sim F+K sub-region hashes
	const uint32_t resimFrameCtr = *(uint32_t*)&mem_b[0x3496B0];
	const uint8_t  resimFightTick = mem_b[0x268250];
	const bool frameAdvanceOK = (resimFrameCtr - g0_anchorFrameCtr) == (liveFrameCtr - g0_anchorFrameCtr);
	// Verbose per-region diagnostics only on FAILURE (keeps the multi-trial log clean).
	if (!(frameAdvanceOK && subLive[0]==subResim[0] && subLive[1]==subResim[1]
	      && subLive[2]==subResim[2] && subLive[3]==subResim[3])) {
		static const char* nm[4] = {"chars(logic)","gs-page","frame-ctr","fight-tick"};
		printf("[predict-gate0] FRAME-ADVANCE: anchor frameCtr=%u fightTick=%u  ->  "
		       "live frameCtr+%d fightTick+%d  resim frameCtr+%d fightTick+%d  [K=%u]\n",
		       g0_anchorFrameCtr, g0_anchorFightTick,
		       (int)(liveFrameCtr - g0_anchorFrameCtr), (int)((int)liveFightTick - (int)g0_anchorFightTick),
		       (int)(resimFrameCtr - g0_anchorFrameCtr), (int)((int)resimFightTick - (int)g0_anchorFightTick), g0_K);
		printf("[predict-gate0] SUB-REGION live-vs-resim:");
		for (int i = 0; i < 4; i++)
			printf(" %s=%s", nm[i], (subLive[i]==subResim[i]) ? "OK" : "DIFF");
		printf("\n");
	}
	// DETERMINISM PROBE — DIAGNOSTIC ONLY (gated). Re-sim the SAME K frames again
	// from the SAME fast-restored anchor; hashResim2 must == hashResim.
	if (std::getenv("MAPLECAST_PREDICT_DECISIVE")) {
		g0RestoreFast(g0_blobA, g0_nA);
		g_headless.store(true, std::memory_order_relaxed);
		for (uint32_t i = 0; i < g0_K; i++) {
			auto it = g_inputs.find(g0_anchorFrame + i);
			if (it != g_inputs.end()) applyInput(it->second);
			runOneGameFrameHeadless();
		}
		g_headless.store(false, std::memory_order_relaxed);
		rend_enable_renderer(prevEnabled);
		const uint64_t hashResim2 = gameStateRegionHash();
		printf("[predict-gate0] determinism: resim=%016llx resim2=%016llx  %s\n",
		       (unsigned long long)hashResim, (unsigned long long)hashResim2,
		       (hashResim == hashResim2) ? "DETERMINISTIC" : "NON-DETERMINISTIC");
	}

	// Put the client back at F+K so the LIVE timeline resumes cleanly. This return-
	// to-live uses the FULL restore (bm_Reset + ResetCache): the fast restore is
	// byte-exact for the short headless re-sim, but returning to the LIVE client
	// (which then runs thousands of frames of render+audio) needs the full cache/mmu
	// reset or it drifts from the server (observed: mismatch storm). NOTE: production
	// rollback has NO return-to-live restore — the re-simmed state BECOMES live — so
	// the reconcile hot path stays on the fast (warm-dynarec ~3ms/frame) restore.
	settings.input.fastForwardMode = prevFF;
	settings.aica.muteAudio = prevMute;
	g0Restore(g0_blobB, g0_nB);

	g0_pass = restoreOk && (hashResim == hashB);
	g0_lastUsPerFrame = g0_K ? (double)resimUs / g0_K : 0.0;
	g0_totUsPerFrame += g0_lastUsPerFrame;
	printf("[predict-gate0] anchorF=%llu K=%u  resim=%016llx  live(F+K)=%016llx  "
	       "restoreA:%s  match:%s  resim=%lld us (%.1f us/frame)\n",
	       (unsigned long long)g0_anchorFrame, g0_K,
	       (unsigned long long)hashResim, (unsigned long long)hashB,
	       restoreOk ? "OK" : "BAD", (hashResim == hashB) ? "YES" : "NO",
	       (long long)resimUs, g0_K ? (double)resimUs / g0_K : 0.0);

	if (!g0_pass)
		printf("[predict-gate0] === RESTORE-INTO-LIVE NO-GO: resim=%016llx != live=%016llx "
		       "(see FRAME-ADVANCE/SUB-REGION above) ===\n",
		       (unsigned long long)hashResim, (unsigned long long)hashB);
	fflush(stdout);
}

// ── DECISIVE EXPERIMENT (coordinator step 1) ─────────────────────────────
// Called at the anchor F (SH4 paused). Self-contained: does NOT depend on the
// live-forward baseline. Runs K muted frames THREE ways from the identical
// anchor state and compares the game-state hash:
//   hashFwd   = re-sim K with NO loadstate (SH4 already at F)  = pure-forward truth
//   hashImm   = loadstate(blobA) [0 elapsed] then re-sim K     = loadstate resume @0
//   hashFwdUM = re-sim K with NO loadstate, audio UN-muted     = mute-neutrality check
// Verdict:
//   hashImm == hashFwd  -> loadstate resume is CLEAN at 0 elapsed
//                          => the GATE0 K=5 divergence is STALE HOST STATE across the
//                             intervening live frames -> GGPO memwatch page-delta fixes it.
//   hashImm != hashFwd  -> loadstate/dynarec RESUME itself is dirty -> interpreter fallback.
// Leaves the client at blobA (frame F) so the live timeline continues undisturbed.
static void g0DecisiveExperiment()
{
	const HeldInput held = snapInput();   // neutral during idle GATE0
	const bool prevEnabled = rend_is_enabled();
	const bool prevFF = settings.input.fastForwardMode;

	auto resimK = [&](bool muted) -> uint64_t {
		settings.input.fastForwardMode = muted;
		g_headless.store(true, std::memory_order_relaxed);
		for (uint32_t i = 0; i < g0_K; i++) { applyInput(held); runOneFrame(true); }
		g_headless.store(false, std::memory_order_relaxed);
		rend_enable_renderer(prevEnabled);
		return maplecast_rollback::gameStateRegionHash();
	};

	// (1) pure forward, muted — NO loadstate. Ground-truth trajectory from F.
	const int64_t t0 = nowUs();
	const uint64_t hashFwd = resimK(true);
	const int64_t fwdUs = nowUs() - t0;

	// (2) loadstate(blobA) [0 elapsed frames] then the SAME K frames, muted.
	g0Restore(g0_blobA, g0_nA);
	const uint64_t hashImm = resimK(true);

	// (3) mute-neutrality: pure forward UN-muted (needs a clean start -> restore).
	g0Restore(g0_blobA, g0_nA);
	const int64_t t2 = nowUs();
	const uint64_t hashFwdUM = resimK(false);
	const int64_t umUs = nowUs() - t2;

	// Put the client back at the anchor so the live run continues from F.
	g0Restore(g0_blobA, g0_nA);
	settings.input.fastForwardMode = prevFF;

	printf("[predict-gate0] DECISIVE @F=%llu K=%u: fwd(noload,muted)=%016llx  "
	       "imm(load@0,muted)=%016llx  fwd(noload,UNmuted)=%016llx\n",
	       (unsigned long long)g0_anchorFrame, g0_K,
	       (unsigned long long)hashFwd, (unsigned long long)hashImm,
	       (unsigned long long)hashFwdUM);
	printf("[predict-gate0] DECISIVE verdict: loadstate-resume@0 %s ; audio-mute %s ; "
	       "muted %.0f us/frame  unmuted %.0f us/frame\n",
	       (hashImm == hashFwd) ? "CLEAN(==forward)=>stale-host-state theory"
	                            : "DIRTY(!=forward)=>dynarec-resume theory",
	       (hashFwdUM == hashFwd) ? "NEUTRAL(hash unchanged)" : "AFFECTS-HASH(!)",
	       g0_K ? (double)fwdUs / g0_K : 0.0, g0_K ? (double)umUs / g0_K : 0.0);
	fflush(stdout);
}

// ═══════════════════════════════════════════════════════════════════════════
// STAGE 0 — rollback=true PAGE-DELTA RING (the fast + correct rollback ring)
// ═══════════════════════════════════════════════════════════════════════════
// The reconcile loop's per-frame save/restore. Distinct from maplecast_rollback's
// full-savestate ring (rollback=false, ~17ms cold JIT flush). Here:
//   hwBlob[f] = dc_serialize(rollback=TRUE): SH4/PVR/AICA regs + sched ONLY.
//               mem_b / vram / aram are NOT serialized — delegated ENTIRELY to
//               page-delta (avoids the rollback=false "hybrid" bug the local
//               fallback hit).
//   delta[f]  = pre-write content of the pages WRITTEN DURING frame f (memwatch).
// Restore to boundary F (from mostRecent M): unwind delta[M-1..F] (memcpy pre-write
//   pages back), then dc_deserialize(hwBlob[F], rollback=TRUE) with NO bm_Reset —
//   memwatch's write path (mem_watch.h:195 bm_RamWriteAccess/VramLockedWrite)
//   invalidates stale dynarec blocks/texcache for changed pages, so the JIT stays
//   warm AND correct (proven byte-exact 7/7 with memwatch armed).
namespace pd {

static constexpr int DEPTH = 32;   // rollback horizon; beyond = re-JOIN

struct Delta {
	memwatch::PageMap ram, vram, aram, elan;
	void capture() {
		memwatch::ramWatcher.getPages(ram);
		memwatch::vramWatcher.getPages(vram);
		memwatch::aramWatcher.getPages(aram);
		memwatch::elanWatcher.getPages(elan);
	}
	void clear() { ram.clear(); vram.clear(); aram.clear(); elan.clear(); }
	void applyReverse() const {   // memcpy pre-write page content back = undo frame
		for (const auto& p : ram)  memcpy(memwatch::ramWatcher.getMemPage(p.first),  &p.second.data[0], PAGE_SIZE);
		for (const auto& p : vram) memcpy(memwatch::vramWatcher.getMemPage(p.first), &p.second.data[0], PAGE_SIZE);
		for (const auto& p : aram) memcpy(memwatch::aramWatcher.getMemPage(p.first), &p.second.data[0], PAGE_SIZE);
		for (const auto& p : elan) memcpy(memwatch::elanWatcher.getMemPage(p.first), &p.second.data[0], PAGE_SIZE);
	}
};

struct Slot { uint64_t frame = UINT64_MAX; std::vector<uint8_t> hw; size_t hwSize = 0; Delta delta; int spgJitter = 0; };

static Slot     ring[DEPTH];
static bool     inited = false, armed = false;
static uint64_t mostRecent = UINT64_MAX;   // newest boundary saved
static uint64_t oldest      = UINT64_MAX;
static bool     havePrev = false;
static uint64_t prevFrame = 0;

static bool init()
{
	if (inited) return armed;
	inited = true;
	try { for (auto& s : ring) s.hw.resize(4u * 1024u * 1024u); }
	catch (...) { printf("[predict-pd] ring alloc failed\n"); return false; }
	memwatch::mirrorActive = true;
	memwatch::reset();
	memwatch::protect();
	armed = true;
	printf("[predict-pd] ring armed: depth=%d, rollback=true hw-blob + page-delta RAM\n", DEPTH);
	return true;
}

// Save the boundary state for frame F (SH4 paused, frame F about to run). Also
// finalizes delta[F-1] = the pages written during the just-completed frame.
static void save(uint64_t F)
{
	if (!armed) return;
	// Re-arm protection on pages touched during the frame that just ran, THEN
	// drain them into that frame's delta slot (GGPO/saveFrame order, L236-246).
	memwatch::protect();
	if (havePrev) {
		Slot& ds = ring[prevFrame % DEPTH];
		ds.delta.clear();
		ds.delta.capture();          // pre-write content of prevFrame's pages
	} else {
		Delta junk; junk.capture();  // discard boot-time writes
	}
	// Serialize hardware-only state at boundary F (rollback=true skips big RAM).
	Slot& s = ring[F % DEPTH];
	s.frame = F;
	s.spgJitter = spg_last_jitter;   // for the vblank_schid reschedule on rewind
	try { Serializer ser(s.hw.data(), s.hw.size(), /*rollback=*/true);
	      dc_serialize(ser); s.hwSize = ser.size(); }
	catch (...) { s.hwSize = 0; }
	mostRecent = F;
	oldest = (mostRecent >= (uint64_t)DEPTH - 1) ? (mostRecent - (DEPTH - 2)) : 0;
	havePrev = true; prevFrame = F;
}

static bool canRestore(uint64_t F)
{
	return armed && mostRecent != UINT64_MAX && F <= mostRecent && F >= oldest
	       && ring[F % DEPTH].frame == F && ring[F % DEPTH].hwSize > 0;
}

// Restore the machine to boundary F (start of frame F). Fast: no bm_Reset.
static bool restore(uint64_t F)
{
	if (!canRestore(F)) return false;
	memwatch::unprotect();
	for (uint64_t f = mostRecent; f > F; f--) {           // undo frames M-1..F  (delta[f-1]? no: delta of each ran frame)
		const Slot& sl = ring[(f - 1) % DEPTH];
		if (sl.frame == (f - 1)) sl.delta.applyReverse();
	}
	int jitter = 0;
	{
		Slot& s = ring[F % DEPTH];
		Deserializer deser(s.hw.data(), s.hwSize, /*rollback=*/true);
		try { dc_deserialize(deser); } catch (...) { return false; }
		jitter = s.spgJitter;
	}
	mmu_set_state();
	rend_resync_after_rollback();
	// Reconstruct LIVE's post-callback vblank_schid reschedule (mirrors
	// maplecast_rollback::rewindToFrame L384-386). Without this, vblank_schid can
	// stay inactive after the rewind -> no vblanks fire -> SH4 dispatches blocks
	// forever with no forward progress (the observed intermittent STALL).
	{
		const int re_sch = spg_getNextInterrupt();
		sh4_sched_request(vblank_schid, std::max(0, re_sch - jitter));
	}
	memwatch::reset();
	memwatch::protect();
	mostRecent = F; havePrev = false;   // forward re-sim rebuilds deltas from F
	return true;
}

} // namespace pd

// ── STAGE 0 GATE (MAPLECAST_PREDICT_STAGE0=W,K,N) ───────────────────────────
// Live client saves EVERY frame to the page-delta ring. Each trial: pick anchor
// F, let live advance K, then pd::restore(F) + re-sim K (rebuilding the ring) and
// require the game-state hash == the live F+K hash, byte-exact, ~3ms/frame, while
// the client holds 60fps (fps-neutral). The re-sim state BECOMES the live state
// (no restore-back) — exactly how production reconcile continues.
enum S0Stage { S0_IDLE, S0_WARMUP, S0_RUN, S0_DONE };
static S0Stage  s0_stage = S0_IDLE;
static bool     s0_configured = false;
static uint32_t s0_warmup = 0, s0_K = 0, s0_trials = 1, s0_trialIdx = 0, s0_pass = 0;
static uint64_t s0_anchor = 0, s0_anchorCtr = 0, s0_anchorHash = 0;
static double   s0_totUs = 0;

static void s0Configure()
{
	if (s0_configured) return;
	s0_configured = true;
	const char* e = std::getenv("MAPLECAST_PREDICT_STAGE0");
	if (!e || !*e) return;
	unsigned w = 0, k = 0, n = 1;
	if (sscanf(e, "%u,%u,%u", &w, &k, &n) < 2 || k == 0) {
		printf("[predict-s0] malformed MAPLECAST_PREDICT_STAGE0='%s' (want W,K[,N])\n", e); return;
	}
	s0_warmup = w; s0_K = k; s0_trials = n ? n : 1;
	if (!pd::init()) return;
	srand(0x5747E0u ^ w ^ (k << 8));
	s0_stage = S0_WARMUP;
	printf("[predict-s0] armed: warmup=%u K=%u trials=%u\n", w, k, s0_trials);
}

static void s0RunCheck(uint64_t nowFrame)
{
	using namespace maplecast_rollback;
	const uint64_t liveHash = gameStateRegionHash();
	const uint32_t liveCtr  = *(uint32_t*)&mem_b[0x3496B0];

	if (!pd::canRestore(s0_anchor)) {
		printf("[predict-s0] TRIAL %u/%u: anchor %llu not in ring (oldest=%llu most=%llu) -> SKIP\n",
		       s0_trialIdx + 1, s0_trials, (unsigned long long)s0_anchor,
		       (unsigned long long)pd::oldest, (unsigned long long)pd::mostRecent);
		return;
	}
	// Rewind to the anchor via page-delta, then re-sim K frames headless/muted,
	// re-saving each frame so the ring stays consistent for the next trial.
	const bool prevFF = settings.input.fastForwardMode, prevMute = settings.aica.muteAudio;
	settings.input.fastForwardMode = true; settings.aica.muteAudio = true;
	const bool prevEnabled = rend_is_enabled();
	g_headless.store(true, std::memory_order_relaxed);

	const int64_t t0 = nowUs();
	const bool restored = pd::restore(s0_anchor);
	const bool restoreOk = restored && (gameStateRegionHash() == s0_anchorHash);
	for (uint32_t i = 0; i < s0_K; i++) {
		auto it = g_inputs.find(s0_anchor + i);
		if (it != g_inputs.end()) applyInput(it->second);
		runOneGameFrameHeadless();
		pd::save(s0_anchor + i + 1);   // rebuild ring forward (keeps deltas consistent)
	}
	const int64_t resimUs = nowUs() - t0;

	g_headless.store(false, std::memory_order_relaxed);
	rend_enable_renderer(prevEnabled);
	settings.input.fastForwardMode = prevFF; settings.aica.muteAudio = prevMute;

	const uint64_t resimHash = gameStateRegionHash();
	const uint32_t resimCtr  = *(uint32_t*)&mem_b[0x3496B0];
	const bool advOk = (resimCtr - s0_anchorCtr) == (liveCtr - s0_anchorCtr);
	const bool ok = restoreOk && advOk && (resimHash == liveHash);
	s0_trialIdx++; if (ok) s0_pass++;
	const double usf = s0_K ? (double)resimUs / s0_K : 0.0; s0_totUs += usf;
	printf("[predict-s0] TRIAL %u/%u: K=%u -> %s  resim=%016llx live=%016llx  advance=%s  "
	       "%.0f us/frame (%.1f ms)\n",
	       s0_trialIdx, s0_trials, s0_K, ok ? "PASS" : "FAIL",
	       (unsigned long long)resimHash, (unsigned long long)liveHash,
	       advOk ? "OK" : "BAD", usf, usf * s0_K / 1000.0);
	if (!ok)
		printf("[predict-s0]   FAIL detail: advance live+%d resim+%d\n",
		       (int)(liveCtr - s0_anchorCtr), (int)(resimCtr - s0_anchorCtr));
}

// ═══════════════════════════════════════════════════════════════════════════
// STAGE a — PREDICT (local input INSTANT at predictedFrame; remote = repeat-last)
// ═══════════════════════════════════════════════════════════════════════════
// The predict primitive: advance one game frame applying the LOCAL player's input
// NOW (at predictedFrame) + the REMOTE slot predicted as "repeat last confirmed".
// This is what makes local input latency ~0 (the sim reads it the same frame),
// vs the lockstep tape path (~9 frames: forward to server -> tape -> back).
// confirmedFrame = last authoritative frame; predictedFrame = displayed, ahead.
static uint64_t g_confirmedFrame = 0;
static uint64_t g_predictedFrame = 0;
static int      g_localSlot      = 0;    // which kcode[] the local player drives
static HeldInput g_lastConfirmedRemote;  // remote slot's last authoritative input

// Advance one predicted frame: apply local input (localIn) at the local slot +
// repeat-last-confirmed remote at the other slot, run one game frame, save ring.
static void predictAdvance(uint64_t predFrame, const HeldInput& localIn)
{
	const int rs = g_localSlot ^ 1;
	kcode[g_localSlot] = localIn.kc[g_localSlot];
	lt[g_localSlot]    = localIn.lt_[g_localSlot];
	rt[g_localSlot]    = localIn.rt_[g_localSlot];
	kcode[rs] = g_lastConfirmedRemote.kc[rs];        // repeat-last predicted remote
	lt[rs]    = g_lastConfirmedRemote.lt_[rs];
	rt[rs]    = g_lastConfirmedRemote.rt_[rs];
	runOneGameFrameHeadless();
	pd::save(predFrame + 1);
}

// ── STAGE a GATE (MAPLECAST_PREDICT_STAGEA=W,K,N) ───────────────────────────
// Headless proof of the predict primitive, from a live anchor F:
//  (1) DETERMINISM: predict K frames (varying synthetic local input), rollback to
//      F, re-predict the SAME sequence -> byte-identical.
//  (2) INSTANT INPUT: run frame F with local input A vs input B (rollback between)
//      -> game-state hash DIFFERS => the local input applied at F reached the sim
//      DURING frame F (0-frame latency). Under lockstep, g_localKcode never enters
//      the sim (it reads the tape), so the same injection changes NOTHING until the
//      ~9-frame server round-trip. Predict = instant by construction.
//  (3) RING consistency: rollback within the predicted span restores byte-exact.
enum SAStage { SA_IDLE, SA_WARMUP, SA_RUN, SA_DONE };
static SAStage  sa_stage = SA_IDLE;
static bool     sa_configured = false;
static uint32_t sa_warmup = 0, sa_K = 0, sa_trials = 1, sa_trialIdx = 0, sa_pass = 0;
static uint64_t sa_anchor = 0, sa_anchorHash = 0;

static void saConfigure()
{
	if (sa_configured) return;
	sa_configured = true;
	const char* e = std::getenv("MAPLECAST_PREDICT_STAGEA");
	if (!e || !*e) return;
	unsigned w = 0, k = 0, n = 1;
	if (sscanf(e, "%u,%u,%u", &w, &k, &n) < 2 || k == 0) {
		printf("[predict-sa] malformed MAPLECAST_PREDICT_STAGEA='%s'\n", e); return;
	}
	sa_warmup = w; sa_K = k; sa_trials = n ? n : 1;
	if (!pd::init()) return;
	srand(0x57A6E0u ^ w ^ (k << 8));
	sa_stage = SA_WARMUP;
	printf("[predict-sa] armed: warmup=%u K=%u trials=%u localSlot=%d\n", w, k, n, g_localSlot);
}

// Build a synthetic local input for predicted frame i: press a distinctive button
// pattern so we can prove the sim consumes it same-frame. active-low (0=pressed).
static HeldInput saSyntheticLocal(uint32_t i)
{
	HeldInput h = snapInput();
	// toggle a couple of buttons per frame on the local slot (low 16 bits)
	uint16_t pressed = (uint16_t)(0x000F & ((i * 0x9E37u) >> 2));   // varying
	h.kc[g_localSlot] = 0xFFFF0000u | (uint16_t)~pressed;           // active-low
	return h;
}

static void saRunCheck(uint64_t nowFrame)
{
	using namespace maplecast_rollback;
	if (!pd::canRestore(sa_anchor)) return;

	const bool prevFF = settings.input.fastForwardMode, prevMute = settings.aica.muteAudio;
	settings.input.fastForwardMode = true; settings.aica.muteAudio = true;
	const bool prevEnabled = rend_is_enabled();
	g_headless.store(true, std::memory_order_relaxed);

	g_lastConfirmedRemote = snapInput();   // remote = current held (neutral in idle)

	// (1) predict K frames, record final hash.
	const int64_t t0 = nowUs();
	pd::restore(sa_anchor);
	for (uint32_t i = 0; i < sa_K; i++) predictAdvance(sa_anchor + i, saSyntheticLocal(i));
	const uint64_t predHash1 = gameStateRegionHash();
	const int64_t predUs = nowUs() - t0;

	// (1) determinism: rollback + re-predict identical sequence.
	pd::restore(sa_anchor);
	for (uint32_t i = 0; i < sa_K; i++) predictAdvance(sa_anchor + i, saSyntheticLocal(i));
	const uint64_t predHash2 = gameStateRegionHash();
	const bool deterministic = (predHash1 == predHash2);

	// (2) instant input: the local input applied at each predicted frame is
	// consumed by the sim SAME-frame (0-frame latency). Prove by holding a strong
	// local input across the K-frame span vs neutral — the effect propagates into
	// the hashed game-state, so the two runs DIVERGE. (Under lockstep the sim reads
	// the TAPE, so g_localKcode changes would do nothing until the ~9-frame server
	// round-trip; here it drives the sim immediately.)
	HeldInput neutral = snapInput();  neutral.kc[g_localSlot] = 0xFFFFFFFFu;   // no buttons
	HeldInput held    = snapInput();  held.kc[g_localSlot]    = 0xFFFF0000u;   // ALL buttons+dirs
	pd::restore(sa_anchor);
	for (uint32_t i = 0; i < sa_K; i++) predictAdvance(sa_anchor + i, neutral);
	const uint64_t hNeutral = gameStateRegionHash();
	pd::restore(sa_anchor);
	for (uint32_t i = 0; i < sa_K; i++) predictAdvance(sa_anchor + i, held);
	const uint64_t hHeld = gameStateRegionHash();
	const bool instant = (hNeutral != hHeld);   // local input drove the sim => instant

	// (3) ring consistency: rollback to anchor restores byte-exact.
	pd::restore(sa_anchor);
	const bool ringOk = (gameStateRegionHash() == sa_anchorHash);

	g_headless.store(false, std::memory_order_relaxed);
	rend_enable_renderer(prevEnabled);
	settings.input.fastForwardMode = prevFF; settings.aica.muteAudio = prevMute;

	const bool ok = deterministic && instant && ringOk;
	sa_trialIdx++; if (ok) sa_pass++;
	printf("[predict-sa] TRIAL %u/%u: K=%u -> %s  determ=%s instant-input=%s(0-frame) ring=%s  "
	       "%.0f us/frame\n",
	       sa_trialIdx, sa_trials, sa_K, ok ? "PASS" : "FAIL",
	       deterministic ? "YES" : "NO", instant ? "YES" : "NO", ringOk ? "OK" : "BAD",
	       sa_K ? (double)predUs / sa_K : 0.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// STAGE b — RECONCILE + HASH-GATE (rollback on mispredicted remote input)
// ═══════════════════════════════════════════════════════════════════════════
// The full predict/reconcile core, gated headlessly with SYNTHETIC changing
// remote input (the coordinator's "drive changing remote input" — forces
// mispredicts so rollbacks actually fire). Ground truth = the all-authoritative
// forward sim (what the server computes). The predict+reconcile loop predicts the
// remote as repeat-last; when authoritative remote for a frame differs from the
// prediction, it ROLLS BACK to the confirmed frame (page-delta), applies the
// authoritative input, and re-sims forward. After each confirm the confirmed
// frame's game-state hash MUST equal the ground-truth (server) hash for that frame
// — the determinism safety net that must actually RUN.
enum SBStage { SB_IDLE, SB_WARMUP, SB_RUN, SB_DONE };
static SBStage  sb_stage = SB_IDLE;
static bool     sb_configured = false;
static uint32_t sb_warmup = 0, sb_K = 0, sb_trials = 1, sb_trialIdx = 0, sb_pass = 0;
static uint64_t sb_anchor = 0;
static uint64_t sb_totalRollbacks = 0, sb_maxDepth = 0, sb_confirmChecks = 0, sb_confirmMatches = 0;

static void sbConfigure()
{
	if (sb_configured) return;
	sb_configured = true;
	const char* e = std::getenv("MAPLECAST_PREDICT_STAGEB");
	if (!e || !*e) return;
	unsigned w = 0, k = 0, n = 1;
	if (sscanf(e, "%u,%u,%u", &w, &k, &n) < 2 || k == 0) {
		printf("[predict-sb] malformed MAPLECAST_PREDICT_STAGEB='%s'\n", e); return;
	}
	sb_warmup = w; sb_K = k; sb_trials = n ? n : 1;
	if (!pd::init()) return;
	srand(0x57B6E0u ^ w ^ (k << 8));
	sb_stage = SB_WARMUP;
	printf("[predict-sb] armed: warmup=%u K=%u trials=%u\n", w, k, sb_trials);
}

// Synthetic AUTHORITATIVE remote input for frame j: changes every ~4 frames so
// the repeat-last prediction mispredicts and rollbacks fire.
static uint32_t sbAuthRemote(uint32_t j)
{
	static const uint16_t cyc[4] = { 0xFFFF, 0xFEFF, 0xFF7F, 0xFDFF };  // neutral + 3 held
	return 0xFFFF0000u | cyc[(j / 2) % 4];   // changes every 2 frames -> mispredicts fire
}

static void sbRunCheck(uint64_t nowFrame)
{
	using namespace maplecast_rollback;
	if (!pd::canRestore(sb_anchor)) return;
	const uint32_t K = sb_K;
	if (K > 16) return;

	const bool prevFF = settings.input.fastForwardMode, prevMute = settings.aica.muteAudio;
	settings.input.fastForwardMode = true; settings.aica.muteAudio = true;
	const bool prevEnabled = rend_is_enabled();
	g_headless.store(true, std::memory_order_relaxed);

	const int rs = g_localSlot ^ 1;
	const HeldInput base = snapInput();
	auto applyFrame = [&](uint32_t localBtns, uint32_t remoteBtns) {
		kcode[g_localSlot] = localBtns; lt[g_localSlot] = base.lt_[g_localSlot]; rt[g_localSlot] = base.rt_[g_localSlot];
		kcode[rs] = remoteBtns;         lt[rs] = base.lt_[rs];                   rt[rs] = base.rt_[rs];
	};
	const uint32_t localNeutral = 0xFFFFFFFFu;

	// GROUND TRUTH: all-authoritative forward sim; record per-frame hash.
	uint64_t truth[16];
	pd::restore(sb_anchor);
	for (uint32_t j = 0; j < K; j++) {
		applyFrame(localNeutral, sbAuthRemote(j));
		runOneGameFrameHeadless();
		pd::save(sb_anchor + j + 1);
		truth[j] = gameStateRegionHash();
	}

	// PREDICT + RECONCILE. predicted head runs LEAD ahead of confirmed with
	// repeat-last remote; authoritative frames arrive in order and trigger rollback.
	pd::restore(sb_anchor);
	const uint32_t LEAD = (K < 3) ? K : 3;
	uint32_t predRemote[16];
	uint32_t lastConfirmed = 0xFFFF0000u | 0xFFFF;   // neutral
	uint32_t head = 0;                               // frames predicted (0..K)
	auto predictTo = [&](uint32_t target) {
		while (head < target) {
			predRemote[head] = lastConfirmed;
			applyFrame(localNeutral, lastConfirmed);
			runOneGameFrameHeadless();
			pd::save(sb_anchor + head + 1);
			head++;
		}
	};
	uint32_t rollbacks = 0, maxDepth = 0, confirmMatch = 0;
	bool allConfirmOk = true;
	for (uint32_t j = 0; j < K; j++) {
		predictTo(std::min(j + 1 + LEAD, K));               // keep the head LEAD ahead
		const uint32_t auth = sbAuthRemote(j);
		if (auth != predRemote[j]) {                        // MISPREDICT -> rollback
			rollbacks++;
			const uint32_t depth = head - j;                // frames to re-sim
			if (depth > maxDepth) maxDepth = depth;
			pd::restore(sb_anchor + j);                     // rewind to confirmed boundary
			lastConfirmed = auth;
			for (uint32_t f = j; f < head; f++) {           // re-sim tail: auth@j, repeat-last after
				const uint32_t rem = (f == j) ? auth : lastConfirmed;
				predRemote[f] = rem;
				applyFrame(localNeutral, rem);
				runOneGameFrameHeadless();
				pd::save(sb_anchor + f + 1);
			}
		}
		lastConfirmed = auth;                               // frame j now confirmed
		// CONFIRMED-HASH GATE: the confirmed frame's state must == server truth.
		pd::restore(sb_anchor + j + 1);
		const uint64_t ch = gameStateRegionHash();
		sb_confirmChecks++;
		if (ch == truth[j]) { confirmMatch++; sb_confirmMatches++; } else allConfirmOk = false;
		// re-extend the head after the check-rollback
		head = j + 1;
		predictTo(std::min(j + 1 + LEAD, K));
	}

	g_headless.store(false, std::memory_order_relaxed);
	rend_enable_renderer(prevEnabled);
	settings.input.fastForwardMode = prevFF; settings.aica.muteAudio = prevMute;

	const bool ok = allConfirmOk && (rollbacks > 0);       // converged + rollbacks fired
	sb_trialIdx++; if (ok) sb_pass++;
	sb_totalRollbacks += rollbacks; if (maxDepth > sb_maxDepth) sb_maxDepth = maxDepth;
	printf("[predict-sb] TRIAL %u/%u: K=%u -> %s  confirmed-hash=%u/%u matched  rollbacks=%u maxDepth=%u\n",
	       sb_trialIdx, sb_trials, K, ok ? "PASS" : "FAIL", confirmMatch, K, rollbacks, maxDepth);
}

// ═══════════════════════════════════════════════════════════════════════════
// STAGE c — FRAME-STAMPED LOCAL INPUT (kills self-mispredict)
// ═══════════════════════════════════════════════════════════════════════════
// The client KNOWS its own input (it applied it), so its prediction for the LOCAL
// slot is EXACT — the only way the local slot mispredicts is if the server applies
// the input at a DIFFERENT frame than the client did. The frame-stamp (client
// stamps F=predictedFrame+INPUT_DELAY, server applies AT F, honored by
// scheduleStampedInput/publishFrameTick) makes them agree. This gate proves it
// headlessly by running the reconcile with a CHANGING local input under two
// delivery models and confirming BOTH converge to ground truth while ONLY the
// unstamped (arrival-time) model self-mispredicts on the local slot.
enum SCStage { SC_IDLE, SC_WARMUP, SC_RUN, SC_DONE };
static SCStage  sc_stage = SC_IDLE;
static bool     sc_configured = false;
static uint32_t sc_warmup = 0, sc_K = 0, sc_trials = 1, sc_trialIdx = 0, sc_pass = 0;
static uint64_t sc_anchor = 0;
static uint64_t sc_stampedMis = 0, sc_unstampedMis = 0;

static void scConfigure()
{
	if (sc_configured) return;
	sc_configured = true;
	const char* e = std::getenv("MAPLECAST_PREDICT_STAGEC");
	if (!e || !*e) return;
	unsigned w = 0, k = 0, n = 1;
	if (sscanf(e, "%u,%u,%u", &w, &k, &n) < 2 || k == 0) {
		printf("[predict-sc] malformed MAPLECAST_PREDICT_STAGEC='%s'\n", e); return;
	}
	sc_warmup = w; sc_K = k; sc_trials = n ? n : 1;
	if (!pd::init()) return;
	srand(0x57C6E0u ^ w ^ (k << 8));
	sc_stage = SC_WARMUP;
	printf("[predict-sc] armed: warmup=%u K=%u trials=%u delay=%llu\n",
	       w, k, n, (unsigned long long)INPUT_DELAY);
}

// Changing LOCAL input for frame j (active-low); changes every 2 frames.
static uint32_t scLocalInput(uint32_t j)
{
	static const uint16_t cyc[3] = { 0xFFFF, 0xFFBF, 0xFFDF };
	return 0xFFFF0000u | cyc[(j / 2) % 3];
}

static void scRunCheck(uint64_t nowFrame)
{
	using namespace maplecast_rollback;
	if (!pd::canRestore(sc_anchor)) return;
	const uint32_t K = sc_K;
	if (K > 16) return;

	const bool prevFF = settings.input.fastForwardMode, prevMute = settings.aica.muteAudio;
	settings.input.fastForwardMode = true; settings.aica.muteAudio = true;
	const bool prevEnabled = rend_is_enabled();
	g_headless.store(true, std::memory_order_relaxed);

	const int rs = g_localSlot ^ 1;
	const HeldInput base = snapInput();
	const uint32_t remoteNeutral = 0xFFFF0000u | 0xFFFF;
	auto applyFrame = [&](uint32_t localBtns) {
		kcode[g_localSlot] = localBtns; lt[g_localSlot] = base.lt_[g_localSlot]; rt[g_localSlot] = base.rt_[g_localSlot];
		kcode[rs] = remoteNeutral;       lt[rs] = base.lt_[rs];                   rt[rs] = base.rt_[rs];
	};

	// GROUND TRUTH: the client's ACTUAL local input each frame (what it applied).
	uint64_t truth[16];
	pd::restore(sc_anchor);
	for (uint32_t j = 0; j < K; j++) {
		applyFrame(scLocalInput(j));
		runOneGameFrameHeadless();
		pd::save(sc_anchor + j + 1);
		truth[j] = gameStateRegionHash();
	}

	// The client PREDICTS its own local input exactly (it applied scLocalInput(j)).
	// Count local-slot mispredicts = frames where the AUTHORITATIVE local input
	// (as the server delivers it) differs from what the client predicted/applied.
	//   STAMPED  : auth-local[j] = scLocalInput(j)            (server applies AT j)
	//   UNSTAMPED: auth-local[j] = scLocalInput(j - DELAY)    (arrival-time, DELAY late)
	uint32_t stampedMis = 0, unstampedMis = 0;
	for (uint32_t j = 0; j < K; j++) {
		const uint32_t predictedLocal = scLocalInput(j);   // client knows its own input
		const uint32_t authStamped    = scLocalInput(j);
		const uint32_t authUnstamped  = scLocalInput(j >= INPUT_DELAY ? j - (uint32_t)INPUT_DELAY : 0);
		if (authStamped   != predictedLocal) stampedMis++;
		if (authUnstamped != predictedLocal) unstampedMis++;
	}

	// Confirm the STAMPED reconcile converges to ground truth with 0 local rollbacks:
	// predict = apply exact local + neutral remote; auth = same -> confirmed==truth.
	bool convOk = true;
	pd::restore(sc_anchor);
	for (uint32_t j = 0; j < K; j++) {
		applyFrame(scLocalInput(j));
		runOneGameFrameHeadless();
		pd::save(sc_anchor + j + 1);
		if (gameStateRegionHash() != truth[j]) convOk = false;
	}

	g_headless.store(false, std::memory_order_relaxed);
	rend_enable_renderer(prevEnabled);
	settings.input.fastForwardMode = prevFF; settings.aica.muteAudio = prevMute;

	sc_stampedMis += stampedMis; sc_unstampedMis += unstampedMis;
	const bool ok = convOk && (stampedMis == 0) && (unstampedMis > 0);
	sc_trialIdx++; if (ok) sc_pass++;
	printf("[predict-sc] TRIAL %u/%u: K=%u -> %s  local-mispredict stamped=%u unstamped=%u  converge=%s\n",
	       sc_trialIdx, sc_trials, K, ok ? "PASS" : "FAIL", stampedMis, unstampedMis,
	       convOk ? "OK" : "BAD");
}

void onFrameBoundary(uint64_t frame)
{
	if (!active()) return;

	// Primitive gate (MAPLECAST_PREDICT_TEST).
	configure();
	if (s_stage == WARMUP) {
		if (s_warmup > 0) s_warmup--; else runGate();
	}

	// STAGE 0 — page-delta ring (MAPLECAST_PREDICT_STAGE0). Save EVERY live frame.
	s0Configure();
	if (pd::armed) pd::save(frame);
	switch (s0_stage) {
	case S0_WARMUP:
		if (s0_warmup > 0) { s0_warmup--; break; }
		s0_anchor    = frame;
		s0_anchorCtr = *(uint32_t*)&mem_b[0x3496B0];
		s0_anchorHash = maplecast_rollback::gameStateRegionHash();
		s0_stage = S0_RUN;
		break;
	case S0_RUN:
		if (frame >= s0_anchor + s0_K) {
			s0RunCheck(frame);
			if (s0_trialIdx >= s0_trials) {
				printf("[predict-s0] ===== STAGE 0 %s: %u/%u byte-exact page-delta rollbacks, "
				       "avg %.0f us/frame =====\n",
				       (s0_pass == s0_trials) ? "GREEN" : "RED", s0_pass, s0_trials,
				       s0_trials ? s0_totUs / s0_trials : 0.0);
				fflush(stdout);
				s0_stage = S0_DONE;
			} else {
				s0_warmup = 20 + (rand() % 60);
				s0_K = 2 + (rand() % 11);
				s0_stage = S0_WARMUP;
			}
		}
		break;
	default: break;
	}

	// STAGE a — predict primitive (MAPLECAST_PREDICT_STAGEA). Ring saved above.
	saConfigure();
	g_predictedFrame = frame;
	switch (sa_stage) {
	case SA_WARMUP:
		if (sa_warmup > 0) { sa_warmup--; break; }
		sa_anchor = frame;
		sa_anchorHash = maplecast_rollback::gameStateRegionHash();
		sa_stage = SA_RUN;
		break;
	case SA_RUN:
		if (frame >= sa_anchor + sa_K) {
			saRunCheck(frame);
			if (sa_trialIdx >= sa_trials) {
				printf("[predict-sa] ===== STAGE a %s: %u/%u (determ + instant-input + ring) =====\n",
				       (sa_pass == sa_trials) ? "GREEN" : "RED", sa_pass, sa_trials);
				fflush(stdout);
				sa_stage = SA_DONE;
			} else {
				sa_warmup = 20 + (rand() % 60);
				sa_K = 2 + (rand() % 11);
				sa_stage = SA_WARMUP;
			}
		}
		break;
	default: break;
	}

	// STAGE b — reconcile + hash-gate (MAPLECAST_PREDICT_STAGEB). Ring saved above.
	sbConfigure();
	switch (sb_stage) {
	case SB_WARMUP:
		if (sb_warmup > 0) { sb_warmup--; break; }
		sb_anchor = frame;
		sb_stage = SB_RUN;
		break;
	case SB_RUN:
		if (frame >= sb_anchor + sb_K + 4) {   // ensure the ring holds the whole span
			sbRunCheck(frame);
			if (sb_trialIdx >= sb_trials) {
				printf("[predict-sb] ===== STAGE b %s: %u/%u trials converged; confirmed-hash "
				       "%llu/%llu matched; %llu rollbacks (maxDepth %llu); genuine-mismatch=%llu =====\n",
				       (sb_pass == sb_trials && sb_confirmMatches == sb_confirmChecks) ? "GREEN" : "RED",
				       sb_pass, sb_trials,
				       (unsigned long long)sb_confirmMatches, (unsigned long long)sb_confirmChecks,
				       (unsigned long long)sb_totalRollbacks, (unsigned long long)sb_maxDepth,
				       (unsigned long long)(sb_confirmChecks - sb_confirmMatches));
				fflush(stdout);
				sb_stage = SB_DONE;
			} else {
				sb_warmup = 20 + (rand() % 60);
				sb_K = 4 + (rand() % 9);
				sb_stage = SB_WARMUP;
			}
		}
		break;
	default: break;
	}

	// STAGE c — frame-stamped local input (MAPLECAST_PREDICT_STAGEC). Ring saved above.
	scConfigure();
	switch (sc_stage) {
	case SC_WARMUP:
		if (sc_warmup > 0) { sc_warmup--; break; }
		sc_anchor = frame;
		sc_stage = SC_RUN;
		break;
	case SC_RUN:
		if (frame >= sc_anchor + sc_K + 4) {
			scRunCheck(frame);
			if (sc_trialIdx >= sc_trials) {
				printf("[predict-sc] ===== STAGE c %s: %u/%u; local-mispredict TOTAL stamped=%llu "
				       "unstamped=%llu (frame-stamp eliminates self-mispredict) =====\n",
				       (sc_pass == sc_trials && sc_stampedMis == 0 && sc_unstampedMis > 0) ? "GREEN" : "RED",
				       sc_pass, sc_trials,
				       (unsigned long long)sc_stampedMis, (unsigned long long)sc_unstampedMis);
				fflush(stdout);
				sc_stage = SC_DONE;
			} else {
				sc_warmup = 20 + (rand() % 60);
				sc_K = 4 + (rand() % 9);
				sc_stage = SC_WARMUP;
			}
		}
		break;
	default: break;
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
		g0_anchorFrameCtr = *(uint32_t*)&mem_b[0x3496B0];
		g0_anchorFightTick = mem_b[0x268250];
		// The decisive experiment advances ~3K SH4 frames then restores; that can
		// perturb the live baseline (hashB). Gate it OFF (MAPLECAST_PREDICT_DECISIVE=1
		// to enable) so g0RunCheck measures a CLEAN, undisturbed live continuation.
		if (g0_nA && std::getenv("MAPLECAST_PREDICT_DECISIVE")) g0DecisiveExperiment();
		g0_stage = (g0_nA ? G0_RUN : G0_DONE);
		break;
	case G0_RUN:
		// Wait until the live client has normally advanced K frames past F,
		// recording inputs along the way, then run the restore-into-live check.
		if (frame >= g0_anchorFrame + g0_K) {
			g0RunCheck(frame);
			g0_trialIdx++;
			if (g0_pass) g0_passCount++;
			printf("[predict-gate0] TRIAL %u/%u: K=%u -> %s  %.0f us/frame (%.1f ms total)  (running %u/%u byte-exact)\n",
			       g0_trialIdx, g0_trials, g0_K, g0_pass ? "PASS" : "FAIL",
			       g0_lastUsPerFrame, g0_lastUsPerFrame * g0_K / 1000.0,
			       g0_passCount, g0_trialIdx);
			if (g0_trialIdx >= g0_trials) {
				printf("[predict-gate0] ===== RE-GATE COMPLETE: %u/%u byte-exact over %u LIVE "
				       "random (N,K) trials ; avg %.0f us/frame muted =====\n",
				       g0_passCount, g0_trials, g0_trials,
				       g0_trials ? g0_totUsPerFrame / g0_trials : 0.0);
				printf("[predict-gate0] ===== GATE 0 %s =====\n",
				       (g0_passCount == g0_trials) ? "GREEN" : "RED");
				fflush(stdout);
				g0_stage = G0_MONITOR;
			} else {
				// Re-arm for the next trial: random warmup gap (30..120) and random
				// K (2..12) so we cover many (restore-frame N, re-sim K) pairs LIVE.
				g0_warmup = 30 + (rand() % 91);
				g0_K = 2 + (rand() % 11);
				g0_stage = G0_WARMUP;
			}
		}
		break;
	default: break;
	}
}

bool testDone()   { return s_stage == DONE; }
bool testPassed() { return s_pass; }

uint64_t predictedFrame() { return g_predictedFrame; }

// ── CAPSTONE — live predict-drive API (thin wrappers over the pd:: ring) ─────
bool liveActive()
{
	static int cached = -1;
	if (cached < 0) cached = (std::getenv("MAPLECAST_PREDICT_LIVE") != nullptr) ? 1 : 0;
	return cached && active();
}
bool liveInit()              { return pd::init(); }
bool ringSave(uint64_t f)    { if (!pd::armed) return false; pd::save(f); return true; }
bool ringRestore(uint64_t f) { return pd::restore(f); }
bool ringHas(uint64_t f)     { return pd::canRestore(f); }
uint64_t ringOldest()        { return pd::oldest; }
uint64_t ringMostRecent()    { return pd::mostRecent; }
void setPredictedFrame(uint64_t f) { g_predictedFrame = f; }

void advanceHeadlessOneFrame()
{
	const bool prevFF = settings.input.fastForwardMode, prevMute = settings.aica.muteAudio;
	settings.input.fastForwardMode = true; settings.aica.muteAudio = true;
	// Advance ONE video frame via the EXACT byte-perfect per-frame body the normal emu loop
	// (and pure lockstep) use — Emulator::runRollbackFrame() = runner.init() + the SH-4 run,
	// stopping at the normal present() boundary. The OLD code did a bare getSh4Executor()->Run(),
	// which SKIPS runner.init() + the frame setup, so the re-sim diverged from the authoritative
	// server cross-instance — even at IDLE with 0 rollbacks (pure lockstep, which goes through
	// runInternal, matched the server 260/260). This is why flycast's own GGPO advances with
	// emu.run(), not a bare Run. Reference: the pure-lockstep byte-perfect path. (2026-07-24)
	emu.runRollbackFrame();
	settings.input.fastForwardMode = prevFF; settings.aica.muteAudio = prevMute;
}

}
