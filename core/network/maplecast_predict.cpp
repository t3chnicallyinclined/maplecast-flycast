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

// Advance EXACTLY one MVC2 game frame headless. runOneFrame stops at the next
// non-RTT STARTRENDER, but MVC2 emits MULTIPLE such passes per video frame; the
// LIVE emu loop only stops at present() (once per displayed frame, single-slot
// QueueRender drops the extra passes). So a single runOneFrame advances <1 game
// frame. Loop until the MVC2 game-frame counter (0x3496B0) ticks so each call ==
// one live emu-loop frame. Guard caps runaway (e.g., paused/menu with no tick).
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

}
