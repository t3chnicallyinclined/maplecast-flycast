/*
	MapleCast Mirror v3  --  stream TA command buffers + memory diffs.

	Instead of streaming pre-parsed rend_context (which loses texture resolution),
	stream the RAW TA command buffer. The client runs ta_parse() on it, which
	builds rend_context AND resolves textures from VRAM  --  exactly like flycast
	normally works.

	Server: each frame, captures the TA command buffer + PVR registers + memory diffs
	Client: loads server sync state, then applies diffs + feeds TA commands to renderer
*/
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <cmath>
#include "types.h"
#include "maplecast_mirror.h"
#include "hub_discovery.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/aica/aica_if.h"
#ifndef MAPLECAST_HEADLESS_BUILD
#include "rend/gles/gles.h"
#endif
#include "rend/TexCache.h"
#include "serialize.h"
#include "emulator.h"
#include "hw/mem/mem_watch.h"
#include "maplecast_ws_server.h"
#include "maplecast_audio_ws.h"
#include "maplecast_state_sync.h"
#include "maplecast_lockstep.h"
#include "maplecast_input_server.h"
#include "replay_writer.h"
#include "maplecast_rollback.h"
#include "maplecast_audio_client.h"
#include "maplecast_input_sink.h"
#include "maplecast_control_ws.h"
#include "maplecast_compress.h"
#include "gsta_stage.h"        // native MVC2 stage renderer (global-namespace decls)
#include "gsta_charpass.h"     // Phase 2a: native char-pass driver (global-namespace decls)
#ifdef MAPLECAST_GSTA_CLIENT_BUILD
#include "gsta_render_debug.h" // live render-debug globals (control-WS); MUST be at GLOBAL scope
                               // (the namespace maplecast_mirror opens at line ~287, so including
                               // this inside it would nest gsta_render_debug -> link/lookup errors).
#endif

// Reserved for future palette bank probe (see NOTE in serverPublish).
uint64_t g_activePalBanks = 0;
#include "rend/texconv.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>
#include <mutex>
#include <thread>
#include <deque>
#include <chrono>
#include <random>          // Tele-0.10
#include <curl/curl.h>     // Tele-0.10
#include "net_platform.h"
#include "maplecast_compat.h"
#ifndef _WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#include <netinet/tcp.h>
#endif
#include "maplecast_gamestate.h"
#include "maplecast_oracle_hook.h"
#include <errno.h>


extern Renderer* renderer;
extern bool pal_needs_update;

namespace maplecast_mirror
{

static const char* SHM_NAME = "/maplecast_mirror";
static const size_t HEADER_SIZE = 4096;
static const size_t BRAIN_SIZE = 32 * 1024 * 1024;
static const size_t RING_START = HEADER_SIZE + BRAIN_SIZE;
static const size_t SHM_SIZE = RING_START + 128 * 1024 * 1024;
static const size_t RING_SIZE = SHM_SIZE - RING_START;
static const size_t MEM_PAGE_SIZE = 4096;

static bool _isServer = false;
static bool _isClient = false;
static uint8_t* _shmPtr = nullptr;
static int _shmFd = -1;

// Shadow copies for diff
static uint8_t* _shadowRAM = nullptr;
static uint8_t* _shadowVRAM = nullptr;
static uint8_t* _shadowARAM = nullptr;

struct RingHeader {
	volatile uint64_t write_pos;
	volatile uint64_t frame_count;
	volatile uint64_t latest_offset;
	volatile uint32_t latest_size;
	volatile uint32_t client_request_sync;
	volatile uint32_t sync_ready;
	volatile uint64_t server_vram_hash;     // server's VRAM hash for client to verify
	uint8_t pad[4096 - 44];
};

static uint64_t _clientFrameCount = 0;
static bool _clientNeedsFullSync = true;

// Forward-declared helper used by client telemetry updates further up
// in the file; defined next to the render-path client code below.
static int64_t _clientNowUs();

// ---- Client telemetry (consumed by the ImGui debug overlay) ----
// All atomic so the overlay can snapshot them lock-free. Updated once per
// frame from clientReceive() below (video path) and from wsClientRun() /
// wsReadFrame (arrival timing).
static std::atomic<uint64_t> _clientPacketsReceived{0};
static std::atomic<uint64_t> _clientBytesReceived{0};
static std::atomic<int64_t>  _clientLastDecodeUs{0};
static std::atomic<int64_t>  _clientDecodeEmaUs{0};
// Tele-0.5: max decode_us within the current reporting window. Reset
// to 0 by the stats thread after each push to the server.
static std::atomic<int64_t>  _clientDecodeMaxUs{0};
// Tele-0.10: counters used by the HTTP-POST stats reporter to derive
// fps + sync rate over each 1s window. Reset to 0 by the reporter.
static std::atomic<uint64_t> _clientFramesDecoded{0};
static std::atomic<uint64_t> _clientSyncCount{0};
static std::atomic<uint32_t> _clientLastDirtyPages{0};
static std::atomic<uint32_t> _clientLastTaSize{0};
static std::atomic<bool>     _clientLastVramDirty{false};
static std::atomic<int64_t>  _clientLastArrivalUs{0};
static std::atomic<int64_t>  _clientArrivalEmaUs{0};
static std::atomic<int64_t>  _clientArrivalMaxUs{0};
static std::atomic<bool>     _clientWsConnected{false};

// Game state received from server (for overlay/HUD)
static maplecast_gamestate::GameState _clientGameState{};
static std::mutex _clientGameStateMtx;
static std::atomic<bool> _clientGameStateReady{false};

// GSTA-ONLY mode (state-replica): the mirror WS is used purely as a transport
// for GSTA/OBJF state packets. The local SH4 owns the render, so we must NOT
// apply the server's TA delta or VRAM/PVR SYNC (they would clobber the local
// game -> black screen). When set, wsClientRun skips every TA/SYNC/VRAM apply,
// parsing ONLY GSTA + OBJF.
static std::atomic<bool> _gstaOnly{false};
// When true (state-replica with vramSync=true), wsClientRun still waits for and
// applies the initial SYNC frame (VRAM + PVR) to seed local textures from prod,
// then switches to GSTA-only for all subsequent frames.
static std::atomic<bool> _gstaVramSync{false};

// Full object pool received from server (OBJF) — for the state-replica inject.
static maplecast_gamestate::ObjectState _clientObjects[48];
static int _clientObjectCount = 0;
static std::mutex _clientObjectsMtx;
static std::atomic<bool> _clientObjectsReady{false};

// MCSV mid-match join: server ships the full dc_serialize blob when the client
// connects while in_match is active. frameInject() drains this and calls
// dc_loadstate_from_memory() so the local SH4 enters the fight from a known-good
// state rather than waiting to reach in_match=1 on its own.
static std::vector<uint8_t> _pendingSaveState;
static std::mutex            _pendingSaveStateMtx;
static std::atomic<bool>     _pendingSaveStateReady{false};

// Most-recently-received SYNC frame VRAM + PVR snapshot. Re-applied by
// reapplyLastSyncVram() after dc_loadstate_from_memory() so the MCSV's
// match-start VRAM doesn't clobber the server's current texture state.
static std::vector<uint8_t> _lastSyncVram;
static std::vector<uint8_t> _lastSyncPvr;

// Fast hash for VRAM comparison (sample every 64th byte for speed)
static uint64_t fastVramHash()
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < VRAM_SIZE; i += 64) {
		h ^= vram[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

struct MemRegion {
	uint8_t* ptr;
	uint8_t* shadow;
	size_t size;
	uint8_t id;
	const char* name;
};
static MemRegion _regions[4];
static int _numRegions = 0;

// ==================== DMA-write force-dirty bitmap ====================
//
// memcmp against a shadow copy misses VRAM writes that arrive via DMA paths
// (Ch2 DMA, PVR DMA, TAWriteSQ 64-bit, ELAN texture DMA, YUV converter).
// Those paths memcpy directly into vram[] without tripping the page-protect
// SIGSEGV handler. The shadow copy gets updated to the new content too, so
// when serverPublish() runs memcmp the next frame the page looks unchanged
// and never streams to clients  --  they keep their stale texture.
//
// Fix: DMA write paths call markVramDirty(off, size) to set bits in this
// bitmap. serverPublish() drains it in addition to running memcmp.
//
// Max VRAM is 8MB on Dreamcast (Naomi has the same). 8MB / 4KB pages =
// 2048 pages = 32 uint64 words. We size for the maximum because VRAM_SIZE
// is a runtime value (settings.platform.vram_size), not constexpr.
// Lock-free atomic word fetch_or; serverPublish swaps with 0.
static constexpr size_t VRAM_MAX_BYTES   = 8 * 1024 * 1024;
static constexpr size_t VRAM_BITMAP_WORDS = (VRAM_MAX_BYTES / MEM_PAGE_SIZE + 63) / 64;
static std::atomic<uint64_t> _vramDirtyBitmap[VRAM_BITMAP_WORDS];

void markVramDirty(uint32_t offset, uint32_t size)
{
	// Hot path  --  bail before any work when no mirror server is running.
	// `_isServer` becomes true once initServer() finishes; before that the
	// bitmap is unallocated and DMA paths must not touch it.
	if (!_isServer || size == 0) return;
	if (offset >= VRAM_SIZE) return;
	uint32_t startPage = offset / MEM_PAGE_SIZE;
	uint32_t endPage   = (offset + size - 1) / MEM_PAGE_SIZE;
	if (endPage >= VRAM_SIZE / MEM_PAGE_SIZE) endPage = VRAM_SIZE / MEM_PAGE_SIZE - 1;
	for (uint32_t p = startPage; p <= endPage; p++) {
		_vramDirtyBitmap[p >> 6].fetch_or(1ULL << (p & 63), std::memory_order_relaxed);
	}
}

// Set by requestSyncBroadcast() (any thread). Drained by serverPublish() on
// the render thread, which builds & broadcasts the SYNC there to avoid
// touching vram[] mid-update from another thread.
static std::atomic<bool> _forceSyncBroadcast{false};

// Phase A  --  frame counter + monotonic-clock stamp of the most recent
// serverPublish() call. Mirror the existing hdr->frame_count++ into a
// std::atomic so the input latch path (ggpo::getLocalInput) can read the
// current frame number cheaply without touching the shm header. Also stamp
// the wall-clock time of the latest publish so the latch can compute
// "frames-since-now" and "ms-since-last-frame-published" for telemetry +
// the frame_phase block in status JSON. Both updated under the same
// memory_order_release at the bottom of serverPublish().
static std::atomic<uint64_t> _atomicCurrentFrame{0};
static std::atomic<int64_t> _atomicLastLatchTimeUs{0};

// Phase B  --  live frame period in microseconds, smoothed across the last
// few publishes via an exponential moving average. PVR can run slightly
// off 60 Hz; this gives the browser-side phase-aligner an accurate
// "next vblank in N Âµs" estimate. Initialized to a sane default (16670 Âµs
// = 60 fps) so the first few publishes have a reasonable starting value.
static std::atomic<int64_t> _atomicFramePeriodUs{16670};

static inline int64_t _publishNowUs() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void requestSyncBroadcast()
{
	if (!_isServer) return;
	_forceSyncBroadcast.store(true, std::memory_order_relaxed);
}

// Set by requestFullSaveStateBroadcast()  --  same drain pattern. The
// serverPublish drain calls maplecast_ws::broadcastFullSaveState() which
// builds the dc_serialize blob, compresses it, and ships it to all
// connected clients as a "SAVE" envelope.
static std::atomic<bool> _forceFullSaveStateBroadcast{false};

void requestFullSaveStateBroadcast()
{
	if (!_isServer) return;
	_forceFullSaveStateBroadcast.store(true, std::memory_order_relaxed);
}

}  // namespace maplecast_mirror

// C wrapper so the SIGUSR1 handler in core/linux/common.cpp can call this
// without dragging in the C++ namespace declaration via the mirror header.
extern "C" void maplecast_mirror_request_full_save_state()
{
	maplecast_mirror::requestFullSaveStateBroadcast();
}

namespace maplecast_mirror
{

static bool openShm(bool create)
{
#ifdef _WIN32
	// Linux uses /dev/shm so a separate relay process can read the ring
	// buffer without TCP. Windows has no relay process and no /dev/shm,
	// but the rest of the mirror server (serverPublish ring writes, WS
	// broadcast) still expects _shmPtr to be a valid buffer of SHM_SIZE.
	// Allocate a private heap buffer instead. Same in-process layout, no
	// cross-process sharing (which we don't need on Windows for V1).
	(void)create;
	if (_shmPtr == nullptr) {
		_shmPtr = (uint8_t*)malloc(SHM_SIZE);
		if (_shmPtr == nullptr) {
			printf("[MIRROR] heap alloc for SHM_SIZE failed on Windows\n");
			return false;
		}
		memset(_shmPtr, 0, SHM_SIZE);
		printf("[MIRROR] Windows: SHM backed by private heap (no cross-process share)\n");
	}
	return true;
#else
	if (create) shm_unlink(SHM_NAME);
	_shmFd = shm_open(SHM_NAME, create ? (O_CREAT | O_RDWR) : O_RDWR, 0666);
	if (_shmFd < 0) { printf("[MIRROR] shm_open failed\n"); return false; }
	if (create) ftruncate(_shmFd, SHM_SIZE);
	_shmPtr = (uint8_t*)mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, _shmFd, 0);
	if (_shmPtr == MAP_FAILED) { _shmPtr = nullptr; return false; }
	if (create) memset(_shmPtr, 0, SHM_SIZE);
	return true;
#endif
}

static void initRegions()
{
	_numRegions = 0;

	// SKIP RAM  --  renderer doesn't read from main RAM
	// SKIP ARAM  --  audio RAM not needed for rendering
	// ONLY diff VRAM (textures) and PVR regs (palette, fog, hardware state)

	_shadowVRAM = (uint8_t*)malloc(VRAM_SIZE);
	memcpy(_shadowVRAM, &vram[0], VRAM_SIZE);
	_regions[_numRegions++] = { &vram[0], _shadowVRAM, VRAM_SIZE, 1, "VRAM" };

	// PVR registers: 32KB  --  palette RAM, FOG_TABLE, ISP_FEED_CFG
	static uint8_t* _shadowPVR = nullptr;
	_shadowPVR = (uint8_t*)malloc(pvr_RegSize);
	memcpy(_shadowPVR, pvr_regs, pvr_RegSize);
	_regions[_numRegions++] = { pvr_regs, _shadowPVR, (size_t)pvr_RegSize, 3, "PVR" };

	// Only 2 regions: VRAM + PVR (no RAM, no ARAM)
}

static void serverSaveSync()
{
	const char* syncPath = "/dev/shm/maplecast_sync.state";
	Serializer ser;
	dc_serialize(ser);
	void* data = malloc(ser.size());
	if (!data) return;
	ser = Serializer(data, ser.size());
	dc_serialize(ser);
	FILE* f = fopen(syncPath, "wb");
	if (f) { fwrite(data, 1, ser.size(), f); fclose(f); }
	free(data);
	printf("[MIRROR] Sync state saved: %.1f MB\n", ser.size() / (1024.0*1024.0));
}

// Public wrapper: call serverSaveSync() then broadcast the resulting file
// to all WS clients wrapped in a "SAVE" envelope. Trigger via SIGUSR1.
void doForcedSaveStateBroadcast()
{
	if (!_isServer) return;

	// 1. Run the same function that produces the on-disk save state
	serverSaveSync();

	// 2. Read the file back
	FILE* f = fopen("/dev/shm/maplecast_sync.state", "rb");
	if (!f) { printf("[MIRROR] forced sync: failed to read save state\n"); return; }
	fseek(f, 0, SEEK_END);
	size_t fileSize = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> fileBuf(fileSize);
	if (fread(fileBuf.data(), 1, fileSize, f) != fileSize) {
		printf("[MIRROR] forced sync: short read\n");
		fclose(f);
		return;
	}
	fclose(f);

	// 3. Wrap in "SAVE" envelope: "SAVE"(4) + uncompSize(4) + bytes
	std::vector<uint8_t> wrapped(8 + fileSize);
	memcpy(wrapped.data(), "SAVE", 4);
	uint32_t fs = (uint32_t)fileSize;
	memcpy(wrapped.data() + 4, &fs, 4);
	memcpy(wrapped.data() + 8, fileBuf.data(), fileSize);

	// 4. Broadcast via WS server
	maplecast_ws::broadcastSaveStateBytes(wrapped.data(), wrapped.size());

	printf("[MIRROR] forced save state broadcast: %.1f MB raw\n",
		fileSize / (1024.0 * 1024.0));
}

// Public API: serialize the full DC state into a freshly malloc'd buffer.
// Caller owns the returned pointer and must free() it. Returns nullptr on
// failure. This is the SAME data serverSaveSync() writes to disk  --  useful
// for shipping the full save state over the wire to test theories about
// what data the WASM client is missing.
uint8_t* buildFullSaveState(size_t& outSize)
{
	// Fixed-allocation pattern. The earlier dry-run-then-real-run
	// approach was racy: `dc_serialize` size differs between back-to-
	// back calls on a live emu (length-prefixed dynamic arrays and
	// `Serializer::skip()` reservations don't match between dry and
	// real runs), which made the old code hit "size mismatch" on
	// every frame. We solve it by allocating generously, serializing
	// once, catching any overflow, and trusting `ser.size()` as the
	// authoritative used length.
	//
	// IMPORTANT: rollback=false. GGPO uses rollback=true at
	// core/network/ggpo.cpp:425, but in that mode several modules
	// (AICA RAM, SH4 MMR cache, PVR, Elan) deliberately SKIP themselves
	// on the assumption GGPO's memwatch::PageMap delta cache will
	// restore them separately. We have no such cache  --  we ship a
	// standalone full state that must be self-contained on the wire.
	// Setting rollback=true here would produce a ~500KB blob that
	// looks valid to the Deserializer but leaves AICA/MMR/PVR stale,
	// then crashes the SH4 a few seconds after load with
	// "SH4 exception when blocked".
	//
	// 40 MB covers both DC (real ~28 MB) and Naomi (~28-32 MB) with
	// headroom. The wasted tail is touched only by the downstream
	// compressor and elides to ~nothing in the wire payload.
	outSize = 0;
	constexpr size_t kAllocSize = 40 * 1024 * 1024;
	uint8_t* data = (uint8_t*)malloc(kAllocSize);
	if (!data) {
		printf("[MIRROR] buildFullSaveState malloc(%zu) failed\n", kAllocSize);
		return nullptr;
	}
	try {
		Serializer ser(data, kAllocSize, /*rollback=*/false);
		dc_serialize(ser);
		outSize = ser.size();
		return data;
	} catch (const Serializer::Exception& e) {
		printf("[MIRROR] buildFullSaveState serializer overflow (%zu-byte buffer): %s\n",
		       kAllocSize, e.what());
		free(data);
		return nullptr;
	} catch (const std::exception& e) {
		printf("[MIRROR] buildFullSaveState exception: %s\n", e.what());
		free(data);
		return nullptr;
	}
}

// Double-buffered TA for zero-copy delta (replaces std::vector prevTA)
static uint8_t* _taBuf[2] = { nullptr, nullptr };
static uint32_t _taBufSize[2] = { 0, 0 };
static int _taCur = 0;
static bool _taHasPrev = false;
static MirrorCompressor _compressor;

// === MAPLECAST_ZSTREAM (TA-Wire v2 Phase 1, docs/TA-WIRE-V2-PLAN.md) ===
// Streaming-zstd envelope for delta frames: ONE persistent ZSTD_CStream whose
// window (wlog=24, 16MiB) spans prior frames, flushed once per frame. The
// cross-frame redundancy (dup pages, 92%-identical keyframes, idle-loop TA
// periodicity) is invisible to the per-frame ZCST compressor and free here —
// measured 6.87 -> 1.93 Mbps at ~0.11 ms/frame (docs/render-state/07).
// Wire msg: 'ZCS2'(4) epoch(1) flags(1: bit0=stream-start) innerSize(4 LE)
//           + one zstd-frame chunk (decoder: feed chunks of one long zstd
//           frame to a streaming decompressor; each msg yields exactly
//           innerSize bytes = one legacy UNCOMPRESSED inner delta frame).
// Stream resets (new epoch, flags bit0): on every fresh-SYNC broadcast (so a
// joiner decodes from its SYNC onward) and every MAPLECAST_ZSTREAM_RESET
// frames (default 300; 0=never). SYNC itself stays legacy ZCST (one-shot).
// Env-gated MAPLECAST_ZSTREAM=1, default OFF — legacy ZCST path unchanged.
static std::atomic<bool> _zstreamResetPending{true};   // true => first frame is stream-start

// === MAPLECAST_TACANON (TA-Wire v2 Phase 1.5, docs/TA-WIRE-V2-PLAN.md) ======
// Dead-byte canonicalization: zero the TA byte ranges NO parser reads (PVR HW
// spec "ignored" fields + the four-parser read set) in the WIRE copy before the
// delta diff, so engine staging-buffer scratch patterns stop churning the wire.
// MEASURED (_bwlab/STAGE-SHARE-REPORT.md + real-play re-run): 72% of idle churn /
// 62% of in-play stage churn is these bytes; canonicalization = -17.3% wire on
// real gameplay, server-only, no wire-format or client change.
// Classes (validated vs FrameDecoder+TAParser md5 11/11, byte-exact run rebuild
// 5257/5257 — _bwlab/stage_share.py DEAD_RANGES):
//   eol    paraType-0 control block: bytes 4..32 (reserved)
//   p32c01 32B poly param, colType 0/1, !volume: bytes 16..32 (unread tail)
//   sprp   sprite param: bytes 24..32 (DMA bookkeeping)
//   sprv   64B sprite vertex: bytes 48..52
//   v64pad 64B textured floating-color vertex (colType 1, !vol): bytes 24..32
//   v32nt0 32B non-tex packed-color vertex (colType 0): bytes 16..24 + 28..32
// =measure counts (no mutation); =1 zeroes. Stats -> [TACANON] every 600 frames.
static uint64_t _tacanonDead = 0, _tacanonFrames = 0;
static int tacanonMode()
{
	static const int m = [](){
		const char* e = std::getenv("MAPLECAST_TACANON");
		if (!e || !*e || *e == '0') return 0;
		return (*e == 'm' || *e == 'M') ? 1 : 2;
	}();
	return m;
}
static void taCanonicalize(uint8_t* ta, uint32_t taSize, bool zero)
{
	auto killRange = [&](uint32_t lo, uint32_t hi){
		if (hi > taSize) hi = taSize;
		if (lo >= hi) return;
		_tacanonDead += hi - lo;
		if (zero) memset(ta + lo, 0, hi - lo);
	};
	uint32_t off = 0;
	int  curList = -1;
	bool inPolyList = false, isSpr = false, haveParam = false;
	uint8_t cObj = 0;
	while (off + 32 <= taSize) {
		uint32_t pcw; memcpy(&pcw, ta + off, 4);
		uint32_t paraType = (pcw >> 29) & 7;
		if (paraType == 0 || paraType == 1 || paraType == 2 || paraType == 3 || paraType == 6) {
			haveParam = false;
			if (paraType == 0) { curList = -1; inPolyList = false; killRange(off + 4, off + 32); }
			off += 32; continue;
		}
		if (paraType == 4) {   // polygon param
			uint32_t lt = (pcw >> 24) & 7;
			if (curList == -1) { curList = (int)lt; inPolyList = (lt == 0 || lt == 2 || lt == 4); }
			if (curList == 1 || curList == 3) { haveParam = false; off += 32; continue; }  // modvol
			cObj = (uint8_t)(pcw & 0xFF);
			isSpr = false; haveParam = true;
			uint32_t colType = (cObj >> 4) & 3, vol = (cObj >> 6) & 1;
			uint32_t sz;
			if (colType == 2 && !vol && ((cObj >> 2) & 1)) sz = (off + 64 <= taSize) ? 64 : 32;
			else if (colType >= 1 && vol)                  sz = (off + 64 <= taSize) ? 64 : 32;
			else                                           sz = 32;
			if (sz == 32 && (colType == 0 || colType == 1) && !vol) killRange(off + 16, off + 32);
			off += sz; continue;
		}
		if (paraType == 5) {   // sprite param
			uint32_t lt = (pcw >> 24) & 7;
			if (curList == -1) { curList = (int)lt; inPolyList = (lt == 0 || lt == 2 || lt == 4); }
			cObj = (uint8_t)(pcw & 0xFF);
			isSpr = true; haveParam = true;
			killRange(off + 24, off + 32);
			off += 32; continue;
		}
		if (paraType == 7) {   // vertex
			if (!inPolyList || !haveParam) { off += 32; continue; }
			uint32_t tex = (cObj >> 3) & 1, colType = (cObj >> 4) & 3, vol = (cObj >> 6) & 1;
			if (isSpr && off + 64 <= taSize) { killRange(off + 48, off + 52); off += 64; continue; }
			uint32_t sz;
			if (!tex) {
				sz = 32;
				if (colType == 0) { killRange(off + 16, off + 24); killRange(off + 28, off + 32); }
			} else if (!vol) {
				sz = (colType == 1 && off + 64 <= taSize) ? 64 : 32;
				if (sz == 64) killRange(off + 24, off + 32);
			} else sz = 32;
			off += sz; continue;
		}
		off += 32;
	}
}

static bool zstreamEnabled()
{
	static const bool on = [](){ const char* e = std::getenv("MAPLECAST_ZSTREAM");
		return e && *e && *e != '0'; }();
	return on;
}

void initServer()
{
	// Lockstep: serverPublish (which builds the JOIN snapshot AND computes the
	// game-state checksum) runs on the RENDER thread when ThreadedRendering is
	// on — concurrently with the SH4 emu thread mutating mem_b. That makes the
	// JOIN and the shipped hash capture DIFFERENT, racing SH4 states, so a
	// lockstep client's restored JOIN never matches that frame's hash (endless
	// resync). Forcing single-threaded rendering makes serverPublish run
	// synchronously on the emu thread at STARTRENDER (SH4 quiescent), so the
	// JOIN + hash are atomic and mutually consistent. Must be set before the
	// emu/render threads spawn (initServer runs during Emulator::start setup).
	if (maplecast_lockstep::active()) {
		config::ThreadedRendering.override(false);
		printf("[MIRROR] lockstep: forcing single-threaded rendering so JOIN+hash "
		       "are atomic w.r.t. the SH4\n");
	}

	if (!openShm(true)) return;
	_isServer = true;
	initRegions();
	RingHeader* hdr = (RingHeader*)_shmPtr;
	hdr->write_pos = 0;
	hdr->frame_count = 0;
	hdr->latest_offset = 0;
	hdr->latest_size = 0;
	serverSaveSync();

	for (int i = 0; i < _numRegions; i++)
		memcpy(_regions[i].shadow, _regions[i].ptr, _regions[i].size);

	// Allocate TA double buffers
	for (int i = 0; i < 2; i++) {
		_taBuf[i] = (uint8_t*)malloc(256 * 1024);
		_taBufSize[i] = 0;
	}
	_taCur = 0;
	_taHasPrev = false;

	// zstd compression for WebSocket broadcast
	_compressor.init(256 * 1024);

	// Start lightweight WebSocket server  --  no CUDA, no NVENC
	int wsPort = 7200;
	const char* portEnv = std::getenv("MAPLECAST_SERVER_PORT");
	if (portEnv) wsPort = std::atoi(portEnv);
	maplecast_ws::init(wsPort);

	// Dedicated audio-only WebSocket server on its own port + io_service
	// thread. Keeps PCM audio packets off the TA mirror socket entirely so
	// video frames never contend for TCP ordering or asio event-loop time.
	// See maplecast_audio_ws.h for the full rationale.
	int audioWsPort = wsPort + 3;  // default: 7203 alongside 7200, 7213 alongside 7210
	const char* audioPortEnv = std::getenv("MAPLECAST_AUDIO_WS_PORT");
	if (audioPortEnv) audioWsPort = std::atoi(audioPortEnv);
	maplecast_audio_ws::init(audioWsPort);

	// Phase 3 of lockstep-player-client: start the state-sync TCP listener
	// so native player clients can subscribe to periodic dc_serialize
	// snapshots. Failure to start is non-fatal  --  the TA mirror still works.
	maplecast_state_sync::serverStart();

	// Lockstep-mirror game-state-hash channel (env-gated MAPLECAST_LOCKSTEP=1,
	// default OFF). Ships the deterministic game-state-region checksum to
	// native lockstep clients every MAPLECAST_LOCKSTEP_INTERVAL frames so they
	// can verify parity + resync. No-op / not bound unless enabled.
	maplecast_lockstep::serverStart();

	printf("[MIRROR] === SERVER MODE === streaming TA + memory diffs\n");
}

static void clientLoadSync()
{
	const char* syncPath = "/dev/shm/maplecast_sync.state";
	FILE* f = fopen(syncPath, "rb");
	if (!f) { printf("[MIRROR] No sync state\n"); return; }
	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* data = malloc(size);
	if (!data) { fclose(f); return; }
	fread(data, 1, size, f);
	fclose(f);
	Deserializer deser(data, size);
	emu.loadstate(deser);
	free(data);
	// loadstate re-protects VRAM  --  unprotect so our memcpy patches work
	memwatch::unprotect();
	printf("[MIRROR] Loaded server sync state: %.1f MB\n", size / (1024.0*1024.0));
}

static void initClientWebSocket();  // forward declaration

#ifdef MAPLECAST_GSTA_CLIENT_BUILD
static void initGstaClient();   // forward decl (GSTA section below)
bool gstaModeActive();
#endif

void initClient()
{
	// Idempotent  --  if already initialized, don't start a second WS thread.
	// This happens when flycast GUI settings change triggers stop()+start().
	if (_isClient) return;

#ifdef MAPLECAST_GSTA_CLIENT_BUILD
	// === NATIVE GSTA CLIENT (feat/render-replica-live) ========================
	// Opt-in: MAPLECAST_GSTA_CLIENT=1 connects to the replica-live GSTA wire
	// (7212) and renders it through flycast's OWN renderer via the transpiled
	// render_frame. Completely separate from the TA-mirror (7200) path below —
	// it sets _isClient so the mirror render loop runs, but routes to
	// clientReceiveGsta() instead of clientReceive(). See the GSTA section.
	if (const char* g = std::getenv("MAPLECAST_GSTA_CLIENT"); g && *g && *g != '0') {
		initGstaClient();
		return;
	}
#endif

	// Use WebSocket if:
	//   - MAPLECAST_SERVER_HOST is set (explicit host override), or
	//   - MAPLECAST_HUB_URL is set (want hub-aware discovery), or
	//   - shm_open fails (no local flycast-server on this machine)
	// Hub discovery only runs inside the WS path, so if the user sets
	// MAPLECAST_HUB_URL but there's stale SHM from a previous local run,
	// we must skip SHM or discovery never triggers.
	if (std::getenv("MAPLECAST_SERVER_HOST")
	    || std::getenv("MAPLECAST_HUB_URL")
	    || !openShm(false)) {
		initClientWebSocket();
		return;
	}
	_isClient = true;
	_clientFrameCount = 0;
	_clientNeedsFullSync = false;

	// Request server to save a FRESH sync state right now
	RingHeader* hdr = (RingHeader*)_shmPtr;
	hdr->sync_ready = 0;
	hdr->client_request_sync = 1;
	printf("[MIRROR] Requesting fresh sync state from server...\n");

	// Wait for server to save it (up to 5 seconds)
	for (int i = 0; i < 500; i++) {
		if (hdr->sync_ready) break;
		usleep(10000);  // 10ms
	}

	if (hdr->sync_ready) {
		// Direct memory copy instead of emu.loadstate  --  avoids corrupting scheduler/interrupt state
		uint8_t* snap = _shmPtr + HEADER_SIZE;
		size_t off = 0;
		memcpy(&mem_b[0], snap + off, 16 * 1024 * 1024); off += 16 * 1024 * 1024;
		memcpy(&vram[0], snap + off, VRAM_SIZE); off += VRAM_SIZE;
		memcpy(&aica::aica_ram[0], snap + off, 2 * 1024 * 1024);
		// Also copy PVR regs from the server's current state
		// (they're diffed per-frame anyway, but this gives us a clean start)

		memwatch::unprotect();
		if (renderer) {
			renderer->resetTextureCache = true;
			renderer->updatePalette = true;
		}
		pal_needs_update = true;
		palette_update();
		if (renderer) renderer->updateFogTable = true;

		_clientFrameCount = hdr->frame_count;
		printf("[MIRROR] === CLIENT MODE === synced at frame %lu (direct memory copy)\n", _clientFrameCount);
	} else {
		printf("[MIRROR] WARNING: server didn't respond\n");
	}
}

// ==================== WebSocket client transport ====================

static bool _useWebSocket = false;
static std::thread _wsThread;
static std::mutex _frameMutex;
static std::deque<std::vector<uint8_t>> _frameQueue;
static std::atomic<uint32_t> _wsFramesReceived{0};

// Double-buffered TA contexts  --  background decodes into one, render reads the other
static TA_context _decodeTaCtx[2];
static bool _decodeTaAlloced = false;
static int _decodeIdx = 0;  // which buffer background thread writes to
static bool _decodeHasFullFrame = false;

// Decoded frame metadata  --  written by background thread, read by render thread
struct DecodedPage {
	uint8_t  regionId;
	uint32_t pageIdx;
	uint8_t  data[4096];
};
struct DecodedFrame {
	uint32_t frameNum;
	uint32_t pvr_snapshot[16];
	uint32_t taSize;
	int taBufferIdx;  // which _decodeTaCtx[] has the TA data
	uint32_t dirtyCount;
	// Heap-allocated to support full VRAM+PVR resync (2048 VRAM + 8 PVR = 2056
	// pages on a scene change). The previous fixed pages[128] silently
	// truncated and lost the bulk of new-scene textures. 4096 entries =
	// ~16.8MB per DecodedFrame, fine on the heap.
	std::vector<DecodedPage> pages;
	bool vramDirty;
};
static DecodedFrame _decoded;
static std::atomic<bool> _decodedReady{false};
// Guards _decoded.pages and the merge/move operations on it. Without this,
// the producer can std::move() a new vector into _decoded while the consumer
// is iterating the previous vector, corrupting both. ~60Hz contention,
// trivial cost, eliminates the residual PVR phase noise.
static std::mutex _decodedMtx;

// Raw TCP WebSocket client  --  bypasses websocketpp/asio resolver entirely
// Implements RFC 6455 WebSocket framing over a plain POSIX socket

static int _wsFd = -1;

static bool wsHandshake(int fd, const char* host, int port)
{
	// Send HTTP upgrade request
	char req[512];
	int len = snprintf(req, sizeof(req),
		"GET / HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"\r\n", host, port);
	if (mc_send(fd, req, len, 0) != len) return false;

	// Read HTTP response
	char resp[1024];
	int total = 0;
	while (total < (int)sizeof(resp) - 1) {
		int n = mc_recv(fd, resp + total, 1, 0);
		if (n <= 0) return false;
		total += n;
		if (total >= 4 && memcmp(resp + total - 4, "\r\n\r\n", 4) == 0) break;
	}
	resp[total] = 0;
	return strstr(resp, "101") != nullptr;
}

// Send a masked WebSocket TEXT frame (RFC 6455 client framing). The native
// client is otherwise receive-only; this is the one path that sends data back
// to the relay — a short JSON subscription control message. Client→server
// frames MUST be masked per spec; the mask value is arbitrary (the relay
// unmasks with whatever we send). Single-byte length path only (msg <= 125
// bytes), which covers every control message we send. Thread note: the recv
// loop runs on _wsThread and only ever calls mc_recv; this is the sole sender,
// so concurrent send+recv on the same fd is safe (POSIX + Winsock both allow it).
static bool wsSendTextMasked(int fd, const char* msg)
{
	size_t mlen = strlen(msg);
	if (fd < 0 || mlen > 125) return false;
	uint8_t frame[2 + 4 + 125];
	frame[0] = 0x81;                       // FIN + text opcode (0x1)
	frame[1] = 0x80 | (uint8_t)mlen;       // MASK bit + payload length
	const uint8_t mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
	frame[2] = mask[0]; frame[3] = mask[1]; frame[4] = mask[2]; frame[5] = mask[3];
	for (size_t i = 0; i < mlen; i++)
		frame[6 + i] = (uint8_t)msg[i] ^ mask[i & 3];
	int total = (int)(6 + mlen);
	return mc_send(fd, (const char*)frame, total, 0) == total;
}

// The relay-side subscription control message. Sending this tells the relay to
// forward ONLY GSTA/OBJF/MCSV and drop all TA/VRAM/SYNC/audio video for this
// socket — dropping per-client egress from ~510 KB/s to ~10-30 KB/s.
static const char* kSubscribeStateMsg = "{\"type\":\"subscribe\",\"mode\":\"state\"}";

static bool wsReadFrame(int fd, std::vector<uint8_t>& out)
{
	// Read WebSocket frame header (2 bytes min)
	uint8_t hdr[2];
	if (mc_recv(fd, hdr, 2, MSG_WAITALL) != 2) return false;

	bool fin = (hdr[0] & 0x80) != 0;
	int opcode = hdr[0] & 0x0F;
	bool masked = (hdr[1] & 0x80) != 0;
	uint64_t payloadLen = hdr[1] & 0x7F;

	if (payloadLen == 126) {
		uint8_t ext[2];
		if (mc_recv(fd, ext, 2, MSG_WAITALL) != 2) return false;
		payloadLen = (ext[0] << 8) | ext[1];
	} else if (payloadLen == 127) {
		uint8_t ext[8];
		if (mc_recv(fd, ext, 8, MSG_WAITALL) != 8) return false;
		payloadLen = 0;
		for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | ext[i];
	}

	// Skip mask key if present (serverâ†’client should not be masked)
	if (masked) {
		uint8_t mask[4];
		if (mc_recv(fd, mask, 4, MSG_WAITALL) != 4) return false;
	}

	// Read payload
	out.resize(payloadLen);
	size_t read = 0;
	while (read < payloadLen) {
		ssize_t n = mc_recv(fd, out.data() + read, payloadLen - read, 0);
		if (n <= 0) return false;
		read += n;
	}

	// Handle close/ping/text
	if (opcode == 0x8) return false;  // close
	// PING -- ignore on the native client. Earlier we tried to send a
	// PONG inline here for Tele-0.3 RTT measurement, but the synchronous
	// mc_send on the recv thread head-of-line-blocked the next TA-frame
	// read and added perceptible play lag. Browser clients still respond
	// automatically (the WS spec mandates it), so server-side RTT
	// telemetry continues to work for them; native clients just stay at
	// rtt_us=-1 in the status JSON. A non-blocking PONG via a queue is
	// the proper fix; deferring until we need it.
	if (opcode == 0x9) { out.clear(); return true; }
	if (opcode == 0x1) { out.clear(); return true; }  // text (JSON status)  --  ignore

	return fin && opcode == 0x2;  // binary frame
}

// Tele-0.10: dedicated stats reporter thread that POSTs to /api/telemetry
// on the relay (or whatever the user's MAPLECAST_TELEMETRY_URL points
// at). Different transport from the WS recv loop -- libcurl over a
// fresh TCP/TLS connection per post -- so it can't head-of-line-block
// TA-frame decode like the WS-text-on-same-fd attempt did.
//
// Schema matches the relay's existing ClientReport (client_telemetry.rs)
// so browser + native + ops dashboards aggregate from the same source.
// Native-only fields (render_us_avg/max) are silently dropped by serde
// today -- they'll be wired in 0.13 when we extend the relay.
static std::atomic<bool> _statsReporterRun{false};
static std::thread       _statsReporterThread;

static size_t _statsReporterCurlSink(void*, size_t size, size_t nmemb, void*) {
	return size * nmemb;  // discard response body
}

static void statsReporterRun(std::string telemetryUrl, std::string clientId)
{
	auto wallNowUs = []() -> int64_t {
		return (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	};
	uint64_t prevPackets = _clientPacketsReceived.load(std::memory_order_relaxed);
	uint64_t prevBytes   = _clientBytesReceived.load(std::memory_order_relaxed);
	uint64_t prevFrames  = _clientFramesDecoded.load(std::memory_order_relaxed);
	int64_t  prevUs      = wallNowUs();

	while (_statsReporterRun.load(std::memory_order_relaxed)
	    && _clientWsConnected.load(std::memory_order_relaxed))
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
		if (!_statsReporterRun.load(std::memory_order_relaxed)) break;

		const int64_t  nowUs   = wallNowUs();
		const uint64_t pkts    = _clientPacketsReceived.load(std::memory_order_relaxed);
		const uint64_t bytes   = _clientBytesReceived.load(std::memory_order_relaxed);
		const uint64_t frames  = _clientFramesDecoded.load(std::memory_order_relaxed);
		const uint64_t syncs   = _clientSyncCount.load(std::memory_order_relaxed);
		const int64_t  arrivalAvg = _clientArrivalEmaUs.load(std::memory_order_relaxed);
		const int64_t  arrivalMax = _clientArrivalMaxUs.exchange(0, std::memory_order_relaxed);
		const int64_t  decodeAvg = _clientDecodeEmaUs.load(std::memory_order_relaxed);
		const int64_t  decodeMax = _clientDecodeMaxUs.exchange(0, std::memory_order_relaxed);

		const int64_t  intervalUs = std::max<int64_t>(1, nowUs - prevUs);
		const double   intervalS  = (double)intervalUs / 1e6;
		const double   fps        = (double)(frames - prevFrames) / intervalS;
		const double   mbps       = (double)(bytes - prevBytes) * 8.0 / 1e6 / intervalS;
		(void)pkts; (void)prevPackets;  // currently unused; kept for future packet-rate field
		prevPackets = pkts;
		prevBytes   = bytes;
		prevFrames  = frames;
		prevUs      = nowUs;

		// ClientReport (existing schema in relay/src/client_telemetry.rs).
		// Extra render_us fields piggyback for future relay extension.
		char body[640];
		int len = std::snprintf(body, sizeof(body),
			"{\"client_id\":\"%s\",\"ua\":\"maplecast-native\","
			"\"rtt_ms\":0,\"fps\":%.2f,\"mbps\":%.3f,"
			"\"frame_jitter_avg_us\":%lld,\"frame_jitter_max_us\":%lld,"
			"\"sync_count\":%llu,\"streaming\":1,"
			"\"render_us_avg\":%lld,\"render_us_max\":%lld}",
			clientId.c_str(), fps, mbps,
			(long long)arrivalAvg, (long long)arrivalMax,
			(unsigned long long)syncs,
			(long long)decodeAvg, (long long)decodeMax);
		if (len <= 0 || len >= (int)sizeof(body)) continue;

		CURL* c = curl_easy_init();
		if (!c) continue;
		struct curl_slist* hdrs = nullptr;
		hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
		curl_easy_setopt(c, CURLOPT_URL, telemetryUrl.c_str());
		curl_easy_setopt(c, CURLOPT_POST, 1L);
		curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)len);
		curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
		curl_easy_setopt(c, CURLOPT_TIMEOUT, 3L);
		curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 2L);
		curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, _statsReporterCurlSink);
		curl_easy_setopt(c, CURLOPT_USERAGENT, "maplecast-native/1.0");
		curl_easy_perform(c);
		curl_slist_free_all(hdrs);
		curl_easy_cleanup(c);
	}
}

static void wsClientRun(std::string host, int port)
{
	printf("[MIRROR-WS] Connecting to %s:%d...\n", host.c_str(), port); fflush(stdout);

	// Bump priority — frame decode is on the critical path. Default
	// priority lets background OS work (search indexer, AV scans, Windows
	// Defender) preempt this thread, causing render stalls. THREAD_PRIORITY_
	// HIGHEST is sufficient — TIME_CRITICAL would compete with input which
	// is more latency-sensitive.
#ifdef _WIN32
	if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST))
		printf("[MIRROR-WS] recv thread -> THREAD_PRIORITY_HIGHEST\n");
#endif

	// Resolve hostname (inet_pton only handles IP literals, not DNS names)
	struct addrinfo hints = {}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char portBuf[16];
	snprintf(portBuf, sizeof(portBuf), "%d", port);
	int gaiErr = getaddrinfo(host.c_str(), portBuf, &hints, &res);
	if (gaiErr != 0 || !res) {
		printf("[MIRROR-WS] getaddrinfo('%s:%d') failed: %s\n",
		       host.c_str(), port, gai_strerror(gaiErr));
		return;
	}

	_wsFd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (_wsFd < 0) { printf("[MIRROR-WS] socket() failed\n"); freeaddrinfo(res); return; }

	if (connect(_wsFd, res->ai_addr, res->ai_addrlen) != 0) {
		printf("[MIRROR-WS] connect() failed: %s\n", strerror(errno));
		mc_closesocket(_wsFd); _wsFd = -1; freeaddrinfo(res); return;
	}
	freeaddrinfo(res);
	int one = 1;
	mc_setsockopt(_wsFd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
	printf("[MIRROR-WS] TCP connected (NODELAY)\n"); fflush(stdout);

	if (!wsHandshake(_wsFd, host.c_str(), port)) {
		printf("[MIRROR-WS] WebSocket handshake failed\n");
		mc_closesocket(_wsFd); _wsFd = -1; return;
	}
	printf("[MIRROR-WS] WebSocket handshake OK  --  waiting for initial sync\n"); fflush(stdout);
	_clientWsConnected.store(true, std::memory_order_release);

	// If we're already in GSTA-only mode at connect time — a cold-start
	// startGstaStream(), or a reconnect after switchToGstaOnly() — the relay
	// defaults every new socket to full-mirror, so re-assert our state-only
	// subscription now. (The mid-session live switch is sent by switchToGstaOnly
	// on the existing socket.) Best-effort: failure just falls back to local drop.
	if (_gstaOnly.load(std::memory_order_relaxed)) {
		if (wsSendTextMasked(_wsFd, kSubscribeStateMsg))
			printf("[MIRROR-WS] state-only subscribe sent to relay on connect (shedding TA/VRAM)\n");
		fflush(stdout);
	}

	// Tele-0.10 stats reporter DISABLED again -- even with HTTP POST
	// (separate transport, no shared fd), connections still drop after
	// ~15s. The thread itself or curl init might be doing something
	// system-level that interferes. Keeping the impl compiled in but
	// not spawning, so we can isolate further.

	if (!_decodeTaAlloced) {
		_decodeTaCtx[0].Alloc();
		_decodeTaCtx[1].Alloc();
		_decodeTaAlloced = true;
	}
	_decodeIdx = 0;

	// zstd decompressor  --  reused across all frames
	MirrorDecompressor decomp;
	decomp.init(16 * 1024 * 1024);  // 16MB covers SYNC + worst-case frames

	// Wait for initial SYNC message (VRAM + PVR regs).
	// GSTA-ONLY (state-replica): normally skip the SYNC wait entirely — we never
	// apply the server's TA/VRAM (the local SH4 owns the framebuffer). But if
	// _gstaVramSync is set (vramSync=true in startGstaStream), we DO wait for
	// and apply the initial SYNC to seed local VRAM with prod's current textures,
	// then fall through to GSTA-only for all subsequent frames.
	const bool wantVramSync = _gstaVramSync.load(std::memory_order_relaxed);
	bool synced = _gstaOnly.load(std::memory_order_relaxed) && !wantVramSync;
	std::vector<uint8_t> frame;
	if (synced)
		printf("[MIRROR-WS] GSTA-ONLY mode — skipping SYNC wait + TA/VRAM apply\n");
	else if (_gstaOnly.load(std::memory_order_relaxed) && wantVramSync)
		printf("[MIRROR-WS] GSTA+VRAM-SYNC mode — waiting for initial SYNC to seed local VRAM, then GSTA-only\n");
	while (!synced) {
		if (!wsReadFrame(_wsFd, frame)) {
			printf("[MIRROR-WS] Connection lost waiting for sync\n"); fflush(stdout);
			mc_closesocket(_wsFd); _wsFd = -1; decomp.destroy(); return;
		}
		// Audio packet  --  [0xAD][0x10][seqHi][seqLo][PCM]  --  ignore on the
		// native client (no audio playback implemented here). Would be
		// misparsed as a video frame otherwise.
		if (frame.size() >= 4 && frame[0] == 0xAD && frame[1] == 0x10)
			continue;
		// Decompress if needed
		size_t decompSize = 0;
		const uint8_t* decompData = decomp.decompress(frame.data(), frame.size(), decompSize);

		if (decompSize > 8 && memcmp(decompData, "SYNC", 4) == 0) {
			const uint8_t* src = decompData + 4;
			uint32_t vramSize; memcpy(&vramSize, src, 4); src += 4;
			if (vramSize <= VRAM_SIZE) {
				memcpy(&vram[0], src, vramSize); src += vramSize;
				uint32_t pvrSize; memcpy(&pvrSize, src, 4); src += 4;
				if (pvrSize <= (uint32_t)pvr_RegSize)
					memcpy(pvr_regs, src, pvrSize);
				// Save for post-MCSV re-apply: dc_loadstate overwrites VRAM with
				// match-start textures; reapplyLastSyncVram() restores current state.
				_lastSyncVram.assign(&vram[0], &vram[0] + vramSize);
				if (pvrSize <= (uint32_t)pvr_RegSize)
					_lastSyncPvr.assign((const uint8_t*)pvr_regs,
					                    (const uint8_t*)pvr_regs + pvrSize);
			}
			// Unprotect VRAM so per-frame memcpy patches work (nvmem page protection)
			memwatch::unprotect();
			// NOTE: renderer cache/palette updates happen on render thread in clientReceive()

			synced = true;
			printf("[MIRROR-WS] Initial sync received: %.1f MB (%.1f MB compressed)  --  VRAM + PVR loaded\n",
				decompSize / (1024.0 * 1024.0), frame.size() / (1024.0 * 1024.0));
			fflush(stdout);
		}
	}

	while (true) {
		if (!wsReadFrame(_wsFd, frame)) {
			printf("[MIRROR-WS] Connection lost\n"); fflush(stdout);
			_clientWsConnected.store(false, std::memory_order_release);
			// Stats thread disabled -- no join needed.
			break;
		}
		// Audio packet  --  skip (native client has dedicated audio WS)
		if (frame.size() >= 4 && frame[0] == 0xAD && frame[1] == 0x10)
			continue;

		// Game state packet  --  "GSTA" magic + serialized MVC2 state.
		// Deserialize into a shared GameState struct for the overlay/HUD and the
		// state-replica inject. Accept any frame >= the legacy 253-byte block
		// (deserialize tolerates the optional input/stage trailers) so a wire
		// size bump on either side never silently drops the packet.
		if (frame.size() >= 4 && frame[0] == 'G' && frame[1] == 'S'
		    && frame[2] == 'T' && frame[3] == 'A') {
			static const int LEGACY = 5 + 2+2+2+2 + 4+4+4 + 6*57;   // 367 (GSTA wire ext: per-char stride 49->57)
			if ((int)frame.size() >= 4 + LEGACY) {
				maplecast_gamestate::GameState gs;
				maplecast_gamestate::deserialize(frame.data() + 4,
				    (int)(frame.size() - 4), gs);
				// Store atomically for the overlay thread / state-replica inject
				{
					std::lock_guard<std::mutex> lk(_clientGameStateMtx);
					_clientGameState = gs;
				}
				bool firstStore = !_clientGameStateReady.exchange(true, std::memory_order_release);
				if (firstStore)
					printf("[MIRROR-WS] GSTA STORED (first) — frame_counter=%u, getClientGameState now true\n",
					       gs.frame_counter);
			} else {
				printf("[MIRROR-WS] GSTA too short: %zu bytes (need %d)\n",
				       frame.size(), 4 + LEGACY);
			}
			static uint64_t _gstaN = 0;
			if ((++_gstaN % 120) == 1) {
				printf("[MIRROR-WS] GSTA rx #%llu (%zu bytes)\n",
				       (unsigned long long)_gstaN, frame.size());
				fflush(stdout);
			}
			continue;
		}

		// OBJF full-object packet — pool objects for the state-replica inject.
		if (frame.size() >= 5 && frame[0] == 'O' && frame[1] == 'B'
		    && frame[2] == 'J' && frame[3] == 'F') {
			maplecast_gamestate::ObjectState tmp[48];
			int got = maplecast_gamestate::deserializeObjects(
			    frame.data() + 4, (int)(frame.size() - 4), tmp, 48);
			std::lock_guard<std::mutex> lk(_clientObjectsMtx);
			_clientObjectCount = got;
			for (int i = 0; i < got; i++) _clientObjects[i] = tmp[i];
			_clientObjectsReady.store(true, std::memory_order_release);
			static uint64_t _objfN = 0;
			if ((++_objfN % 120) == 1) {
				printf("[MIRROR-WS] OBJF rx #%llu (%d objs)\n",
				       (unsigned long long)_objfN, got);
				fflush(stdout);
			}
			continue;
		}

		// GSTA-ONLY (state-replica): everything past here is TA/VRAM/SYNC video
		// data that would clobber the local SH4 render.  Skip all of it, but
		// first check for MCSV (mid-match join savestate from server) — those
		// frames start with 'M','C','S','V' so they're trivially distinguishable
		// from both raw GSTA/OBJF packets and ZCST-wrapped TA frames.
		if (_gstaOnly.load(std::memory_order_relaxed)) {
			if (frame.size() > 12
			    && frame[0] == 'M' && frame[1] == 'C'
			    && frame[2] == 'S' && frame[3] == 'V') {
				uint32_t uncompSize;
				memcpy(&uncompSize, frame.data() + 4, 4);
				// Inner payload is a ZCST-compressed blob starting at byte 8.
				size_t dSize = 0;
				const uint8_t* d = decomp.decompress(
				    frame.data() + 8, frame.size() - 8, dSize);
				if (d && dSize > 0) {
					std::lock_guard<std::mutex> lk(_pendingSaveStateMtx);
					_pendingSaveState.assign(d, d + dSize);
					_pendingSaveStateReady.store(true, std::memory_order_release);
					printf("[MIRROR-WS] MCSV savestate received: %.1f MB — queued for apply\n",
					       dSize / (1024.0 * 1024.0));
					fflush(stdout);
				} else {
					printf("[MIRROR-WS] MCSV: decompress failed (got %zu bytes)\n", dSize);
				}
			}
			continue;
		}

		// MCSV frame — also handled here for state-replica clients that started
		// as a full mirror stream (not GSTA-only). Store the savestate so
		// frameInject() can apply it and switch to SH4 mode.
		if (frame.size() > 4 && frame[0] == 'M' && frame[1] == 'C'
		    && frame[2] == 'S' && frame[3] == 'V') {
			if (frame.size() > 12) {
				uint32_t uncompSize = 0;
				memcpy(&uncompSize, frame.data() + 4, 4);
				size_t dSize = 0;
				const uint8_t* d = decomp.decompress(
				    frame.data() + 8, frame.size() - 8, dSize);
				if (d && dSize > 0) {
					std::lock_guard<std::mutex> lk(_pendingSaveStateMtx);
					_pendingSaveState.assign(d, d + dSize);
					_pendingSaveStateReady.store(true, std::memory_order_release);
					printf("[MIRROR-WS] MCSV savestate received: %.1f MB — queued for apply\n",
					       dSize / (1024.0 * 1024.0));
					fflush(stdout);
				}
			}
			continue;
		}

		// ---- Client-side arrival telemetry (video WS) ----
		{
			const int64_t now = _clientNowUs();
			const int64_t prev = _clientLastArrivalUs.exchange(now, std::memory_order_relaxed);
			if (prev != 0) {
				const int64_t delta = now - prev;
				const int64_t emaPrev = _clientArrivalEmaUs.load(std::memory_order_relaxed);
				const int64_t ema = emaPrev + ((delta - emaPrev) >> 4);
				_clientArrivalEmaUs.store(ema, std::memory_order_relaxed);
				int64_t mx = _clientArrivalMaxUs.load(std::memory_order_relaxed);
				while (delta > mx
				    && !_clientArrivalMaxUs.compare_exchange_weak(mx, delta,
				        std::memory_order_relaxed)) {}
			}
			_clientPacketsReceived.fetch_add(1, std::memory_order_relaxed);
			_clientBytesReceived.fetch_add(frame.size(), std::memory_order_relaxed);
		}
		// Decompress if needed
		size_t decompSize = 0;
		const uint8_t* decompData = decomp.decompress(frame.data(), frame.size(), decompSize);
		if (decompSize < 8) continue;

		// Handle mid-stream SYNC frames (triggered by palette changes,
		// soft resets, etc.)  --  re-apply VRAM + PVR snapshot.
		if (decompSize > 8 && memcmp(decompData, "SYNC", 4) == 0) {
			const uint8_t* src = decompData + 4;
			uint32_t vramSize; memcpy(&vramSize, src, 4); src += 4;
			if (vramSize <= VRAM_SIZE) {
				memcpy(&vram[0], src, vramSize); src += vramSize;
				uint32_t pvrSize; memcpy(&pvrSize, src, 4); src += 4;
				if (pvrSize <= (uint32_t)pvr_RegSize)
					memcpy(pvr_regs, src, pvrSize);
				// Update saved snapshot for post-MCSV re-apply
				_lastSyncVram.assign(&vram[0], &vram[0] + vramSize);
				if (pvrSize <= (uint32_t)pvr_RegSize)
					_lastSyncPvr.assign((const uint8_t*)pvr_regs,
					                    (const uint8_t*)pvr_regs + pvrSize);
			}
			memwatch::unprotect();
			_decodeHasFullFrame = false;  // force next TA frame as keyframe
			_clientSyncCount.fetch_add(1, std::memory_order_relaxed);  // Tele-0.10
			printf("[MIRROR-WS] Mid-stream SYNC applied (%.1f MB)\n",
				decompSize / (1024.0 * 1024.0));
			continue;
		}

		if (decompSize < 80) continue;

		const uint8_t* src = decompData;
		uint32_t frameSize; memcpy(&frameSize, src, 4); src += 4;
		uint32_t frameNum; memcpy(&frameNum, src, 4); src += 4;

		uint32_t pvr_snap[16];
		memcpy(pvr_snap, src, sizeof(pvr_snap)); src += sizeof(pvr_snap);

		uint32_t taSize; memcpy(&taSize, src, 4); src += 4;
		uint32_t deltaPayloadSize; memcpy(&deltaPayloadSize, src, 4); src += 4;

		// Sanity check  --  TA buffers are ~50-300KB, never megabytes
		if (taSize > 512 * 1024 || deltaPayloadSize > 512 * 1024 ||
		    frameSize > decompSize) {
			// Skip silently if this is a known non-TA packet type that was
			// ZCST-wrapped (STAF stripped frames, TXTR/TX64 texture atlases,
			// EFCT/EFKY effect packets, OBJS compact object lists). These share
			// the WS channel but are not TA deltas; their first 4 bytes parse as
			// a garbage frameSize. OBJS in particular bursts during supers (the
			// "frameSize=1397375567" spam == 'O','B','J','S' little-endian).
			if (decompSize >= 4) {
				const uint8_t* m = decompData;
				if ((m[0]=='S'&&m[1]=='T'&&m[2]=='A'&&m[3]=='F') ||
				    (m[0]=='T'&&m[1]=='X'&&m[2]=='T'&&m[3]=='R') ||
				    (m[0]=='T'&&m[1]=='X'&&m[2]=='6'&&m[3]=='4') ||
				    (m[0]=='E'&&m[1]=='F'&&m[2]=='C'&&m[3]=='T') ||
				    (m[0]=='E'&&m[1]=='F'&&m[2]=='K'&&m[3]=='Y') ||
				    (m[0]=='O'&&m[1]=='B'&&m[2]=='J'&&m[3]=='S')) {
					continue;
				}
			}
			printf("[MIRROR-WS] BAD FRAME: taSize=%u delta=%u frameSize=%u bufSize=%zu -- skipping\n",
				taSize, deltaPayloadSize, frameSize, decompSize);
			continue;
		}

		// TA delta decode into double-buffered context
		// _decodeIdx = buffer we write to NOW
		// 1-_decodeIdx = buffer that has PREVIOUS frame (render thread may be reading it)
		uint8_t* taDst = _decodeTaCtx[_decodeIdx].tad.thd_root;
		uint8_t* taPrev = _decodeTaCtx[1 - _decodeIdx].tad.thd_root;

		if (deltaPayloadSize == taSize)
		{
			// Keyframe: straight memcpy into current buffer
			memcpy(taDst, src, taSize);
			src += taSize;
			_decodeHasFullFrame = true;
		}
		else if (!_decodeHasFullFrame)
		{
			src += deltaPayloadSize + 4;
			continue;
		}
		else
		{
			// Delta: copy previous frame into current buffer, then apply runs
			// This is needed because the previous buffer might be in use by render thread
			memcpy(taDst, taPrev, taSize);

			const uint8_t* dd = src;
			const uint8_t* de = src + deltaPayloadSize;
			while (dd + 4 <= de) {
				uint32_t off; memcpy(&off, dd, 4); dd += 4;
				if (off == 0xFFFFFFFF) break;
				uint16_t runLen; memcpy(&runLen, dd, 2); dd += 2;
				if (off + runLen <= taSize && dd + runLen <= de)
					memcpy(taDst + off, dd, runLen);
				dd += runLen;
			}
			src += deltaPayloadSize;
		}

		// Skip checksum  --  TCP guarantees data integrity, checksum was for shm race detection
		// (commented out, not deleted  --  can re-enable for debugging)
		// uint32_t serverChecksum; memcpy(&serverChecksum, src, 4);
		src += 4;

		// Stage dirty pages  --  copy page data so render thread can apply safely.
		// Allow up to a full VRAM+PVR resync (~2056 pages on scene change).
		uint32_t dirtyCount; memcpy(&dirtyCount, src, 4); src += 4;
		if (dirtyCount > 4096) dirtyCount = 4096;  // sanity bound

		DecodedFrame df;
		df.frameNum = frameNum;
		memcpy(df.pvr_snapshot, pvr_snap, sizeof(pvr_snap));
		df.taSize = taSize;
		df.dirtyCount = dirtyCount;
		df.vramDirty = false;
		df.pages.resize(dirtyCount);

		for (uint32_t d = 0; d < dirtyCount; d++) {
			df.pages[d].regionId = *src++;
			memcpy(&df.pages[d].pageIdx, src, 4); src += 4;
			memcpy(df.pages[d].data, src, MEM_PAGE_SIZE); src += MEM_PAGE_SIZE;
			if (df.pages[d].regionId == 1) df.vramDirty = true;
		}

		// Publish  --  render thread picks it up.
		//
		// CRITICAL: if the consumer hasn't drained the previous frame yet
		// (_decodedReady still set), we used to OVERWRITE _decoded and lose
		// the previous frame's dirty pages permanently. Permanently because
		// the next memcmp on the server side sees `shadow == ptr` for those
		// pages and never re-ships them.
		//
		// Now: prepend the previous frame's dirty pages to the new frame's
		// pages so the consumer sees the union. The TA buffer is always the
		// newest (the consumer is going to render whatever's current anyway,
		// so an older TA is moot), but EVERY dirty page record from every
		// dropped frame is preserved and applied.
		df.taBufferIdx = _decodeIdx;
		{
			std::lock_guard<std::mutex> lock(_decodedMtx);
			if (_decodedReady.load(std::memory_order_relaxed)) {
				// Carry forward unconsumed pages from the previous frame.
				df.pages.insert(df.pages.begin(),
					_decoded.pages.begin(), _decoded.pages.end());
				df.dirtyCount += _decoded.dirtyCount;
				df.vramDirty = df.vramDirty || _decoded.vramDirty;
			}
			_decoded = std::move(df);
			_decodedReady.store(true, std::memory_order_release);
		}

		// Swap to other buffer for next frame's decode
		// Render thread reads buffer [df.taBufferIdx], we write to the other one
		_decodeIdx = 1 - _decodeIdx;

		uint32_t n = _wsFramesReceived.fetch_add(1, std::memory_order_relaxed);
		if (n == 0) printf("[MIRROR-WS] First frame decoded\n");
		if (n > 0 && n % 300 == 0) printf("[MIRROR-WS] %u frames decoded\n", n);
	}

	mc_closesocket(_wsFd); _wsFd = -1;
	decomp.destroy();
}

// Phase 2: when hub-discovery picks a winner, we ALSO learn the runner-up
// from the ranked probe list. Stash both so the input-sink (initialized
// later in emulator.cpp) can configure a hot-standby UDP socket.
static std::string _hubBackupHost;  // empty = no backup available

const std::string& clientBackupServerHost() { return _hubBackupHost; }

static void initClientWebSocket()
{
	_isClient = true;
	_useWebSocket = true;

	// Hub-aware discovery (Phase 1): if MAPLECAST_HUB_URL is set, fetch the
	// list of nearby input servers, UDP-probe each, pick the lowest-RTT one.
	// Falls back to MAPLECAST_SERVER_HOST/PORT if hub unreachable or no
	// servers found. Explicit host/port override the hub if both are set.
	//
	// Phase 2 extension: discoverAndRank returns top-N. winner = primary,
	// runner-up = hot-standby for input failover (input-sink reads
	// clientBackupServerHost()).
	std::string hubHost;
	int hubPort = 0;
	if (const char* hubUrl = std::getenv("MAPLECAST_HUB_URL")) {
		// Don't override an explicit host
		const char* explicitHost = std::getenv("MAPLECAST_SERVER_HOST");
		if (!explicitHost || strlen(explicitHost) == 0) {
			printf("[MIRROR] Hub discovery enabled  --  querying %s\n", hubUrl);
			auto ranked = maplecast_hub::discoverAndRank(hubUrl, 2);
			if (!ranked.empty()) {
				const auto& winner = ranked[0];
				hubHost = winner.public_host;
				hubPort = winner.relay_ws_port;
				printf("[MIRROR] Hub picked input server '%s' at %s:%d (%.1fms RTT)\n",
				       winner.name.c_str(), hubHost.c_str(), hubPort,
				       winner.avg_rtt_ms);

				// Cascade to input sink via env var (input sink is init'd
				// later in emulator.cpp from MAPLECAST_SERVER_HOST)
				setenv("MAPLECAST_SERVER_HOST", hubHost.c_str(), 1);

				if (ranked.size() >= 2) {
					// MAPLECAST_NO_FAILOVER=1 skips hot-standby setup. The Phase 2
					// failover assumes both servers run synced game state; until
					// cross-node state sync ships, swapping inputs to a different
					// flycast instance breaks gameplay continuity. 100ms primary
					// silence over public-internet WAN is also a normal jitter
					// spike, not a real outage — easy to fire spuriously.
					if (std::getenv("MAPLECAST_NO_FAILOVER")) {
						printf("[MIRROR] Hot-standby SUPPRESSED (MAPLECAST_NO_FAILOVER=1) — would have used '%s' at %s\n",
						       ranked[1].name.c_str(), ranked[1].public_host.c_str());
					} else {
						_hubBackupHost = ranked[1].public_host;
						printf("[MIRROR] Hot-standby input server: '%s' at %s (%.1fms RTT)\n",
						       ranked[1].name.c_str(), _hubBackupHost.c_str(),
						       ranked[1].avg_rtt_ms);
					}
				}
			} else {
				printf("[MIRROR] Hub discovery failed  --  falling back to MAPLECAST_SERVER_HOST\n");
			}
		}
	}

	const char* host = hubHost.empty() ? std::getenv("MAPLECAST_SERVER_HOST") : hubHost.c_str();
	if (!host) host = "127.0.0.1";
	const char* portStr = std::getenv("MAPLECAST_SERVER_PORT");
	// Native clients connect directly to flycast's WS (7200), NOT the relay
	// (7201). The relay is a spectator fanout multiplexer  --  players who are
	// sending input get lower latency by skipping it. Hub discovery returns
	// the relay port; we subtract 1 to get the direct port. Override with
	// MAPLECAST_SERVER_PORT or MAPLECAST_USE_RELAY=1 for spectator mode.
	int directPort = hubPort > 0 ? (hubPort - 1) : 7200;
	if (std::getenv("MAPLECAST_USE_RELAY") || std::getenv("MAPLECAST_SPECTATE"))
		directPort = hubPort > 0 ? hubPort : 7201;  // spectators use relay
	int port = portStr ? std::atoi(portStr) : directPort;

	printf("[MIRROR] === CLIENT MODE (WebSocket) === ws://%s:%d/\n", host, port);

	// Unprotect VRAM BEFORE spawning WS thread  --  the thread will memcpy into
	// VRAM pages during SYNC and per-frame diffs. Without this, nvmem page
	// protection causes SIGSEGV on the first VRAM write.
	memwatch::unprotect();

	std::string hostStr(host);
	_wsThread = std::thread(wsClientRun, hostStr, port);
	_wsThread.detach();

	// Audio WS lives on flycast's direct port (7200+3=7203), NOT relay+3.
	// The relay doesn't proxy audio. If the video port came from hub
	// discovery (relay at 7201), audio still goes to the server's own
	// audio port. Default: 7203. MAPLECAST_AUDIO_WS_PORT overrides.
	int audioPort = 7203;
	if (const char* audioPortStr = std::getenv("MAPLECAST_AUDIO_WS_PORT"))
		audioPort = std::atoi(audioPortStr);
	maplecast_audio_client::init(host, audioPort);
}

void startMirrorStream(const char* host, int port)
{
	// Same as initClientWebSocket but does NOT set _isClient, so the GUI
	// and SH4 keep running normally. Used by maplecast_replica for TA
	// correction alongside a live local SH4.
	_useWebSocket = true;
	printf("[MIRROR] Starting TA correction stream ws://%s:%d/\n", host, port);
	memwatch::unprotect();
	std::string hostStr(host);
	_wsThread = std::thread(wsClientRun, hostStr, port);
	_wsThread.detach();
}

// State-replica transport: connect the mirror WS but consume ONLY GSTA/OBJF
// state packets — never apply TA delta frames (the local SH4 renders). If
// vramSync=true, the initial SYNC frame (VRAM + PVR) IS applied once to seed
// the local VRAM with prod's current textures, eliminating savestate parity issues.
void startGstaStream(const char* host, int port, bool vramSync)
{
	_gstaOnly.store(true, std::memory_order_release);
	_gstaVramSync.store(vramSync, std::memory_order_release);
	_useWebSocket = true;
	if (vramSync)
		printf("[MIRROR] Starting GSTA+VRAM-SYNC stream ws://%s:%d/ (one-time VRAM seed, then GSTA-only)\n", host, port);
	else
		printf("[MIRROR] Starting GSTA-ONLY state stream ws://%s:%d/ (no TA/VRAM apply)\n", host, port);
	std::string hostStr(host);
	_wsThread = std::thread(wsClientRun, hostStr, port);
	_wsThread.detach();
}

bool isServer() { return _isServer; }
bool isClient() { return _isClient; }

// Phase A  --  accessors for the input latch path. Cheap atomic loads,
// no shm header touching, safe from any thread.
uint64_t currentFrame()      { return _atomicCurrentFrame.load(std::memory_order_acquire); }
int64_t  lastLatchTimeUs()   { return _atomicLastLatchTimeUs.load(std::memory_order_acquire); }
// Phase B  --  live frame period EMA, used by frame_phase in status JSON.
int64_t  framePeriodUs()     { return _atomicFramePeriodUs.load(std::memory_order_relaxed); }

// ==================== Client telemetry API ====================
// All lock-free atomic reads. The ImGui debug overlay calls getClientStats()
// once per frame to refresh its tables and graphs.

ClientStats getClientStats()
{
	ClientStats s;
	s.wsConnected      = _clientWsConnected.load(std::memory_order_relaxed);
	s.frameCount       = _clientFrameCount;  // updated from render thread, best-effort read
	s.packetsReceived  = _clientPacketsReceived.load(std::memory_order_relaxed);
	s.bytesReceived    = _clientBytesReceived.load(std::memory_order_relaxed);
	s.lastDecodeUs     = _clientLastDecodeUs.load(std::memory_order_relaxed);
	s.decodeEmaUs      = _clientDecodeEmaUs.load(std::memory_order_relaxed);
	s.lastDirtyPages   = _clientLastDirtyPages.load(std::memory_order_relaxed);
	s.lastTaSize       = _clientLastTaSize.load(std::memory_order_relaxed);
	s.lastVramDirty    = _clientLastVramDirty.load(std::memory_order_relaxed);
	s.lastArrivalUs    = _clientLastArrivalUs.load(std::memory_order_relaxed);
	s.arrivalEmaUs     = _clientArrivalEmaUs.load(std::memory_order_relaxed);
	s.arrivalMaxUs     = _clientArrivalMaxUs.load(std::memory_order_relaxed);
	return s;
}

bool getClientGameState(maplecast_gamestate::GameState& out)
{
	if (!_clientGameStateReady.load(std::memory_order_acquire))
		return false;
	std::lock_guard<std::mutex> lk(_clientGameStateMtx);
	out = _clientGameState;
	return true;
}

int getClientObjects(maplecast_gamestate::ObjectState* out, int maxObjs)
{
	if (!_clientObjectsReady.load(std::memory_order_acquire))
		return 0;
	std::lock_guard<std::mutex> lk(_clientObjectsMtx);
	int n = _clientObjectCount;
	if (n > maxObjs) n = maxObjs;
	for (int i = 0; i < n; i++) out[i] = _clientObjects[i];
	return n;
}

void resetClientStatsPeaks()
{
	_clientArrivalMaxUs.store(0, std::memory_order_relaxed);
}

void requestClientVideoReconnect()
{
	// Closing the socket causes wsReadFrame() to return false on the next
	// recv, which breaks the drain loop. wsClientRun() doesn't currently
	// retry on its own (it falls out and exits), so a manual reconnect
	// request here is primarily informational  --  the overlay shows the
	// disconnected state and the user knows to restart the client.
	// TODO: wrap wsClientRun() in an outer reconnect loop similar to the
	// audio client. For now, just slam the fd so the ops-side UX matches
	// user expectation: button click â†’ "Disconnected" in the overlay.
	int fd = _wsFd;
	if (fd >= 0) {
#ifdef _WIN32
		closesocket(fd);
#else
		mc_closesocket(fd);
#endif
	}
	_wsFd = -1;
	_clientWsConnected.store(false, std::memory_order_release);
}

void switchToGstaOnly()
{
	_gstaOnly.store(true, std::memory_order_release);
	// Live mid-session switch: tell the relay to stop sending TA/VRAM/SYNC/audio
	// on the CURRENT socket. From here the local SH4 renders, so we only need
	// GSTA/OBJF/MCSV. Drops per-client egress from ~510 KB/s to ~10-30 KB/s.
	// Best-effort — if the send fails (or fd is mid-reconnect) we still drop the
	// video locally as before, and the post-handshake re-assert covers reconnect.
	int fd = _wsFd;
	if (fd >= 0) {
		if (wsSendTextMasked(fd, kSubscribeStateMsg))
			printf("[MIRROR] sent state-only subscribe to relay (shedding TA/VRAM)\n");
		else
			printf("[MIRROR] WARN: state-only subscribe send failed — dropping video locally only\n");
		fflush(stdout);
	}
	printf("[MIRROR] switched to GSTA-only mode — TA delta processing stopped\n");
	fflush(stdout);
}

void setClientRendering(bool enabled)
{
	_isClient = enabled;
}

void reapplyLastSyncVram()
{
	if (_lastSyncVram.empty()) {
		printf("[MIRROR] reapplyLastSyncVram: no saved SYNC VRAM — skipping\n");
		fflush(stdout);
		return;
	}
	const size_t vramSz = _lastSyncVram.size();
	if (vramSz <= VRAM_SIZE)
		memcpy(&vram[0], _lastSyncVram.data(), vramSz);
	if (!_lastSyncPvr.empty() && _lastSyncPvr.size() <= (size_t)pvr_RegSize)
		memcpy(pvr_regs, _lastSyncPvr.data(), _lastSyncPvr.size());
	// Unprotect so the renderer sees updated VRAM pages on next render.
	memwatch::unprotect();
	printf("[MIRROR] reapplyLastSyncVram: restored %.1f MB VRAM from saved SYNC — char textures current\n",
	       vramSz / (1024.0 * 1024.0));
	fflush(stdout);
}

bool hasPendingSaveState()
{
	return _pendingSaveStateReady.load(std::memory_order_acquire);
}

bool takePendingSaveState(std::vector<uint8_t>& out)
{
	if (!_pendingSaveStateReady.load(std::memory_order_acquire))
		return false;
	std::lock_guard<std::mutex> lk(_pendingSaveStateMtx);
	if (!_pendingSaveStateReady.load(std::memory_order_relaxed))
		return false;
	out = std::move(_pendingSaveState);
	_pendingSaveState.clear();
	_pendingSaveStateReady.store(false, std::memory_order_release);
	return true;
}

bool isHeadless()
{
#ifdef MAPLECAST_HEADLESS_BUILD
	// Compile-out build  --  always headless, env var optional.
	return true;
#else
	// GPU-capable build  --  headless mode is opt-in via env var.
	// Evaluated once on first call. Checked this way (rather than a static
	// initializer) so we're resilient to early-boot call sites that might
	// beat any namespace-scope ctor order.
	static const bool _headless = (std::getenv("MAPLECAST_HEADLESS") != nullptr);
	return _headless;
#endif
}

// ==================== SERVER: publish TA commands + memory diffs ====================
//
// !!! FRAGILE  --  WIRE FORMAT IS A CONTRACT WITH FOUR CLIENTS !!!
//
// Anything you write into the dst buffer here is decoded by:
//   1. clientReceive() below  --  desktop flycast mirror client
//   2. packages/renderer/src/wasm_bridge.cpp renderer_frame()  --  king.html WASM
//   3. core/network/maplecast_wasm_bridge.cpp mirror_render_frame()  --  emulator.html WASM
//   4. relay/src/fanout.rs in the Rust VPS relay (parses for SYNC cache + dirty pages)
//
// Change the format here without touching all four parsers and you will get
// silently corrupted frames. The breakage will look LIKE the renderer is buggy
// (broken character select, missing textures on scene transition) because
// in-match scenes are stable enough that small format errors don't show.
//
// Wire format produced below (after zstd "ZCST" envelope, in clear bytes):
//   frameSize(4) + frameNum(4) + pvr_snapshot[16](64) +
//   taOrigSize(4) + deltaPayloadSize(4) + (TA bytes OR delta runs + 0xFFFFFFFF) +
//   checksum(4) + dirtyPageCount(4) + [regionId(1) + pageIdx(4) + data(4096)] * N
//
// Region IDs: 0=mem_b (16MB SH4 RAM), 1=vram, 2=aica_ram, 3=pvr_regs.
// Keyframe is forced every 60 frames (forceKeyframe). Browser clients waiting
// for first keyframe will drop up to 60 deltas  --  DO NOT lengthen this interval
// without also updating the keyframe-wait logic in every client.
//
// See docs/ARCHITECTURE.md "Mirror Wire Format  --  Rules of the Road" for the
// canonical list of rules all four parsers must obey.
// ============================================================================
//
// === VRAM CONTENT-CACHE (MAPLECAST_VCACHE) ===================================
// Opt-in (env MAPLECAST_VCACHE) bandwidth optimisation that REUSES the existing
// renderer untouched. The mirror re-ships the same 4KB VRAM/PVR dirty pages
// over and over (a freshly-revisited menu re-uploads identical textures every
// time it's drawn). VCACHE content-hashes each dirty page (FNV-1a) and keeps a
// per-stream "already sent" set. A page whose hash was sent before ships as a
// compact reference (NO 4096 bytes); a novel page ships once with its data and
// is recorded. The client keeps a hash->page cache, fills references from it,
// reconstructs a STANDARD delta frame, and feeds it to the unmodified
// renderer_frame(). The renderer never sees the difference.
//
// Wire change (inside the same ZCST envelope, ONLY when VCACHE is active):
// the dirtyPageCount u32 is replaced by the sentinel 0xFFFFFFFF followed by the
// real count(4); then each page is:
//     regionId(1) + pageIdx(4) + hash(8) + hasData(1) + [data(4096) if hasData]
// A normal (non-VCACHE) frame never writes 0xFFFFFFFF as its page count, so the
// client distinguishes the two by peeking that field. Plain mirror clients that
// don't understand the sentinel simply must not be pointed at a VCACHE stream
// (it is a separate, env-gated mode, off by default — production is unaffected).
//
// Periodic re-seed: the sent-set is cleared every VCACHE_RESEED_FRAMES (~600)
// so a mid-stream joiner recovers full pages within ~10s, exactly like the EFCT
// channel clears _stafSent every 600 frames.
static inline uint64_t vcacheHashPage(const uint8_t* p)
{
	uint64_t h = 1469598103934665603ULL;      // FNV-1a 64 offset basis
	for (size_t i = 0; i < MEM_PAGE_SIZE; i++) { h ^= p[i]; h *= 1099511628211ULL; }
	return h;
}
static constexpr uint32_t VCACHE_PAGE_SENTINEL = 0xFFFFFFFFu;
static constexpr uint64_t VCACHE_RESEED_FRAMES = 600;
// ============================================================================
// Effect-texture decode + content-addressed hashing (EFCT/TXTR path). Ported from
// the client decoder (web/webgpu/texture-manager.mjs). Effects are 16-bit direct
// color (fmt 0/1/2), so we de-twiddle + unpack in software and ship each unique
// texture once (keyed by content hash) — the state-only client draws the EXACT
// game texture. fmt 5/6 (palette), VQ and mip are skipped here (added later).
// ============================================================================
namespace mcfx {
static uint32_t s_twTab[2][11][1024];
static bool s_twInit = false;
static void initTwiddle() {
	auto tw = [](int x, int y, int xs, int ys) {
		int r = 0, s = 0; xs >>= 1; ys >>= 1;
		while (xs || ys) { if (ys) { r |= (y & 1) << s; ys >>= 1; y >>= 1; s++; } if (xs) { r |= (x & 1) << s; xs >>= 1; x >>= 1; s++; } }
		return r;
	};
	for (int s = 0; s < 11; s++) { int ys = 1 << s; for (int i = 0; i < 1024; i++) { s_twTab[0][s][i] = tw(i, 0, 1024, ys); s_twTab[1][s][i] = tw(0, i, ys, 1024); } }
	s_twInit = true;
}
static inline int texBsr(int v) { int r = 0; while ((1 << r) < v) r++; return r; }
static inline int te5(int v) { return (v << 3) | (v >> 2); }
static inline int te6(int v) { return (v << 2) | (v >> 4); }
static inline int te4(int v) { return (v << 4) | v; }
static inline void unpack16(int fmt, uint16_t c, uint8_t* o) {
	if (fmt == 0) { o[0] = te5((c >> 10) & 31); o[1] = te5((c >> 5) & 31); o[2] = te5(c & 31); o[3] = (c >> 15) ? 255 : 0; }
	else if (fmt == 1) { o[0] = te5((c >> 11) & 31); o[1] = te6((c >> 5) & 63); o[2] = te5(c & 31); o[3] = 255; }
	else { o[0] = te4((c >> 8) & 15); o[1] = te4((c >> 4) & 15); o[2] = te4(c & 15); o[3] = te4((c >> 12) & 15); }
}
// FNV-1a over fmt/w/h + the raw 16-bit VRAM region = the texture's content id.
static uint32_t texHash(uint32_t addr, int fmt, int w, int h, int vq) {
	uint32_t hsh = 2166136261u;
	auto mix = [&](uint32_t b) { hsh ^= (b & 0xff); hsh *= 16777619u; };
	mix(fmt); mix(w); mix(w >> 8); mix(h); mix(h >> 8); mix(vq);
	uint32_t n = vq ? (uint32_t)(2048 + w * h / 4) : (uint32_t)(w * h * 2);
	for (uint32_t i = 0; i < n; i += 2) { if (addr + i >= VRAM_SIZE) break; mix(vram[addr + i]); }
	return hsh ? hsh : 1;
}
// Decode a non-VQ, non-mip fmt 0/1/2 texture to RGBA8888. Returns false if unsupported.
static bool decodeTex16(uint32_t tcw, uint32_t tsp, uint8_t* out) {
	if (!s_twInit) initTwiddle();
	int fmt = (tcw >> 27) & 7, texU = (tsp >> 3) & 7, texV = tsp & 7;
	int w = 8 << texU, h = 8 << texV, scan = (tcw >> 26) & 1, vq = (tcw >> 30) & 1, mip = (tcw >> 31) & 1;
	if (fmt > 2 || mip) return false;
	uint32_t addr = (tcw & 0x1FFFFF) << 3;
	int bx = texBsr(w), by = texBsr(h);
	if (vq) {
		uint8_t cb[256 * 16];                        // codebook: 256 * (2x2 block of 16-bit texels)
		for (int i = 0; i < 256; i++) {
			uint32_t co = addr + i * 8;
			for (int p = 0; p < 4; p++) {
				uint32_t so = co + p * 2; uint8_t* cd = &cb[i * 16 + p * 4];
				if (so + 1 >= VRAM_SIZE) { cd[0] = cd[1] = cd[2] = cd[3] = 0; continue; }
				unpack16(fmt, (uint16_t)(vram[so] | (vram[so + 1] << 8)), cd);
			}
		}
		uint32_t idxAddr = addr + 2048; int hw = w >> 1, hh = h >> 1, bcx = bx - 1, bcy = by - 1;
		for (int y = 0; y < hh; y++) for (int x = 0; x < hw; x++) {
			uint32_t io = idxAddr + s_twTab[0][bcy][x] + s_twTab[1][bcx][y];
			if (io >= VRAM_SIZE) continue;
			int ci = vram[io] * 16, px = x * 2, py = y * 2;
			memcpy(&out[(py * w + px) * 4], &cb[ci], 4);
			memcpy(&out[((py + 1) * w + px) * 4], &cb[ci + 4], 4);
			memcpy(&out[(py * w + px + 1) * 4], &cb[ci + 8], 4);
			memcpy(&out[((py + 1) * w + px + 1) * 4], &cb[ci + 12], 4);
		}
		return true;
	}
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
		uint32_t idx = scan ? (uint32_t)(y * w + x) : (s_twTab[0][by][x] + s_twTab[1][bx][y]);
		uint32_t so = addr + idx * 2; uint8_t* d = &out[(y * w + x) * 4];
		if (so + 1 >= VRAM_SIZE) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
		unpack16(fmt, (uint16_t)(vram[so] | (vram[so + 1] << 8)), d);
	}
	return true;
}

// --- STAF additions: 64-bit content id + paletted decode ---------------------
// The STAF channel ships ALL textured quads, so the texture id must (a) be wide
// enough for a few-thousand-entry working set and (b) fold the palette for
// paletted formats (skins / team-color swaps reuse the same indexed pixels with
// a different palette — same address+content but a DIFFERENT drawn texture).
// Palette unpack mirrors flycast's canonical path (core/rend/texconv.h):
// PAL_RAM_CTRL&3 selects 1555/565/4444/8888; palette_index from tcw.PalSelect
// exactly as TexCache.cpp:463-472.
static inline uint32_t palEntryRGBA(uint32_t pe) {
	// PALETTE_RAM[pe] is a raw 16/32-bit palette word; unpack to RGBA8888 (R,G,B,A bytes).
	if (pe >= 1024) return 0;
	uint32_t w = PALETTE_RAM[pe];
	uint8_t r, g, b, a;
	switch (PAL_RAM_CTRL & 3) {
	case 0: /* 1555 */ r = te5((w >> 10) & 31); g = te5((w >> 5) & 31); b = te5(w & 31); a = (w & 0x8000) ? 255 : 0; break;
	case 1: /* 565 */  r = te5((w >> 11) & 31); g = te6((w >> 5) & 63); b = te5(w & 31); a = 255; break;
	case 2: /* 4444 */ r = te4((w >> 8) & 15); g = te4((w >> 4) & 15); b = te4(w & 15); a = te4((w >> 12) & 15); break;
	default: /* 8888 */ r = (w >> 16) & 0xff; g = (w >> 8) & 0xff; b = w & 0xff; a = (w >> 24) & 0xff; break;
	}
	return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}
// 64-bit FNV-1a over fmt/w/h/vq + raw indexed/texel VRAM bytes, AND (for paletted
// formats) the live palette window the texture selects. Pure content id.
static uint64_t texHash64(uint32_t tcw, int w, int h) {
	int fmt = (tcw >> 27) & 7, vq = (tcw >> 30) & 1;
	uint32_t addr = (tcw & 0x1FFFFF) << 3;
	uint64_t hsh = 1469598103934665603ull;
	auto mix = [&](uint32_t bbb) { hsh ^= (bbb & 0xff); hsh *= 1099511628211ull; };
	mix(fmt); mix(w); mix(w >> 8); mix(h); mix(h >> 8); mix(vq);
	uint32_t n;
	if (fmt == 5)      n = (uint32_t)(w * h / 2);   // PAL4: 4bpp
	else if (fmt == 6) n = (uint32_t)(w * h);       // PAL8: 8bpp
	else if (vq)       n = (uint32_t)(2048 + w * h / 4);
	else               n = (uint32_t)(w * h * 2);   // 16bpp direct
	for (uint32_t i = 0; i < n; i++) { if (addr + i >= VRAM_SIZE) break; mix(vram[addr + i]); }
	// Fold the selected palette window for paletted formats.
	if (fmt == 5) { uint32_t pi = ((tcw >> 21) & 0x3F) << 4; for (int k = 0; k < 16; k++) { uint32_t e = palEntryRGBA(pi + k); mix(e); mix(e >> 8); mix(e >> 16); mix(e >> 24); } }
	else if (fmt == 6) { uint32_t pi = (((tcw >> 21) & 0x3F) >> 4) << 8; for (int k = 0; k < 256; k++) { uint32_t e = palEntryRGBA(pi + k); mix(e); mix(e >> 8); mix(e >> 16); mix(e >> 24); } }
	return hsh ? hsh : 1;
}
// Decode any STAF-supported format to RGBA8888. fmt 0/1/2 (and VQ) delegate to
// decodeTex16; fmt 5/6 are paletted (de-twiddle the index, then palette lookup).
// Returns false for unsupported (mip, exotic) — caller falls back / skips.
static bool decodeTexAny(uint32_t tcw, uint32_t tsp, uint8_t* out) {
	if (!s_twInit) initTwiddle();
	int fmt = (tcw >> 27) & 7, mip = (tcw >> 31) & 1;
	if (mip) return false;
	if (fmt <= 2) return decodeTex16(tcw, tsp, out);
	if (fmt != 5 && fmt != 6) return false;            // only PAL4/PAL8 paletted
	int texU = (tsp >> 3) & 7, texV = tsp & 7;
	int w = 8 << texU, h = 8 << texV, scan = (tcw >> 26) & 1;
	uint32_t addr = (tcw & 0x1FFFFF) << 3;
	int bx = texBsr(w), by = texBsr(h);
	uint32_t palBase = (fmt == 5) ? (((tcw >> 21) & 0x3F) << 4) : ((((tcw >> 21) & 0x3F) >> 4) << 8);
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
		uint32_t lin = scan ? (uint32_t)(y * w + x) : (s_twTab[0][by][x] + s_twTab[1][bx][y]);
		uint32_t pidx;
		if (fmt == 5) {           // 4bpp: two indices per byte
			uint32_t so = addr + (lin >> 1);
			if (so >= VRAM_SIZE) { pidx = 0; }
			else pidx = (lin & 1) ? (vram[so] >> 4) : (vram[so] & 0xf);
		} else {                  // 8bpp
			uint32_t so = addr + lin;
			pidx = (so < VRAM_SIZE) ? vram[so] : 0;
		}
		uint32_t rgba = palEntryRGBA(palBase + pidx);
		uint8_t* d = &out[(y * w + x) * 4];
		d[0] = rgba & 0xff; d[1] = (rgba >> 8) & 0xff; d[2] = (rgba >> 16) & 0xff; d[3] = (rgba >> 24) & 0xff;
	}
	return true;
}
} // namespace mcfx

void serverPublish(TA_context* ctx)
{
	if (!_isServer || !_shmPtr || !ctx) return;

	// Skip heavy work (diff, compress, broadcast) when no clients connected.
	// Still increment the frame counter so local overlays/telemetry work.
	static uint32_t _localFrameNum = 0;
	_localFrameNum++;

	// LOCKSTEP DIAG (throttled): resolve which counter the tape / JOIN / hash
	// each use, and whether the tape publisher is still draining to subscribers.
	if (maplecast_lockstep::active()) {
		static uint32_t _dbgCtr = 0;
		if ((_dbgCtr++ % 120) == 0) {
			auto ts = maplecast_input::getTapeStats();
			bool wsOn = maplecast_ws::active();
			uint32_t wsN = wsOn ? maplecast_ws::clientCount() : 0;
			printf("[lockstep-diag] serverPublish: _localFrameNum=%u "
			       "currentFrame=%llu | ws=%d/%u path=%s | tape lastPub=%llu sent=%llu "
			       "subs=%u drops=%llu\n",
			       _localFrameNum,
			       (unsigned long long)currentFrame(),
			       (int)wsOn, wsN, (wsOn && wsN > 0) ? "FULL" : "early",
			       (unsigned long long)ts.lastPublishedFrame,
			       (unsigned long long)ts.packetsSent, ts.subscribers,
			       (unsigned long long)ts.entriesDropped);
			fflush(stdout);
		}
	}

	// === MAPLECAST_FRAME_ORACLE_HOOK — flush the LIVE block-entry attribution that
	// the recompiler hook (0x8C03093C begin / 0x8C033E90 quad) buffered during this
	// frame's SH4 draw walk. serverPublish runs once per frame AFTER that walk
	// completes, so it is the natural frame boundary. No-op when the hook is OFF.
	maplecast_oracle_hook::mc_oracleInit();   // one-time stderr log if enabled
	// GENERIC PROBE v2 — no-restart live reload (RENDER-THREAD watcher half). Cheap
	// throttled stat() of /dev/shm/mc_oracle_probe.conf; on a changed mtime it sets
	// an internal pending flag ONLY. The actual re-parse + block-cache flush runs on
	// the SH4 thread at the emu-loop boundary (mc_probeApplyReload, emulator.cpp) —
	// NEVER here, because in non-threaded mode serverPublish runs synchronously inside
	// the SH4's STARTRENDER write (a dynarec block can be on the stack). No-op when
	// the probe is disabled.
	maplecast_oracle_hook::mc_probeCheckReload();

	// CHARACTER-PASS TABLE SNAPSHOT (re_kb/50 idxtab effect-range fix). EMPIRICAL: charPassCapture
	// (rend_start_render) only ever sees the HUD pass (realBody=0), but serverPublish HERE gets the
	// CHARACTER pass ctx (the [ORACLE-PASS] frameFlush log shows realBody=22..172 here). The
	// idxtab/rectab EFFECT entries (idxtab[972..1074]) are written by the char-pass submit and
	// reverted by the HUD pass; charPassCapture/captureFrame ship them STALE -> the scale walker
	// resolves the wrong (body) texture for super/projectile effects. So snapshot the LIVE tables
	// HERE (char pass) into replica-live side buffers that captureFrame ships instead of the
	// HUD-pass RAM. Read-only side-snapshot of 2 regions; free when no replica client / not in-match
	// / tables not built. NOT a pass-gate — captureFrame still ships every frame at the HUD pass.
	maplecast_oracle_hook::mc_replicaSnapshotCharPassTables();

	// Pass the live ctx so the flush can ta_parse() the completed frame and recover
	// the per-frame SCREEN quads (real screen x,y) to attribute per OBJ_BEGIN object.
	//
	// CHARQ: when MAPLECAST_CHARQ is set, the per-part CHARACTER body quads come from the
	// pre-QueueRender hook (mc_oracle_charPassCapture, called in rend_start_render for
	// EVERY STARTRENDER context BEFORE QueueRender drops the dropped character pass). The
	// `ctx` serverPublish holds here is the SURVIVING pass — on MVC2 the HUD/composite pass
	// with ZERO body quads. Re-flushing it would clobber the character snapshot the
	// pre-QueueRender hook just published. So SKIP the serverPublish flush in CHARQ mode;
	// the body capture is authoritative from the upstream hook. (Without CHARQ this is the
	// only flush, unchanged.)
	if (std::getenv("MAPLECAST_CHARQ") == nullptr)
		maplecast_oracle_hook::mc_oracle_frameFlush(ctx, _localFrameNum);

	// === MAPLECAST_STATELOG — per-frame RAM state probe (ROM-asset-client test)
	// Read-only readGameState() + CSV append. Placed BEFORE the PVR snapshot
	// and all wire/diff/compress work so it cannot perturb the deterministic
	// mirror stream (see docs/ARCHITECTURE.md "THE WIRE IS DETERMINISTIC").
	// Runs every frame regardless of client connection, so it works in
	// -ServerOnly mode too. Completely inert unless MAPLECAST_STATELOG is set.
	// This is the Tele-0 per-frame state read from docs/MATCH-DATA-PLATFORM.md.
	{
		static FILE* _stateLog = nullptr;
		static bool  _stateLogInit = false;
		if (!_stateLogInit) {
			_stateLogInit = true;
			if (const char* path = std::getenv("MAPLECAST_STATELOG")) {
				_stateLog = fopen(path, "w");
				if (_stateLog)
					fprintf(_stateLog, "frame,in_match,slot,char_id,active,"
						"sprite_id,anim_state,anim_timer,facing,screen_x,"
						"screen_y,palette,anim_ptr\n");
			}
		}
		if (_stateLog) {
			maplecast_gamestate::GameState gs;
			maplecast_gamestate::readGameState(gs);
			for (int i = 0; i < 6; i++) {
				const maplecast_gamestate::CharacterState& c = gs.chars[i];
				if (!c.active) continue;
				fprintf(_stateLog,
					"%u,%u,%d,%u,%u,%u,%u,%u,%u,%.2f,%.2f,%u,0x%08X\n",
					_localFrameNum, gs.in_match, i, c.character_id, c.active,
					c.sprite_id, c.animation_state, c.anim_timer,
					c.facing_right, c.screen_x, c.screen_y, c.palette_id,
					c.anim_pointer);
			}
			if ((_localFrameNum % 60) == 0) fflush(_stateLog);
		}
	}

	// Emu-thread MCSV capture: dc_serialize must run here (between frames,
	// SR.BL=0), NOT on the status thread that requests it — a mid-interrupt
	// snapshot crashes replica clients on load ("SH4 exception when blocked").
	// Cheap atomic check every frame; the heavy serialize fires once per match.
	if (maplecast_ws::active())
		maplecast_ws::drainMcsvCapture();

	if (!maplecast_ws::active() || maplecast_ws::clientCount() == 0) {
		// Update frame counter + basic telemetry for local overlays
		_atomicCurrentFrame.store(_localFrameNum, std::memory_order_release);
		maplecast_ws::updateTelemetry({_localFrameNum, 0, 0, 0, 0,
			60, 0, 0}); // approximate 60fps
		// A.5 — predictor needs to see authoritative input every frame
		// even without clients. publishFrameTick is cheap (tape-ring push
		// + predictor compare) so fire it regardless. Used for: rollback
		// netcode predictor (A.5/A.6), .mcrec recording when active.
		maplecast_input::publishFrameTick(_localFrameNum);

		// Lockstep-mirror (env-gated MAPLECAST_LOCKSTEP=1, default OFF): a
		// native lockstep client can connect to a headless authoritative
		// server that has NO browser WS clients — this is that path. The
		// full-client path below ships JOIN + tape + hash keyed on
		// hdr->frame_count; here there is no WS client so we key everything on
		// _localFrameNum instead (which _atomicCurrentFrame was just set to, so
		// currentFrame() and the JOIN's serverFrame agree). Ship the JOIN
		// snapshot (only builds when a state-sync client still needs it) and
		// the game-state checksum so the lockstep client can sync + verify.
		// Entirely inert unless MAPLECAST_LOCKSTEP is set.
		if (maplecast_lockstep::active()) {
			maplecast_state_sync::onServerFramePublished(_localFrameNum);
			maplecast_lockstep::onServerFrame(_localFrameNum);
		}
		return;
	}

	auto publishStart = std::chrono::high_resolution_clock::now();
	rend_context& rc = ctx->rend;
	// DON'T skip RTT frames  --  MVC2 renders character sprites via render-to-texture!

	// === PVR ATOMIC SNAPSHOT ===
	// Snapshot the entire 32 KB pvr_regs block ONCE at the top of the
	// function, into a thread-local buffer. Then everything downstream in
	// this function (the inline pvr_snapshot[16], the diff loop's PVR
	// region scan, and the hash log) reads from this snapshot, NOT live
	// pvr_regs. This eliminates the SPG-vs-diff race where SPG_STATUS
	// (and other hardware-driven PVR registers) tick mid-function and
	// cause server-vs-client hash divergence (the "PVR phase noise").
	//
	// We point _regions[].ptr for the PVR region at the snapshot for the
	// duration of this function and restore it before returning.
	static uint8_t _pvrAtomicSnap[pvr_RegSize];
	memcpy(_pvrAtomicSnap, pvr_regs, pvr_RegSize);
	uint8_t* _origPvrPtr = nullptr;
	for (int r = 0; r < _numRegions; r++) {
		if (_regions[r].id == 3) {
			_origPvrPtr = _regions[r].ptr;
			_regions[r].ptr = _pvrAtomicSnap;
			break;
		}
	}

	RingHeader* hdr = (RingHeader*)_shmPtr;
	uint8_t* ring = _shmPtr + RING_START;

	uint64_t writePos = hdr->write_pos;
	if (writePos + RING_SIZE / 3 > RING_SIZE) writePos = 0;

	uint8_t* dst = ring + writePos;
	uint8_t* dstStart = dst;

	// Frame header
	dst += 4;  // placeholder for size
	uint32_t frameNum = (uint32_t)(hdr->frame_count + 1);
	memcpy(dst, &frameNum, 4); dst += 4;

	// === PVR registers needed by rend_start_render ===
	// These set up the rend_context hardware params. All values come from
	// the atomic snapshot taken at the top of the function  --  NOT from live
	// pvr_regs  --  so they're consistent with what the diff loop ships.
	#define _SNAP_U32(addr) (*(u32*)&_pvrAtomicSnap[(addr) & pvr_RegMask])
	uint32_t pvr_snapshot[16];
	pvr_snapshot[0] = _SNAP_U32(TA_GLOB_TILE_CLIP_addr);
	pvr_snapshot[1] = _SNAP_U32(SCALER_CTL_addr);
	pvr_snapshot[2] = _SNAP_U32(FB_X_CLIP_addr);
	pvr_snapshot[3] = _SNAP_U32(FB_Y_CLIP_addr);
	pvr_snapshot[4] = _SNAP_U32(FB_W_LINESTRIDE_addr);
	pvr_snapshot[5] = _SNAP_U32(FB_W_SOF1_addr);
	pvr_snapshot[6] = _SNAP_U32(FB_W_CTRL_addr);
	pvr_snapshot[7] = _SNAP_U32(FOG_CLAMP_MIN_addr);
	pvr_snapshot[8] = _SNAP_U32(FOG_CLAMP_MAX_addr);
	#undef _SNAP_U32
	pvr_snapshot[9] = rc.framebufferWidth;
	pvr_snapshot[10] = rc.framebufferHeight;
	pvr_snapshot[11] = rc.clearFramebuffer ? 1 : 0;
	float fz = rc.fZ_max;
	memcpy(&pvr_snapshot[12], &fz, 4);
	pvr_snapshot[13] = rc.isRTT ? 1 : 0;
	memcpy(dst, pvr_snapshot, sizeof(pvr_snapshot)); dst += sizeof(pvr_snapshot);

	// === Raw TA command buffer  --  double-buffered delta ===
	uint32_t taSize = (uint32_t)(ctx->tad.thd_data - ctx->tad.thd_root);
	uint8_t* taData = ctx->tad.thd_root;

	// NOTE: Palette bank probe (dynamic targeting) was attempted but
	// the TA buffer scan had alignment issues  --  TA commands are variable
	// size (32B/64B) and a fixed 32B scan step misses polygon headers.
	// For now, applyPaletteOverrides() blasts all specified entries.
	// This is ~1Âµs overhead per frame  --  negligible. A correct probe
	// would need to hook into the actual TA parser (ta_vtx.cpp) or the
	// renderer's texture processing (gldraw.cpp:176 PalSelect read).
	// TODO: revisit when needed.

	// === TA DUMP  --  determinism / decomposition test ===
	// MAPLECAST_DUMP_TA=1 â†’ write each frame's raw TA buffer to
	// /tmp/ta-dumps/frame_NNNNNN.bin. Use to capture TA buffers from a
	// reproducible save state, then byte-diff across runs (determinism)
	// or across save state variants (per-character decomposition).
	{
		static bool _dumpInit = false;
		static bool _dumpEnabled = false;
		static std::string _dumpDir;
		if (!_dumpInit) {
			const char* e = std::getenv("MAPLECAST_DUMP_TA");
			_dumpEnabled = (e && *e && *e != '0');
			if (_dumpEnabled) {
				const char* d = std::getenv("MAPLECAST_DUMP_TA_DIR");
				_dumpDir = (d && *d) ? d : "/tmp/ta-dumps";
#ifdef _WIN32
				int rc = _mkdir(_dumpDir.c_str());
#else
				int rc = mkdir(_dumpDir.c_str(), 0755);
#endif
				printf("[TA-DUMP] server enabled — writing %s/frame_NNNNNN.bin (mkdir rc=%d, errno=%d)\n",
				       _dumpDir.c_str(), rc, errno);
				fflush(stdout);
			}
			_dumpInit = true;
		}
		if (_dumpEnabled && taSize > 0) {
			char path[512];
			snprintf(path, sizeof(path), "%s/frame_%06u.bin", _dumpDir.c_str(), frameNum);
			FILE* f = fopen(path, "wb");
			if (f) {
				fwrite(taData, 1, taSize, f);
				fclose(f);
			} else {
				static int _warnedFopen = 0;
				if (_warnedFopen++ < 3)
					printf("[TA-DUMP] fopen(%s) failed: errno=%d\n", path, errno);
			}
		}
	}

	int cur = _taCur;
	int prev = 1 - cur;
	uint8_t* prevData = _taBuf[prev];
	uint32_t prevSize = _taBufSize[prev];

	// Copy current TA into double buffer
	memcpy(_taBuf[cur], taData, taSize);
	_taBufSize[cur] = taSize;

	// TACANON (Phase 1.5): canonicalize the WIRE copy only — the live TA and the
	// MAPLECAST_DUMP_TA server dump above are untouched. Both delta legs (this
	// buffer and prev, canonicalized last frame) are canonical, so dead-byte
	// churn never reaches the run encoder. Keyframes ship canonical too — the
	// zeroed ranges are parser-ignored by construction (see taCanonicalize).
	{
		const int _tcMode = tacanonMode();
		if (_tcMode) {
			taCanonicalize(_taBuf[cur], taSize, _tcMode == 2);
			_tacanonFrames++;
			if (frameNum % 600 == 0 && _tacanonFrames > 0) {
				printf("[TACANON] mode=%s deadB/frame=%llu (%.1f%% of %u B buffer)\n",
					_tcMode == 1 ? "measure" : "zero",
					(unsigned long long)(_tacanonDead / _tacanonFrames),
					taSize ? 100.0 * (_tacanonDead / (double)_tacanonFrames) / taSize : 0.0,
					taSize);
				fflush(stdout);
			}
		}
	}

	static uint64_t totalDeltaPayload = 0;
	static uint64_t totalTABytes = 0;
	static uint32_t deltaFrames = 0;

	memcpy(dst, &taSize, 4); dst += 4;

	bool forceKeyframe = (frameNum % 60 == 0);
	bool canDelta = _taHasPrev && taSize > 0 && !forceKeyframe;

	if (canDelta)
	{
		uint8_t* deltaStart = dst;
		dst += 4;

		uint32_t commonSize = std::min(taSize, prevSize);

		uint32_t i = 0;
		while (i < taSize)
		{
			while (i < commonSize && taData[i] == prevData[i]) i++;
			if (i >= taSize) break;

			uint32_t runStart = i;
			while (i < taSize && (i - runStart) < 65535 &&
				   (i >= commonSize || taData[i] != prevData[i])) i++;
			if (i < taSize) {
				uint32_t gapEnd = std::min(i + 8, taSize);
				bool moreChanges = false;
				for (uint32_t j = i; j < gapEnd; j++)
					if (j >= commonSize || taData[j] != prevData[j]) { moreChanges = true; break; }
				if (moreChanges)
					while (i < gapEnd) i++;
			}

			// CRITICAL: clamp runLen to u16 max. The gap-merge above can push
			// (i - runStart) up to 65535+8 = 65543, which would overflow u16
			// and emit a tiny truncated run on the wire  --  the client then
			// mis-decodes the rest of the wire as garbage. Manifested as
			// scene-change garble on buffer growth (prev=320 â†’ cur=89120):
			// the first run's wire length wrapped, all subsequent runs shifted.
			uint32_t fullLen = i - runStart;
			if (fullLen > 65535) {
				i = runStart + 65535;
				fullLen = 65535;
			}
			uint16_t runLen = (uint16_t)fullLen;
			memcpy(dst, &runStart, 4); dst += 4;
			memcpy(dst, &runLen, 2); dst += 2;
			memcpy(dst, taData + runStart, runLen); dst += runLen;
		}
		uint32_t term = 0xFFFFFFFF;
		memcpy(dst, &term, 4); dst += 4;

		uint32_t deltaPayloadSize = (uint32_t)(dst - deltaStart - 4);
		memcpy(deltaStart, &deltaPayloadSize, 4);

		totalDeltaPayload += deltaPayloadSize;
		totalTABytes += taSize;
		deltaFrames++;

		if (frameNum % 600 == 0 && deltaFrames > 0) {
			float avgDelta = (float)totalDeltaPayload / deltaFrames;
			float avgTA = (float)totalTABytes / deltaFrames;
			printf("[MIRROR] TA DELTA: %.1f KB / %.1f KB (%.1f%%) | stream: %.1f MB/s\n",
				avgDelta / 1024.0, avgTA / 1024.0,
				avgDelta * 100.0 / avgTA, avgDelta * 60.0 / 1024.0 / 1024.0);
		}
	}
	else
	{
		uint32_t deltaPayloadSize = taSize;
		memcpy(dst, &deltaPayloadSize, 4); dst += 4;
		if (taSize > 0) { memcpy(dst, taData, taSize); dst += taSize; }
	}

	// Swap double buffer
	_taCur = prev;
	_taHasPrev = true;

	// Checksum disabled  --  client skips it, TCP guarantees integrity
	// uint32_t taChecksum = 0;
	// for (uint32_t i = 0; i < taSize; i += 4)
	// 	taChecksum ^= *(uint32_t*)(taData + i);
	uint32_t taChecksum = 0;  // placeholder  --  client expects 4 bytes here
	memcpy(dst, &taChecksum, 4); dst += 4;

	// Persistent palette overrides  --  re-apply custom palette colors to
	// PVR palette RAM BEFORE the diff scan so the changes are captured
	// in this frame's dirty pages and shipped to all viewers.
	maplecast_control_ws::applyPaletteOverrides();

	// === Memory diffs ===
	uint32_t totalDirty = 0;

	// VRAM content-cache mode (env MAPLECAST_VCACHE). See the VCACHE block near
	// vcacheHashPage() above for the wire change. Per-stream sent-set of page
	// content hashes, cleared every VCACHE_RESEED_FRAMES so mid-stream joiners
	// recover. Resolved once; default OFF (production unaffected).
	static const bool _vcacheOn = (std::getenv("MAPLECAST_VCACHE") != nullptr);
	// MEASURE-ONLY: ship the NORMAL wire (render unbroken) but count would-be-saved
	// pages, so we can read the real dedup savings while the game runs a real match.
	static const bool _vcacheMeasure = (std::getenv("MAPLECAST_VCACHE_MEASURE") != nullptr);
	static std::unordered_set<uint64_t> _vcacheSent;
	if ((_vcacheOn || _vcacheMeasure) && frameNum > 0 && (frameNum % VCACHE_RESEED_FRAMES) == 0)
		_vcacheSent.clear();

	// dirtyPageCount slot. In VCACHE mode we write the sentinel + a real-count
	// slot so the client can tell the two encodings apart.
	uint8_t* dirtyCountPtr;
	if (_vcacheOn) {
		uint32_t sentinel = VCACHE_PAGE_SENTINEL;
		memcpy(dst, &sentinel, 4); dst += 4;   // marks this frame as VCACHE-encoded
		dirtyCountPtr = dst; dst += 4;          // real count patched in below
	} else {
		dirtyCountPtr = dst; dst += 4;
	}

	// Drain the DMA force-dirty bitmap atomically. Any page bit set here
	// is guaranteed to ship even if memcmp would say it's unchanged (e.g.
	// when DMA wrote new texture data and shadow already matches because
	// the previous frame already saw it but never sent it).
	uint64_t forcedDirty[VRAM_BITMAP_WORDS];
	for (size_t i = 0; i < VRAM_BITMAP_WORDS; i++)
		forcedDirty[i] = _vramDirtyBitmap[i].exchange(0, std::memory_order_acquire);

	// === MAPLECAST_PAGEGATE (TA-Wire v2 Phase 0, docs/TA-WIRE-V2-PLAN.md) ===
	// The forced-ship above predates the deterministic wire: with strict
	// memcmp-vs-shadow and every shadow write paired with a client SYNC/ship
	// (sites :491/:2210/:2325/:3518), shadow == client state, so a forced page
	// whose bytes EQUAL shadow is a page the client provably already holds.
	// Measured (render-state/07): 56.9% of shipped pages are such re-ships.
	//   =measure : wire UNCHANGED, count forced-equal pages (live validation)
	//   =1       : forced bit no longer forces a memcmp-equal ship
	// Default (unset/0): legacy behavior, ship forced pages unconditionally.
	static const int _pageGate = [](){
		const char* e = std::getenv("MAPLECAST_PAGEGATE");
		if (!e || !*e || *e == '0') return 0;
		return (*e == 'm' || *e == 'M') ? 1 : 2;
	}();
	static uint64_t _pgForcedEqual = 0, _pgForcedTotal = 0, _pgShipped = 0;

	// VRAM + PVR regs: memcmp against shadow copies, OR forced-dirty bitmap (VRAM only)
	//
	// Snapshot live â†’ shadow ONCE per dirty page, then ship from shadow. The
	// SH4 thread can race during the diff and write new bytes between the
	// memcmp and the wire copy; reading via the shadow keeps wire and next
	// frame's memcmp consistent.
	uint32_t vcacheRefPages = 0;   // pages shipped as references (no 4096 bytes)
	for (int r = 0; r < _numRegions; r++) {
		MemRegion& reg = _regions[r];
		size_t numPages = reg.size / MEM_PAGE_SIZE;
		bool isVram = (reg.id == 1);
		for (size_t p = 0; p < numPages; p++) {
			size_t off = p * MEM_PAGE_SIZE;
			bool forced = isVram && (forcedDirty[p >> 6] & (1ULL << (p & 63)));
			// PAGEGATE: when active, always run the content compare so a forced
			// bit on identical bytes can be counted (measure) or skipped (gate).
			bool changed;
			if (_pageGate && forced) {
				changed = memcmp(reg.ptr + off, reg.shadow + off, MEM_PAGE_SIZE) != 0;
				_pgForcedTotal++;
				if (!changed) {
					_pgForcedEqual++;
					if (_pageGate == 2) continue;   // gate mode: client provably has these bytes
				}
			} else {
				changed = forced || memcmp(reg.ptr + off, reg.shadow + off, MEM_PAGE_SIZE) != 0;
			}
			if (changed || forced) {
				// Worst-case slot size: VCACHE header (1+4+8+1=14) + data, or
				// standard header (1+4=5) + data. Use 14 to cover both.
				if ((size_t)(dst - dstStart) + 14 + MEM_PAGE_SIZE > RING_SIZE / 3)
					goto done_diff;
				memcpy(reg.shadow + off, reg.ptr + off, MEM_PAGE_SIZE);
				uint32_t pi = (uint32_t)p;
				if (_vcacheOn) {
					uint64_t h = vcacheHashPage(reg.shadow + off);
					bool seen = !_vcacheSent.insert(h).second;  // insert; seen if already present
					*dst++ = reg.id;
					memcpy(dst, &pi, 4); dst += 4;
					memcpy(dst, &h, 8);  dst += 8;
					*dst++ = seen ? 0 : 1;                       // hasData flag
					if (seen) vcacheRefPages++;
					else { memcpy(dst, reg.shadow + off, MEM_PAGE_SIZE); dst += MEM_PAGE_SIZE; }
				} else {
					if (_vcacheMeasure) {  // count would-be-deduped pages; ship normal wire
						uint64_t h = vcacheHashPage(reg.shadow + off);
						if (!_vcacheSent.insert(h).second) vcacheRefPages++;
					}
					*dst++ = reg.id;
					memcpy(dst, &pi, 4); dst += 4;
					memcpy(dst, reg.shadow + off, MEM_PAGE_SIZE); dst += MEM_PAGE_SIZE;
				}
				totalDirty++;
			}
		}
	}
done_diff:
	memcpy(dirtyCountPtr, &totalDirty, 4);

	// PAGEGATE telemetry (both modes), every 600 frames. Prints even when
	// forcedTotal==0 — "the forced path never fired" is itself a finding.
	if (_pageGate) {
		_pgShipped += totalDirty;
		if (frameNum % 600 == 0) {
			printf("[PAGEGATE] mode=%s forced=%llu forcedEqual=%llu (%.1f%%) shipped=%llu %s\n",
				_pageGate == 1 ? "measure" : "gate",
				(unsigned long long)_pgForcedTotal,
				(unsigned long long)_pgForcedEqual,
				_pgForcedTotal ? 100.0 * _pgForcedEqual / _pgForcedTotal : 0.0,
				(unsigned long long)_pgShipped,
				_pageGate == 1 ? "(wire unchanged)" : "(equal pages skipped)");
			fflush(stdout);
		}
	}

	// VCACHE byte accounting — pre-compression (uncompressed inner-frame) bytes
	// saved by shipping references instead of full pages this frame, and a
	// running total. Logged alongside the periodic server-frame line below.
	if (_vcacheOn || _vcacheMeasure) {
		static uint64_t _vcSavedTotal = 0;
		const uint64_t savedThisFrame = (uint64_t)vcacheRefPages * MEM_PAGE_SIZE;
		_vcSavedTotal += savedThisFrame;
		// Log at the LAST frame before reseed (sent-set fullest = steady-state dedup),
		// NOT frame 0 where the set was just cleared (would always read 0 refs).
		if (frameNum % VCACHE_RESEED_FRAMES == VCACHE_RESEED_FRAMES - 1) {
			printf("[VCACHE] frame %u | dirty=%u (%u refs, %u full) | full*60=%.0f KB/s new-VRAM | saved %llu KB this frame, %llu MB total | sent-set=%zu\n",
				frameNum, totalDirty, vcacheRefPages, totalDirty - vcacheRefPages,
				(totalDirty - vcacheRefPages) * (double)MEM_PAGE_SIZE * 60.0 / 1024.0,
				(unsigned long long)(savedThisFrame / 1024),
				(unsigned long long)(_vcSavedTotal / (1024 * 1024)), _vcacheSent.size());
			fflush(stdout);
		}
	}

	// === Scene-change & forced SYNC broadcast ===
	// Two trigger paths:
	//   1. Forced flag (soft reset SB_SFRES, hard reset, explicit request)  -- 
	//      always fires regardless of rate limit. The caller knows the
	//      renderer state is invalid.
	//   2. Heuristic: lots of dirty pages in one frame = scene transition
	//      (stage load, character select â†’ match, intro cinematic, etc.).
	//      ~512KB threshold  --  half what was guessed last time. The DMA
	//      bitmap now catches more pages so the threshold can be lower
	//      without losing precision. Rate-limited to 1 per 2 seconds so a
	//      noisy game can't DoS us with constant fresh syncs.
	//
	// Scene-change heuristic SYNC was REMOVED. With ARCHITECTURE.md bugs
	// #6/#7/#8 fixed, the per-frame delta path ships scene transitions
	// (300-540 dirty pages in one envelope) byte-perfect to the wasm via
	// the MAX_FRAME oversized fallback. The heuristic SYNC is redundant
	// for correctness.
	//
	// HOWEVER: a low-frequency (60s) periodic SYNC remains, NOT as a
	// correctness band-aid but as a NETWORK RESILIENCE measure. Real-world
	// failure modes the periodic SYNC catches:
	//   - Browser tab throttling causing the WS worker to fall behind and
	//     miss frames; when the tab regains focus the per-frame deltas are
	//     all that's coming and there's no recovery mechanism otherwise.
	//   - Diff-loop torn page: SH4 thread mutates a VRAM page mid-memcpy.
	//     The wasm renders garbage for that page until the page is touched
	//     again. Over a long match this can manifest as persistent corner
	//     glitches that scene-change SYNCs used to clear up.
	//   - Relay restarts / brief disconnects.
	// At ~600 KB compressed every 60s = 10 KB/s overhead, this is free.
	// Crank the interval lower if you observe more frequent drift.
	static constexpr uint64_t PERIODIC_SYNC_FRAMES = 60 * 60;  // 60s @ 60fps
	static uint64_t _lastSceneSyncFrame = 0;

	bool forced = _forceSyncBroadcast.exchange(false, std::memory_order_relaxed);
	bool manualSave = _forceFullSaveStateBroadcast.exchange(false, std::memory_order_relaxed);
	bool periodic = (frameNum > 60 && frameNum - _lastSceneSyncFrame >= PERIODIC_SYNC_FRAMES);

	if ((forced || manualSave || periodic) && maplecast_ws::active())
	{
		_lastSceneSyncFrame = frameNum;
		const char* reason = manualSave ? "MANUAL SAVE STATE PUSH"
			: forced ? "FORCED SYNC"
			: "60s periodic resilience SYNC";
		printf("[MIRROR] %s on frame %u (%u dirty pages)  --  broadcasting fresh SYNC\n",
			reason, frameNum, totalDirty);
		// Ship a fresh SYNC envelope (full VRAM + PVR). The flycast wasm
		// browser routes this through renderer_sync() which does the full
		// _prevTA.clear() + cache reset ritual that the per-frame delta
		// path doesn't. The flycast client's per-frame loop ignores SYNC
		// magic mid-stream (sanity-skips), so this is a no-op for it  -- 
		// safe to fire on both.
		maplecast_ws::broadcastFreshSync();
		// ARCHITECTURE.md "Mirror Wire Format  --  Rules of the Road" bug #7:
		// reset the TA delta encoder. The wasm's renderer_sync() clears its
		// _prevTA on SYNC receipt. If the very next frame we ship is a delta
		// (because _taHasPrev is still true here), the wasm hits the
		// _prevTA.empty() branch in renderer_frame() and silently drops the
		// delta payload  --  measured at ~23 dropped frames per scene transition.
		// Forcing the next frame to be a keyframe re-populates wasm's _prevTA.
		_taHasPrev = false;
		// ARCHITECTURE.md bug #8: also re-snapshot the per-region shadows to
		// match what broadcastFreshSync() just shipped. Without this, the
		// next frame's memcmp diff is computed against the pre-SYNC shadow
		// (broadcastFreshSync reads live vram[]/pvr_regs but never touches
		// _regions[].shadow), shipping wrong-base deltas grafted on top of
		// the SYNC bytes  --  permanent vram divergence. Matches the existing
		// client_request_sync handler pattern.
		for (int i = 0; i < _numRegions; i++)
			memcpy(_regions[i].shadow, _regions[i].ptr, _regions[i].size);
		// ZCS2: restart the streaming envelope at this SYNC so joiners decode onward.
		_zstreamResetPending.store(true, std::memory_order_release);
	}

	// Patch frame size
	uint32_t totalSize = (uint32_t)(dst - dstStart);
	uint32_t frameSizeVal = totalSize - 4;
	memcpy(dstStart, &frameSizeVal, 4);

	__sync_synchronize();
	hdr->latest_offset = writePos;
	hdr->latest_size = totalSize;
	hdr->write_pos = writePos + totalSize;
	hdr->frame_count++;

	// Lockstep player-client tape: push one entry per slot per server
	// frame, stamped with the brand-new frame number we just committed.
	// This is the GGPO-equivalent dense input log  --  see publishFrameTick
	// in maplecast_input_server.cpp for the rationale.
	maplecast_input::publishFrameTick(hdr->frame_count);

	// Phase 1 A.4 rollback ring — hook moved to Emulator::start's emu-thread
	// loop (right after runInternal() returns) so saveFrame runs on the SAME
	// thread that produced the page-faults. serverPublish runs on the rend
	// thread, which would race with the SH4's writes to memwatch state.

	// Periodic state checkpoint for the replay sidecar — every N frames
	// during recording, capture a fresh savestate so the reader can seek
	// into long replays without playing through leading idle. Default
	// cadence is 600 frames (~10s @ 60Hz). Tunable via env. Only fires
	// when recording is active; checkpoint() is a cheap no-op otherwise.
	{
		static const uint32_t _ckptInterval = []() {
			const char* e = std::getenv("MAPLECAST_REPLAY_CKPT_INTERVAL");
			if (e && *e) {
				int n = std::atoi(e);
				if (n > 0) return (uint32_t)n;
			}
			return (uint32_t)600;
		}();
		if (hdr->frame_count > 0 && (hdr->frame_count % _ckptInterval) == 0)
			maplecast_replay::checkpoint(hdr->frame_count);
	}

	// Phase A  --  mirror the new frame number + publish wall-clock time into
	// the read-only atomics that ggpo::getLocalInput() / status JSON consume.
	// Release ordering ensures the input latch sees the bumped frame counter
	// and a fresh latch-time-stamp consistently.
	//
	// Phase B  --  compute live frame period as an exponential moving average of
	// (this publish - prev publish). EMA factor 1/16 â†’ ~16-frame smoothing,
	// converges in <0.3 s. Used by the frame_phase block in status JSON for
	// browser-side phase-aligned send scheduling.
	const int64_t nowPub = _publishNowUs();
	const int64_t prevPub = _atomicLastLatchTimeUs.load(std::memory_order_relaxed);
	if (prevPub > 0) {
		const int64_t delta = nowPub - prevPub;
		// Reject obvious outliers (paused emulator, savestate load, etc.)
		//  --  anything more than 4 frames or less than 1 ms is not a normal
		// frame interval and would corrupt the EMA.
		if (delta >= 1000 && delta <= 70000) {
			const int64_t prevPeriod = _atomicFramePeriodUs.load(std::memory_order_relaxed);
			const int64_t newPeriod = prevPeriod + ((delta - prevPeriod) >> 4);  // EMA, 1/16
			_atomicFramePeriodUs.store(newPeriod, std::memory_order_relaxed);
		}
	}
	_atomicCurrentFrame.store(hdr->frame_count, std::memory_order_release);
	_atomicLastLatchTimeUs.store(nowPub, std::memory_order_release);

	// Phase 3 of lockstep-player-client: on a STATE_SYNC_INTERVAL
	// schedule (default 60 frames), build a fresh dc_serialize snapshot
	// and broadcast it to connected native player clients. Runs on the
	// emu thread  --  the build+compress happens synchronously here, the
	// wire send is async on per-client send threads. A no-op unless
	// the state-sync server is running AND at least one client is
	// connected.
	maplecast_state_sync::onServerFramePublished(hdr->frame_count);

	// Lockstep-mirror: ship the game-state-region checksum for this committed
	// frame (every MAPLECAST_LOCKSTEP_INTERVAL frames). Same frame number the
	// tape + JOIN snapshot use here, so client-side compares key correctly.
	maplecast_lockstep::onServerFrame(hdr->frame_count);

	// Compress + broadcast over WebSocket to browser clients
	uint64_t compressUs = 0;
	uint32_t compressedSize = totalSize;
	if (maplecast_ws::active())
	{
		// Legacy ZCST broadcast — ALWAYS emitted, byte-for-byte unchanged, so every
		// existing client (king.html, emulator.html, native, relay cache) is
		// unaffected by the ZCS2 experiment. ZCS2 rides alongside as an EXTRA
		// message (shadow mode) until all parsers migrate.
		{
		size_t compSize = 0;
		const uint8_t* compData = _compressor.compress(dstStart, totalSize, compSize, compressUs);
		maplecast_ws::broadcastBinary(compData, compSize);
		compressedSize = (uint32_t)compSize;
		}

		if (zstreamEnabled()) {
			// ZCS2 streaming envelope (see block comment at _zstreamResetPending).
			static ZSTD_CCtx* _zc = nullptr;
			static uint8_t _zEpoch = 0;
			static uint32_t _zSinceReset = 0;
			static std::vector<uint8_t> _zBuf;
			static const int _zLevel = [](){ const char* e = std::getenv("MAPLECAST_ZSTREAM_LEVEL");
				int v = e ? atoi(e) : 3; return (v >= 1 && v <= 19) ? v : 3; }();
			static const uint32_t _zResetEvery = [](){ const char* e = std::getenv("MAPLECAST_ZSTREAM_RESET");
				return (uint32_t)(e ? atoi(e) : 300); }();
			auto t0z = std::chrono::steady_clock::now();
			bool streamStart = false;
			if (!_zc) { _zc = ZSTD_createCCtx(); streamStart = true; }
			if (_zstreamResetPending.exchange(false, std::memory_order_acq_rel)) streamStart = true;
			if (_zResetEvery && _zSinceReset >= _zResetEvery) streamStart = true;
			if (streamStart) {
				ZSTD_CCtx_reset(_zc, ZSTD_reset_session_only);
				ZSTD_CCtx_setParameter(_zc, ZSTD_c_compressionLevel, _zLevel);
				ZSTD_CCtx_setParameter(_zc, ZSTD_c_windowLog, 24);
				_zEpoch++; _zSinceReset = 0;
			}
			_zSinceReset++;
			// --- Phase 2 runSoA (MAPLECAST_ZSTREAM_SOA=1, flags bit1) -------------
			// Rewrite the interleaved delta runs [off u32][len u16][data]...term as
			// [nRuns u32][gap-offs u32×n][lens u16×n][data]. Same info, SoA layout:
			// ascending small gaps + repetitive lens compress far better and drop the
			// per-run header interleave (39% of the delta section). Keyframes and
			// non-delta frames pass through untransformed (bit1 clear). The client
			// inverse-transform reconstructs the EXACT legacy inner bytes.
			static const bool _zSoA = [](){ const char* e = std::getenv("MAPLECAST_ZSTREAM_SOA");
				return e && *e && *e != '0'; }();
			static std::vector<uint8_t> _soaBuf;
			const uint8_t* zPayload = dstStart; uint32_t zPayloadSize = totalSize; bool soaDone = false;
			uint32_t _dPay; memcpy(&_dPay, dstStart + 76, 4);
			uint32_t _dTa;  memcpy(&_dTa,  dstStart + 72, 4);
			if (_zSoA && totalSize > 84 && _dPay != _dTa) {   // delta frame only
				const uint8_t* runs = dstStart + 80; uint32_t rp = 0, nRuns = 0, dataB = 0; bool ok = true;
				while (rp + 4 <= _dPay) {           // count pass
					uint32_t roff; memcpy(&roff, runs + rp, 4);
					if (roff == 0xFFFFFFFFu) { rp += 4; break; }
					if (rp + 6 > _dPay) { ok = false; break; }
					uint16_t rl; memcpy(&rl, runs + rp + 4, 2);
					if (rp + 6 + rl > _dPay) { ok = false; break; }
					nRuns++; dataB += rl; rp += 6 + rl;
				}
				if (ok && rp == _dPay) {
					uint32_t tailOff = 80 + _dPay, tailLen = totalSize - tailOff;
					uint32_t v2Sec = 4 + nRuns * 6 + dataB;
					_soaBuf.resize(80 + v2Sec + tailLen);
					memcpy(_soaBuf.data(), dstStart, 80);
					memcpy(_soaBuf.data() + 76, &v2Sec, 4);          // v2 deltaPayloadSize
					uint8_t* o = _soaBuf.data() + 80;
					memcpy(o, &nRuns, 4);
					uint8_t* offs = o + 4; uint8_t* lens = offs + nRuns * 4; uint8_t* dat = lens + nRuns * 2;
					uint32_t prevOff = 0; rp = 0;
					for (uint32_t i = 0; i < nRuns; i++) {
						uint32_t roff; memcpy(&roff, runs + rp, 4);
						uint16_t rl;  memcpy(&rl,  runs + rp + 4, 2);
						uint32_t gap = roff - prevOff; prevOff = roff;
						memcpy(offs + i * 4, &gap, 4);
						memcpy(lens + i * 2, &rl, 2);
						memcpy(dat, runs + rp + 6, rl); dat += rl;
						rp += 6 + rl;
					}
					memcpy(dat, dstStart + tailOff, tailLen);
					zPayload = _soaBuf.data(); zPayloadSize = (uint32_t)(80 + v2Sec + tailLen);
					soaDone = true;
				}
			}
			_zBuf.resize(10 + ZSTD_compressBound(zPayloadSize));
			_zBuf[0]='Z'; _zBuf[1]='C'; _zBuf[2]='S'; _zBuf[3]='2';
			_zBuf[4]=_zEpoch; _zBuf[5]=(uint8_t)((streamStart ? 1 : 0) | (soaDone ? 2 : 0));
			memcpy(_zBuf.data()+6, &zPayloadSize, 4);
			ZSTD_outBuffer ob{ _zBuf.data()+10, _zBuf.size()-10, 0 };
			ZSTD_inBuffer  ib{ zPayload, zPayloadSize, 0 };
			size_t zr = ZSTD_compressStream2(_zc, &ob, &ib, ZSTD_e_flush);
			if (ZSTD_isError(zr) || ib.pos != ib.size || zr != 0) {
				// Should not happen (bound-sized output). Legacy ZCST already went
				// out above, so viewers are unaffected; skip this ZCS2 msg and
				// force a stream restart so ZCS2 listeners resync cleanly.
				printf("[ZSTREAM] compress error (%s) — ZCS2 msg skipped, stream restart queued\n",
					ZSTD_isError(zr) ? ZSTD_getErrorName(zr) : "incomplete flush");
				_zstreamResetPending.store(true, std::memory_order_release);
			} else {
				maplecast_ws::broadcastBinary(_zBuf.data(), (uint32_t)(10 + ob.pos));
				uint64_t zUs = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - t0z).count();
				static uint64_t _zBytes = 0, _zFrames = 0, _zUsTot = 0;
				_zBytes += 10 + ob.pos; _zFrames++; _zUsTot += zUs;
				if (frameNum % 600 == 0 && _zFrames > 0) {
					printf("[ZSTREAM] epoch=%u lvl=%d avg=%llu B/frame (%.3f Mbps @60) avg=%.3f ms/frame (shadow alongside ZCST)\n",
						_zEpoch, _zLevel, (unsigned long long)(_zBytes/_zFrames),
						(_zBytes/(double)_zFrames)*60.0*8.0/1e6, (_zUsTot/(double)_zFrames)/1000.0);
					fflush(stdout);
				}
			}
		}

		// Broadcast game state every frame (~60Hz). "GSTA" magic +
		// 253-byte serialized MVC2 state. Native client parses this for
		// the hitbox/frame-data overlay; the ROM-asset/sprite client needs
		// per-frame state so fast motion (dash/jump) tracks smoothly.
		// Bandwidth ~5 KB/s (265 B * 60Hz) — still negligible.
		static uint32_t _gsCounter = 0;
		if (++_gsCounter >= 1) {
			_gsCounter = 0;
			maplecast_gamestate::GameState gs;
			maplecast_gamestate::readGameState(gs);
			uint8_t gsBuf[4 + maplecast_gamestate::WIRE_SIZE];
			gsBuf[0] = 'G'; gsBuf[1] = 'S'; gsBuf[2] = 'T'; gsBuf[3] = 'A';
			maplecast_gamestate::serialize(gs, gsBuf + 4, maplecast_gamestate::WIRE_SIZE);
			maplecast_ws::broadcastBinary(gsBuf, 4 + maplecast_gamestate::WIRE_SIZE);
			// PALF — per-slot palette-effect (hit-flash) flag (char+0x40). The browser
			// tints the flashing body toward white (electric -> blue-white). Own 16B
			// packet; other parsers ignore it (no GSTA wire change).
			uint8_t palfBuf[16];
			int palfN = maplecast_gamestate::serializePalEffects(palfBuf, sizeof(palfBuf));
			if (palfN > 0) maplecast_ws::broadcastBinary(palfBuf, palfN);
			// WTCH live bit-probe (debug, MAPLECAST_WATCH).
			static uint8_t wbuf[8 + 6 * (2 + 512)];
			int wN = maplecast_gamestate::serializeWatch(wbuf, sizeof(wbuf));
			if (wN > 0) maplecast_ws::broadcastBinary(wbuf, wN);
		}

			// === OBJS: pool satellite objects (cape/effects/projectiles) -> rip sprites ===
			// Ship owner, sprite_id (with the 0x8000 hflip bit), and the object's nominal
			// position (+0xC8/+0xCC). The CLIENT decides where to draw: attached parts (near
			// the owner) snap to the owner's LIVE screen pos + the true anchor (no lag; an
			// off-screen assist's parts vanish with it); spawned objects (far) use +0xC8/+0xCC.
			{
				// Cap = 255, the 1-byte OBJS count field's hard maximum, which is
				// well above MVC2's finite drawable-object ceiling (the pool is a
				// fixed-stride region; a heavy 3v3-super frame is ~120 owner'd
				// objects). So the array is never the limiter — readObjects scans
				// the WHOLE pool and LOGS "[OBJS] CAP HIT ... DROPPED K" if it ever
				// can't fit everything (it shouldn't). If that log fires, widen the
				// count field to 2 bytes (touches the OBJS parsers). ~2KB/frame
				// worst case, negligible after the TA shed.
				maplecast_gamestate::ObjectState objs[255];
				int no = maplecast_gamestate::readObjects(objs, 255);
				if (no > 0) {
					// 14-byte stride: trailing 6 bytes are flags(1) + hot_dx(s8) + hot_dy(s8)
					//   + effect_key(u16 LE) + blend(u8). (Was 13B; blend appended 2026-06-11
					//   with the GSTA/OBJF wire extension so the browser OBJS path gets it too;
					//   13B added effect_key earlier the same day; 11B was the pre-effect_key form.)
					//   flags bit0 = is_effect (node+0x15c points into Effect Poly 0x0CED0000 ->
					//          client routes to the effects atlas, not PL{cid}). bits1-7 reserved.
					//   hot_dx/hot_dy (PATH A) = the object's TRUE assembly hotspot (min dx,dy over
					//          node+0x178 extras); the client anchors satellites here instead of the
					//          baked body-relative sp.dx (0,0 => no extras, client keeps baked anchor).
					//   effect_key = low 16 bits of the node's GFX base (node+0x15c) — stable
					//          per-effect content key (parsed-but-unused on the client for now).
					//   blend = per-object PVR blend/list-type (0=PT/opaque 1=alpha 2=additive),
					//          RAM-derived (computeObjectBlend): effects render additively. Lets the
					//          lean client transparency-match sparks/supers/auras (G2 wire gap).
					// The client auto-detects 14B vs the older 13B/11B/9B/legacy 8B from the packet
					// length, so an old client harmlessly ignores the trailing bytes.
					uint8_t obuf[4 + 1 + 255 * 14];   // per obj: cid(1)+sid(2)+type(1)+x(2)+y(2)+flags(1)+hotdx(1)+hotdy(1)+effkey(2)+blend(1)
					obuf[0]='O'; obuf[1]='B'; obuf[2]='J'; obuf[3]='S'; obuf[4]=(uint8_t)no;
					int oo = 5;
					for (int i = 0; i < no; i++) {
						uint16_t sid = (objs[i].sprite_id & 0x7fff) | (objs[i].xflip ? 0x8000 : 0);  // per-object flip (node+0x130) in the high bit
						obuf[oo++]=objs[i].owner_cid;
						obuf[oo++]=sid&0xff; obuf[oo++]=(sid>>8)&0xff;
						obuf[oo++]=objs[i].type;
						obuf[oo++]=objs[i].screen_x&0xff; obuf[oo++]=(objs[i].screen_x>>8)&0xff;
						obuf[oo++]=objs[i].screen_y&0xff; obuf[oo++]=(objs[i].screen_y>>8)&0xff;
						obuf[oo++]=(uint8_t)(objs[i].is_effect ? 0x01 : 0x00);   // OBJS flags
						obuf[oo++]=(uint8_t)objs[i].hot_dx;                       // PATH A true anchor dx
						obuf[oo++]=(uint8_t)objs[i].hot_dy;                       // PATH A true anchor dy
						obuf[oo++]=(uint8_t)(objs[i].effect_key & 0xff);          // GSTA wire ext: effect_key u16 LE
						obuf[oo++]=(uint8_t)((objs[i].effect_key >> 8) & 0xff);
						obuf[oo++]=objs[i].blend;                                 // GSTA wire ext: blend/list-type u8
					}
					maplecast_ws::broadcastBinary(obuf, oo);

					// OBJF — FULL object record for the state-replica inject
					// (writeObjects). Carries category/xflip/owner_slot the 8B
					// OBJS packet omits. Browser ignores OBJF; replica consumes it.
					uint8_t fbuf[4 + 1 + 255 * maplecast_gamestate::OBJF_REC_SIZE];
					fbuf[0]='O'; fbuf[1]='B'; fbuf[2]='J'; fbuf[3]='F';
					int fn = maplecast_gamestate::serializeObjects(
					    objs, no, fbuf + 4, (int)sizeof(fbuf) - 4);
					if (fn > 0) maplecast_ws::broadcastBinary(fbuf, 4 + fn);
				}
			}

			// === TAEFF: TA-analysis. For each effect quad, log it RELATIVE to the nearest
			// character + that char's current sprite_id (pose). Lets us DERIVE the rules:
			// cape offset per pose, projectile patterns, etc. Gated MAPLECAST_TAEFF -> file.
			{
				static bool _taeff = getenv("MAPLECAST_TAEFF") != nullptr;
				static uint32_t _ec = 0; ++_ec;
				if (_taeff) {
					static FILE* ef = nullptr; static long ew = 0;
					if (!ef) ef = fopen("/dev/shm/mc_taeff.log", "w");
					if (ef && ew < 16L*1024*1024) {
						ta_parse(ctx, true);
						auto& rc = ctx->rend;
						maplecast_gamestate::GameState egs; maplecast_gamestate::readGameState(egs);
						auto logList = [&](std::vector<PolyParam>& lst, const char* tag) {
							for (PolyParam& pp : lst) {
								if (pp.count < 3 || !((pp.pcw.full >> 3) & 1)) continue;
								uint32_t tsp = pp.tsp.full; int bs=(tsp>>29)&7, bd=(tsp>>26)&7;
								float mnX=1e9f,mxX=-1e9f,mnY=1e9f,mxY=-1e9f;
								uint32_t end=pp.first+pp.count; if(end>rc.verts.size()) end=(uint32_t)rc.verts.size();
								for(uint32_t v=pp.first;v<end;v++){float x=rc.verts[v].x,y=rc.verts[v].y;if(x<mnX)mnX=x;if(x>mxX)mxX=x;if(y<mnY)mnY=y;if(y>mxY)mxY=y;}
								float cx=(mnX+mxX)*0.5f,cy=(mnY+mxY)*0.5f,w=mxX-mnX,h=mxY-mnY;
								if(cy<=20.f||w<2.f||h<2.f) continue;
								int best=-1; float bd2=1e9f;
								for(int i=0;i<6;i++){ if(!egs.chars[i].active) continue; float dx=cx-egs.chars[i].screen_x,dy=cy-egs.chars[i].screen_y,d=dx*dx+dy*dy; if(d<bd2){bd2=d;best=i;} }
								if(best<0||bd2>40000.f) continue;
								auto&c=egs.chars[best];
								char b[220]; int n=snprintf(b,sizeof b,"f%u cid=%d csid=%d cscr=(%.0f,%.0f) %s off=(%.0f,%.0f) sz=%.0fx%.0f blend=%d/%d\n",
									_ec, c.character_id, c.sprite_id, c.screen_x, c.screen_y, tag, cx-c.screen_x, cy-c.screen_y, w, h, bs, bd);
								fwrite(b,1,n,ef); ew+=n;
							}
						};
						logList(rc.global_param_tr,"tr");
						logList(rc.global_param_pt,"pt");
						fflush(ef);
					}
				}
			}

			// === TADBG: renderer-oracle. EVERY FRAME (short moves last a few frames), for each
			// pool object find the nearest drawn quad -> its z-ORDER (draw index), BLEND, and
			// whether it is drawn at all (NO-MATCH = dead/afterimage to filter). Writes
			// /dev/shm/mc_tadbg.log (16MB cap). Gated MAPLECAST_TADBG.
			{
				static bool _tadbg = getenv("MAPLECAST_TADBG") != nullptr;
				static uint32_t _tc = 0; ++_tc;
				if (_tadbg) {
					static FILE* tf = nullptr; static long tw = 0;
					if (!tf) tf = fopen("/dev/shm/mc_tadbg.log", "w");
					if (tf && tw < 16L*1024*1024) {
						ta_parse(ctx, true);
						auto& rc = ctx->rend;
						struct OP { int idx; float cx, cy; int bs, bd; };
						static OP polys[1024]; int np = 0, li = 0;
						auto collect = [&](std::vector<PolyParam>& lst) {
							for (PolyParam& pp : lst) {
								int idx = li++;
								if (np >= 1024 || pp.count < 3 || !((pp.pcw.full >> 3) & 1)) continue;
								uint32_t tsp = pp.tsp.full;
								float mnX=1e9f,mxX=-1e9f,mnY=1e9f,mxY=-1e9f;
								uint32_t end = pp.first + pp.count; if (end > rc.verts.size()) end = (uint32_t)rc.verts.size();
								for (uint32_t v = pp.first; v < end; v++) { float x=rc.verts[v].x,y=rc.verts[v].y; if(x<mnX)mnX=x;if(x>mxX)mxX=x;if(y<mnY)mnY=y;if(y>mxY)mxY=y; }
								float cy=(mnY+mxY)*0.5f; if (cy<=20.f) continue;
								polys[np++] = { idx, (mnX+mxX)*0.5f, cy, (int)((tsp>>29)&7), (int)((tsp>>26)&7) };
							}
						};
						collect(rc.global_param_tr);
						collect(rc.global_param_pt);
						maplecast_gamestate::ObjectState dobjs[48];
						int dno = maplecast_gamestate::readObjects(dobjs, 48);
						char b[200];
						for (int i = 0; i < dno; i++) {
							int best=-1; float bdist=1e9f;
							for (int j=0;j<np;j++){ float dx=polys[j].cx-dobjs[i].screen_x,dy=polys[j].cy-dobjs[i].screen_y,d=dx*dx+dy*dy; if(d<bdist){bdist=d;best=j;} }
							int n;
							if (best>=0 && bdist<1600.f)
								n = snprintf(b, sizeof b, "f%u cid=%d sid=%d @(%d,%d) z=%d blend=%d/%d d2=%.0f\n", _tc, dobjs[i].owner_cid, dobjs[i].sprite_id, dobjs[i].screen_x, dobjs[i].screen_y, polys[best].idx, polys[best].bs, polys[best].bd, bdist);
							else
								n = snprintf(b, sizeof b, "f%u cid=%d sid=%d @(%d,%d) NO-MATCH\n", _tc, dobjs[i].owner_cid, dobjs[i].sprite_id, dobjs[i].screen_x, dobjs[i].screen_y);
							fwrite(b, 1, n, tf); tw += n;
						}
						fflush(tf);
					}
				}
			}

			// === FRAME ORACLE — per-frame RAW capture of {drawn objects + their source
			// pointers} and {every classified sprite quad}. PIVOT 2026-06-08: the prior
			// server-side nearest-anchor attribution matched ~0 character quads (and emitted
			// only the matched quads, so we debugged blind). The server now DUMPS RAW — two
			// flat, unattributed lists — and attribution + analysis run OFFLINE in
			// _oracle/oracle_attribute.py for fast iteration with no rebuild/redeploy.
			// Runs on the live dynarec after ta_parse (NO SH4 PC-hook). Read-only
			// instrument; no wire-format / client change. Writes JSON lines to
			// /dev/shm/mc_oracle.jsonl, in_match only, frame-capped. Gated
			// MAPLECAST_FRAME_ORACLE. (Object walk mirrors readAllDrawn (gamestate.cpp:370)
			// but is self-contained here to expose node_addr + the full asm pointer cluster
			// (pal/file/fac/scale/flip/hotspot) that ObjectState doesn't carry.)
			{
				static bool _oracle = getenv("MAPLECAST_FRAME_ORACLE") != nullptr;
				// Capacity guard: stop appending once the file passes the size cap so a long
				// session can't fill /dev/shm. ~64 MB holds a few thousand frames of objects.
				static const long ORACLE_CAP = 64L * 1024 * 1024;
				static FILE* of = nullptr; static long ow = 0; static bool oFull = false;
				static uint32_t ofc = 0; ++ofc;
				if (_oracle && !oFull && addrspace::read8(0x8C289624)) {  // in_match @0x8C289624
					if (!of) { of = fopen("/dev/shm/mc_oracle.jsonl", "a"); }
					if (of && ow < ORACLE_CAP) {
						ta_parse(ctx, true);
						auto& rc = ctx->rend;

						// ---- 1) Parse TA polys: screen bbox + UV sub-rect + tex source, via the
						// EXACT index-buffer de-index loop used by the EFCT scan (rc.idx, not raw verts).
						struct OQuad {
							float cx, cy, x, y, w, h;
							float uMn, uMx, vMn, vMx;
							float zMn, zMx;  // depth (Vertex.z = 1/w) range over the quad; near=char, far=stage
							uint32_t tcw, tsp, pcw, vramAddr, texId;
							int fmt, srcBlend, dstBlend, tw, th, vq;
							bool isSprite;   // survives the non-sprite filter (see classify below)
						};
						static OQuad qs[2048]; int nq = 0;
						uint32_t nverts = (uint32_t)rc.verts.size();
						auto collect = [&](std::vector<PolyParam>& lst) {
							for (PolyParam& pp : lst) {
								if (nq >= 2048) return;
								if (pp.count < 3) continue;
								uint32_t pcw = pp.pcw.full, tcw = pp.tcw.full, tsp = pp.tsp.full;
								bool textured = ((pcw >> 3) & 1) != 0;
								float mnX=1e9f,mxX=-1e9f,mnY=1e9f,mxY=-1e9f;
								float uMn=1e9f,uMx=-1e9f,vMn=1e9f,vMx=-1e9f;
								float zMn=1e30f,zMx=-1e30f;   // Vertex.z = 1/w depth (WebGPU floor-cutoff value)
								// ROOT-CAUSE FIX: pp.first/.count are NOT always rc.idx offsets.
								// makePrimRestartIndex/makeIndex (op/pt and non-autosort tr) rewrite
								// them to index rc.idx; but sortTriangles (autosort translucent — the
								// MVC2 character/projectile sprites) leaves them indexing rc.verts
								// DIRECTLY (ta_util.cpp:88). The old rc.idx-only de-index therefore
								// read garbage for every autosort-tr sprite -> empty quads. Detect
								// the convention per poly: try rc.idx de-index first; if it yields no
								// valid verts (degenerate), fall back to the direct rc.verts read
								// (the proven TADBG/TAEFF pattern, ta_vtx.cpp ~2382).
								int seen = 0;
								{
									uint32_t iend = pp.first + pp.count;
									if (iend > rc.idx.size()) iend = (uint32_t)rc.idx.size();
									for (uint32_t k = pp.first; k < iend; k++) {
										uint32_t vi = rc.idx[k]; if (vi >= nverts) continue;
										const auto& vt = rc.verts[vi];
										if (vt.x<mnX)mnX=vt.x; if (vt.x>mxX)mxX=vt.x;
										if (vt.y<mnY)mnY=vt.y; if (vt.y>mxY)mxY=vt.y;
										if (vt.u<uMn)uMn=vt.u; if (vt.u>uMx)uMx=vt.u;
										if (vt.v<vMn)vMn=vt.v; if (vt.v>vMx)vMx=vt.v;
										if (vt.z<zMn)zMn=vt.z; if (vt.z>zMx)zMx=vt.z;
										seen++;
									}
								}
								if (seen == 0) {
									// vert-addressed (autosort tr / sortTriangles): pp.first/.count index rc.verts
									uint32_t vend = pp.first + pp.count;
									if (vend > nverts) vend = nverts;
									for (uint32_t v = pp.first; v < vend; v++) {
										const auto& vt = rc.verts[v];
										// skip flycast's inf/NaN strip-restart sentinels (ta_util is_vertex_inf, inlined)
										if (std::isnan(vt.x) || fabsf(vt.x) > 1e25f || std::isnan(vt.y) || fabsf(vt.y) > 1e25f) continue;
										if (vt.x<mnX)mnX=vt.x; if (vt.x>mxX)mxX=vt.x;
										if (vt.y<mnY)mnY=vt.y; if (vt.y>mxY)mxY=vt.y;
										if (vt.u<uMn)uMn=vt.u; if (vt.u>uMx)uMx=vt.u;
										if (vt.v<vMn)vMn=vt.v; if (vt.v>vMx)vMx=vt.v;
										if (vt.z<zMn)zMn=vt.z; if (vt.z>zMx)zMx=vt.z;
										seen++;
									}
								}
								if (seen == 0) continue;
								float w = mxX-mnX, h = mxY-mnY;
								if (w < 2.f || h < 2.f) continue;
								float cy = (mnY+mxY)*0.5f; if (cy <= 20.f) continue;  // strip top HUD row
								OQuad& q = qs[nq];
								q.cx = (mnX+mxX)*0.5f; q.cy = cy; q.x = mnX; q.y = mnY; q.w = w; q.h = h;
								q.uMn=uMn; q.uMx=uMx; q.vMn=vMn; q.vMx=vMx;
								q.zMn=(zMn> 1e29f)?0.f:zMn; q.zMx=(zMx<-1e29f)?0.f:zMx;
								q.tcw=tcw; q.tsp=tsp; q.pcw=pcw;
								q.srcBlend = (int)((tsp>>29)&7); q.dstBlend = (int)((tsp>>26)&7);
								if (textured) {
									q.fmt = (int)((tcw>>27)&7);
									q.vq  = (int)((tcw>>30)&1);
									q.tw  = 8 << ((tsp>>3)&7); q.th = 8 << (tsp&7);
									q.vramAddr = (tcw & 0x1FFFFF) << 3;
									q.texId = mcfx::texHash(q.vramAddr, q.fmt, q.tw, q.th, q.vq);
								} else {
									q.fmt=-1; q.vq=0; q.tw=0; q.th=0; q.vramAddr=0; q.texId=0;
								}
								// ---- SPRITE CLASSIFIER (thresholds derived from the prod capture
								// _oracle/mc_oracle.jsonl, 13420 frames; see commit msg / analysis).
								// A character/effect part-quad is: TEXTURED, NOT page-tiled, MODEST
								// size, and translucent/additive (autosort-tr) — never an opaque
								// screen-clear or a recurring opaque stage backdrop. Filtering these
								// out BEFORE attribution is what lets a quad reach the character it
								// covers instead of the 1152x480 clear / 613x411 backdrop that merely
								// contains the same screen point.
								//   (1) untextured       -> screen clear / flat fill (texId 0)
								//   (2) page-tiled       -> scrolling bg (u or v outside ~[0,1])
								//   (3) oversized >200px  -> stage backdrop / parallax layer. Capture:
								//       real parts cluster med 30-64px, p90 H=70; the big recurring
								//       translucent stage layers sit at 137-209px and the opaque
								//       backdrop at 613px. 200 keeps parts (incl. larger super body
								//       parts) while dropping every recurring stage layer.
								//   (4) opaque [src=1,dst=0] -> stage/HUD fill. Char sprites are
								//       autosort-translucent [4,5] or additive effects [4,1]. In the
								//       capture, dropping [1,0] removes the backdrop fragments with
								//       zero loss of [4,5]/[4,1] parts.
								// After (1)-(4) the survivors' centers sit a median 50px (p90 86px,
								// all <120px) from their object's screen_xy -> clean proximity match.
								bool tiled = (uMn < -0.05f || uMx > 1.05f || vMn < -0.05f || vMx > 1.05f);
								bool opaque = (q.srcBlend == 1 && q.dstBlend == 0);
								bool oversized = (w > 200.f || h > 200.f);
								q.isSprite = textured && !tiled && !opaque && !oversized
								             && q.texId != 0 && tcw != 0;
								nq++;
							}
						};
						collect(rc.global_param_op);
						collect(rc.global_param_pt);
						collect(rc.global_param_tr);

						// ---- DIAGNOSTIC (hint #1/#3): does the TA poly list exist here, and
						// how many quads survive the de-index? Logged to stderr every 60 frames
						// so a stale capture self-explains. If op/pt/tr or verts are 0 the parse
						// isn't populating at this call site; nq>0 with sprite==0 means the
						// classifier filtered everything (RTT/native-space coords, all oversized).
						static int _oDiag = 0;
						bool _oDiagNow = (++_oDiag % 60) == 0;
						if (_oDiagNow)
							fprintf(stderr,
								"[ORACLE] f%u rtt=%d op=%zu pt=%zu tr=%zu verts=%zu idx=%zu -> nq=%d\n",
								ofc, (int)rc.isRTT,
								rc.global_param_op.size(), rc.global_param_pt.size(),
								rc.global_param_tr.size(), rc.verts.size(), rc.idx.size(), nq);

						// ---- 2) Enumerate drawn objects from MVC2's OWN slot table (mirrors
						// readAllDrawn) but keep node_addr + the asm pointer cluster.
						static const uint32_t SLOT_COUNT_BASE = 0x8C2895E0;
						static const uint32_t SLOT_PTR_BASE   = 0x8C287DE0;
						static const uint32_t SLOT_ROW_STRIDE = 0x180;
						static const uint32_t CB[6] = { 0x8C268340,0x8C2688E4,0x8C268E88,
						                                0x8C26942C,0x8C2699D0,0x8C269F74 };
						auto rdF = [](uint32_t a){ uint32_t r=addrspace::read32(a); float f; memcpy(&f,&r,4); return f; };
						auto inRam = [](uint32_t a){ return a>=0x0C000000 && a<0x10000000; };

						struct OObj {
							uint32_t node; int slot; int owner_cid; int category;
							int sprite_id; float sx, sy; float scaleX, scaleY; int flip;
							uint32_t gfx1, pal, extras, file, fac;
							const char* region;
							int hotDx, hotDy;                  // legacy +0x178 walk (body-constant)
							int hotCellDx, hotCellDy, hotCellSlot;  // cell-slot walk (slot=read16(cell+0x12))
						};
						static OObj objs[128]; int no = 0;

						for (int layer = 0; layer < 16 && no < 128; layer++) {
							int count = (int)addrspace::read8(SLOT_COUNT_BASE + layer);
							if (count <= 0 || count > 0x60) continue;
							uint32_t row = SLOT_PTR_BASE + (uint32_t)layer * SLOT_ROW_STRIDE;
							for (int i = 0; i < count && no < 128; i++) {
								uint32_t node = addrspace::read32(row + i*4);
								if (node < 0x8C000000 || node >= 0x8D000000) continue;
								bool isBody = false;
								for (int s=0;s<6;s++) if (node==CB[s]) { isBody=true; break; }
								if (isBody) continue;
								if (addrspace::read8(node + 0x12C) == 0) continue;       // visibility gate
								int sid = (int)(uint16_t)addrspace::read16(node + 0x144);
								if (sid == 0) continue;
								float sx = rdF(node + 0xE0), sy = rdF(node + 0xE4);
								if (sx<-64.f||sx>704.f||sy<-64.f||sy>544.f) continue;
								int slot = -1;
								uint32_t oA = addrspace::read32(node+0x18), oB = addrspace::read32(node+0x80);
								for (int s=0;s<6;s++) if (oA==CB[s]||oB==CB[s]) { slot=s; break; }
								OObj& o = objs[no];
								o.node = node; o.slot = slot;
								o.owner_cid = slot>=0 ? (int)(uint8_t)addrspace::read8(CB[slot]+0x001) : 0;
								o.category  = (int)(uint8_t)addrspace::read8(node + 0x03);
								o.sprite_id = sid; o.sx = sx; o.sy = sy;
								o.scaleX = rdF(node + 0x50); o.scaleY = rdF(node + 0x54);
								o.flip   = addrspace::read16(node + 0x130) ? 1 : 0;
								o.gfx1   = addrspace::read32(node + 0x15C);
								o.pal    = addrspace::read32(node + 0x164);
								o.extras = addrspace::read32(node + 0x178);
								o.file   = addrspace::read32(node + 0x17C);
								o.fac    = addrspace::read32(node + 0x184);
								uint32_t gl = o.gfx1 & 0x0FFFFFFF;
								o.region = (gl>=0x0CED0000 && gl<0x0CEE0000) ? "EFFECTS_BANK"
								         : (gl>=0x0CE60000 && gl<0x0CE70000) ? "DECOMP_BUF" : "CHAR_GFX";
								// ---- HOTSPOT — RAW, BOTH READINGS (the "always 0" investigation).
								// WHY the old field was 0: it walked node+0x178 (Sprite_Extras) at a FIXED
								// +0x18. node+0x178 is the EXTRAS *table base*, not this object's assembly,
								// so +0x18 lands in the table header -> sentinel on record 0 -> 0,0 for the
								// whole 13420-frame capture.
								// WHY a single "true" read is NOT available here (do-not-repeat, per
								// docs/HANDOFF-2026-06-08 #2/#3):
								//   - The cell-slot walk (cell=read32(node+0x154); slot=read16(cell+0x12);
								//     recs=EXTRAS+slot*0x400+8) was tried: keyframe+0x12 turned out to be
								//     HitboxGroup, NOT the assembly slot, so the slot index is wrong for
								//     satellites; AND the offline sid->assembly map was never solved.
								//   - The direct +0x178 walk (readHotspot) is DEGENERATE: it returns a
								//     per-CHARACTER constant = the body's hotspot (Storm every object
								//     (-76,8)), so it can't separate a projectile from its body.
								// DECISION: this is raw instrumentation -> emit BOTH candidate readings
								// unmodified (hotspot_dx/dy = the legacy +0x178 walk, for continuity with
								// readHotspot; hot_cell_dx/dy = the cell-slot walk) and let the OFFLINE tool
								// judge whether either anchors satellites correctly. No server-side "fix" is
								// claimed; the honest answer is the per-object hotspot is not cleanly
								// readable at this hook (both candidates are known-degenerate).
								auto cl=[](int v){ if(v<-32768)v=-32768; else if(v>32767)v=32767; return v; };
								// (a) legacy direct +0x178 walk (8B records, mode@+6 0xFF end)
								o.hotDx = 0; o.hotDy = 0;
								if (inRam(o.extras)) {
									int mnDx=0x7fffffff, mnDy=0x7fffffff; bool any=false;
									uint32_t rec = o.extras + 0x18;
									for (int g=0; g<64; ++g, rec+=8) {
										uint8_t mode = (uint8_t)addrspace::read8(rec+6);
										if (mode==0xFF) break;
										int dx=(int16_t)addrspace::read16(rec+0), dy=(int16_t)addrspace::read16(rec+2);
										if (dx<mnDx)mnDx=dx; if (dy<mnDy)mnDy=dy; any=true;
									}
									if (any) { o.hotDx=cl(mnDx); o.hotDy=cl(mnDy); }
								}
								// (b) cell-slot walk (EXTRAS+slot*0x400+8; slot=read16(cell+0x12))
								o.hotCellDx = 0; o.hotCellDy = 0; o.hotCellSlot = -1;
								{
									uint32_t cell = addrspace::read32(node + 0x154);
									if (inRam(cell) && inRam(o.extras)) {
										int slot = (uint16_t)addrspace::read16(cell + 0x12);
										o.hotCellSlot = slot;
										if (slot < 64) {
											uint32_t recs = o.extras + (uint32_t)slot*0x400 + 0x08;
											int mnDx=0x7fffffff, mnDy=0x7fffffff; bool any=false;
											for (int r=0; r<128; ++r) {
												uint32_t rec = recs + (uint32_t)r*8;
												uint16_t sel = (uint16_t)addrspace::read16(rec + 6);
												if (sel == 0x00FF) break;
												int16_t rdx=(int16_t)addrspace::read16(rec+0), rdy=(int16_t)addrspace::read16(rec+2);
												uint16_t pw = (uint16_t)addrspace::read16(rec+4);
												if (rdx==0 && rdy==0 && sel==0 && pw==0) continue;
												if (rdx<mnDx)mnDx=rdx; if (rdy<mnDy)mnDy=rdy; any=true;
											}
											if (any) { o.hotCellDx=cl(mnDx); o.hotCellDy=cl(mnDy); }
										}
									}
								}
								no++;
							}
						}

						// ---- 3) RAW DUMP (PIVOT 2026-06-08). The server-side nearest-anchor
						// attribution matched ~0 character quads and was debugging-blind, so it
						// is REMOVED. The server now emits two FLAT, UNATTRIBUTED lists and all
						// attribution + analysis moves OFFLINE (_oracle/oracle_attribute.py),
						// where it can iterate on radius/anchor without a rebuild+redeploy:
						//   objects[]      = object metadata + source pointers (NO per-object quads)
						//   sprite_quads[] = every quad that passed isSprite, raw positions, no owner
						// Header keeps frame/in_match/nq/sprite/filtered/rtt. We still COUNT
						// sprite/filtered here (cheap, mirrors the classifier) for at-a-glance health.
						int nSprite = 0, nFiltered = 0;
						for (int j=0;j<nq;j++) { if (qs[j].isSprite) nSprite++; else nFiltered++; }
						if (_oDiagNow)
							fprintf(stderr, "[ORACLE]   no=%d nq=%d sprite=%d filtered=%d (raw-dump, attribution offline)\n",
							        no, nq, nSprite, nFiltered);

						// ---- 4) Emit one JSON line for this frame: header + objects[] + sprite_quads[].
						std::string line; line.reserve(8192);
						char nb[640];   // object line with both hotspot readings ~410B; headroom vs truncation
						snprintf(nb,sizeof nb,"{\"frame\":%u,\"in_match\":1,\"nq\":%d,\"sprite\":%d,\"filtered\":%d,\"rtt\":%d,\"objects\":[",
						         ofc, nq, nSprite, nFiltered, (int)rc.isRTT);
						line += nb;
						bool firstObj=true;
						for (int k=0;k<no;k++) {
							OObj& o = objs[k];
							if (!firstObj) line += ","; firstObj=false;
							snprintf(nb,sizeof nb,
								"{\"slot\":%d,\"owner_cid\":%d,\"category\":%d,\"sprite_id\":%d,"
								"\"screen_xy\":[%d,%d],\"scale\":[%.3f,%.3f],\"flip\":%d,\"node_addr\":\"0x%08X\","
								"\"tex_src\":{\"vram_addr\":\"0x00000000\",\"region\":\"%s\",\"gfx1_ptr\":\"0x%08X\","
								"\"pal_ptr\":\"0x%08X\",\"part_idx\":-1},"
								"\"asm_src\":{\"extras_ptr\":\"0x%08X\",\"file_ptr\":\"0x%08X\",\"fac_ptr\":\"0x%08X\","
								"\"hotspot_dx\":%d,\"hotspot_dy\":%d,"
								"\"hot_cell_dx\":%d,\"hot_cell_dy\":%d,\"hot_cell_slot\":%d}}",
								o.slot, o.owner_cid, o.category, o.sprite_id,
								(int)o.sx, (int)o.sy, o.scaleX, o.scaleY, o.flip, o.node,
								o.region, o.gfx1, o.pal,
								o.extras, o.file, o.fac, o.hotDx, o.hotDy,
								o.hotCellDx, o.hotCellDy, o.hotCellSlot);
							line += nb;
						}
						line += "],\"sprite_quads\":[";
						bool firstQ=true;
						for (int j=0;j<nq;j++) {
							OQuad& q = qs[j];
							if (!q.isSprite) continue;
							if (!firstQ) line += ","; firstQ=false;
							snprintf(nb,sizeof nb,
								"{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"u\":[%.4f,%.4f],\"v\":[%.4f,%.4f],"
								"\"z\":[%.6g,%.6g],"
								"\"texId\":\"%08X\",\"tcw\":\"0x%08X\",\"blend\":[%d,%d]}",
								(int)q.x,(int)q.y,(int)q.w,(int)q.h, q.uMn,q.uMx,q.vMn,q.vMx,
								q.zMn, q.zMx,
								q.texId, q.tcw, q.srcBlend, q.dstBlend);
							line += nb;
						}
						line += "]}\n";
						fwrite(line.data(), 1, line.size(), of);
						ow += (long)line.size();
						fflush(of);
						if (ow >= ORACLE_CAP) oFull = true;
					}
				}
			}

		// === SERVER-SIDE EFFECT ISOLATION -> content-addressed "EFCT" + "TXTR" ===
		// Per additive (DstInstr==ONE) effect quad: hash the texture's VRAM bytes
		// (content id, stable across the transient VRAM address) and emit
		// (hash,cx,cy,w,h) in screen space. When a hash is NEW, decode the 16-bit
		// texture to RGBA and ship it ONCE as a zstd "TXTR" packet; the state-only
		// client caches by hash and draws the EXACT game texture. ta_parse fills
		// rc.tr/pt here (Process re-parses later; raw mirror wire untouched).
		{
			ta_parse(ctx, true);
			const int FX_MAX = 24;
			uint8_t fxBuf[4 + 1 + FX_MAX * 20];
			fxBuf[0] = 'E'; fxBuf[1] = 'F'; fxBuf[2] = 'C'; fxBuf[3] = 'T';
			int off = 5; int nfx = 0; int hudOff = 5; int nhud = 0; const int HUD_MAX = 80;
			static uint8_t hudBuf[4 + 1 + 80 * 20]; hudBuf[0]='H'; hudBuf[1]='U'; hudBuf[2]='D'; hudBuf[3]='Q';
			static std::unordered_set<uint32_t> _sentHashes;
			static uint8_t* _rgbaBuf = (uint8_t*)malloc(1024 * 1024 * 4);
			// The relay hides browser joins, so we can't detect a new client. Re-ship
			// active textures ~every 3s: clearing the sent-set means only on-screen
			// textures re-ship (cached ones stay quiet between).
			static uint32_t _txClear = 0;
			if ((++_txClear % 180) == 0) _sentHashes.clear();
			auto scanList = [&](std::vector<PolyParam>& lst, bool isHud) {
				for (PolyParam& pp : lst) {
					if ((isHud ? nhud : nfx) >= (isHud ? HUD_MAX : FX_MAX)) return;
					if (pp.count < 3) continue;
					uint32_t tcw = pp.tcw.full, tsp = pp.tsp.full, pcw = pp.pcw.full;
					if (!((pcw >> 3) & 1)) continue;            // untextured
					if (!isHud && ((tsp >> 26) & 7) != 1) continue;  // fx: additive only; HUD: any blend
					int fmt = (tcw >> 27) & 7;
					if (fmt > 2 || ((tcw >> 31) & 1)) continue; // 16-bit, non-mip (VQ now decoded)
					int tw = 8 << ((tsp >> 3) & 7), th = 8 << (tsp & 7);
					float mnX = 1e9f, mxX = -1e9f, mnY = 1e9f, mxY = -1e9f;
					float uMn = 1e9f, uMx = -1e9f, vMn = 1e9f, vMx = -1e9f;
					// REVIEWED FIX: after ta_parse(primRestart) pp.first/.count index rc.idx, NOT
					// rc.verts (ta_util.cpp:435) -> the old raw rc.verts[v] read random verts (the
					// scattered dots). Walk the index buffer (skip ~0 restart sentinels) for the
					// screen bbox AND the UV sub-rect (EFKYTEX is a shared sheet; each poly samples
					// a sub-rect, not the whole page).
					uint32_t iend = pp.first + pp.count;
					if (iend > rc.idx.size()) iend = (uint32_t)rc.idx.size();
					uint32_t nverts = (uint32_t)rc.verts.size();
					for (uint32_t k = pp.first; k < iend; k++) {
						uint32_t vi = rc.idx[k]; if (vi >= nverts) continue;
						const auto& vt = rc.verts[vi];
						if (vt.x < mnX) mnX = vt.x; if (vt.x > mxX) mxX = vt.x;
						if (vt.y < mnY) mnY = vt.y; if (vt.y > mxY) mxY = vt.y;
						if (vt.u < uMn) uMn = vt.u; if (vt.u > uMx) uMx = vt.u;
						if (vt.v < vMn) vMn = vt.v; if (vt.v > vMx) vMx = vt.v;
					}
					float w = mxX - mnX, h = mxY - mnY;
					float cx = (mnX + mxX) * 0.5f, cy = (mnY + mxY) * 0.5f;
					if (w < 2.f || h < 2.f) continue; if (isHud ? (cy > 56.f && cy < 424.f) : (cy <= 40.f)) continue;
					// MULTI-PART FILTER: a UV bbox spanning much of the sheet = a multi-strip effect
					// (super field) whose single-rect draw garbles. Skip for now; the STAF per-triangle
					// path renders these correctly. Single-frame effects (sparks/tornado) pass through.
					if ((uMx - uMn) > 0.5f || (vMx - vMn) > 0.5f) continue;
					auto q16 = [](float f){ if (f < 0) f = 0; if (f > 1) f = 1; return (uint16_t)(f * 65535.f + 0.5f); };
					uint16_t uv16[4] = { q16(uMn), q16(1.f - vMx), q16(uMx), q16(1.f - vMn) };  // v flipped to match RGBA V-flip
					uint32_t addr = (tcw & 0x1FFFFF) << 3;
					uint32_t hsh = mcfx::texHash(addr, fmt, tw, th, (tcw >> 30) & 1);
					if (_rgbaBuf && _sentHashes.find(hsh) == _sentHashes.end()) {
						if (mcfx::decodeTex16(tcw, tsp, _rgbaBuf)) {
							// V-FLIP: mcfx decodes V-inverted (the select-screen text was upside-down). Flip the
							// rows so the cached texture is right-side-up; the UV v is flipped to match (above).
							{ int _rb = tw * 4; static std::vector<uint8_t> _vf; _vf.resize(_rb);
							  for (int _y = 0; _y < th / 2; _y++) { uint8_t* _a = _rgbaBuf + (size_t)_y*_rb, *_b = _rgbaBuf + (size_t)(th-1-_y)*_rb;
							    memcpy(_vf.data(), _a, _rb); memcpy(_a, _b, _rb); memcpy(_b, _vf.data(), _rb); } }
							// DIAGNOSTIC: dump the mcfx-DECODED effect texture (the full sheet) so we can
							// VIEW it offline and tell decode-vs-placement apart (gated MAPLECAST_DUMP_FX_TEX=N).
							{ static int _fxd = getenv("MAPLECAST_DUMP_FX_TEX") ? atoi(getenv("MAPLECAST_DUMP_FX_TEX")) : 0;
							  static int _fxn = 0;
							  if (_fxd && _fxn < _fxd) {
							    char fn[112]; snprintf(fn, sizeof fn, "/dev/shm/fxtex_%03d_%dx%d_f%d_vq%d.rgba", _fxn, tw, th, fmt, (int)((tcw>>30)&1));
							    FILE* ff = fopen(fn, "wb"); if (ff) { fwrite(_rgbaBuf, 1, (size_t)tw*(size_t)th*4, ff); fclose(ff); }
							    fprintf(stderr, "[FXTEX] %s hash=%08x\n", fn, hsh); _fxn++; } }
							_sentHashes.insert(hsh);
							uint32_t rgbaSize = (uint32_t)(tw * th * 4);
							size_t compSize = 0; uint64_t cus = 0;
							const uint8_t* comp = _compressor.compress(_rgbaBuf, rgbaSize, compSize, cus);
							std::vector<uint8_t> tb(16 + compSize);
							tb[0] = 'T'; tb[1] = 'X'; tb[2] = 'T'; tb[3] = 'R';
							memcpy(&tb[4], &hsh, 4);
							tb[8] = tw & 0xff; tb[9] = (tw >> 8) & 0xff; tb[10] = th & 0xff; tb[11] = (th >> 8) & 0xff;
							memcpy(&tb[12], &rgbaSize, 4); memcpy(&tb[16], comp, compSize);
							maplecast_ws::broadcastBinary(tb.data(), (uint32_t)tb.size());
						}
					}
					int16_t v16[4] = { (int16_t)cx, (int16_t)cy, (int16_t)w, (int16_t)h };
					uint8_t* BUF = isHud ? hudBuf : fxBuf; int& O = isHud ? hudOff : off; memcpy(&BUF[O], &hsh, 4); O += 4;
					for (int k = 0; k < 4; k++) { BUF[O++] = v16[k] & 0xff; BUF[O++] = (v16[k] >> 8) & 0xff; }
					for (int k = 0; k < 4; k++) { BUF[O++] = uv16[k] & 0xff; BUF[O++] = (uv16[k] >> 8) & 0xff; }
					if (isHud) nhud++; else nfx++;
				}
			};
			// IN-MATCH FILTER: capture effects only during a live round (in_match @0x8C289624),
			// so pre-match SELECT/VERSUS menu textures are never grabbed as effects.
			if (addrspace::read8(0x8C289624)) { scanList(rc.global_param_tr, false); scanList(rc.global_param_pt, false); }
			// HUD: top/bottom strip textured quads (health/timer/hit-counter/meters), any blend.
			scanList(rc.global_param_op, true); scanList(rc.global_param_pt, true); scanList(rc.global_param_tr, true);
			fxBuf[4] = (uint8_t)nfx;
			static bool _efctOn = getenv("MAPLECAST_EFCT") != nullptr;
			if (_efctOn && nfx > 0) maplecast_ws::broadcastBinary(fxBuf, off);
			hudBuf[4] = (uint8_t)nhud; static bool _hudOn = getenv("MAPLECAST_HUDQ") != nullptr;
			if (_hudOn && nhud > 0) maplecast_ws::broadcastBinary(hudBuf, hudOff);
		}
			// === STAF: stripped-TA frame channel (MAPLECAST_STAF) ====================
			// Ships the FULL textured-quad list every frame + each unique texture ONCE
			// (content-addressed, ship-once), so the client renders the exact frame
			// from cached textures at low bandwidth. Every texture decoded through
			// flycast's OWN canonical decode keyed by the quad's real TCW/TSP
			// (mcfx::decodeTexAny / texHash64) -- NO per-texture format guessing.
			// Parallel to the mirror wire; A/B-selectable on the client. Wire format
			// in docs/STRIPPED-TA-DESIGN.md (SIMPLE variant: axis-aligned dest/UV rect
			// per quad -- Canvas2D-renderable; per-vertex transform-encode is a later opt).
			{
				static bool _stafOnEmit = getenv("MAPLECAST_STAF") != nullptr;
				// HYBRID: MAPLECAST_HUDF ships the SAME STAF stream but FILTERED to just
				// effects (additive) + HUD (top/bottom screen strips) — chars + stage are
				// rendered client-side (lean sprite + client stage), never streamed. ~20 KB/s.
				static bool _hudfOn = getenv("MAPLECAST_HUDF") != nullptr;
				bool _inMatch = addrspace::read8(0x8C289624) != 0;
				if ((_stafOnEmit || _hudfOn) && _inMatch) {  // HUDF only in-match: no out-of-match STAF flood (client routes STAF only when _skipTA, else it hits applyFrame -> RangeError + CPU burn = char-select 5fps)
					// primRestart=false -> makeIndex() builds rc.idx as a strip-with-
					// degenerate-links (NOT 0xFFFFFFFF restart sentinels), and rewrites
					// each PolyParam.first/.count to index into rc.idx (rc.idx[k] -> rc.verts).
					// This is flycast's OWN triangulation: it handles strip restarts,
					// alternating winding and inf/invalid verts that raw rc.verts iteration
					// (the previous bug) could not. Applies uniformly to op/pt/tr below.
					ta_parse(ctx, false);
					auto& rcS = ctx->rend;
					static std::unordered_set<uint64_t> _stafSent;     // tex_ids already shipped (TX64)
					static uint8_t* _stafRgba = (uint8_t*)malloc(1024 * 1024 * 4);
					static std::vector<uint8_t> _stafBuf;
					// REAL wire-bandwidth probe (MAPLECAST_STAFMEASURE): the modeled probe
					// below (ta_parse(ctx,true)) estimates cost; THIS counts the ACTUAL bytes
					// this STAF emit broadcasts — the zstd'd STAF envelope + every TX64 packet —
					// and flushes total KB/s to /dev/shm/mc_staf.log once per second (60 frames).
					static bool _stafMeasureReal = getenv("MAPLECAST_STAFMEASURE") != nullptr;
					static uint64_t _stafBytesAcc = 0, _tx64BytesAcc = 0;
					static uint32_t _stafMeasFrames = 0;
					// Relay hides browser joins; periodically clear so on-screen textures re-ship.
					static uint32_t _stafClear = 0;
					if ((++_stafClear % 600) == 0) _stafSent.clear();

					// === DE-INDEXED STRIP emit (was per-triangle; that bypassed the
					// client's strip->list winding fix and garbled effects/supers). ====
					// For each kept PolyParam we walk rc.idx[pp.first .. pp.first+pp.count]
					// (the degenerate-linked STRIP produced by ta_parse(ctx,false)) and
					// append rc.verts[idx] CONTIGUOUSLY to one output VERTEX buffer, then
					// emit ONE poly record {firstVert,vertCount,tcw,tsp,pcw,isp,listType}.
					// We do NOT triangulate and do NOT skip degenerate links: the client
					// rebuilds the SAME object shape as ta-parser.mjs (a 28-B/vertex buffer
					// + op/pt/tr PolyParams whose first/count are CONSECUTIVE vertex indices
					// = a strip), so PVR2Renderer._buildIndexBuffer does the winding-correct
					// strip->triangle-list conversion exactly like the working out-of-match
					// TA video. Pixel-identical by construction.
					//
					// Wire (post-zstd), see docs/STRIPPED-TA-DESIGN.md §8.2:
					//   'STAF'(4) frameNum(4) pvr_snapshot[16](64) vertCount(u32) polyCount(u32)
					//   vertCount × vertex (28 B): x,y,z(f32) u,v(f32) col(4 RGBA) spc(4 RGBA)
					//   polyCount × poly  (33 B): firstVert(u32) vertCount(u32) texId(8)
					//                             tcw(4) tsp(4) pcw(4) isp(4) listType(1)
					//   texId is the 64-bit content hash matching the TX64 cache key (0 =
					//   untextured); tcw is the raw PVR TCW (kept for reference/filter).
					_stafBuf.clear();
					_stafBuf.resize(4 + 4 + 64 + 4 + 4);    // header + frameNum + pvr_snapshot + vertCount + polyCount
					_stafBuf[0] = 'S'; _stafBuf[1] = 'T'; _stafBuf[2] = 'A'; _stafBuf[3] = 'F';
					memcpy(&_stafBuf[4], &_localFrameNum, 4);
					memcpy(&_stafBuf[8], pvr_snapshot, 64);
					uint32_t vertCount = 0, polyCount = 0;
					auto putF = [&](float f) { uint32_t u; memcpy(&u, &f, 4); for (int i = 0; i < 4; i++) _stafBuf.push_back((u >> (i * 8)) & 0xff); };
					auto putU16 = [&](uint16_t v) { _stafBuf.push_back(v & 0xff); _stafBuf.push_back((v >> 8) & 0xff); };
					auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) _stafBuf.push_back((v >> (i * 8)) & 0xff); };
					(void)putU16;

					// The poly records reference firstVert into the SAME _stafBuf grown
					// above; but verts and poly records interleave on the wire region-wise
					// (all verts first, then all polys). We stage poly records in a side
					// buffer and append after the vertex region is finalized.
					static std::vector<uint8_t> _stafPoly;
					_stafPoly.clear();
					auto polyPut32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) _stafPoly.push_back((v >> (i * 8)) & 0xff); };
					auto polyPut64 = [&](uint64_t v) { for (int i = 0; i < 8; i++) _stafPoly.push_back((v >> (i * 8)) & 0xff); };

					// Per-list geometry debug: capture the first few polys of each list.
					static bool _stafDbg = getenv("MAPLECAST_STAF_DBG") != nullptr;
					static uint32_t _stafDbgCtr = 0;
					bool dbgFrame = _stafDbg && (_stafDbgCtr % 600) == 0;
					FILE* dgf = nullptr; int dbgShown[3] = { 0, 0, 0 };
					if (dbgFrame) {
						dgf = fopen("/dev/shm/mc_staf_geom.log", "a");
						if (dgf) fprintf(dgf, "frame %u  verts=%zu idx=%zu  (op=%zu pt=%zu tr=%zu polys)\n",
							_localFrameNum, rcS.verts.size(), rcS.idx.size(),
							rcS.global_param_op.size(), rcS.global_param_pt.size(), rcS.global_param_tr.size());
					}

					// listType: 0=op(opaque/bg) 1=pt(punch-through/char) 2=tr(translucent/fx).
					// Drawn in this order on the client (= z-order), preserving sent order within a list.
					auto emitList = [&](std::vector<PolyParam>& lst, int listType) {
						for (PolyParam& pp : lst) {
							if (pp.count < 3) continue;
							if (polyCount >= 16384) return;
							uint32_t tcw = pp.tcw.full, tsp = pp.tsp.full, pcw = pp.pcw.full;
							uint32_t iend = pp.first + pp.count;
							if (iend > rcS.idx.size()) iend = (uint32_t)rcS.idx.size();
							if (pp.first + 3 > iend) continue;
							// HUDF filter BEFORE the texture decode/ship — only pay for quads we keep
							// (effects=additive, HUD=top/bottom strips). Fixes the char-select texture
							// flood (was decoding the whole animated demo screen every frame).
							if (_hudfOn) {
								bool _add = ((tsp >> 26) & 7) == 1;
								uint32_t _nv = (uint32_t)rcS.verts.size();
								float _mnY = 1e9f, _mxY = -1e9f;
								for (uint32_t kk = pp.first; kk < iend; kk++) { uint32_t vi = rcS.idx[kk]; if (vi < _nv) { float yy = rcS.verts[vi].y; if (yy < _mnY) _mnY = yy; if (yy > _mxY) _mxY = yy; } }
								float _cyq = (_mnY + _mxY) * 0.5f;
								if (!_add && !(_cyq < 56.f || _cyq > 424.f)) continue;
							}
							bool textured = ((pcw >> 3) & 1) != 0;
							uint64_t texId = 0;
							int tw = 0, th = 0;
							if (textured) {
								int fmt = (tcw >> 27) & 7, mip = (tcw >> 31) & 1;
								if (mip || (fmt > 2 && fmt != 5 && fmt != 6)) {
									textured = false; // unsupported fmt -> draw untextured (vertex color)
								} else {
									tw = 8 << ((tsp >> 3) & 7); th = 8 << (tsp & 7);
									texId = mcfx::texHash64(tcw, tw, th);
									if (_stafRgba && _stafSent.find(texId) == _stafSent.end()) {
										if (mcfx::decodeTexAny(tcw, tsp, _stafRgba)) {
											_stafSent.insert(texId);
											uint32_t rgbaSize = (uint32_t)(tw * th * 4);
											size_t compSize = 0; uint64_t cus = 0;
											const uint8_t* comp = _compressor.compress(_stafRgba, rgbaSize, compSize, cus);
											// TX64: 'TX64'(4) texId(8) w(2) h(2) rawSize(4) zstd(RGBA)
											std::vector<uint8_t> tb(20 + compSize);
											tb[0] = 'T'; tb[1] = 'X'; tb[2] = '6'; tb[3] = '4';
											memcpy(&tb[4], &texId, 8);
											tb[12] = tw & 0xff; tb[13] = (tw >> 8) & 0xff; tb[14] = th & 0xff; tb[15] = (th >> 8) & 0xff;
											memcpy(&tb[16], &rgbaSize, 4); memcpy(&tb[20], comp, compSize);
											maplecast_ws::broadcastBinary(tb.data(), (uint32_t)tb.size());
											if (_stafMeasureReal) _tx64BytesAcc += tb.size();
										} else {
											texId = 0; textured = false;   // couldn't decode -> draw untextured
										}
									}
								}
							}
							// DE-INDEX: append rc.verts[rc.idx[k]] for k in [pp.first, iend),
							// CONTIGUOUSLY. The result is a degenerate-linked strip in the
							// output vertex buffer; firstVert/vertCount span it 1:1. The
							// client feeds it to PVR2Renderer exactly like ta-parser output.
							uint32_t nverts = (uint32_t)rcS.verts.size();
							uint32_t firstVert = vertCount;
							uint32_t emitted = 0;
							for (uint32_t k = pp.first; k < iend; k++) {
								uint32_t vi = rcS.idx[k];
								if (vi >= nverts) vi = 0;   // guard; PVR2Renderer drops zero-area links
								Vertex& v = rcS.verts[vi];
								putF(v.x); putF(v.y); putF(v.z);
								putF(v.u); putF(v.v);
								// Vertex.col is [R,G,B,A]; spc is the offset color (same order).
								_stafBuf.push_back(v.col[0]); _stafBuf.push_back(v.col[1]);
								_stafBuf.push_back(v.col[2]); _stafBuf.push_back(v.col[3]);
								_stafBuf.push_back(v.spc[0]); _stafBuf.push_back(v.spc[1]);
								_stafBuf.push_back(v.spc[2]); _stafBuf.push_back(v.spc[3]);
								vertCount++; emitted++;
							}
							if (emitted < 3) {  // nothing usable — roll back the verts we appended
								_stafBuf.resize(_stafBuf.size() - (size_t)emitted * 28);
								vertCount -= emitted;
								continue;
							}
							// Poly record: firstVert vertCount texId tcw tsp pcw isp listType.
							// !textured -> texId/tcw forced 0 so the client/texMgr draws untextured.
							polyPut32(firstVert);
							polyPut32(emitted);
							polyPut64(textured ? texId : 0ull);
							polyPut32(textured ? tcw : 0u);
							polyPut32(tsp);
							polyPut32(pcw);
							polyPut32(pp.isp.full);
							_stafPoly.push_back((uint8_t)listType);
							polyCount++;
							if (dgf && dbgShown[listType] < 4) {
								Vertex& a = rcS.verts[rcS.idx[pp.first] < nverts ? rcS.idx[pp.first] : 0];
								fprintf(dgf, "  %s poly firstVert=%u vc=%u (%.0f,%.0f uv %.3f,%.3f) tex=%d tcw=%08x\n",
									listType==0?"OP":(listType==1?"PT":"TR"), firstVert, emitted,
									a.x, a.y, a.u, a.v, textured?1:0, textured ? tcw : 0u);
								dbgShown[listType]++;
							}
						}
					};

					// === ISP_BACKGND opaque backdrop poly (RENDER-TIER1-PLAN §5.1.3 / step 3) ===
					// ta_parse(ctx,false) emits the three display lists but NOT flycast's
					// synthesized FillBGP background (the implicit full-screen backdrop drawn
					// from the PVR ISP_BACKGND_T/D regs + a VRAM strip). Without it the full
					// frame renders on a transparent base -> black bleed. We port the client's
					// ta-parser.fillBGP (web/webgpu/ta-parser.mjs:252-392) server-side and emit
					// ONE opaque poly as the FIRST op record (the client draws op->pt->tr in
					// sent order, so the backdrop sits behind everything). HUDF is an effects/
					// HUD overlay and must stay transparent, so the BG poly is full-frame ONLY.
					if (_stafOnEmit && !_hudfOn) {
						uint32_t paramBase = PARAM_BASE & 0xF00000;
						uint32_t ispBgT = ISP_BACKGND_T.full;
						float    ispBgD = ISP_BACKGND_D.f;
						uint32_t tagOffset  = ispBgT & 7;
						uint32_t tagAddress = (ispBgT >> 3) & 0x1FFFFF;
						uint32_t skip       = (ispBgT >> 24) & 7;
						uint32_t stripBase  = (paramBase + tagAddress * 4) & (VRAM_SIZE - 1);
						uint32_t stripVs    = (3 + skip) * 4;            // bytes per strip vertex entry
						uint32_t vptr0      = tagOffset * stripVs + stripBase + 12; // +12 skips ISP/TSP/TCW
						auto vrU32 = [&](uint32_t a) -> uint32_t {
							if (a + 4 > VRAM_SIZE) return 0;
							return vram[a] | (vram[a+1]<<8) | (vram[a+2]<<16) | ((uint32_t)vram[a+3]<<24);
						};
						auto vrF32 = [&](uint32_t a) -> float { uint32_t u = vrU32(a); float f; memcpy(&f,&u,4); return f; };
						if (stripBase + 12 <= VRAM_SIZE) {
							uint32_t bgISP = vrU32(stripBase);
							int isTexture = (bgISP >> 25) & 1;
							int isOffset  = (bgISP >> 24) & 1;
							int isUV16    = (bgISP >> 22) & 1;
							// Read the 3 strip verts (colors only; we build a full-screen quad).
							struct BV { float x,y,z; uint32_t col, spc; } bv[3];
							uint32_t vptr = vptr0; bool ok = true;
							for (int i = 0; i < 3; i++) {
								if (vptr + 12 > VRAM_SIZE) { ok = false; break; }
								bv[i].x = vrF32(vptr); bv[i].y = vrF32(vptr+4); bv[i].z = vrF32(vptr+8);
								uint32_t cptr = vptr + 12;
								if (isTexture) cptr += isUV16 ? 4 : 8;
								bv[i].col = (cptr + 4 <= VRAM_SIZE) ? vrU32(cptr) : 0xFFFFFFFFu;
								bv[i].spc = (isOffset && cptr + 8 <= VRAM_SIZE) ? vrU32(cptr + 4) : 0u;
								vptr += stripVs;
							}
							if (ok) {
								// Background depth (FillBGP nudges it just behind everything).
								float bgDepth = ispBgD - 1e-6f; if (bgDepth < 1e-11f) bgDepth = 1e-11f;
								// Full-screen opaque quad covering the guardband viewport (-256..896, 0..480),
								// matching ta-parser.fillBGP's non-textured branch. We draw it untextured
								// (vertex color) — MVC2's backdrop is a flat/gouraud color; any textured
								// stage art already ships as ordinary op polys.
								struct QV { float x,y; uint32_t col,spc; } q[4] = {
									{ -256.f,   0.f, bv[0].col, bv[0].spc },
									{  896.f,   0.f, bv[1].col, bv[1].spc },
									{ -256.f, 480.f, bv[2].col, bv[2].spc },
									{  896.f, 480.f, bv[2].col, bv[2].spc },
								};
								uint32_t firstVert = vertCount;
								for (int i = 0; i < 4; i++) {
									putF(q[i].x); putF(q[i].y); putF(bgDepth);
									putF(0.f); putF(0.f);
									// col/spc are packed ARGB u32 -> push R,G,B,A (emitList's order).
									_stafBuf.push_back((q[i].col>>16)&0xff); _stafBuf.push_back((q[i].col>>8)&0xff);
									_stafBuf.push_back(q[i].col&0xff);       _stafBuf.push_back((q[i].col>>24)&0xff);
									_stafBuf.push_back((q[i].spc>>16)&0xff); _stafBuf.push_back((q[i].spc>>8)&0xff);
									_stafBuf.push_back(q[i].spc&0xff);       _stafBuf.push_back((q[i].spc>>24)&0xff);
									vertCount++;
								}
								// isp: force CullMode=0, DepthMode=7 (always pass) like fillBGP.
								uint32_t bgIspOut = (bgISP & 0x1FFFFFFF);
								bgIspOut = (bgIspOut & ~(7u << 27)) | (0u << 27); // CullMode=0
								bgIspOut = (bgIspOut & ~(7u << 29)) | (7u << 29); // DepthMode=7
								// Poly record (untextured): firstVert vertCount texId=0 tcw=0 tsp pcw=0 isp listType=0(op).
								polyPut32(firstVert);
								polyPut32(4);
								polyPut64(0ull);            // texId 0 = untextured
								polyPut32(0u);              // tcw 0
								polyPut32(vrU32(stripBase + 4)); // bgTSP (blend/shading state)
								polyPut32(0u);              // pcw 0 -> client draws untextured
								polyPut32(bgIspOut);
								_stafPoly.push_back((uint8_t)0); // op list
								polyCount++;
							}
						}
					}

					emitList(rcS.global_param_op, 0);
					emitList(rcS.global_param_pt, 1);
					emitList(rcS.global_param_tr, 2);
					// Append the staged poly records after the vertex region.
					_stafBuf.insert(_stafBuf.end(), _stafPoly.begin(), _stafPoly.end());
					memcpy(&_stafBuf[72], &vertCount, 4);   // vertCount (u32 LE) at offset 72
					memcpy(&_stafBuf[76], &polyCount, 4);   // polyCount (u32 LE) at offset 76

					// Finalize the gated geometry dump (per-list samples captured during emit).
					if (dgf) { fprintf(dgf, "  -> verts=%u polys=%u bytes=%zu\n", vertCount, polyCount, _stafBuf.size()); fclose(dgf); }
					if (_stafDbg) _stafDbgCtr++;

					// zstd the whole STAF envelope (ZCST outer); client routes 'STAF' after decompress.
					size_t compSize = 0; uint64_t cus = 0;
					const uint8_t* comp = _compressor.compress(_stafBuf.data(), (uint32_t)_stafBuf.size(), compSize, cus);
					maplecast_ws::broadcastBinary(comp, (uint32_t)compSize);

					// Real wire bandwidth: total STAF+TX64 bytes/s actually broadcast.
					if (_stafMeasureReal) {
						_stafBytesAcc += compSize;
						if (++_stafMeasFrames >= 60) {
							double stafKBs = _stafBytesAcc / 1024.0, tx64KBs = _tx64BytesAcc / 1024.0;
							FILE* lf = fopen("/dev/shm/mc_staf.log", "a");
							if (lf) {
								fprintf(lf, "WIRE total=%.1f KB/s (STAF geom=%.1f + TX64 tex=%.1f) | polys=%u verts=%u\n",
									stafKBs + tx64KBs, stafKBs, tx64KBs, polyCount, vertCount);
								fclose(lf);
							}
							_stafBytesAcc = 0; _tx64BytesAcc = 0; _stafMeasFrames = 0;
						}
					}
				}
			}
		// === CHRQ: production per-character PVR sprite-quad channel (MAPLECAST_CHARQ_EMIT) ==
		// Drains the Oracle's structured CHARQ-EMIT accumulator (filled by the two block-entry
		// hooks 0x8C034864 body-part + 0x8C1248CC bank12 submit during this frame's SH4 draw
		// walk) into a binary 'CHRQ' frame and broadcasts it over the SAME ZCST/WS path STAF
		// uses. Carries NO pixels — only tcw refs; textures ride the existing VRAM dirty-page
		// channel. Gated MAPLECAST_CHARQ_EMIT + in-match (0x8C289624). READ-ONLY w.r.t. guest.
		//
		// Wire (uncompressed inner payload, all integers LE; floats IEEE754 LE):
		//   'CHRQ'(4) frameNum(u32) objCount(u32)
		//   per object:
		//     cid(u8) flags(u8) sprite_id(u16) node(u32) quadCount(u16) pad(u16)
		//   per quad:
		//     Ax,Ay,Bx,By,Cx,Cy,Dx,Dy : 8×f32   (screen corners)
		//     AU,AV,BU,BV,CU,CV        : 6×f32   (UVs, already expanded from u16-trunc)
		//     tcw,tsp,pcw              : 3×u32
		{
			static bool _charqOn = getenv("MAPLECAST_CHARQ_EMIT") != nullptr;
			bool _inMatchCq = addrspace::read8(0x8C289624) != 0;
			if (_charqOn && _inMatchCq) {
				const maplecast_oracle_hook::CharqEmitObj* objs = nullptr;
				uint32_t cqFrame = 0;
				int nObj = maplecast_oracle_hook::mc_charqEmit_beginFrame(&objs, &cqFrame);
				if (nObj > 0 && objs) {
					static std::vector<uint8_t> _charqBuf;
					_charqBuf.clear();
					auto cqPutU16 = [&](uint16_t v){ _charqBuf.push_back(v & 0xff); _charqBuf.push_back((v >> 8) & 0xff); };
					auto cqPut32  = [&](uint32_t v){ for (int i = 0; i < 4; i++) _charqBuf.push_back((v >> (i * 8)) & 0xff); };
					auto cqPutF   = [&](float f){ uint32_t u; memcpy(&u, &f, 4); for (int i = 0; i < 4; i++) _charqBuf.push_back((u >> (i * 8)) & 0xff); };
					// header
					_charqBuf.push_back('C'); _charqBuf.push_back('H'); _charqBuf.push_back('R'); _charqBuf.push_back('Q');
					cqPut32(cqFrame);
					cqPut32((uint32_t)nObj);
					uint32_t totalQuads = 0;
					for (int oi = 0; oi < nObj; oi++) {
						int nq = 0;
						const maplecast_oracle_hook::CharqEmitQuad* qs =
							maplecast_oracle_hook::mc_charqEmit_objQuads(oi, &nq);
						if (nq > 0xFFFF) nq = 0xFFFF;
						// object header: cid(u8) flags(u8) sprite_id(u16) node(u32) quadCount(u16) pad(u16)
						_charqBuf.push_back((uint8_t)(objs[oi].cid & 0xff));
						_charqBuf.push_back(objs[oi].flags);
						cqPutU16((uint16_t)(objs[oi].sprite_id & 0xFFFF));
						cqPut32(objs[oi].node);
						cqPutU16((uint16_t)nq);
						cqPutU16(0);   // pad
						for (int qi = 0; qi < nq && qs; qi++) {
							const maplecast_oracle_hook::CharqEmitQuad& q = qs[qi];
							cqPutF(q.Ax); cqPutF(q.Ay); cqPutF(q.Bx); cqPutF(q.By);
							cqPutF(q.Cx); cqPutF(q.Cy); cqPutF(q.Dx); cqPutF(q.Dy);
							cqPutF(q.AU); cqPutF(q.AV); cqPutF(q.BU); cqPutF(q.BV);
							cqPutF(q.CU); cqPutF(q.CV);
							cqPut32(q.tcw); cqPut32(q.tsp); cqPut32(q.pcw);
						}
						totalQuads += (uint32_t)nq;
					}
					maplecast_oracle_hook::mc_charqEmit_endFrame();

					// zstd the whole CHRQ payload (ZCST outer); client routes 'CHRQ' after decompress.
					size_t compSize = 0; uint64_t cus = 0;
					const uint8_t* comp = _compressor.compress(_charqBuf.data(), (uint32_t)_charqBuf.size(), compSize, cus);
					maplecast_ws::broadcastBinary(comp, (uint32_t)compSize);

					// DBG (MAPLECAST_CHARQ_EMIT_DBG): objs/quads/bytes/frame -> /dev/shm/mc_charq.log,
					// flushed once per second (60 frames), reusing the STAFMEASURE cadence.
					static bool _charqDbg = getenv("MAPLECAST_CHARQ_EMIT_DBG") != nullptr;
					if (_charqDbg) {
						static uint64_t _cqBytesAcc = 0, _cqQuadAcc = 0, _cqObjAcc = 0;
						static uint32_t _cqFrames = 0;
						_cqBytesAcc += compSize; _cqQuadAcc += totalQuads; _cqObjAcc += (uint32_t)nObj;
						if (++_cqFrames >= 60) {
							FILE* lf = fopen("/dev/shm/mc_charq.log", "a");
							if (lf) {
								fprintf(lf, "CHRQ frame=%u objs/f=%.1f quads/f=%.1f raw=%zu KB/s=%.1f (last polys=%u verts4=%u)\n",
									cqFrame, _cqObjAcc / 60.0, _cqQuadAcc / 60.0, _charqBuf.size(),
									_cqBytesAcc / 1024.0, totalQuads, totalQuads * 4);
								fclose(lf);
							}
							_cqBytesAcc = 0; _cqQuadAcc = 0; _cqObjAcc = 0; _cqFrames = 0;
						}
					}
				} else {
					// nothing accumulated this frame — still release the (empty) ready set.
					maplecast_oracle_hook::mc_charqEmit_endFrame();
				}
			}
		}
		// === STAF-MEASURE: stripped-TA bandwidth probe (read-only). Splits cost by list —
		// opaque (stage) vs punch-through (characters) vs translucent (fx) — gated to
		// in_match (so the menu's heavy ta_parse never runs). Shows whether caching the
		// stage drops the wire to the character-only number. MAPLECAST_STAFMEASURE -> log/1s.
		{
			static bool _stafOn = getenv("MAPLECAST_STAFMEASURE") != nullptr;
			if (_stafOn) {
				maplecast_gamestate::GameState _sgs; maplecast_gamestate::readGameState(_sgs);
				if (_sgs.in_match) {
					ta_parse(ctx, true);
					auto& rcS = ctx->rend;
					static std::unordered_set<uint32_t> _uniqTex;
					static uint64_t _qb[3] = {0, 0, 0}, _tb[3] = {0, 0, 0}; static uint32_t _q[3] = {0, 0, 0};
					static uint32_t _aF = 0, _aT = 0; static uint64_t _aTB = 0;
					uint32_t fT = 0; uint64_t fTB = 0;
					auto meas = [&](std::vector<PolyParam>& lst, int li) {
						for (PolyParam& pp : lst) {
							if (pp.count < 3) continue;
							_q[li]++; _qb[li] += 11 + (uint64_t)pp.count * 10; _tb[li] += (uint64_t)(pp.count - 2) * 47;
							uint32_t tcw = pp.tcw.full, tsp = pp.tsp.full, pcw = pp.pcw.full;
							if (!((pcw >> 3) & 1)) continue;
							int fmt = (tcw >> 27) & 7, tw = 8 << ((tsp >> 3) & 7), th = 8 << (tsp & 7);
							uint32_t hsh = mcfx::texHash((tcw & 0x1FFFFF) << 3, fmt, tw, th, (tcw >> 30) & 1);
							if (_uniqTex.insert(hsh).second) { fT++; fTB += (uint64_t)tw * th * 4; }
						}
					};
					meas(rcS.global_param_op, 0); meas(rcS.global_param_pt, 1); meas(rcS.global_param_tr, 2);
					_aT += fT; _aTB += fTB;
					if (++_aF >= 60) {
						auto kb = [](uint64_t b) { return (b * 0.45) / 1024.0; };
						FILE* lf = fopen("/dev/shm/mc_staf.log", "a");
						if (lf) { fprintf(lf, "STRIP all=%.0f char-only=%.0f stage=%.0f | TRI all=%.0f char-only=%.0f KB/s | uniqTex=%zu (+%.0f KB/s warmup)\n",
							kb(_qb[0]+_qb[1]+_qb[2]), kb(_qb[1]+_qb[2]), kb(_qb[0]), kb(_tb[0]+_tb[1]+_tb[2]), kb(_tb[1]+_tb[2]), _uniqTex.size(), _aTB / 1024.0); fclose(lf); }
						_q[0] = _q[1] = _q[2] = 0; _qb[0] = _qb[1] = _qb[2] = 0; _tb[0] = _tb[1] = _tb[2] = 0; _aF = 0; _aT = 0; _aTB = 0;
					}
				}
			}
		}
	}

	// Auto-fire the deferred recording capture on in_match 0→1. Lives
	// OUTSIDE the maplecast_ws::active() block above because record_arm
	// must work regardless of whether any mirror client is connected
	// (operator might be solo-recording with no viewers). Cheap — one
	// guest-RAM byte read + a uint8 compare.
	{
		static uint32_t _matchPollCounter = 0;
		static uint8_t  _matchPrevInMatch = 0;
		static int64_t  _matchStartUs = 0;
		static maplecast_gamestate::GameState _matchStartGs{};
		if (++_matchPollCounter >= 3) {
			_matchPollCounter = 0;
			maplecast_gamestate::GameState gs;
			maplecast_gamestate::readGameState(gs);
			maplecast_replay::onFrameInMatchFlag(gs.in_match);

			// Tele-0.9 broadcastMatchEnd DISABLED -- broadcasting a JSON
			// text frame to mirror-WS clients (= the relay upstream
			// connection) somehow stops the relay from forwarding
			// subsequent binary TA frames to its downstream clients.
			// Bisect confirmed: rolling back this single commit restored
			// video. Mechanism still under investigation; suspect the
			// relay's signal-broadcast path or our text frame format.
			// Match-start tracking left intact for when we reroute the
			// emit to a different transport (NATS, side-channel HTTP).
			if (_matchPrevInMatch == 0 && gs.in_match == 1) {
				_matchStartUs = (int64_t)std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::system_clock::now().time_since_epoch()).count();
				_matchStartGs = gs;
			} else if (_matchPrevInMatch == 1 && gs.in_match == 0 && _matchStartUs > 0) {
				// const int64_t endUs = ...;
				// maplecast_ws::broadcastMatchEnd(_matchStartUs, endUs, _matchStartGs, gs);
				_matchStartUs = 0;
			}
			_matchPrevInMatch = gs.in_match;
		}
	}

	// Update telemetry
	{
		auto publishEnd = std::chrono::high_resolution_clock::now();
		uint64_t publishUs = std::chrono::duration_cast<std::chrono::microseconds>(publishEnd - publishStart).count();
		static uint32_t _fpsCounter = 0;
		static auto _fpsStart = std::chrono::high_resolution_clock::now();
		static uint64_t _lastFps = 0;
		_fpsCounter++;
		auto fpsElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(publishEnd - _fpsStart).count();
		if (fpsElapsed >= 1000) {
			_lastFps = _fpsCounter * 1000 / fpsElapsed;
			_fpsCounter = 0;
			_fpsStart = publishEnd;
		}
		maplecast_ws::updateTelemetry({frameNum, taSize, totalDirty, totalSize, publishUs, _lastFps, compressedSize, compressUs});
	}

	// Check if a client is requesting a fresh sync state
	if (hdr->client_request_sync)
	{
		hdr->client_request_sync = 0;
		serverSaveSync();
		// Reset shadows so diffs start from this new sync point
		for (int i = 0; i < _numRegions; i++)
			memcpy(_regions[i].shadow, _regions[i].ptr, _regions[i].size);
		// Reset TA delta so next frame is sent as full
		_taHasPrev = false;
		// ZCS2: restart the streaming envelope at this SYNC so joiners decode onward.
		_zstreamResetPending.store(true, std::memory_order_release);
		hdr->sync_ready = 1;
		printf("[MIRROR] Client requested sync  --  fresh state + TA reset\n");
	}

	// Brain snapshot disabled  --  was 26MB memcpy every 30 frames (~5ms stall)
	// Only needed for shm client initial sync. WebSocket clients use save state instead.
	// if (frameNum % 30 == 0)
	// {
	// 	uint8_t* snap = _shmPtr + HEADER_SIZE;
	// 	size_t off = 0;
	// 	memcpy(snap + off, &mem_b[0], 16 * 1024 * 1024); off += 16 * 1024 * 1024;
	// 	memcpy(snap + off, &vram[0], VRAM_SIZE); off += VRAM_SIZE;
	// 	memcpy(snap + off, &aica::aica_ram[0], 2 * 1024 * 1024);
	// }

	// VRAM hash disabled  --  only used by shm client for drift detection
	// hdr->server_vram_hash = fastVramHash();

	// Audit disabled  --  reduced to VRAM+PVR only

	if (frameNum % 600 == 0)
		printf("[MIRROR] Server frame %u | TA=%u bytes | %u dirty pages | %u->%u bytes (%.1fx) zstd %luus\n",
			frameNum, taSize, totalDirty, totalSize, compressedSize,
			compressedSize > 0 ? (double)totalSize / compressedSize : 0.0, compressUs);

	// Restore PVR region pointer (paired with the atomic snapshot at the top)
	if (_origPvrPtr) {
		for (int r = 0; r < _numRegions; r++) {
			if (_regions[r].id == 3) { _regions[r].ptr = _origPvrPtr; break; }
		}
	}
}

// ==================== CLIENT: receive TA commands + diffs, run ta_parse ====================

static int64_t _clientNowUs() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#ifdef MAPLECAST_GSTA_CLIENT_BUILD
// =============================================================================
// NATIVE GSTA CLIENT  --  render the replica-live (7212) GSTA wire through
// flycast's OWN renderer (feat/render-replica-live: the validated endgame).
//
// The proof this works: the TA-mirror (7200) renders PIXEL-PERFECT via flycast's
// renderer; the GSTA wire (7212) carries the SAME game state but the browser
// WebGPU reconstruction has bugs. Reconstruct the body TA from the GSTA state
// with the byte-exact transpiled render_frame (re_kb finding:render_replica_
// phase1_codederived, params 16/16, 100% pixel match), feed flycast's OWN
// renderer->Process  ->  pixel-perfect by construction.
//
// FLOW (mirrors web/render-replica/replay.html parsePrefix/seedFrom/liveApplyFrame):
//   7212 WS  ->  msg1 ZCST static prefix: MCRR header + region tables + 8MB VRAM +
//                32KB PVR + static regions + 16MB RAM  ->  seed _gstaRam/vram/pvr (ONCE)
//   per FRMx: overlay dynamic regions (splat addr&0xFFFFFF) + GFX/PALETTE/HUDQ tails
//   ->  render_frame(&ctx{.ram=_gstaRam})  ->  SceneQuad[]
//   ->  [M3] body_decoder: GFX1[sel] -> vram at TCW
//   ->  emit 96B sprite-TA into TA_context.tad  (+ HUDQ quads)
//   ->  renderer->Process  ->  flycast OpenGL.
//
// Separate from the TA-mirror path: this never calls clientReceive(); the render
// loop routes to clientReceiveGsta() when gstaModeActive(). _isClient is set so
// the mirror render loop in mainui.cpp runs.
// =============================================================================
#include "gsta_render_frame.h"

// MCRR / FRMx / HUDQ magics (LE on the wire)  --  match maplecast_replica_live.cpp.
static constexpr uint32_t GSTA_MCRR_MAGIC = 0x5252434Du;   // "MCRR"
static constexpr uint32_t GSTA_FRMX_MAGIC = 0x784D5246u;   // "FRMx"
static constexpr uint32_t GSTA_HUDQ_MAGIC = 0x48554451u;   // "HUDQ"
static constexpr uint32_t GSTA_BTCW_MAGIC = 0x57435442u;   // "BTCW" (resolved-body-tcw tail)
static constexpr uint32_t GSTA_PL3D_MAGIC = 0x44334C50u;   // "PL3D" (3D-machine SQ-flush tail)

static bool                 _gstaMode = false;
static std::atomic<bool>    _gstaSeeded{false};       // static prefix applied
static std::thread          _gstaThread;

// The flat 16MB area-3 RAM image render_frame reads (NOT the emulator's mem_b,
// which a CLIENT_ONLY build with no SH4/ROM may not have populated). Seeded once
// from the prefix's "ram16" static region, overlaid per FRMx by the dynamic regions.
static std::vector<uint8_t> _gstaRam;                 // 16MB

// One parsed dynamic-region descriptor (addr,len,tag) from the prefix table.
struct GstaRegion { uint32_t addr, len; char tag[9]; };
static std::vector<GstaRegion> _gstaDynRegs;
static uint32_t             _gstaDynTotal = 0;        // sum of dynamic region lens

// Producer -> consumer handoff: the WS thread parses+seeds+renders into a
// double-buffered TA staging area; the render thread drains it in clientReceiveGsta.
// One decoded body tile staged for VRAM application ON THE RENDER THREAD. The body
// texture decode runs on the WS thread but MUST land in vram[] paired with THIS frame's
// TA — the rectab arena reuses the same vaddrs every frame, so if the WS thread decodes
// frame N+k's textures into the single shared vram[] while the render thread is still
// drawing frame N's TA, every quad samples the WRONG part at the right position = the
// Cable "fragmentation" (wrong-limb-at-correct-place). Staging the (vaddr,bytes) here and
// applying them just before Process() guarantees TA<->texture are always the same frame.
struct GstaTileWrite { uint32_t vaddr; uint8_t bytes[512]; };
struct GstaFrame {
	std::vector<uint8_t> ta;       // emitted PVR2 sprite TA (96B/quad + EOL)
	std::vector<GstaTileWrite> tiles;  // body tiles to apply to vram[] on the render thread
	uint32_t             vframe = 0;
	bool                 vramDirty = false;   // body decode / palette touched VRAM/pal
	bool                 palDirty  = false;
};
static GstaFrame              _gstaFrame;              // last rendered frame
static std::mutex             _gstaMtx;
static std::atomic<bool>      _gstaReady{false};

// Per-frame scratch reused across frames (avoid realloc churn).
static GstaSh4Ctx             _gstaCtx;

bool gstaModeActive() { return _gstaMode; }

// forward decls (defined below)
static int gstaDecodeBodies(int nQuad, std::vector<GstaTileWrite>& outTiles);
static std::vector<uint8_t> gstaBuildHudTA(const uint8_t* hud, uint32_t nHud);

// ---- LE readers over a byte buffer (wire is little-endian) ----
static inline uint32_t gle32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

// Apply the MCRR static prefix (decompressed bytes). Seeds _gstaRam (16MB), the
// emulator's vram[] (8MB) + pvr_regs[] (32KB), and the static regions; records the
// dynamic-region table for per-FRMx overlay. Returns false on a malformed prefix.
// Mirrors replay.html parsePrefix() + seedFrom().
static bool gstaApplyPrefix(const uint8_t* d, size_t n)
{
	if (n < 32 || gle32(d) != GSTA_MCRR_MAGIC) {
		printf("[GSTA] bad MCRR magic in prefix (%zu B)\n", n); return false;
	}
	size_t p = 0;
	auto u32 = [&]() { uint32_t v = gle32(d + p); p += 4; return v; };
	u32();                          // magic (checked)
	uint32_t version  = u32();
	uint32_t nStatic  = u32();
	uint32_t nDynamic = u32();
	u32();                          // nFrames (0, streamed)
	uint32_t vramBytes= u32();
	uint32_t pvrBytes = u32();
	u32();                          // reserved

	auto region = [&](GstaRegion& r) {
		r.addr = u32(); r.len = u32();
		memcpy(r.tag, d + p, 8); r.tag[8] = 0; p += 8;
	};
	std::vector<GstaRegion> staticRegs(nStatic);
	for (auto& r : staticRegs) region(r);
	_gstaDynRegs.assign(nDynamic, {});
	for (auto& r : _gstaDynRegs) region(r);
	_gstaDynTotal = 0;
	for (auto& r : _gstaDynRegs) _gstaDynTotal += r.len;

	// ---- static payload: VRAM, PVR regs, then each static region's bytes ----
	if (p + vramBytes + pvrBytes > n) { printf("[GSTA] prefix truncated (vram/pvr)\n"); return false; }
	if (_gstaRam.size() != 16u*1024*1024) _gstaRam.assign(16u*1024*1024, 0);

	if (vramBytes <= (uint32_t)VRAM_SIZE) memcpy(&vram[0], d + p, vramBytes);
	p += vramBytes;
	if (pvrBytes <= (uint32_t)pvr_RegSize) memcpy(pvr_regs, d + p, pvrBytes);
	p += pvrBytes;

	for (auto& r : staticRegs) {
		if (p + r.len > n) { printf("[GSTA] prefix truncated at static region '%s'\n", r.tag); return false; }
		if (strcmp(r.tag, "ram16") == 0)
			memcpy(&_gstaRam[0], d + p, std::min<size_t>(r.len, _gstaRam.size()));
		else {
			uint32_t off = r.addr & 0x00FFFFFFu;
			if ((size_t)off + r.len <= _gstaRam.size())
				memcpy(&_gstaRam[off], d + p, r.len);
		}
		p += r.len;
	}

	// Seed flycast's renderer with the static VRAM/PVR: unprotect, mark caches dirty.
	memwatch::unprotect();
	if (renderer) { renderer->resetTextureCache = true; renderer->updatePalette = true; renderer->updateFogTable = true; }
	pal_needs_update = true; palette_update();

	printf("[GSTA] prefix seeded: v%u, %u static + %u dynamic regions (dynTotal=%u), vram=%u pvr=%u, ram=16MB\n",
		version, nStatic, nDynamic, _gstaDynTotal, vramBytes, pvrBytes);
	for (auto& r : _gstaDynRegs) printf("[GSTA]   dyn region '%s' @%08x len=%u\n", r.tag, r.addr, r.len);
	return true;
}

// Build one PVR2 sprite-TA from the current scene, APPENDING to `out` (port of
// wasm_entry_frame.c render_frame_ta emit loop). Appends after any already-present
// bytes (e.g. the stage OPAQUE list) so the body TR sprites follow the OP list in the
// TA stream. Returns the NEW TOTAL size of `out` after appending.
static uint32_t gstaEmitSpriteTA_append(std::vector<uint8_t>& out)
{
	auto h16 = [](float f){ uint32_t u; memcpy(&u,&f,4); return (uint16_t)((u>>16)&0xFFFF); };
	// LIVE render-debug (control-WS): body force-color (isolate body coverage).
	auto& _DBG = ::gsta_render_debug::g();
	const bool _bodyForceC = _DBG.bodyForceColorOn.load(std::memory_order_relaxed) != 0;
	const uint32_t _bodyColARGB = _DBG.bodyForceColorARGB.load(std::memory_order_relaxed);
	int n = render_frame_nscene();
	const GstaSceneQuad* S = render_frame_scene();
	const size_t startOff = out.size();
	out.resize(startOff + (size_t)n * 96 + 32, 0);
	uint8_t* base = out.data() + startOff;
	uint32_t o = 0;
	auto W32 = [&](uint32_t off, uint32_t v){ uint8_t* q=base+off; q[0]=v;q[1]=v>>8;q[2]=v>>16;q[3]=v>>24; };
	auto WF  = [&](uint32_t off, float f){ uint32_t u; memcpy(&u,&f,4); W32(off,u); };
	for (int k = 0; k < n; k++) {
		const GstaSceneQuad* q = &S[k];
		uint8_t* p = base + o; memset(p, 0, 96);
		// --- TA SPRITE GLOBAL PARAM ---
		// q->pcw is the engine's INTERNAL region-array PolyParam PCW (submit_params /
		// gen_submit_params.c finalize_body OR's 0x02000000 = ListType bit, leaving
		// ParaType=0). Fed verbatim into the TA stream that is FATAL: native flycast
		// ta_parse (core/hw/pvr/ta.cpp ta_handle_cmd) reads PCW.ParaType (bits 29-31)
		// and PCW.ListType (bits 24-26) to drive the TA FSM. ParaType=0 == End_Of_List,
		// so every "sprite" closed the list and emitted NO geometry -> blank window.
		// Synthesize a VALID TA sprite global param: ParaType=5 (Sprite), keep the
		// engine obj_ctrl/group-control low bits, force textured + 16-bit UV (the
		// vertex layout below writes f16 UVs), translucent list (MVC2 bodies are
		// MODULATE/translucent). CONFIRMED vs ta_structs.h PCW bitfield + ta.cpp FSM.
		uint32_t pcw_ta = (q->pcw & 0x0000FFFFu)   // keep group-control + obj_ctrl bytes
		                | (5u << 29)               // ParaType = Sprite
		                | (2u << 24);              // ListType = Translucent
		pcw_ta |= 0x00000008u;                     // Texture = 1 (obj_ctrl bit3)
		pcw_ta |= 0x00000001u;                     // UV_16bit = 1 (obj_ctrl bit0)
		// EFFECT BLEND. A quad whose GFX1 base lives in the shared Effect-Poly bank
		// [0x0CED0000,0x0CEE0000) is a hitspark/aura/super-flash effect, NOT a translucent
		// body/cape. The engine submits these through a DIFFERENT finalize branch (the type==4
		// cell-TSP path loc_8c124740, gated on r13[0x30]) that reads the cells own SrcInstr/
		// DstInstr (finding:objs_effect_blend, reference_mvc2_effects_bank: effect-poly = additive).
		// The lean GSTA render_frame runs ONLY the BODY translucent finalize (gen_submit_params.c
		// finalize_body forces SRCA->INVSRCA, which is REQUIRED for the raw-template body tiles),
		// so q->tsp is the generic translucent blend for every quad. We re-apply the engine effect
		// rule HERE: for an Effect-Poly gfx1, set DstInstr=ONE (additive accumulate, keep
		// SrcInstr=SRCA). MEASUREMENT (2026-06-20, _fxwin_3min.zcst, 60s+3min live engine TA,
		// 326k+ sprites + 10790 OBJF frames): the slot-0 match fired NO super/projectile in any
		// window -- 100% SRCA->INVSRCA, ZERO is_effect/additive -- so this branch is a verified
		// no-op on all CURRENT traffic and only takes effect when a real Effect-Poly cell renders.
		// The deeper-faithful path (read the effect cells own resident TSP via the type==4 branch)
		// is the OPEN item, blocked on a live contact-frame capture (docs/GSTA-FINDINGS-FOR-BROWSER.md).
		uint32_t tsp_ta = q->tsp;
		const bool isEffect = (q->gfx1 >= 0x0CED0000u && q->gfx1 < 0x0CEE0000u);
		if (isEffect) {
			// GAP 2 (blend). The effect texels are already resident in the shipped VRAM
			// (GAP 1 is the SKIP — the engine pre-uploaded them; see gstaDecodeBodies). The
			// engine derives an effect cell's blend from the type==4 cell-TSP finalize
			// (bank12 loc_8c124740) reading the
			// cell's OWN SrcInstr/DstInstr — not reachable on the lean GSTA body-finalize
			// path. is_effect (GFX in the Effect-Poly bank) is the cited engine-faithful
			// additive signal (re_kb finding:objs_effect_blend / reference_mvc2_effects_bank:
			// hitsparks/energy/super flashes accumulate). Keep SrcInstr=SRCA, set DstInstr=ONE
			// (additive glow). The exact per-cell TSP (e.g. an alpha-blended ARGB4444 aura vs
			// an additive RGB565 beam) is the remaining refinement, blocked on a live effect
			// quad to A/B against the engine TA (no captured frame has one yet — MEASURED:
			// _live_fx2 has resident effect textures but ZERO active effect render nodes and
			// ZERO engine additive sprites across 1199 frames).
			tsp_ta = (tsp_ta & ~(7u << 26)) | (1u << 26);   // DstInstr (bits 28:26) = ONE -> additive
		}
		// param header
		W32(o+0,pcw_ta); W32(o+4,q->isp); W32(o+8,tsp_ta); W32(o+12,q->tcw);
		W32(o+16, _bodyForceC ? _bodyColARGB : 0xFFFFFFFFu);   // sprite base color (white=MODULATE id; live override)
		W32(o+32,0xE0000000u);          // sprite vtx PCW
		WF(o+36,q->Ax); WF(o+40,q->Ay); WF(o+44,q->z);
		WF(o+48,q->Bx); WF(o+52,q->By); WF(o+56,q->z);
		WF(o+60,q->Cx); WF(o+64,q->Cy); WF(o+68,q->z);
		WF(o+72,q->Dx); WF(o+76,q->Dy);
		{
			float U = q->u1, V = q->v1;
			float uLo = q->mirror ? U : 0.0f;
			float uHi = q->mirror ? 0.0f : U;
			uint16_t v0=h16(V),u0=h16(uLo),v1=h16(V),u1=h16(uHi),v2=h16(0.0f),u2=h16(uHi);
			p[84]=v0;p[85]=v0>>8; p[86]=u0;p[87]=u0>>8;
			p[88]=v1;p[89]=v1>>8; p[90]=u1;p[91]=u1>>8;
			p[92]=v2;p[93]=v2>>8; p[94]=u2;p[95]=u2>>8;
		}
		o += 96;
	}
	memset(base + o, 0, 32); o += 32;   // EndOfList
	return o;
}

// =============================================================================
// M3: BODY TEXTURE DECODE  --  faithful C++ port of web/render-replica/body_decoder.mjs
// ensureBodyTextures (per_tile_retile_decode). For each emitted body quad: decode its
// GFX1 part (LZSS loc_8c0354c0, byte-exact) from _gstaRam, detwiddle to a linear W×H
// index buffer, slice the (col,row) 32×32 tile, re-twiddle it as a standalone 32×32
// PAL4_TW, and write 512B at the quad's own TCW vaddr in vram[]. (re_kb
// finding:per_tile_retile_decode + finding:faithful_texture_decode_transpile.)
// =============================================================================
// PAL4 twiddle tables (port of body_decoder.mjs _twiddleSlow / _DETW / _PAL4_ORDER).
static int gsta_twiddleSlow(int x, int y, int xs, int ys) {
	int rv = 0, sh = 0; xs >>= 1; ys >>= 1;
	while (xs || ys) {
		if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; }
		if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; }
	}
	return rv;
}
static int gsta_DETW[2][11][1024];
static const int gsta_PAL4_ORDER[16][2] = {
	{0,0},{0,1},{1,0},{1,1},{0,2},{0,3},{1,2},{1,3},{2,0},{2,1},{3,0},{3,1},{2,2},{2,3},{3,2},{3,3}
};
static bool gsta_twInit = false;
static void gstaTwInit() {
	if (gsta_twInit) return;
	for (int s = 0; s < 11; s++) {
		int ys = 1 << s;
		for (int i = 0; i < 1024; i++) {
			gsta_DETW[0][s][i] = gsta_twiddleSlow(i, 0, 1024, ys);
			gsta_DETW[1][s][i] = gsta_twiddleSlow(0, i, ys, 1024);
		}
	}
	gsta_twInit = true;
}
static int gsta_log2i(int v) { int n = -1; while (v) { v >>= 1; n++; } return n; }

// TWIDDLED W×H PAL4 bytes -> linear W×H index buffer (1 byte/px, low nibble).
static void gstaDetwiddlePal4(const uint8_t* data, size_t dataLen, int w, int h, std::vector<uint8_t>& idx) {
	int bcx = gsta_log2i(w), bcy = gsta_log2i(h);
	idx.assign((size_t)w * h, 0);
	for (int y = 0; y < h; y += 4) for (int x = 0; x < w; x += 4) {
		int blk = (gsta_DETW[0][bcy][x] + gsta_DETW[1][bcx][y]) / 16;
		int base = blk * 8;
		for (int i = 0; i < 16; i++) {
			int cx = gsta_PAL4_ORDER[i][0], cy = gsta_PAL4_ORDER[i][1];
			uint8_t b = ((size_t)(base + (i >> 1)) < dataLen) ? data[base + (i >> 1)] : 0;
			idx[(size_t)(y + cy) * w + (x + cx)] = (i & 1) ? ((b >> 4) & 0xF) : (b & 0xF);
		}
	}
}
// Re-twiddle one 32×32 linear index region (1024) into 512B of PAL4_TW.
static void gstaRetwiddle32(const uint8_t* lin, uint8_t* out512) {
	memset(out512, 0, 512);
	for (int y = 0; y < 32; y += 4) for (int x = 0; x < 32; x += 4) {
		int blk = (gsta_DETW[0][5][x] + gsta_DETW[1][5][y]) / 16;
		int base = blk * 8;
		for (int i = 0; i < 16; i++) {
			int cx = gsta_PAL4_ORDER[i][0], cy = gsta_PAL4_ORDER[i][1];
			uint8_t nib = lin[(size_t)(y + cy) * 32 + (x + cx)] & 0xF;
			if (i & 1) out512[base + (i >> 1)] |= nib << 4; else out512[base + (i >> 1)] |= nib;
		}
	}
}
// GFX1 LZSS decoder (bank03 loc_8c0354c0). Flag MSB-first from 0x80; clear=literal,
// set=back-ref (b=*src++, dist=b>>4, count=(b&0x0F)+2 from out-(dist+1)). Byte-exact.
static void gstaDecodeA(const uint8_t* src, size_t sp, size_t srcEnd, size_t destLen, std::vector<uint8_t>& out) {
	out.assign(destLen, 0);
	size_t o = 0; uint32_t bc = 0; uint32_t flags = 0;
	while (o < destLen && sp < srcEnd) {
		if (bc == 0) { flags = src[sp++]; bc = 0x80; if (sp >= srcEnd) break; }
		if ((flags & bc) == 0) { out[o++] = src[sp++]; }
		else {
			uint8_t b = src[sp++];
			long s = (long)o - (b >> 4) - 1;
			int cnt = (b & 0x0F) + 2;
			for (int k = 0; k < cnt && o < destLen; k++, s++)
				out[o++] = (s >= 0 && (size_t)s < o) ? out[s] : 0;
		}
		bc >>= 1;
	}
}
// LE readers over _gstaRam (area-3 low-24-bit).
static inline uint32_t gramU32(const uint8_t* r, uint32_t a){ a &= 0x00FFFFFF; return (uint32_t)r[a]|((uint32_t)r[a+1]<<8)|((uint32_t)r[a+2]<<16)|((uint32_t)r[a+3]<<24); }
static inline uint16_t gramU16(const uint8_t* r, uint32_t a){ a &= 0x00FFFFFF; return (uint16_t)(r[a]|(r[a+1]<<8)); }
static inline uint8_t  gramU8 (const uint8_t* r, uint32_t a){ return r[a & 0x00FFFFFF]; }

// GFX1 part-offset table for a given gfx1 base (cached per frame-set).
struct GstaGfx { uint32_t n; std::vector<uint32_t> offs, srt; };
static std::unordered_map<uint32_t, GstaGfx> _gstaGfxCache;          // gfx1 base -> table
struct GstaDecodedPart { std::vector<uint8_t> lin, raw; int W=0, H=0; size_t destLen=0; bool ok=false; };

// PVR rect-twiddle of TILE coords (col,row) in a Tw×Th tile grid -> storage chunk index k.
// Port of body_decoder.mjs twTile. (re_kb finding:wide_part_tile_storage_order, MEASURED 0/16384.)
[[maybe_unused]] static int gstaTwTile(int x, int y, int bx, int by) {  /* x-first: superseded by gstaTwTileYFirst (see carve) */
	int r = 0, b = 0; int sq = bx < by ? bx : by;
	for (int i = 0; i < sq; i++) { r |= ((x >> i) & 1) << b; b++; r |= ((y >> i) & 1) << b; b++; }
	if (bx > by) r |= (x >> sq) << b; else if (by > bx) r |= (y >> sq) << b;
	return r;
}
// Y-FIRST rectangular twiddle of TILE coords (col,row) in a Tw×Th tile grid (Tw,Th = tile
// counts, NOT log2) -> chunk index. flycast's real _twiddleSlow interleave (y-bit before
// x-bit) = the correct storage order for a NON-SQUARE multi-32×32-tile part. CONFIRMED-BY-
// MEASUREMENT 2026-06-15: byte-exact vs engine VRAM for sel267/285/273 64×128 2×4 (Storm cape).
static int gstaTwTileYFirst(int col, int row, int Tw, int Th) {
	int rv = 0, sh = 0, xs = Tw >> 1, ys = Th >> 1, x = col, y = row;
	while (xs || ys) {
		if (ys) { rv |= (y & 1) << sh; ys >>= 1; y >>= 1; sh++; }
		if (xs) { rv |= (x & 1) << sh; xs >>= 1; x >>= 1; sh++; }
	}
	return rv;
}
static std::map<uint64_t, GstaDecodedPart> _gstaPartCache;          // (gfx1<<32|sel) -> part

static GstaGfx& gstaGfx1Offsets(const uint8_t* ram, uint32_t gfx1) {
	auto it = _gstaGfxCache.find(gfx1);
	if (it != _gstaGfxCache.end()) return it->second;
	GstaGfx g; g.n = gramU32(ram, gfx1) >> 2;
	if (g.n > 0x40000) g.n = 0;                                     // sanity
	g.offs.resize(g.n);
	for (uint32_t i = 0; i < g.n; i++) g.offs[i] = gramU32(ram, gfx1 + i * 4);
	std::set<uint32_t> uniq(g.offs.begin(), g.offs.end());
	g.srt.assign(uniq.begin(), uniq.end());                        // sorted
	return _gstaGfxCache.emplace(gfx1, std::move(g)).first->second;
}
static uint32_t gstaEndOf(const std::vector<uint32_t>& srt, uint32_t off) {
	auto it = std::upper_bound(srt.begin(), srt.end(), off);
	return it != srt.end() ? *it : off + 0x4000;
}

static int gstaDecodeBodies(int nQuad, std::vector<GstaTileWrite>& outTiles)
{
	if (nQuad <= 0) return 0;
	gstaTwInit();
	const uint8_t* ram = _gstaRam.data();
	const GstaSceneQuad* S = render_frame_scene();

	// per-quad storage (col,row) from the walker (Ax-rank / Ay-rank). gsta_quad_colrow
	// fills out_cr[2*q]=col, out_cr[2*q+1]=row.
	static std::vector<int> colrow;
	colrow.assign((size_t)nQuad * 2, 0);
	gsta_quad_colrow(colrow.data(), (unsigned)nQuad);
	// per-quad bit15 flag: the DESC-KEYED carve below applies to NON-effect quads only
	// (a bit15 quad's recidx is a scale-walker alloc, not a DESC_TABLE index).
	static std::vector<uint8_t> gIsEff;
	gIsEff.assign((size_t)nQuad, 0);
	gsta_quad_is_effect(gIsEff.data(), (unsigned)nQuad);
	// per-quad SOURCE DESCRIPTOR [m,cx,ry,flags] (emit-time snapshot; gsta_render_frame.h).
	static std::vector<uint8_t> gSrcDesc;
	gSrcDesc.assign((size_t)nQuad * 4, 0);
	gsta_quad_srcdesc(gSrcDesc.data(), (unsigned)nQuad);

	// PRE-PASS — per-(gfx1,sel) RUN tile-grid extent (cols=maxcol+1, rows=maxrow+1). The carve
	// step is the ENGINE TILE SIZE m = W/cols = H/rows (8/16/32, square per re_kb
	// finding:wide_part_tile_storage_order_v2), NOT a hardcoded 32. For a multi-ROW part tiled
	// at m<32 (sel285 W=64 H=16 -> 4×1 m16 / 8×2 m8) the old row*32 over-stepped past H so every
	// row>=1 sampled zero -> the flat grey blocks. The walker's per-tile (col,row) ranks are
	// correct (geometry 0.00px); only the carve PITCH was wrong. CONFIRMED-BY-MEASUREMENT
	// (ASMTRACE PC 0x8C034864 steps screenY per row; the Y-pen was NEVER dropped) 2026-06-15.
	struct RunExt { int mc, mr; };
	std::unordered_map<uint64_t, RunExt> runExt;
	for (int q = 0; q < nQuad; q++) {
		uint32_t g = S[q].gfx1;
		if (!(g & 0x0C000000u) && !(g & 0x8C000000u)) continue;
		uint64_t k = ((uint64_t)g << 32) | S[q].sel;
		int c = colrow[2 * q], r = colrow[2 * q + 1];
		auto it = runExt.find(k);
		if (it == runExt.end()) runExt[k] = { c, r };
		else { if (c > it->second.mc) it->second.mc = c; if (r > it->second.mr) it->second.mr = r; }
	}

	// Bound the decode-part memo; clear if it grows unbounded across many poses.
	if (_gstaPartCache.size() > 4096) _gstaPartCache.clear();

	int written = 0;
	std::vector<uint8_t> raw, tileLin(1024), tile512(512);
	for (int q = 0; q < nQuad; q++) {
		uint32_t gfx1 = S[q].gfx1;
		if (!(gfx1 & 0x0C000000u) && !(gfx1 & 0x8C000000u)) continue;   // no body art
		// BIT15 EFFECT QUADS ARE RESIDENT-BACKED — NEVER STAGE (2026-07-05, _live4 byte-gate).
		// Their textures are ENGINE-UPLOADED (effect slots 0x475xxx/0x60xxxx/0x400xxx, shipped
		// in the GSTA prefix VRAM byte-exact — MEASURED 512/512 vs engine at every sampled
		// slot, both arena parities). Their sels are NOT GFX1 indices (0xC000-class sentinels)
		// — decoding them staged GARBAGE tiles that overwrote the good resident texels (the
		// pal17 Z48 at vf53770 once the cull revision let them reach this loop; pre-revision
		// they were culled before decode, so this skip restores the validated staging set).
		if (q < (int)gIsEff.size() && gIsEff[q]) continue;
		// EFFECT-POLY GUARD (GAP 1, decode). A gfx1 in the shared Effect-Poly bank
		// [0x0CED0000,0x0CEE0000) is NOT a char GFX1 LZSS offset table — it is an
		// ABSOLUTE-POINTER texture DIRECTORY (head=0x0CED0010; dir base at *(0x0CED0008)
		// = 0x0CED03D8; 0x10-byte entries {e0=w|h<<16, e4=fmt desc, e8=ABSOLUTE texel ptr,
		// ec=0}, already-decoded PVR texels — NO LZSS, NO twiddle decode). Feeding it to
		// the body LZSS/detwiddle path below mis-reads n = u32[gfx1]>>2 = 0x033B4004 (the
		// n>0x40000 sanity then zeroes the table -> sel<n false -> the part decode produces
		// a corrupt 512B blob written at the effect quad's TCW = the pink-streak/blue-block
		// garble (docs/GSTA-FINDINGS-FOR-BROWSER.md GAP 1). SKIP it here so the effect quad
		// keeps whatever the engine already placed at its TCW instead of corruption — a
		// strict improvement over the mis-parse. The faithful directory decode (resolve
		// sel -> directory entry -> upload the e8 texels at the right format) is the OPEN
		// item — NOW RESOLVED (2026-06-21, CONFIRMED-BY-MEASUREMENT, _live_fx2.gsta.mcrr):
		// the FAITHFUL action for an Effect-Poly quad is to SKIP the body decode and leave
		// the quad sampling the ALREADY-RESIDENT VRAM texels — exactly the fc7072a69 skip.
		// The engine flow is fundamentally different from a body and does NOT decode per
		// frame:
		//   * The Effect-Poly bank is an absolute-pointer DIRECTORY (head 0x0CED0010, base
		//     *(0x0CED0008)=0x0CED03D8, 0x10-byte entries {e0=w|(h<<16), e4=PVR PixelFmt
		//     |0x300, e8=texel ptr, ec=0}, 25 valid entries [0..24] terminated by e0==0).
		//   * e8 (e.g. 0x0CDA4000 = 13.6 MB) is a SYSTEM-RAM source of already-PVR twiddled
		//     16-bit texels (NO LZSS, NO palette) — it is NOT a VRAM address (it exceeds the
		//     8 MB DC VRAM). The engine DMAs that block to a DYNAMICALLY-ALLOCATED VRAM slot
		//     and points the effect cell's TCW there. MEASURED: dir[0]'s exact texels are
		//     resident in the captured VRAM at 0x4BF000 (content match), NOT at e8 and NOT at
		//     e8 & vram_mask (0x5A4000 holds DIFFERENT data). So the binding from a directory
		//     entry to a VRAM slot is the engine's allocator, recoverable only from a live
		//     effect quad's TCW.
		//   * That VRAM is SHIPPED verbatim in the GSTA prefix (vramBytes=8 MB). When a real
		//     effect quad renders, its TCW (from the resident rectab template, gen_submit_
		//     params.c) already points at the resident VRAM slot — the texels are present.
		//     The ONLY thing the client must NOT do is corrupt that VRAM by mis-LZSS'ing the
		//     directory (the old pink/blue garble). The skip does exactly that. Uploading the
		//     e8 texels to e8&mask was TRIED and is WRONG (different VRAM address, different
		//     data) — reverted.
		//   * NOTE: this offline capture has the effect TEXTURES resident but ZERO active
		//     effect render nodes and ZERO engine additive sprites across all 1199 frames, so
		//     the live node->VRAM-slot binding and the per-cell blend (GAP 2 type==4 cell-TSP,
		//     loc_8c124740) still need a contact-frame capture to A/B. The decode model above
		//     is settled by measurement; only the live binding diff remains.
		if (gfx1 >= 0x0CED0000u && gfx1 < 0x0CEE0000u) continue;
		uint32_t sel = S[q].sel;
		// TCW -> vram byte addr (fmt5 PAL4): (TCW & 0x1FFFFF) << 3.
		uint32_t vaddr = (S[q].tcw & 0x1FFFFF) << 3;
		if ((size_t)vaddr + 512 > VRAM_SIZE) continue;

		uint64_t key = ((uint64_t)gfx1 << 32) | sel;
		auto pit = _gstaPartCache.find(key);
		if (pit == _gstaPartCache.end()) {
			GstaDecodedPart pd;
			GstaGfx& G = gstaGfx1Offsets(ram, gfx1);
			if (sel < G.n) {
				uint32_t pbase = gfx1 + G.offs[sel];
				int sw = gramU8(ram, pbase + 2), sh = gramU8(ram, pbase + 3);
				int W = sw * 8, H = sh * 8;
				if (W > 0 && H > 0 && W <= 1024 && H <= 1024) {
					size_t destLen = (size_t)(W * H) >> 1;
					uint32_t srcStart = (pbase + 4) & 0x00FFFFFF;
					uint32_t srcEnd   = (gfx1 + gstaEndOf(G.srt, G.offs[sel])) & 0x00FFFFFF;
					gstaDecodeA(ram, srcStart, srcEnd, destLen, raw);
					gstaDetwiddlePal4(raw.data(), raw.size(), W, H, pd.lin);
					pd.raw = raw;   // kept for the W>32 && H>32 SQUARE-part native-chunk carve
					pd.W = W; pd.H = H; pd.destLen = destLen; pd.ok = true;
				}
			}
			pit = _gstaPartCache.emplace(key, std::move(pd)).first;
		}
		const GstaDecodedPart& pd = pit->second;
		if (!pd.ok) continue;

		// ONE-SHOT GROUND-TRUTH DUMP (MAPLECAST_DUMP_PART_SEL=<sel>): write the full-part
		// linear index buffer (pd.lin, W*H bytes) + the raw twiddled blob (pd.raw) so the
		// carve can be validated offline against the engine's VRAM for a chosen sel.
		{
			static int _psN = 0;
			const char* psE = std::getenv("MAPLECAST_DUMP_PART_SEL");
			const char* psD = std::getenv("MAPLECAST_DUMP_GSTA_VRAM");
			if (psE && psD && _psN < 1 && (uint32_t)atoi(psE) == S[q].sel) {
				_psN++;
				char pp[600];
				snprintf(pp, sizeof(pp), "%s/part_sel%u_lin_%dx%d.bin", psD, S[q].sel, pd.W, pd.H);
				FILE* lf = fopen(pp, "wb"); if (lf) { fwrite(pd.lin.data(), 1, pd.lin.size(), lf); fclose(lf); }
				snprintf(pp, sizeof(pp), "%s/part_sel%u_raw.bin", psD, S[q].sel);
				FILE* rf = fopen(pp, "wb"); if (rf) { fwrite(pd.raw.data(), 1, pd.raw.size(), rf); fclose(rf); }
				printf("[PARTDUMP] sel=%u W=%d H=%d linLen=%zu rawLen=%zu\n", S[q].sel, pd.W, pd.H, pd.lin.size(), pd.raw.size());
			}
		}

		int colRaw = colrow[2 * q], row = colrow[2 * q + 1];   // colRaw = SCREEN col (Ax-ASC); -> storage col below
		int W = pd.W, H = pd.H;
		// TILE SIZE m = W/cols = H/rows (engine's square per-tile pitch, finding 22-v2),
		// derived from this run's emitted grid extent. Clamp to the 32×32 carve window the
		// pvr2 renderer reads (the engine UV-clamps to the top-left m×m). Fallback m=32 for
		// a single 32-square tile (the prior validated path).
		auto re = runExt.find(key);   /* key == (gfx1<<32)|sel, same run id */
		int cols = (re != runExt.end()) ? (re->second.mc + 1) : 1;
		int rows = (re != runExt.end()) ? (re->second.mr + 1) : 1;
		// STORAGE COLUMN vs the EFFECTIVE MIRROR (per-side multi-column fix, 2026-07-03). colRaw is
		// the SCREEN col (Ax-ASC rank); the storage↔screen column direction REVERSES whenever the
		// tile is horizontally mirrored on screen (S[q].mirror = facing XOR per-part flip4000, the
		// texU mirror / loc_8c0346c4 neg-r8 gate). The leftmost screen tile must sample the RIGHTMOST
		// storage column so that, once the per-tile texU mirror flips each tile, the WHOLE part reads
		// as the correctly L/R-mirrored image. The old `col ASC for both` flipped each column IN PLACE
		// but never reordered them -> multi-column parts scrambled on the mirrored side (facing=1
		// sel252/261/265: 264/222/3714px wrong vs baker-mirrored; reversed col = 0px). cols==1 and
		// un-mirrored tiles are a no-op. Lockstep with body_decoder.mjs ensureBodyTextures. MEASURED
		// vs the byte-exact baker-mirrored (_screengate.mjs facing=1 15/15). (re_kb finding:
		// per_side_storage_col_reverses — restored, keyed on the effective texU mirror.)
		int col = (S[q].mirror & 1u) ? (cols - 1 - colRaw) : colRaw;
		int m = (cols > 0) ? (W / cols) : W;
		int mR = (rows > 0) ? (H / rows) : H;
		if (mR < m) m = mR;
		if (m <= 0) m = 32; if (m > 32) m = 32;
		// ---- DESC-KEYED CARVE (2026-07-05, texel_gate.cpp byte-gate; fixes the satellite
		// fragmentation / typhoon tile-debris). The rank-extent grid above is only an
		// APPROXIMATION that BREAKS when the same (gfx1,sel) part is drawn by MORE THAN ONE
		// node per frame (typhoon = 2-4 cat3/cat4 satellite instances): the GLOBAL Ax/Ay
		// ranks merge/interleave across instances, so cols overcounts (sel 0xDEC W=128
		// 8x16px cols ranked 0..15 -> m collapsed 16->8; high ranks carved out of range ->
		// the ZERO tail). MEASURED offline (texel_gate, replica CERTIFIED 200/200 vs the
		// live client band): window-level pal17 satellites were EXACT=0 of 134.
		// THE FIX (engine-authoritative, no ranks):
		//   * engine tile size mq = u1 * (8<<TSP.texU) — render_frame.c's own body-path
		//     u1 = m/tile encoding, == the walker's descriptor byte0 (verified vs desc).
		//   * part grid = FULL-SPAN dims / mq (pCols=W/mq, pRows=H/mq).
		//   * storage (col,row) from the walker's OWN per-tile descriptor
		//     (DESC_TABLE@0x8C1F9F9C entry dc+k: [2]=cx STORAGE column facing-independent,
		//     [3]=rows-row — rebuilt pose-correct by rebuild_tile_grid), delivered via
		//     gsta_quad_srcdesc = the EMIT-TIME snapshot: the table is shared scratch a
		//     LATER node's rebuild clobbers, so a decode-time re-read is unsafe (measured
		//     idx-464 overlap between Cable 0xD4C m32 and satellite 0xDE6 m16).
		//   * PER-RECORD flip4000 (= texU-mirror XOR owner facing) reverses the cx pairing
		//     (MEASURED sel 0xD4C: engine slot0 holds col1); facing alone does NOT reorder
		//     storage (MEASURED 98/98 on facing-mirrored pal17) — the single-source-of-flip
		//     invariant (texU mirror) is untouched.
		//   * GUARD dm==mq: on a torn +0xDC the desc entry belongs to another node
		//     (MEASURED vf1795928: recidx 464 claimed by Cable 0xD4C m32 AND satellite
		//     0xDE6 m16); fall back to part-grid + screen-rank wrap.
		// Single-instance parts are bit-identical to the old path (cols==pCols, W/cols==mq)
		// — bodies stayed EXACT 21-23/30-33 across the gate, hit-flash 4/4. Coherent-frame
		// satellites: pal17 0 -> 98 EXACT + 36 both-zero of 134 (vf1795922). Residual WRONG
		// at torn frames is INPUT skew (engine drew sel 0xDEF art where the shipped cell
		// says 0xDE9 — proven by content search), not a carve defect. Bit15 quads keep the
		// legacy path bit-for-bit. Lockstep with body_decoder.mjs ensureBodyTextures.
		if (!gIsEff[q]) {
			int dm  = gSrcDesc[4*q+0];
			int dcx = gSrcDesc[4*q+1];
			int dry = gSrcDesc[4*q+2];
			int dfl = gSrcDesc[4*q+3];
			// engine tile size: the emit-time desc m (the walker's own byte0); sanity-check
			// against the quad's u1 encoding (u1 = m/tile, tile = 8<<TSP.texU) — same source.
			int usz = 8 << ((S[q].tsp >> 3) & 7);
			int mq = (int)(S[q].u1 * (float)usz + 0.5f);
			if (mq < 1) mq = 1; if (mq > 32) mq = 32;
			int pCols = W / mq; if (pCols < 1) pCols = 1;
			int pRows = H / mq; if (pRows < 1) pRows = 1;
			if ((dfl & 1) && dm == mq) {
				// DESC-KEYED: cx = STORAGE column; flags bit1 = per-record flip4000 pairs
				// columns DESCENDING (facing alone does NOT reorder storage).
				int cc = dcx % pCols;
				col = (dfl & 2) ? (pCols - 1 - cc) : cc;
				int rr = pRows - dry;             // desc[3] = rows - row
				if (rr < 0) rr = 0; if (rr >= pRows) rr = pRows - 1;
				row = rr;
			} else {
				// FALLBACK (no valid desc): part-grid + screen-rank wrap — reversal over the
				// SCREEN extent (rank space, texU-mirror keyed as before), then wrap.
				int cc = (S[q].mirror & 1u) ? (cols - 1 - colRaw) : colRaw;
				col = ((cc % pCols) + pCols) % pCols;
				row = ((row % pRows) + pRows) % pRows;
			}
			m = mq;
			cols = pCols; rows = pRows;           // native-chunk gate keys on the part grid
		}
		// --- W>32 AND H>32 MULTI-TILE PART (m==32, cols>1, rows>1): copy the NATIVE storage chunk.
		// The engine stores such a part as ONE full-W×H PVR-twiddle blob whose 32×32 chunks follow
		// the PVR twiddle of the TILE grid; tile (col,row)'s VRAM is the chunk at twiddle(col,row).
		//   - Y-FIRST twiddle (gstaTwTileYFirst) for BOTH square AND non-square grids — flycast's real
		//     _twiddleSlow interleave (y-bit before x-bit). e.g. sel285 64×128 2×4, sel197 64×64 2×2,
		//     sel124 128×128 4×4. (The prior `Tw==Th ? gstaTwTile(x-first)` split was WRONG; see below.)
		// CONFIRMED-BY-MEASUREMENT 2026-07-03: reassembling the carved tiles reproduces the byte-exact
		// baker (extract_gfx1_atlas full-span detwiddled lin) 0px for all parts, both chars, 4 frames.
		// SUPERSEDES the broken non-square LINEAR fall-through (re_kb/44 was wrong): linear scattered
		// the off-diagonal tiles -> the POSE-DEPENDENT Storm-cape grey-block garble (_gsta_nobg_360).
		int Tw = W / 32, Th = H / 32;
		if (m == 32 && cols > 1 && rows > 1 && !pd.raw.empty()) {
			// NATIVE-CHUNK ORDER = the engine's 2-ROW-BAND desc order (rebuild_tile_grid's own
			// emission: bands of 2 rows top-down, column-major inside a band), SUPERSEDING the
			// Y-first twiddle (2026-07-05, MEASURED vs engine VRAM: _live4 m1345 sel 0xD61
			// 128x128 4x4 — all 9 nonzero tiles chunk == band-index; Y-first mismapped 8/16 =
			// the Cable-knockdown fragments). Band-order == Y-first for every grid with either
			// dim <= 2 (2x2, 2x4, 4x2 — the previously validated cape/sel197 cases produce
			// IDENTICAL indices), so this only changes >2x>2 grids, where the earlier Y-first
			// "validation" was self-consistent reassembly (carve+reassemble with the same
			// function), never engine VRAM. Lockstep with body_decoder.mjs ensureBodyTextures.
			int _by = row & ~1;
			int _bh = (Th - _by < 2) ? (Th - _by) : 2;
			int k = _by * Tw + col * _bh + (row - _by);
			(void)gstaTwTileYFirst;   // kept for reference/diagnostics
			size_t o = (size_t)k * 512;
			if (o + 512 <= pd.raw.size()) {
				outTiles.emplace_back();
				GstaTileWrite& tw = outTiles.back();
				tw.vaddr = vaddr;
				memcpy(tw.bytes, &pd.raw[o], 512);
				written++;
				continue;
			}
		}
		int ox = col * m, oy = row * m;
		std::fill(tileLin.begin(), tileLin.end(), 0);
		for (int yy = 0; yy < m; yy++) {
			int py = oy + yy; if (py >= H) break;
			const uint8_t* rowBase = &pd.lin[(size_t)py * W];
			uint8_t* dst = &tileLin[(size_t)yy * 32];
			for (int xx = 0; xx < m; xx++) {
				int px = ox + xx; if (px >= W) break;
				dst[xx] = rowBase[px];
			}
		}
		gstaRetwiddle32(tileLin.data(), tile512.data());
		// STAGE the tile for application on the render thread (paired with this frame's
		// TA) instead of writing the shared vram[] from the WS thread — kills the
		// texture<->TA frame race (the Cable wrong-limb fragmentation).
		outTiles.emplace_back();
		GstaTileWrite& tw = outTiles.back();
		tw.vaddr = vaddr;
		memcpy(tw.bytes, tile512.data(), 512);
		written++;
	}
	return written;
}

// =============================================================================
// M4: HUD  --  re-emit each wire HudQuad (the engine's REAL HUD primitive) as a
// paraType-5 sprite-TA block. HudQuad wire layout (maplecast_oracle_hook.h):
//   f32 x[4],y[4]; f32 u[4],v[4]; u32 col[4]; u32 pcw,isp,tsp,tcw  (96 bytes).
// The HUD quads are full parallelograms (angled bars), so we emit the first 3
// corners as the sprite A/B/C (D derived) with the engine's own PVR words +
// per-vertex base color. Drawn through the SAME pvr2 sprite path as bodies.
// =============================================================================
// HUD life-bar FILL-from-state reshape (re_kb/54,/55 — issue 3, native client).
// PART A (staged): the HUD life bars must TRACK health. The HUDQ tail carries the engine's
// REAL bar polys (correct position/color/texture) but the captured fill can be a stale/skewed
// frame's width (MEASURED: HUDQ life-bar quad x[439..591] constant as p2hp 144->55, while the
// engine's real fill shrinks x[438.8..592.5]->[438.8..440.5]). In the NATIVE client the HUDQ
// and _gstaRam are the SAME wire frame (no drift), so we RESHAPE the HP-fill quad's inner edge
// to outer + hpFrac*fullWidth using the live per-slot health from _gstaRam. Everything else
// (frame/highlight/portraits/names/meter/timer/combo glyphs) ships verbatim from HUDQ — this is
// the staged bars-from-state win; the full reconstruct + HUDQ drop is the follow-up end state.
//
// HP-fill signature (MEASURED _live_fx7 fnum=441, tools/_hud_bar_geom.mjs): tcw==0x80000, the
// thin bar-body row (height ~6..12px) in the top life-bar band (cy ~70..120). The OUTER end is
// the portrait side (P1=left=minX anchored, fill grows right; P2=right=maxX anchored, fill grows
// left). 3 bars stack per side (cy rows ~78/98/118) = the 3 team chars, active on top. We map a
// fill quad to its team slot by side (screen center x<320 => P1) + row index (cy order), then
// shrink the INNER edge toward the outer anchor by that slot's hpFrac. Conservative: only quads
// matching the signature are reshaped; all others pass through unchanged. ZERO added bytes.
// (_gstaRam is the seeded 16MB RAM image, std::vector<uint8_t> declared above.)
static std::vector<uint8_t> gstaBuildHudTA(const uint8_t* hud, uint32_t nHud)
{
	std::vector<uint8_t> out;
	if (!nHud) return out;
	if (_gstaRam.size() < 16u*1024*1024) return out;   // not seeded yet -> nothing to reshape
	// DIAGNOSTIC: HUD_DIAG>=2 returns empty -> emit NO HUDQ-sourced HUD at all. If the on-screen
	// bars persist with this, they come from a DIFFERENT path (e.g. render_frame's body TA), not here.
	{ const char* dg = std::getenv("MAPLECAST_HUD_DIAG"); if (dg && atoi(dg) >= 2) return out; }
	auto h16 = [](float f){ uint32_t u; memcpy(&u,&f,4); return (uint16_t)((u>>16)&0xFFFF); };
	auto rd8 = [&](uint32_t a)->uint8_t { return _gstaRam[a & 0x00FFFFFFu]; };
	// Per-char struct bases (page 616) + HP offset; slots P1C1..P2C3.
	static const uint32_t CHAR_BASE[6] = { 0x268340,0x2688E4,0x268E88,0x26942C,0x2699D0,0x269F74 };
	static const int P1_SLOTS[3] = {0,2,4}, P2_SLOTS[3] = {1,3,5};
	const uint8_t HP_MAX = 144;
	// Build active-first slot order per side (active point char on the TOP bar), like the browser.
	auto orderSide = [&](const int* slots, int* outOrder){
		int n=0; int rest[3], nr=0;
		for (int i=0;i<3;i++){ uint32_t b=CHAR_BASE[slots[i]]; if (rd8(b+0x000)) outOrder[n++]=slots[i]; else rest[nr++]=slots[i]; }
		for (int i=0;i<nr;i++) outOrder[n++]=rest[i];
	};
	int p1ord[3], p2ord[3]; orderSide(P1_SLOTS,p1ord); orderSide(P2_SLOTS,p2ord);
	// First pass: collect candidate HP-fill quads (signature), remember their cy so we can rank rows.
	struct Cand { int idx; bool p1; float cy; };
	std::vector<Cand> cands;
	for (uint32_t i=0;i<nHud;i++){ const uint8_t* q=hud+(size_t)i*96;
		float x[4],y[4]; uint32_t tcw; memcpy(x,q+0,16); memcpy(y,q+16,16); memcpy(&tcw,q+92,4);
		float minx=x[0],maxx=x[0],miny=y[0],maxy=y[0];
		for (int k=1;k<4;k++){ minx=std::min(minx,x[k]); maxx=std::max(maxx,x[k]); miny=std::min(miny,y[k]); maxy=std::max(maxy,y[k]); }
		float w=maxx-minx, h=maxy-miny, cy=(miny+maxy)*0.5f, cx=(minx+maxx)*0.5f;
		if ((tcw & 0x1FFFFFu)==0x80000u && w>100.f && w<340.f && h>=4.f && h<=14.f && cy>60.f && cy<130.f)
			cands.push_back({(int)i, cx<320.f, cy});
	}
	// Map a cand to its team slot by VISUAL ROW. CRITICAL: each visible bar is drawn as
	// MULTIPLE 0x80000 quads at ~the same cy (body + highlight pass, cy differs by <1px), so
	// we must CLUSTER cands into visual rows (within ~6px) and map each ROW (not each quad) to
	// a slot — otherwise the duplicate quad at the same cy gets ranked as a DIFFERENT row ->
	// mapped to a reserve slot at full HP -> its full-width twin OVERDRAWS the shrunk one and
	// the bar never visibly depletes (MEASURED: _live_hud, this was the no-deplete bug). Build
	// the per-side sorted ROW list (distinct cy clusters), then row = nearest cluster.
	auto rowsForSide = [&](bool p1)->std::vector<float>{
		std::vector<float> cys; for (auto& d:cands) if (d.p1==p1) cys.push_back(d.cy);
		std::sort(cys.begin(), cys.end());
		std::vector<float> rows; for (float cy:cys){ if (rows.empty() || cy-rows.back()>6.f) rows.push_back(cy); }
		return rows;
	};
	std::vector<float> p1rows=rowsForSide(true), p2rows=rowsForSide(false);
	auto slotForCand = [&](const Cand& c)->int{
		const std::vector<float>& rows = c.p1 ? p1rows : p2rows;
		int row=0; for (size_t r=0;r<rows.size();r++){ if (std::fabs(c.cy-rows[r])<=6.f){ row=(int)r; break; } }
		if (row>2) row=2;
		return c.p1 ? p1ord[row] : p2ord[row];
	};
	out.assign((size_t)nHud * 96, 0);
	uint8_t* base = out.data();
	auto W32 = [&](uint32_t off, uint32_t v){ uint8_t* p=base+off; p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; };
	auto WF  = [&](uint32_t off, float f){ uint32_t u; memcpy(&u,&f,4); W32(off,u); };
	uint32_t o = 0;
	for (uint32_t i = 0; i < nHud; i++) {
		const uint8_t* q = hud + (size_t)i * 96;
		// read the HudQuad fields (LE f32/u32)
		float x[4], y[4], u[4], v[4]; uint32_t col[4], pcw, isp, tsp, tcw;
		memcpy(x, q + 0, 16); memcpy(y, q + 16, 16);
		memcpy(u, q + 32, 16); memcpy(v, q + 48, 16);
		memcpy(col, q + 64, 16);
		memcpy(&pcw, q + 80, 4); memcpy(&isp, q + 84, 4); memcpy(&tsp, q + 88, 4); memcpy(&tcw, q + 92, 4);
		// RESHAPE the HP-fill quad inner edge by live health (state-driven, no drift).
		const Cand* myc = nullptr; for (auto& c:cands) if (c.idx==(int)i){ myc=&c; break; }
		if (myc) {
			int slot = slotForCand(*myc);
			float hpF = (float)rd8(CHAR_BASE[slot] + 0x420) / (float)HP_MAX;
			if (hpF<0.f) hpF=0.f; if (hpF>1.f) hpF=1.f;
			// === DIAGNOSTIC (TEMP): force 25% width regardless of health, to test whether
			// this reshape path is what actually draws the on-screen bars. Revert after. ===
			if (std::getenv("MAPLECAST_HUD_DIAG")) hpF = 0.25f;
			float minx=x[0],maxx=x[0]; for (int k=1;k<4;k++){ minx=std::min(minx,x[k]); maxx=std::max(maxx,x[k]); }
			// Outer end = portrait side: P1 left(minx) anchored, inner=maxx; P2 right(maxx) anchored, inner=minx.
			// Move the INNER x-coords toward the outer anchor so width = full*hpF. Preserve U so the
			// (white-swatch) texture still samples; the bar is a flat swatch so the inner UV is moot.
			if (myc->p1) { float inner = minx + (maxx-minx)*hpF; for (int k=0;k<4;k++) if (x[k] > minx + 0.5f) x[k] = inner; }
			else         { float inner = maxx - (maxx-minx)*hpF; for (int k=0;k<4;k++) if (x[k] < maxx - 0.5f) x[k] = inner; }
		}
		// emit as a paraType-5 textured sprite (A=corner0, B=corner1, C=corner2, D=corner3).
		W32(o+0,pcw); W32(o+4,isp); W32(o+8,tsp); W32(o+12,tcw);
		W32(o+16,col[0]);                       // sprite base color (engine's vtx0 color)
		W32(o+32,0xE0000000u);                  // sprite vtx PCW
		WF(o+36,x[0]); WF(o+40,y[0]); WF(o+44,1.0f);
		WF(o+48,x[1]); WF(o+52,y[1]); WF(o+56,1.0f);
		WF(o+60,x[2]); WF(o+64,y[2]); WF(o+68,1.0f);
		WF(o+72,x[3]); WF(o+76,y[3]);
		{
			uint16_t v0=h16(v[0]),u0=h16(u[0]),v1=h16(v[1]),u1=h16(u[1]),v2=h16(v[2]),u2=h16(u[2]);
			base[o+84]=v0;base[o+85]=v0>>8; base[o+86]=u0;base[o+87]=u0>>8;
			base[o+88]=v1;base[o+89]=v1>>8; base[o+90]=u1;base[o+91]=u1>>8;
			base[o+92]=v2;base[o+93]=v2>>8; base[o+94]=u2;base[o+95]=u2>>8;
		}
		o += 96;
	}
	return out;
}

// Apply one raw FRMx record (decompressed). Overlay dynamic regions + tails into
// _gstaRam/vram/pvr, run render_frame, emit the body TA, decode body textures, and
// hand the result to the render thread. Port of replay.html liveApplyFrame().
// ---- per-phase profiler (MAPLECAST_GSTA_PROF=1) — READ-ONLY, gated OFF by default.
// Accumulates wall-clock µs per phase across the run AND tracks vframe progression
// (the wire game-frame) vs produced frames, so we can distinguish "reconstruction too
// slow -> server drop-old skips" from "genuine wire drop" from "match content". ----
struct GstaProf {
	bool   on = false;
	bool   init = false;
	double splat=0, tails=0, render=0, stage=0, bodyta=0, decode=0, hud=0, total=0;
	uint64_t frames=0;            // produced frames (gstaApplyFrame completions)
	uint64_t vframeFirst=0, vframePrev=0, vframeSpanSum=0, vframeJumps=0;
	uint64_t produceDropped=0;    // produced frames overwritten before render consumed them
	// render-thread consume side (what the user actually SEES):
	std::atomic<uint64_t> rConsumed{0}, rVframePrev{0}, rVframeJumps{0}, rVframeSpan{0};
	std::chrono::steady_clock::time_point wallStart;
};
static GstaProf _gprof;
static inline double _gnow() {
	return std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

// PL3D injection TCW-class blocklist (Phase-A tradeoff, 2026-07-06 tuning round 2).
// Defaults = the GIANT PERSPECTIVE SHEETS (P0 classes 0x50096f00/0x50097200 — the 32kpx
// hail ground/sky planes: LEGITIMATE engine content, but injected verbatim WITHOUT the
// engine's projection/clip context they rasterize as screen-covering orange/yellow
// wedges burying the characters — scratchpad/_livefix_312.png) + 0x1009ce00 (the
// RTT-suspect class whose texture is prefix-zero -> would draw garbage). Phase-B
// transpile will carry the proper projection/clip context and lift this blocklist.
// Env MAPLECAST_P3D_SKIP_TCW=comma-hex overrides the list; SET-BUT-EMPTY disables.
static uint32_t s_p3dSkipTcw[16];
static int      s_p3dSkipTcwN = -1;    // -1 = not parsed yet
static void p3dSkipTcwInit()
{
	if (s_p3dSkipTcwN >= 0) return;
	s_p3dSkipTcwN = 0;
	const char* v = std::getenv("MAPLECAST_P3D_SKIP_TCW");
	if (!v) {                                    // unset -> the default blocklist
		s_p3dSkipTcw[0] = 0x50096F00u;           // perspective sheet A (hail ground/sky)
		s_p3dSkipTcw[1] = 0x50097200u;           // perspective sheet B
		s_p3dSkipTcw[2] = 0x1009CE00u;           // RTT-suspect (prefix-zero texture)
		s_p3dSkipTcwN = 3;
		return;
	}
	const char* p = v;                           // set (possibly empty) -> parse; empty disables
	while (*p && s_p3dSkipTcwN < 16) {
		char* end = nullptr;
		unsigned long b = strtoul(p, &end, 16);
		if (end == p) break;
		s_p3dSkipTcw[s_p3dSkipTcwN++] = (uint32_t)b;
		p = (*end == ',') ? end + 1 : end;
	}
}

// TA chunk-size rules (CONFIRMED core/hw/pvr/ta.cpp TaTypeLut — the injection validator
// and the stream sanity scan derive sizes EXACTLY like flycast's TA FSM):
//   POLY GLOBAL is 64B iff (Volume==0 && Col_Type==2 && Texture && Offset) [Type 2]
//                       or (Volume==1 && Col_Type==2)                      [Type 4]
//                (poly_header_type_size, ta.cpp:386-436; else 32B)
//   POLY VERTEX is 64B iff data-type id in {5,6,11,12,13,14} (ta.cpp:141)
//                == Texture && (Volume || Col_Type==1); else 32B.
//   SPRITE GLOBAL (ParaType 5) is 32B and its vertices are 64B (ta.cpp:163).
// PCW obj_ctrl bits: [0]UV_16bit [1]Gouraud [2]Offset [3]Texture [4:5]Col_Type [6]Volume.
static inline bool p3dPcwHdr64(uint32_t pcw) {
	uint32_t colType = (pcw >> 4) & 3, vol = (pcw >> 6) & 1;
	bool tex = ((pcw >> 3) & 1) != 0, off = ((pcw >> 2) & 1) != 0;
	return (vol == 0 && colType == 2 && tex && off) || (vol == 1 && colType == 2);
}
static inline bool p3dPcwVtx64(uint32_t pcw) {
	uint32_t colType = (pcw >> 4) & 3, vol = (pcw >> 6) & 1;
	bool tex = ((pcw >> 3) & 1) != 0;
	return tex && (vol == 1 || colType == 1);
}

// ---- PHASE 2a NATIVE-TA FIXUP: apply the SAME two client-side texture fixes the
// transpile applies to render_frame's SceneQuad, but to the ENGINE's native TA parcels.
// Walks the 32B SQ-chunk stream, and for each TEXTURED Type-4/5 global header rewrites
// its TCW (byte offset 12) with:
//   (2) PALETTE private-bank repoint: engine palsel in the skin base banks
//       {16,24,32,40,48,56} -> the client's baked private banks {0..5} (fixes the shared-
//       bank time-multiplex "Cable-all-blue"); engine non-base palsel (17/18/25 projectile/
//       effect/hit-flash) is PRESERVED (covered by the per-frame palette tail).
//   (3) BODY PARITY-PIN: on a HIGH-parity frame (arena==400) pin the double-buffered body
//       texaddr word [0x88000,0x8C000) down by 0x6000 so it samples the SAME low half that
//       gstaDecodeBodies decoded into (write==sample invariant; else melty/stale texels).
// Correct stream walk: sprite globals are 32B (verts 64B), poly globals 32/64B per
// p3dPcwHdr64 (verts per p3dPcwVtx64); 64B params advance 2 chunks so 2nd-half data is
// never misread as a PCW. Only textured globals carry a TCW (modvols/flat skipped).
static void gstaNativeTAFixup(std::vector<uint8_t>& ta, const uint8_t* ram)
{
	static const uint32_t CB[6]       = {0x268340,0x2688E4,0x268E88,0x26942C,0x2699D0,0x269F74};
	static const uint32_t BASEBANK[6] = {16,24,32,40,48,56};
	int bakedBank[6];
	for (int s = 0; s < 6; s++) {
		bakedBank[s] = -1;
		uint32_t base = CB[s] & 0x00FFFFFFu;
		if (ram[base] == 0) continue;                           // slot inactive
		uint32_t datpal = gle32(&ram[(base + 0x164) & 0x00FFFFFFu]);
		if (((datpal >> 24) & 0x7Fu) != 0x0Cu) continue;        // not area-3 Dat_Pal
		bakedBank[s] = s;                                        // private bank s baked
	}
	uint32_t arena = gle32(&ram[0x1F9D94]);
	const char* pinE = std::getenv("MAPLECAST_BODY_PARITY_PIN");
	bool doPin = !(pinE && pinE[0] == '0') && (arena == 400);

	size_t n = ta.size(), i = 0; bool curVtx64 = false;
	while (i + 32 <= n) {
		uint32_t pcw = gle32(&ta[i]);
		uint32_t pt  = (pcw >> 29) & 7;
		bool tex     = ((pcw >> 3) & 1) != 0;
		int chunks = 1; bool tcwHere = false;
		switch (pt) {
			case 4: chunks = p3dPcwHdr64(pcw) ? 2 : 1; curVtx64 = p3dPcwVtx64(pcw); tcwHere = tex; break;
			case 5: chunks = 1; curVtx64 = true; tcwHere = tex; break;   // sprite global 32B, verts 64B
			case 7: chunks = curVtx64 ? 2 : 1; break;                    // vertex
			default: chunks = 1; break;                                  // EOL / clip / objlist
		}
		if (tcwHere) {
			uint32_t tcw = gle32(&ta[i + 12]);
			uint32_t palsel = (tcw >> 21) & 0x3Fu;
			for (int s = 0; s < 6; s++)
				if (palsel == BASEBANK[s] && bakedBank[s] >= 0) {
					tcw = (tcw & 0xF81FFFFFu) | ((uint32_t)(bakedBank[s] & 0x3F) << 21);
					break;
				}
			if (doPin) {
				uint32_t taddr = tcw & 0x1FFFFFu;
				if (taddr >= 0x88000u && taddr < 0x8C000u)
					tcw = (tcw & ~0x1FFFFFu) | (taddr - 0x6000u);
			}
			ta[i+12] = tcw & 0xFF; ta[i+13] = (tcw>>8)&0xFF;
			ta[i+14] = (tcw>>16)&0xFF; ta[i+15] = (tcw>>24)&0xFF;
		}
		i += (size_t)chunks * 32;
	}
}

// Walk a TA parcel stream and collect the TexAddr (TCW bits 0-20) of every textured
// SPRITE global (ParaType 5), in emission order. Used to correlate render_frame's body
// sprites with the native char-pass sprites (validated 1:1 same-order by measurement).
static void gstaExtractSpriteAddrs(const std::vector<uint8_t>& ta, std::vector<uint32_t>& out)
{
	out.clear();
	size_t n = ta.size(), i = 0; bool curVtx64 = false;
	while (i + 32 <= n) {
		uint32_t pcw = gle32(&ta[i]);
		uint32_t pt  = (pcw >> 29) & 7; bool tex = ((pcw >> 3) & 1) != 0;
		int chunks = 1;
		switch (pt) {
			case 4: chunks = p3dPcwHdr64(pcw) ? 2 : 1; curVtx64 = p3dPcwVtx64(pcw); break;
			case 5: chunks = 1; curVtx64 = true;
			        if (tex) out.push_back(gle32(&ta[i + 12]) & 0x1FFFFFu); break;
			case 7: chunks = curVtx64 ? 2 : 1; break;
			default: chunks = 1; break;
		}
		i += (size_t)chunks * 32;
	}
}

// FLICKER FIX (concern #3, temporal): rewrite the TexAddr of the native TA's textured
// SPRITE globals (bodies) IN ORDER from newAddr[] — the render_frame scene addresses
// that gstaDecodeBodies actually decodes to. This makes the native bodies SAMPLE exactly
// where the texture was WRITTEN (== the transpile's proven, flicker-free body addressing),
// while the native POLYS (effects) keep their own resident-backed addrs. Returns #rewritten.
static int gstaSyncSpriteAddrs(std::vector<uint8_t>& ta, const std::vector<uint32_t>& newAddr)
{
	size_t n = ta.size(), i = 0, k = 0; bool curVtx64 = false; int changed = 0;
	while (i + 32 <= n && k < newAddr.size()) {
		uint32_t pcw = gle32(&ta[i]);
		uint32_t pt  = (pcw >> 29) & 7; bool tex = ((pcw >> 3) & 1) != 0;
		int chunks = 1;
		switch (pt) {
			case 4: chunks = p3dPcwHdr64(pcw) ? 2 : 1; curVtx64 = p3dPcwVtx64(pcw); break;
			case 5:
				chunks = 1; curVtx64 = true;
				if (tex) {
					uint32_t tcw = gle32(&ta[i + 12]);
					uint32_t want = newAddr[k++] & 0x1FFFFFu;
					if ((tcw & 0x1FFFFFu) != want) {
						tcw = (tcw & ~0x1FFFFFu) | want;
						ta[i+12]=tcw&0xFF; ta[i+13]=(tcw>>8)&0xFF;
						ta[i+14]=(tcw>>16)&0xFF; ta[i+15]=(tcw>>24)&0xFF;
						changed++;
					}
				}
				break;
			case 7: chunks = curVtx64 ? 2 : 1; break;
			default: chunks = 1; break;
		}
		i += (size_t)chunks * 32;
	}
	return changed;
}

static void gstaApplyFrame(const uint8_t* d, size_t n)
{
	if (!_gstaSeeded.load(std::memory_order_acquire)) return;
	if (n < 12 || gle32(d) != GSTA_FRMX_MAGIC) { printf("[GSTA] dropped: bad FRMx magic\n"); return; }

	if (!_gprof.init) {
		_gprof.init = true;
		const char* pe = std::getenv("MAPLECAST_GSTA_PROF");
		_gprof.on = (pe && *pe && *pe != '0');
		_gprof.wallStart = std::chrono::steady_clock::now();
		if (_gprof.on) printf("[GPROF] per-phase profiler ON\n");
	}
	const bool prof = _gprof.on;
	double t0 = prof ? _gnow() : 0.0;
	double tApply0 = t0;

	size_t p = 4;
	uint32_t vframe = gle32(d + p); p += 4;
	uint32_t taSize = gle32(d + p); p += 4;     // 0 in the live stream

	// ---- dynamic regions in table order: splat each into _gstaRam at addr&0xFFFFFF ----
	for (auto& r : _gstaDynRegs) {
		if (p + r.len > n) { printf("[GSTA] frame truncated in dyn region '%s'\n", r.tag); return; }
		if (strcmp(r.tag, "bodytex") != 0) {     // legacy texture band (ignored)
			uint32_t off = r.addr & 0x00FFFFFFu;
			if ((size_t)off + r.len <= _gstaRam.size())
				memcpy(&_gstaRam[off], d + p, r.len);
		}
		p += r.len;
	}

	if (prof) { double t=_gnow(); _gprof.splat += t-t0; t0=t; }

	bool palDirty = false;

	// ---- VARIABLE GFX TAIL: u32 nGfx, then nGfx x { u32 base, u32 len, len bytes } ----
	if (p + 4 <= n) {
		uint32_t nGfx = gle32(d + p);
		if (nGfx <= 64) {
			p += 4;
			for (uint32_t i = 0; i < nGfx && p + 8 <= n; i++) {
				uint32_t base = gle32(d + p); p += 4;
				uint32_t len  = gle32(d + p); p += 4;
				if (len > 0x800000 || p + len > n) break;
				uint32_t off = base & 0x00FFFFFFu;
				if ((size_t)off + len <= _gstaRam.size())
					memcpy(&_gstaRam[off], d + p, len);
				p += len;
			}
		}
	}

	// ---- PALETTE TAIL: u32 pvrPalLen, then pvrPalLen bytes of fresh pvr_regs ----
	if (p + 4 <= n) {
		uint32_t palLen = gle32(d + p); p += 4;
		if (palLen && palLen <= (uint32_t)pvr_RegSize && p + palLen <= n) {
			memcpy(pvr_regs, d + p, palLen);
			palDirty = true;
			p += palLen;
		}
	}

	// ---- HUDQ TAIL: u32 magic, u32 nHud, nHud x 96-byte HudQuad ----
	std::vector<uint8_t> hudTa;
	if (p + 8 <= n && gle32(d + p) == GSTA_HUDQ_MAGIC) {
		p += 4;
		uint32_t nHud = gle32(d + p); p += 4;
		if (nHud <= 4096 && p + (size_t)nHud * 96 <= n) {
			// HudQuads are already-final PVR quads (4 corners + UV + col + PVR words). The wire
			// HudQuad layout: f32 x[4],y[4]; f32 u[4],v[4]; u32 col[4]; u32 pcw,isp,tsp,tcw.
			// Re-emit each as a paraType-5 sprite-TA block (the M4 HUD path). LIVE: hudqOn gate.
			if (::gsta_render_debug::g().hudqOn.load(std::memory_order_relaxed))
				hudTa = gstaBuildHudTA(d + p, nHud);
			p += (size_t)nHud * 96;
		}
	}

	// ---- BTCW TAIL: u32 magic "BTCW", u32 nWords, nWords u32 (per body [node][ntiles][tcw...]).
	// The engine's RESOLVED per-tile body tcws (parity-flip fix). Hand them to render_frame, which
	// uses them verbatim for body tiles instead of the parity-sensitive rectab[idxtab[alloc]] lookup. ----
	render_frame_set_body_tcws(nullptr, 0);          // clear last frame's (default: no override)
	if (p + 8 <= n && gle32(d + p) == GSTA_BTCW_MAGIC) {
		p += 4;
		uint32_t nWords = gle32(d + p); p += 4;
		// Sanity bound must match the oracle capture caps (maplecast_oracle_hook.h MC_BTCW_MAX_*).
		// EXTENDED 2026-07-04: the BTCW tail now carries satellite/effect nodes too, not just the 6
		// bodies, so the old 6*(2+64)=396 bound would REJECT the whole tail (dropping bodies too) the
		// moment a super fires. Raised in lockstep with the server.
		if (nWords <= (size_t)(MC_BTCW_MAX_NODES * (2 + MC_BTCW_MAX_TILES)) && p + (size_t)nWords * 4 <= n) {
			render_frame_set_body_tcws((const uint32_t*)(d + p), (int)nWords);
			p += (size_t)nWords * 4;
		}
	}

	// ---- PL3D TAIL: u32 magic "PL3D", u32 nBytes, then nBytes of 36-byte flush records
	// { u8 kind (0=param line1, 1=param line2/face-colors, 2=vertex), u8 slot, u8 cls
	// (0x10 = TA-direct slot0 / 0xAC = deferred P2 buffer), u8 pad, 32B SQ line } — the
	// 3D-machine (bank12 loc_8c129cc0 POL drawer: impact sparks / cast flashes) TA parcels
	// captured VERBATIM at the engine's own SQ flushes (re_kb/64 finding:3d_draw_emit_map).
	// Injected into fr.ta below (Phase A scaffold). Bound must match MC_P3D_* (oracle_hook.h). ----
	static std::vector<uint8_t> _gstaP3d;
	_gstaP3d.clear();
	if (p + 8 <= n && gle32(d + p) == GSTA_PL3D_MAGIC) {
		p += 4;
		uint32_t nb = gle32(d + p); p += 4;
		if (nb <= (uint32_t)(MC_P3D_MAX_LINES * MC_P3D_LINE_BYTES)
		    && (nb % MC_P3D_LINE_BYTES) == 0 && p + nb <= n) {
			_gstaP3d.assign(d + p, d + p + nb);
			p += nb;
		}
	}

	if (prof) { double t=_gnow(); _gprof.tails += t-t0; t0=t; }

	// ---- render the bodies from the seeded RAM (byte-exact transpiled path) ----
	memset(&_gstaCtx, 0, sizeof(_gstaCtx));
	_gstaCtx.ram = _gstaRam.data();
	render_frame(&_gstaCtx);
	int nQuad = render_frame_nscene();

	// ---- PALETTE FIX (Cable-all-blue root cause; verified 402/402 + Storm 68/0 in replay of
	// gsta_rec.bin). MVC2 body sprites are paletted and the ENGINE latches the palette PER-DRAW,
	// time-multiplexing the shared PVR PALETTE_RAM bank (e.g. Cable=bank24). The serverPublish()
	// snapshot catches whatever was LAST written, so ~44% of frames snapshot bank24 mid-BLUE (an
	// effect/aura palette), and the body then samples blue. The STABLE, per-object truth is the
	// resident Dat_Pal @char+0x164 (a 16-color ARGB4444 window that never corrupts — MEASURED
	// 0x0C90BA00 valid + purple in all 913 frames). FIX: bake each active body slot's Dat_Pal into
	// a PRIVATE, otherwise-unused PVR palette bank (banks 0..5 — banks 16..63 hold the shared/effect
	// palettes; 0..15 are empty across the whole recording) and repoint that slot's body quads'
	// TCW.PalSelect to it. Pure client change; Dat_Pal already ships in the prefix's full 16MB RAM.
	// (finalize_body's bank-25 preserve is the WRONG diagnosis — bank25 is a constant, never Cable.)
	{
		static const uint32_t GFIX_CHAR_BASE[6] =
			{ 0x268340,0x2688E4,0x268E88,0x26942C,0x2699D0,0x269F74 };
		// PALETTE_RAM lives at pvr_regs offset 0x1000 (u32/entry). Private bank b -> entries b*16..b*16+15.
		uint32_t* palRam = (uint32_t*)(pvr_regs + 0x1000);
		uint32_t gfxOfSlot[6]; int privBankOfSlot[6];
		for (int s = 0; s < 6; s++) { gfxOfSlot[s] = 0; privBankOfSlot[s] = -1; }
		for (int s = 0; s < 6; s++) {
			uint32_t base = GFIX_CHAR_BASE[s];
			if (_gstaRam[base & 0x00FFFFFFu] == 0) continue;   // slot inactive (+0x000 active)
			uint32_t gfx1 = gle32(&_gstaRam[(base + 0x15C) & 0x00FFFFFFu]);   // node+0x15C GFX1 base
			uint32_t datpal = gle32(&_gstaRam[(base + 0x164) & 0x00FFFFFFu]); // node+0x164 Dat_Pal ptr
			if (((datpal >> 24) & 0x7Fu) != 0x0Cu) continue;   // not an area-3 pointer -> skip
			int b = s;                                          // private bank = slot index (0..5)
			const uint8_t* dp = &_gstaRam[datpal & 0x00FFFFFFu];// 16 ARGB4444 LE entries
			for (int i = 0; i < 16; i++) {
				uint16_t argb4 = (uint16_t)(dp[i*2] | (dp[i*2+1] << 8));
				// PVR PALETTE_RAM holds the raw palette word in the PAL_RAM_CTRL format; MVC2 body
				// palettes are ARGB4444, matching the recording's PAL_RAM_CTRL=2. Store verbatim.
				palRam[b*16 + i] = (uint32_t)argb4;
			}
			gfxOfSlot[s] = gfx1; privBankOfSlot[s] = b;
			palDirty = true;
		}
		// Repoint each body quad's TCW.PalSelect (bits 21..26) to its owning slot's private bank.
		// The scene is a static array in render_frame.c; safe to mutate in-place for this frame.
		GstaSceneQuad* Sq = const_cast<GstaSceneQuad*>(render_frame_scene());
		// DISCRIMINATOR (CONFIRMED-BY-MEASUREMENT 2026-07-05, _live3 enumeration): repoint a quad
		// ONLY when its engine-resolved palsel == the owning slot's own BASE shared bank
		// (skin formula: bank = 16*(pair+1) + 8*side -> slots 0..5 = {16,24,32,40,48,56}).
		// The repoint exists solely to protect the base bank from time-multiplex corruption
		// (Cable-all-blue). Any quad the engine deliberately points elsewhere — pal17 projectiles
		// (353 enumerated), pal18 non-bit15 streaks (53), pal25 hit-flash (4, emitted at exact
		// engine positions but painted in the victim's base palette = INVISIBLE) — must keep its
		// engine palsel and be colored by the per-frame palette tail (ships byte-exact).
		// A gfx1-only match swept ALL of these into banks 0/1: wrong projectile colors + invisible
		// hit-flash. The earlier bit15/is_effect exclusion was necessary but too narrow (non-bit15
		// satellites/hit-flash still swept); kept as belt-and-suspenders.
		static const uint32_t GFIX_BASE_BANK[6] = { 16, 24, 32, 40, 48, 56 };
		static std::vector<uint8_t> _isEff; _isEff.assign((size_t)nQuad, 0);
		gsta_quad_is_effect(_isEff.data(), (unsigned)nQuad);
		for (int q = 0; q < nQuad; q++) {
			if (q < (int)_isEff.size() && _isEff[q]) continue;   // bit15 effect quad: keep engine palsel
			uint32_t curPal = (Sq[q].tcw >> 21) & 0x3Fu;
			for (int s = 0; s < 6; s++) {
				if (privBankOfSlot[s] < 0) continue;
				if (Sq[q].gfx1 != gfxOfSlot[s]) continue;
				if (curPal != GFIX_BASE_BANK[s]) break;          // engine non-base (17/18/25...): PRESERVE
				Sq[q].tcw = (Sq[q].tcw & 0xF81FFFFFu)
				          | ((uint32_t)(privBankOfSlot[s] & 0x3F) << 21);
				break;
			}
		}
	}

	// ---- TEXCACHE-STABILITY FIX: body sprite "bounce" (2026-07-03, CONFIRMED-BY-MEASUREMENT).
	// The engine DOUBLE-BUFFERS each body's decoded texture into TWO VRAM parity halves that are
	// exactly 0x30000 bytes apart, selected per-frame by the arena parity *(0x8C1F9D94) — MEASURED
	// 16 => LOW half (texaddr 0x41xxxx) and 400 => HIGH half (texaddr 0x44xxxx). The body quad's
	// TCW texaddr therefore ALTERNATES 0x41xxxx<->0x44xxxx every frame. Content at both halves is
	// byte-identical (expert VRAM check + parity_probe.mjs: delta is a constant 0x30000 for BOTH
	// bodies AND their satellites across every frame), so the flip is HARMLESS to the engine's
	// render — but on the CLIENT the pvr2 texture cache keys on texaddr, so the per-frame
	// alternation thrashes two cache entries and re-uploads out of step with the render thread =
	// the visible sprite "bounce" (offline reconstruction is PROVEN bounce-free: sel+col/row 0/56
	// stable across the flip, write-dest == sample-src). FIX: PIN every body quad to the LOW
	// (arena==16) parity. On a HIGH-parity frame subtract 0x30000 bytes (= 0x6000 in the TCW
	// texaddr WORD field) so the quad samples ONE stable address every frame -> the texcache sees a
	// single entry per tile -> no thrash. gstaDecodeBodies reads this SAME mutated scene, so the
	// tile WRITE lands at the identical canonical address (write==sample invariant preserved). The
	// low half is always resident (shipped in the prefix + it is exactly what the correct arena==16
	// frames already render). Lossless (identical bytes). Escape hatch: MAPLECAST_BODY_PARITY_PIN=0.
	{
		const char* pinE = std::getenv("MAPLECAST_BODY_PARITY_PIN");
		bool pin = !(pinE && pinE[0]=='0');
		uint32_t arena = gle32(&_gstaRam[0x1F9D94]);
		if (pin && arena == 400) {
			GstaSceneQuad* Sq2 = const_cast<GstaSceneQuad*>(render_frame_scene());
			for (int q = 0; q < nQuad; q++) {
				uint32_t g = Sq2[q].gfx1;
				bool isBody = ((g & 0x0C000000u) || (g & 0x8C000000u))
				              && !(g >= 0x0CED0000u && g < 0x0CEE0000u);
				if (!isBody) continue;
				uint32_t ta = Sq2[q].tcw & 0x1FFFFFu;
				// PIN SCOPE FIX (2026-07-05, _live4 byte-gate): only the DOUBLE-BUFFERED body
				// arena alternates parity halves (byteaddr 0x410000-0x42FFFF <-> 0x440000-
				// 0x45FFFF, delta 0x30000). The old blanket `ta >= 0x6000` ALSO shifted the
				// bit15 effect tiles' tcws (0x475xxx / 0x60xxxx / 0x400xxx bands — MEASURED:
				// the engine uses those SAME addrs at BOTH arena parities, m1330 arena==16 vs
				// m1345 arena==400, i.e. NOT double-buffered) => they sampled addr-0x30000
				// (stale/foreign texels = the restored-tile W12/Z50) AND their re-pinned
				// STAGING clobbered innocent resident regions (prefix content at 0x400600 was
				// byte-exact vs engine until a mis-pinned write). Scope: HIGH arena half only,
				// byteaddr in [0x440000,0x460000) == ta word in [0x88000,0x8C000).
				if (ta >= 0x88000u && ta < 0x8C000u)
					Sq2[q].tcw = (Sq2[q].tcw & ~0x1FFFFFu) | (ta - 0x6000u);
			}
		}
	}

	if (prof) { double t=_gnow(); _gprof.render += t-t0; t0=t; }

	// ---- emit the body sprite-TA (+ append HUD) ----  [fr declared early so the body
	// texture decode can STAGE its tiles into fr.tiles, applied on the render thread] ----
	GstaFrame fr;

	// ---- PHASE 3 STAGE (background): emit the OPAQUE polygon-TA FIRST so the FSM opens
	// the OP list before the translucent body sprites (flycast draws OP -> PT -> TR, so the
	// stage lands BEHIND the bodies). stage_id @0x8C289638, camera M2@0x8C2D6AD8 /
	// M1@0x8C2D6B18 read from the seeded RAM image (both ship every frame: gstate + cam_mat).
	// Stage geometry/control-words come from the engine-TA bake STGxx_ta.json; the textures
	// are already resident in vram[] at each mesh TCW (the GSTA prefix shipped full VRAM).
	// NOTE: the stage is INNOCENT of the cape grey-blocks — that was the non-square carve
	// (Y-first twiddle, same commit); confirmed via multi-frame capture stage on-vs-off.
	{
		auto ru32 = [&](uint32_t a)->uint32_t { a &= 0x00FFFFFFu;
			const uint8_t* r=_gstaRam.data();
			return (uint32_t)r[a]|((uint32_t)r[a+1]<<8)|((uint32_t)r[a+2]<<16)|((uint32_t)r[a+3]<<24); };
		uint32_t stageId = _gstaRam[0x289638 & 0x00FFFFFF];   // u8 stage_id (page-649)
		float M2[16], M1[16];
		for (int i=0;i<16;i++){ uint32_t u=ru32(0x2D6AD8 + i*4); memcpy(&M2[i],&u,4); }
		for (int i=0;i<16;i++){ uint32_t u=ru32(0x2D6B18 + i*4); memcpy(&M1[i],&u,4); }
		::gstaStageEnsureLoaded(stageId);
		if (::gstaStageReady()) {
			std::vector<uint8_t> stageTa;
			size_t sLen = ::gstaStageEmitTA(stageTa, M1, M2, _gstaRam.data());
			if (sLen) {
				fr.ta.insert(fr.ta.end(), stageTa.begin(), stageTa.end());
				// close the OPAQUE list before the body TR sprites open (FSM: EOL resets
				// ta_fsm_cl to 7 so the next param's ListType re-opens a list).
				std::vector<uint8_t> eol(32, 0); fr.ta.insert(fr.ta.end(), eol.begin(), eol.end());
			}
		}
	}

	if (prof) { double t=_gnow(); _gprof.stage += t-t0; t0=t; }

	size_t stagePrefixLen = fr.ta.size();      // bytes occupied by the stage OP list (+EOL)

	// ---- PHASE 2a: NATIVE CHAR-PASS (MAPLECAST_GSTA_NATIVE_CHARPASS=1) ----
	// Replace the hand-assembled body sprite-TA (+P3D injection) with the ENGINE's own
	// char-pass render driver run in-process (gsta_charpass) on this frame's _gstaRam.
	// The captured SQ parcels ARE the bodies + effects, correctly list-ordered (engine
	// output) — spliced as its own segment after the stage OP list. Byte-exact vs the
	// engine (md5 be1377d2..., proven by the standalone runner + in-process selftest).
	// render_frame() above still ran (its SceneQuad drives palette-fix + gstaDecodeBodies
	// texture decode). Gated OFF by default; transpile stays the fallback.
	size_t bodyLen;
	static int s_nativeCharpass = -1;
	if (s_nativeCharpass < 0) {
		const char* e = std::getenv("MAPLECAST_GSTA_NATIVE_CHARPASS");
		s_nativeCharpass = (e && *e && *e != '0') ? 1 : 0;
	}
	bool nativeDone = false;
	if (s_nativeCharpass) {
		static std::vector<uint8_t> _nta; _nta.clear(); double _cpMs = 0;
		if (::gsta_charpass::run_live(_gstaRam.data(), _nta, &_cpMs) && !_nta.empty()) {
			// concerns #2 (palette private-bank repoint) + #3 (body parity-pin), applied
			// to the native engine TCWs exactly as the transpile does to render_frame's scene.
			gstaNativeTAFixup(_nta, _gstaRam.data());

			// ---- FLICKER FIX v2 — DECODE-FOLLOWS-NATIVE (concern #3, temporal).
			// The body texture DECODE (gstaDecodeBodies below) writes render_frame's
			// byte-faithful CONTENT (gfx1/sel/col/row from the runtime tile descriptor
			// 0x8C1F9F9C — re_kb/22, correct) but render_frame RESOLVES the dest ADDRESS
			// itself, which DRIFTS/COLLIDES vs the engine on some frames (MEASURED f150:
			// 7/89 sprites differ, incl a duplicate addr -> two tiles collide at one addr,
			// one overwrites the other = the gray/washed body on alternating frames). The
			// native char-pass render RESOLVED each tile's address AUTHORITATIVELY (its TCW
			// IS the engine's own output). FIX: override each SceneQuad's decode TexAddr with
			// the native char-pass addr (lockstep, 1:1 emit order), so gstaDecodeBodies
			// decodes the correct content to the AUTHORITATIVE, DISTINCT addresses the native
			// TA samples -> decode==sample every frame, no collision, no drift. The native TA
			// keeps its (parity-pinned) addrs; content stays render_frame's (byte-faithful).
			{
				static std::vector<uint8_t> _rfta; _rfta.clear();
				gstaEmitSpriteTA_append(_rfta);            // render_frame body TA (scene emit order)
				static std::vector<uint32_t> _rfA, _ntA;
				gstaExtractSpriteAddrs(_rfta, _rfA);       // render_frame (drifty) decode addrs
				gstaExtractSpriteAddrs(_nta,  _ntA);       // native authoritative sample addrs
				size_t mm = std::min(_rfA.size(), _ntA.size());
				GstaSceneQuad* Sq = const_cast<GstaSceneQuad*>(render_frame_scene());
				size_t k = 0; int _remap = 0;
				for (int q = 0; q < nQuad && k < mm; q++) {
					uint32_t a = Sq[q].tcw & 0x1FFFFFu;
					if (a != _rfA[k]) continue;            // not the next emitted body sprite
					if (_ntA[k] != a) {                    // redirect the decode to the native addr
						Sq[q].tcw = (Sq[q].tcw & ~0x1FFFFFu) | (_ntA[k] & 0x1FFFFFu);
						_remap++;
					}
					k++;
				}
				static int _rn = 0;
				if ((_rn++ % 120) == 0)
					printf("[charpass] decode-follows-native: %d/%zu body decode addrs redirected "
					       "to authoritative native addrs\n", _remap, mm);
			}

			fr.ta.insert(fr.ta.end(), _nta.begin(), _nta.end());
			std::vector<uint8_t> eol(32, 0); fr.ta.insert(fr.ta.end(), eol.begin(), eol.end());
			bodyLen = fr.ta.size();
			nativeDone = true;
			if (prof) { double t=_gnow(); (void)t; }
			static int _pn = 0;
			if ((_pn++ % 120) == 0)
				printf("[charpass] NATIVE TA %zu parcels %.2fms (byte-exact vs engine)\n",
				       _nta.size()/32, _cpMs);
		} else {
			static bool _warned = false;
			if (!_warned) { _warned = true;
				printf("[charpass] NATIVE requested but run_live unavailable "
				       "(set MAPLECAST_GSTA_CHARPASS_SEED=<RTSEED02>) — using transpile\n"); }
		}
	}
	if (!nativeDone)
		bodyLen = stagePrefixLen + gstaEmitSpriteTA_append(fr.ta);   // transpile fallback

	// LIVE render-debug (control-WS): bodyOn=0 (or SOLO stage/hud) strips the body/scene TA
	// (render_frame output), leaving stage+HUD only — isolate HUD/stage from bodies, no rebuild.
	{
		auto& _D = ::gsta_render_debug::g();
		int _solo = _D.soloMode.load(std::memory_order_relaxed);   // 1 stage, 2 body, 3 hud
		bool _hideBody = (_D.bodyOn.load(std::memory_order_relaxed) == 0) || _solo == 1 || _solo == 3;
		if (_hideBody) { fr.ta.resize(stagePrefixLen); bodyLen = stagePrefixLen; }
	}

	// DIAGNOSTIC: HUD_DIAG>=3 strips the body/scene TA (render_frame output) too, leaving ONLY
	// the stage. If the bars STILL persist with =3, they are in neither hudTa nor render_frame.
	{ const char* dg=std::getenv("MAPLECAST_HUD_DIAG");
	  if (dg && atoi(dg)>=3) { fr.ta.resize(stagePrefixLen); bodyLen=stagePrefixLen; }
	  if (dg) { static int _dn=0; if ((_dn++ % 60)==0)
	    printf("[HUDDIAG=%s] bodyQuads=%d hudTaBytes=%zu fr.ta=%zu stage=%zu\n",
	           dg, nQuad, hudTa.size(), fr.ta.size(), stagePrefixLen); fflush(stdout); }
	}

	if (prof) { double t=_gnow(); _gprof.bodyta += t-t0; t0=t; }

	// ---- M3: decode body textures, STAGED into fr.tiles (applied on render thread) ----
	int written = gstaDecodeBodies(nQuad, fr.tiles);

	if (prof) { double t=_gnow(); _gprof.decode += t-t0; t0=t; }

	// ---- DIAG (MAPLECAST_DUMP_GSTA_VRAM=<dir>): one-shot dump of the body texture
	// VRAM band [0x80000..0xE0000] + a per-quad manifest (sel,pal,tcw_addr,Ax,Ay,
	// col,row,mirror) so we can byte-diff GSTA-decoded VRAM vs the real client VRAM
	// at each Cable wide-part TCW. Read-only diagnostic, no behavior change. ----
	{
		static int _vdumpN = 0;
		const char* vd = std::getenv("MAPLECAST_DUMP_GSTA_VRAM");
		if (vd && *vd && _vdumpN < 4000 && nQuad > 0) {
			_vdumpN++;
			// apply staged tiles into vram[] so the diagnostic reflects this frame's decode
			for (const auto& tw : fr.tiles) if (tw.vaddr + 512 <= VRAM_SIZE) memcpy(&vram[tw.vaddr], tw.bytes, 512);
			char vpath[512]; snprintf(vpath, sizeof(vpath), "%s/gsta_vram_%u.bin", vd, vframe);
			FILE* vf2 = fopen(vpath, "wb");
			if (vf2) { fwrite(&vram[0x400000], 1, 0x80000, vf2); fclose(vf2); }
			// EFFECT-BAND texels (0x600000..0x620000: bit15 super bolts/hail) — was a validation
			// blind spot (the texel gate could not score the LSTORM/HAIL effect-band quads).
			snprintf(vpath, sizeof(vpath), "%s/gsta_vram6_%u.bin", vd, vframe);
			FILE* vf6 = fopen(vpath, "wb");
			if (vf6) { fwrite(&vram[0x600000], 1, 0x20000, vf6); fclose(vf6); }
			// EFFECTIVE palette (post PAL_RAM_CTRL convert) — the exact ARGB flycast samples
			// for PAL4 index lookups. Diff vs the engine's palette to find index-0 / bank defects.
			pal_needs_update = true; palette_update();
			char ppath[512]; snprintf(ppath, sizeof(ppath), "%s/gsta_pal_%u.bin", vd, vframe);
			FILE* pf2 = fopen(ppath, "wb");
			if (pf2) { fwrite(palette32_ram, 4, 1024, pf2); fclose(pf2); }
			char mpath[512]; snprintf(mpath, sizeof(mpath), "%s/gsta_manifest_%u.txt", vd, vframe);
			FILE* mf = fopen(mpath, "w");
			if (mf) {
				static std::vector<int> _crd; _crd.assign((size_t)nQuad*2,0);
				gsta_quad_colrow(_crd.data(),(unsigned)nQuad);
				static std::vector<uint8_t> _mrd; _mrd.assign((size_t)nQuad,0);
				gsta_quad_mirror(_mrd.data(),(unsigned)nQuad);
				const GstaSceneQuad* SS = render_frame_scene();
				// PixelFmt names — enum PixelFormat @ core/hw/pvr/ta_structs.h:706-715
				static const char* _pfName[8] = {"1555","565","4444","yuv","bump","pal4","pal8","rsvd"};
				for (int q=0;q<nQuad;q++){
					const GstaSceneQuad& Q = SS[q];
					uint32_t tcw = Q.tcw, tsp = Q.tsp;
					// --- TCW decode (union TCW @ core/hw/pvr/ta_structs.h:108-126) ---
					uint32_t pal      =(tcw>>21)&0x3F;       // PalSelect  : bits 21-26
					uint32_t addr     =(tcw&0x1FFFFF)<<3;    // TexAddr    : bits 0-20 (*8 = byte addr)
					uint32_t strideSel=(tcw>>25)&0x1;        // StrideSel  : bit 25
					uint32_t scanOrder=(tcw>>26)&0x1;        // ScanOrder  : bit 26 (0 = twiddled)
					uint32_t pixFmt   =(tcw>>27)&0x7;        // PixelFmt   : bits 27-29
					uint32_t vq       =(tcw>>30)&0x1;        // VQ_Comp    : bit 30
					uint32_t mip      =(tcw>>31)&0x1;        // MipMapped  : bit 31
					// --- TSP decode (union TSP @ core/hw/pvr/ta_structs.h:77-101) ---
					uint32_t texV     =(tsp>>0)&0x7;         // TexV       : bits 0-2
					uint32_t texU     =(tsp>>3)&0x7;         // TexU       : bits 3-5
					uint32_t usizeU   =8u<<texU;             // width  = 8<<TexU (TexCache.h:237)
					uint32_t vsizeV   =8u<<texV;             // height = 8<<TexV (TexCache.h:238)
					uint32_t filter   =(tsp>>13)&0x3;        // FilterMode : bits 13-14
					uint32_t clampV   =(tsp>>15)&0x1;        // ClampV     : bit 15
					uint32_t clampU   =(tsp>>16)&0x1;        // ClampU     : bit 16
					uint32_t flipV    =(tsp>>17)&0x1;        // FlipV      : bit 17
					uint32_t flipU    =(tsp>>18)&0x1;        // FlipU      : bit 18
					uint32_t srcInstr =(tsp>>29)&0x7;        // SrcInstr   : bits 29-31
					uint32_t dstInstr =(tsp>>26)&0x7;        // DstInstr   : bits 26-28
					// UVs are NOT stored on the quad; synthesized in gstaEmitSpriteTA_append
					// (~L3722-3730) from Q.u1 (== U max == V max) + Q.mirror. Dump the raw carry.
					fprintf(mf,
						"q%d sel=%u gfx1=%x pal=%u tcw_addr=%x col=%d row=%d mir=%d"
						" tcw=%08x tsp=%08x pcw=%08x isp=%08x recidx=%u"
						" pixfmt=%s scanorder=%u twiddled=%u vq=%u mip=%u stride=%u"
						" usize=%u vsize=%u texU=%u texV=%u filter=%u clampU=%u clampV=%u flipU=%u flipV=%u"
						" srcinstr=%u dstinstr=%u"
						" Ax=%.2f Ay=%.2f Bx=%.2f By=%.2f Cx=%.2f Cy=%.2f Dx=%.2f Dy=%.2f"
						" u1=%.5f v1=%.5f z=%.6f facing=%u\n",
						q, Q.sel, Q.gfx1, pal, addr, _crd[2*q], _crd[2*q+1], _mrd[q],
						tcw, tsp, Q.pcw, Q.isp, Q.recidx,
						_pfName[pixFmt], scanOrder, (scanOrder==0?1u:0u), vq, mip, strideSel,
						usizeU, vsizeV, texU, texV, filter, clampU, clampV, flipU, flipV,
						srcInstr, dstInstr,
						Q.Ax, Q.Ay, Q.Bx, Q.By, Q.Cx, Q.Cy, Q.Dx, Q.Dy,
						Q.u1, Q.v1, Q.z, Q.facing);
				}
				fclose(mf);
			}
		}
	}

	// ---- append HUD to the already-emitted body sprite-TA ----
	if (!hudTa.empty()) {
		// strip the body EOL (last 32B) then append HUD quads + a single EOL.
		if (bodyLen >= 32) fr.ta.resize(bodyLen - 32);
		fr.ta.insert(fr.ta.end(), hudTa.begin(), hudTa.end());
		std::vector<uint8_t> eol(32, 0);
		fr.ta.insert(fr.ta.end(), eol.begin(), eol.end());
	}

	// ---- PL3D INJECTION (Phase A, re_kb/64): append the 3D-machine parcels VERBATIM into
	// the open TR list before its EndOfList — they are already TA-format screen-space polys
	// composed by the engine's own drawer; textures are static POL-embedded TCWs resolving
	// from resident VRAM (P0 verdict, finding:3d_texture_binding — no palettes). Placed
	// AFTER the HUD append: the HUD block resizes to the ABSOLUTE bodyLen-32, which would
	// truncate anything appended before it. The CLASS FILTER now lives at CAPTURE
	// (maplecast_oracle_hook.cpp p3dCapture, default cls 0x10 = TA-direct only, widen via
	// MAPLECAST_P3D_CLS on the HEADLESS) — whatever classes the wire ships are injected
	// here, counted separately (slot0 vs deferred). Env MAPLECAST_GSTA_POLY3D=0 skips
	// injection (parse/dump still run). ----
	size_t p3dSlot0 = 0, p3dDefer = 0, p3dOrphan = 0, p3dPolys = 0, p3dTcwSkip = 0, p3dBboxSkip = 0;
	size_t p3dFormSkip = 0, p3dK1Drop = 0, p3dNoK1Skip = 0, p3dRuleDiv = 0;
	if (!_gstaP3d.empty()) {
		const char* pe3 = std::getenv("MAPLECAST_GSTA_POLY3D");
		if (!(pe3 && pe3[0] == '0')) {
			p3dSkipTcwInit();
			const size_t nRec = _gstaP3d.size() / MC_P3D_LINE_BYTES;
			// LIST-ORDER FIX (2026-07-07, ROOT-CAUSED via mirror-native comparison + frozen live
			// gate @ vf53900): the injected 3D-machine/HUD parcels carry PCW.ListType=0 (OPAQUE)
			// and the ENGINE emits them IN THE OPAQUE LIST (mirror _live11 @vf53900: pcw
			// 808c002e/3c/3e tcw 0809be00 all list-0, byte-identical strip structure to ours).
			// The DC TA is list-ORDER-strict (OP -> OP-mod -> TR -> TR-mod -> PT, each ONCE). The
			// old code appended these AFTER the body/HUD TR list. Two prior attempts failed:
			//   (a) append into the open TR list (strip TR EOL) -> flycast's ta_fsm_cl binds the
			//       OPAQUE-intended parcels into TR (ta_vtx.cpp ta_main:476 only startList()s when
			//       CurrentList==None; mid-list globals inherit the open list, IGNORING PCW.ListType)
			//       -> per-triangle autosorted/alpha-blended (gldraw.cpp drawSorted) -> mega-strip.
			//   (b) keep the TR EOL so a 2nd OP list opens after TR -> flycast's ta_poly_data vertex
			//       loop (ta_vtx.cpp:381-388) is still mid-strip at the boundary and SWALLOWS that
			//       EOL as a vertex (release verify() is a no-op) -> endList never runs, CurrentList
			//       stays TR -> injected globals swallowed into the TR mega-strip (LIVE-MEASURED:
			//       MAPLECAST_DUMP_TR_EXTENTS ON showed TR count=185 span=743 despite the static EOL).
			// FIX: emit the injected OPAQUE parcels INTO THE STAGE OP PREFIX, before its EOL, so
			// they are in-order within the single OP list and flycast bins them OP per their PCW.
			// Build into p3dOut, then insert at (stagePrefixLen-32) = just before the stage OP EOL.
			// (Validated offline: render_ta.mjs renders HUD/portraits/meters clean in OPAQUE, floor
			// blue+behind, ZERO olive wedge; _listframe shows ONE OP list + ONE TR list, DC-legal.)
			std::vector<uint8_t> p3dOut;
			p3dOut.reserve(nRec * 32);
			// PARCEL-GRANULAR walk. Per parcel (a kind-0 param + its kind-1/2 lines), BEFORE
			// appending:
			//   1. ORPHAN GUARD (round 1): lines before any param are skipped — a capture gap
			//      degrades to absence, not the 800-vert giant-poly garbage.
			//   2. KIND-1 BY THE STRICT ColType RULE (iteration 4c FINAL — raw mirror stream
			//      m1680 +0x198A0, per-variant table exact 31/31 + 0/33; SUPERSEDES both the
			//      iter-3 ta.cpp-rule validator AND the first iter-4 "kind-1 never injected"
			//      spec, whose 'hook artifact' verdict was the differ's own walker bug):
			//      PVR spec — ColType==2 ((PCW>>4)&3) => 64-byte global, the face-color line
			//      IS in-stream at +32 (PCWs 0x808c002c/2d/2e: 31/31 exact); ColType==3 =>
			//      32-byte global, NO second line (0x808c003c/3e: 0/33). The capture is
			//      byte-faithful INCLUDING kind-1 (64/64 byte-exact at the calm frame).
			//      EITHER wrong polarity wedges one family: drop kind-1 on ColType-2 and the
			//      TA eats v0's coords as intensity colors (saturated bar-yellow, one-line
			//      shift); expect it on ColType-3 and the real v0 is eaten as colors — same
			//      wedge. POLICY: inject kind-1 IFF ColType==2 STRICT. A ColType-2 parcel
			//      missing its captured kind-1 (capture gap) -> SKIP the whole parcel, NEVER
			//      synthesize (nok1skip). A ColType!=2 parcel carrying one -> drop that line
			//      only (k1drop). If the strict rule and flycast's own TaTypeLut
			//      (p3dPcwHdr64) DISAGREE on a PCW -> skip + count (rulediv): flycast's
			//      parser consumes fr.ta, so a divergent parcel mis-sizes in EITHER form
			//      (cannot fire on the measured families — 0x2c/2d/2e are Tex+Offset so both
			//      rules say 64B; 0x3c/3e are ColType-3 so both say 32B).
			//   3. TA-FORM VALIDATION (iteration 3): ParaType==4; only kind-1/2 after the param;
			//      64B-vertex parcels arrive as [head][cont] pairs with ParaType-7 heads; the
			//      LAST vertex carries End_Of_Strip (bit28). Malformed -> skip (formskip).
			//   4. TCW-CLASS BLOCKLIST (Phase-A tradeoff, see p3dSkipTcwInit) + 5. BBOX net.
			// CAPTURE GAP FILED (iteration 4 item 3, not fixed here): the capture holds the
			// OP-pass submission only (captured PCWs have list bits = 0); the mirror shows the
			// engine RE-SUBMITS the same classes on the TR pass (list2) — the biggest orphan
			// class (14-30/frame). The per-pass rewind (oracle_hook p3dCapture) may be
			// discarding one of the two submissions if OP and TR emit in DIFFERENT passes —
			// candidate fixes: keep the last COMPLETE pass PAIR, or capture list bits per
			// parcel and re-emit both lists here. Coordinate with the emission expert's differ
			// (byte truth per class per list) before changing the capture.
			size_t i = 0;
			while (i < nRec) {
				const uint8_t* rec = &_gstaP3d[i * MC_P3D_LINE_BYTES];
				if (rec[0] != 0) { p3dOrphan++; i++; continue; }        // GUARD: param-less line
				size_t j = i + 1;                                        // parcel = records [i, j)
				while (j < nRec && _gstaP3d[j * MC_P3D_LINE_BYTES] != 0) j++;
				const uint8_t* pl = rec + 4;                             // param line 1 [PCW][ISP][TSP][TCW]
				uint32_t pcw = gle32(pl), tcw = gle32(pl + 12);
				// --- 2. header-size policy: STRICT ColType==2 rule (iteration 4c) ----------
				size_t hdrLines = (i + 1 < j && _gstaP3d[(i + 1) * MC_P3D_LINE_BYTES] == 1) ? 2 : 1;
				bool   strict64 = (((pcw >> 4) & 3) == 2);               // PROVEN: ColType==2 <=> 64B global
				if (strict64 != p3dPcwHdr64(pcw)) { p3dRuleDiv++; i = j; continue; }   // parser-divergent PCW: skip
				if (strict64 && hdrLines != 2)    { p3dNoK1Skip++; i = j; continue; }  // capture gap: NEVER synthesize
				// --- 3. TA-form validation -------------------------------------------------
				bool v64       = p3dPcwVtx64(pcw);
				bool malformed = ((pcw >> 29) != 4);                     // drawer emits poly globals only
				if (!malformed)
					for (size_t t = i + hdrLines; t < j; t++)            // only vertex lines after the header
						if (_gstaP3d[t * MC_P3D_LINE_BYTES] != 2) { malformed = true; break; }
				size_t vtxLines = j - i - hdrLines;
				if (!malformed && vtxLines) {
					if (v64 && (vtxLines & 1)) malformed = true;         // 64B verts = [head][cont] pairs
					size_t step = v64 ? 2 : 1;
					for (size_t t = i + hdrLines; !malformed && t < j; t += step) {
						const uint8_t* q = &_gstaP3d[t * MC_P3D_LINE_BYTES];
						if ((gle32(q + 4) >> 29) != 7) malformed = true; // every vertex head = ParaType 7
					}
					if (!malformed) {                                    // last vertex must close the strip
						size_t lastHead = v64 ? (j - 2) : (j - 1);
						const uint8_t* q = &_gstaP3d[lastHead * MC_P3D_LINE_BYTES];
						if (!(gle32(q + 4) & 0x10000000u)) malformed = true;   // End_Of_Strip bit28
					}
				}
				if (malformed) { p3dFormSkip++; i = j; continue; }
				// --- 3. TCW-class blocklist ------------------------------------------------
				bool skip = false;
				if (pcw & 0x8u) {                                        // textured param
					for (int k = 0; k < s_p3dSkipTcwN; k++)
						if ((tcw & 0xFFFFFF00u) == (s_p3dSkipTcw[k] & 0xFFFFFF00u)) {
							skip = true; p3dTcwSkip++; break;
						}
				}
				// --- 4. bbox safety net (structure-aware: heads only, v64-paired) ----------
				if (!skip && vtxLines) {
					float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
					size_t step = v64 ? 2 : 1;
					for (size_t t = i + hdrLines; t < j; t += step) {
						const uint8_t* vl = &_gstaP3d[t * MC_P3D_LINE_BYTES] + 4;
						float x, y; uint32_t ux = gle32(vl + 4), uy = gle32(vl + 8);
						memcpy(&x, &ux, 4); memcpy(&y, &uy, 4);
						if (x < bx0) bx0 = x; if (x > bx1) bx1 = x;
						if (y < by0) by0 = y; if (y > by1) by1 = y;
					}
					if (bx1 >= bx0 && ((bx1 - bx0) > 1400.f || (by1 - by0) > 1400.f)) {
						skip = true; p3dBboxSkip++;
					}
				}
				if (!skip) {
					p3dPolys++;
					for (size_t t = i; t < j; t++) {
						const uint8_t* lr = &_gstaP3d[t * MC_P3D_LINE_BYTES];
						// KIND-1 BY STRICT ColType (iteration 4c): in-stream for ColType-2
						// (the 64B global's +32 face-color half), dropped for ColType!=2.
						if (lr[0] == 1 && !strict64) { p3dK1Drop++; continue; }
						if (lr[2] == 0xAC) p3dDefer++; else p3dSlot0++;
						p3dOut.insert(p3dOut.end(), lr + 4, lr + 4 + 32);  // the 32B SQ line, verbatim
					}
				}
				i = j;
			}
			// Splice the injected OPAQUE parcels into the stage OP list, just BEFORE its EOL
			// (which lives at [stagePrefixLen-32, stagePrefixLen)). This keeps DC list order legal
			// (single OP list) and puts them where flycast bins them OPAQUE per their PCW.ListType.
			if (!p3dOut.empty()) {
				if (stagePrefixLen >= 32 && stagePrefixLen <= fr.ta.size()) {
					fr.ta.insert(fr.ta.begin() + (ptrdiff_t)(stagePrefixLen - 32),
					             p3dOut.begin(), p3dOut.end());
				} else {
					// No stage OP prefix this frame: prepend a self-contained OP list
					// [injected][EOL] at the front so the parcels still bin OPAQUE, in order.
					std::vector<uint8_t> eol(32, 0);
					p3dOut.insert(p3dOut.end(), eol.begin(), eol.end());
					fr.ta.insert(fr.ta.begin(), p3dOut.begin(), p3dOut.end());
				}
			}
			static int _p3dLog = 0;
			if ((_p3dLog++ % 120) == 0)
				printf("[GSTA] p3d: %zu polys, %zu lines injected (slot0=%zu defer=%zu orphan=%zu "
				       "k1drop=%zu nok1skip=%zu rulediv=%zu formskip=%zu tcwskip=%zu bboxskip=%zu) vf=%u\n",
				       p3dPolys, p3dSlot0 + p3dDefer, p3dSlot0, p3dDefer, p3dOrphan,
				       p3dK1Drop, p3dNoK1Skip, p3dRuleDiv, p3dFormSkip, p3dTcwSkip, p3dBboxSkip, vframe);
		}
	}

	// ---- PL3D DUMP (gate [6] input): gsta_polys_<vframe>.txt under MAPLECAST_DUMP_GSTA_VRAM.
	// One line per poly: slot, cls, param=[w0]=PCW, w1=ISP, w2=TSP, w3=TCW (the LIVE param
	// flush 0x8C129FE6 carries the real 4-word global — 2026-07-06 kind retag; only the RARE
	// culled-path params 0x8C129E66 have w1-w3 zeroed by the engine @0x8C129E5E-64), vertex
	// count (lines whose w0 ParaType bits 31:29 == 7), raw line count, bbox from the vertex
	// heads' x=w1/y=w2 floats. Vertex records before any param open an implicit poly
	// (param=0) — those are exactly what the injection's orphan guard skips. ----
	{
		// ONCE-PER-VFRAME (iteration 4 item 2 — 352-vs-62 dump inflation): the render-side
		// consume path CANNOT accumulate (clientReceiveGsta drains _gstaReady once per
		// published frame and ctx.rend.Clear()/tad.Clear() run per Process), so any
		// multiplication is message-cadence or stale-file artifact. This guard makes the
		// dump literally once per vframe (first message wins; repeats counted + logged so
		// the upstream cadence is MEASURED, not guessed). NOTE for the gate: the dump dir
		// may hold stale gsta_polys_*.txt files from a PREVIOUS run past the 4000-file cap —
		// clear it per run; the [P3DDUMP] stdout line (vf + parcels + msg seq) is the
		// cross-check that a file belongs to this run.
		static int      _p3dDumpN = 0;
		static uint32_t _p3dDumpVframe = 0xFFFFFFFFu;
		static uint32_t _p3dDumpRepeats = 0;
		static uint32_t _p3dMsgSeq = 0;
		_p3dMsgSeq++;
		const char* vd3 = std::getenv("MAPLECAST_DUMP_GSTA_VRAM");
		if (vd3 && *vd3 && vframe == _p3dDumpVframe) {
			_p3dDumpRepeats++;
			if (_p3dDumpRepeats <= 16 || (_p3dDumpRepeats % 256) == 0)
				printf("[P3DDUMP] repeat vframe %u (msg=%u repeats=%u) — dump skipped, injection unaffected\n",
				       vframe, _p3dMsgSeq, _p3dDumpRepeats);
		}
		if (vd3 && *vd3 && _p3dDumpN < 4000 && vframe != _p3dDumpVframe) {
			_p3dDumpN++;
			_p3dDumpVframe = vframe;
			printf("[P3DDUMP] vf=%u msg=%u tailLines=%zu tailBytes=%zu\n",
			       vframe, _p3dMsgSeq, _gstaP3d.size() / MC_P3D_LINE_BYTES, _gstaP3d.size());
			char p3path[512]; snprintf(p3path, sizeof(p3path), "%s/gsta_polys_%u.txt", vd3, vframe);
			FILE* p3f = fopen(p3path, "w");
			if (p3f) {
				const size_t nRec = _gstaP3d.size() / MC_P3D_LINE_BYTES;
				int  polyN = 0, verts = 0, lines = 0;
				bool open = false;
				uint32_t slot = 0, cls = 0, w[4] = {0,0,0,0};
				float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
				auto flushPoly = [&]() {
					if (!open) return;
					fprintf(p3f, "poly%d slot=%u cls=%02x param=%08x w1=%08x w2=%08x w3=%08x "
					             "verts=%d lines=%d bbox=%.1f,%.1f,%.1f,%.1f\n",
					        polyN++, slot, cls, w[0], w[1], w[2], w[3], verts, lines, bx0, by0, bx1, by1);
					open = false;
				};
				for (size_t i = 0; i < nRec; i++) {
					const uint8_t* rec = &_gstaP3d[i * MC_P3D_LINE_BYTES];
					const uint8_t* ln  = rec + 4;
					uint32_t w0 = gle32(ln);
					if (rec[0] == 0) {                     // param line 1: new poly record
						flushPoly();
						open = true; slot = rec[1]; cls = rec[2];
						for (int k = 0; k < 4; k++) w[k] = gle32(ln + k * 4);
						verts = 0; lines = 0;
						bx0 = by0 = 1e9f; bx1 = by1 = -1e9f;
					} else if (rec[0] == 2) {              // vertex line
						if (!open) {                       // implicit poly (mid-frame carry)
							open = true; slot = rec[1]; cls = rec[2];
							w[0] = w[1] = w[2] = w[3] = 0;
							verts = 0; lines = 0;
							bx0 = by0 = 1e9f; bx1 = by1 = -1e9f;
						}
						lines++;
						if ((w0 >> 29) == 7) {             // TA vertex parcel head (ParaType 7)
							verts++;
							float x, y; uint32_t ux = gle32(ln + 4), uy = gle32(ln + 8);
							memcpy(&x, &ux, 4); memcpy(&y, &uy, 4);
							if (x < bx0) bx0 = x; if (x > bx1) bx1 = x;
							if (y < by0) by0 = y; if (y > by1) by1 = y;
						}
					}
					// rec[0] == 1 (param line 2, face colors) needs no dump field
				}
				flushPoly();
				fclose(p3f);
			}

			// TA-STREAM SANITY (iteration-3 unit test, logged to stdout so the gate file
			// stays clean): walk the FULLY-assembled fr.ta (stage + bodies + HUD + PL3D +
			// EOLs) with flycast's exact size rules (p3dPcwHdr64/p3dPcwVtx64 == ta.cpp
			// TaTypeLut; sprite globals -> 64B verts, ta.cpp:163). anomalies MUST be 0 and
			// glob >= injectedPolys — a nonzero anomaly count pinpoints a splice desync
			// (vertex with no open list, truncated 64B tail, leftover bytes).
			{
				size_t off2 = 0, nGlob = 0, nSpr = 0, nVtx = 0, nEol = 0, nAnom = 0;
				bool vtx64 = false, openList = false;
				const uint8_t* T = fr.ta.data(); const size_t TN = fr.ta.size();
				while (off2 + 32 <= TN) {
					uint32_t w0 = gle32(T + off2);
					uint32_t pt = w0 >> 29;
					size_t sz = 32;
					if (pt == 0)      { nEol++; openList = false; }
					else if (pt == 4) { nGlob++; openList = true; vtx64 = p3dPcwVtx64(w0);
						// header size by the STRICT ColType rule (iteration 4c, mirror-proven);
						// a strict-vs-TaTypeLut divergence would mis-size in flycast's parser ->
						// counted as an anomaly (injection already skips such parcels).
						bool s64 = ((w0 >> 4) & 3) == 2;
						if (s64 != p3dPcwHdr64(w0)) nAnom++;
						if (s64) sz = 64; }
					else if (pt == 5) { nSpr++;  openList = true; vtx64 = true; }
					else if (pt == 7) { nVtx++; if (!openList) nAnom++; if (vtx64) sz = 64; }
					if (off2 + sz > TN) { nAnom++; break; }        // truncated 64B tail
					off2 += sz;
				}
				if (off2 != TN) nAnom++;                           // leftover bytes
				printf("[P3DTA] vf=%u ta=%zuB glob=%zu spr=%zu vtx=%zu eol=%zu anomalies=%zu injectedPolys=%zu\n",
				       vframe, TN, nGlob, nSpr, nVtx, nEol, nAnom, p3dPolys);
			}
		}
	}

	fr.vframe    = vframe;
	fr.vramDirty = (written > 0);
	fr.palDirty  = palDirty;

	// Capture the REAL emitted TA size BEFORE the std::move (the old log read
	// fr.ta.size() AFTER the move -> moved-from vector -> always 0 -> "ta=0B").
	size_t taSizeForLog = fr.ta.size();

	// One-shot TA dump for the byte-diff against the gold-standard clientReceive()
	// path. MAPLECAST_DUMP_GSTA_TA=1 writes one frame's emitted sprite-TA so we can
	// structurally compare it to a real MVC2 in-match TA. Dumps the first non-empty
	// frame, then stops.
	{
		static bool _gstaDumpDone = false;
		const char* de = std::getenv("MAPLECAST_DUMP_GSTA_TA");
		if (de && *de && *de != '0' && !_gstaDumpDone && !fr.ta.empty()) {
			const char* dp = std::getenv("MAPLECAST_DUMP_GSTA_TA_PATH");
			std::string path = (dp && *dp) ? dp : "gsta_ta_dump.bin";
			FILE* f = fopen(path.c_str(), "wb");
			if (f) { fwrite(fr.ta.data(), 1, fr.ta.size(), f); fclose(f);
				printf("[GSTA] dumped %zuB TA -> %s (vframe %u, quads=%d)\n",
					fr.ta.size(), path.c_str(), vframe, nQuad); fflush(stdout); }
			_gstaDumpDone = true;
		}
	}

	// Multi-frame keyed TA dump for the A/B param diff vs the mirror client (7200).
	// MAPLECAST_DUMP_GSTA_TA_DIR=<dir> writes <dir>/frame_<vframe>.bin every frame so a
	// single parser can align GSTA-emitted TA to the gold-standard mirror-client TA.
	{
		static bool _gdInit = false;
		static std::string _gdDir;
		if (!_gdInit) {
			const char* d = std::getenv("MAPLECAST_DUMP_GSTA_TA_DIR");
			if (d && *d) {
				_gdDir = d;
#ifdef _WIN32
				_mkdir(_gdDir.c_str());
#else
				mkdir(_gdDir.c_str(), 0755);
#endif
				printf("[GSTA] keyed TA dump -> %s/frame_NNNNNN.bin (by vframe)\n", _gdDir.c_str());
			}
			_gdInit = true;
		}
		if (!_gdDir.empty() && !fr.ta.empty()) {
			char path[512];
			snprintf(path, sizeof(path), "%s/frame_%06u.bin", _gdDir.c_str(), vframe);
			FILE* f = fopen(path, "wb");
			if (f) { fwrite(fr.ta.data(), 1, fr.ta.size(), f); fclose(f); }
		}
	}

	{
		std::lock_guard<std::mutex> lk(_gstaMtx);
		// PROFILE: if the render thread hasn't consumed the previous produced frame yet
		// (_gstaReady still true), overwriting _gstaFrame DROPS that frame -> the render
		// thread skips a wire frame -> motion judder even though produce is 60Hz.
		if (prof && _gstaReady.load(std::memory_order_acquire)) _gprof.produceDropped++;
		_gstaFrame = std::move(fr);
		_gstaReady.store(true, std::memory_order_release);
	}

	static uint64_t _fn = 0;
	if ((_fn++ % 60) == 0)
		printf("[GSTA] frame %u: bodies=%u quads=%d texWritten=%d palDirty=%d ta=%zuB\n",
			vframe, render_frame_body_count(), nQuad, written, (int)palDirty, taSizeForLog);

	if (prof) {
		// HUD-append phase is small; fold remaining apply time into 'hud'.
		double tEnd = _gnow();
		_gprof.hud   += tEnd - t0;
		_gprof.total += tEnd - tApply0;

		// vframe progression: how far the wire game-frame jumped between two PRODUCED
		// frames. ~1 = smooth (we reconstruct every wire frame). >1 = wire frames are
		// being skipped (server drop-old) because we produce slower than 60Hz.
		if (_gprof.frames == 0) { _gprof.vframeFirst = vframe; }
		else {
			int64_t dv = (int64_t)vframe - (int64_t)_gprof.vframePrev;
			if (dv > 0) { _gprof.vframeSpanSum += (uint64_t)dv; if (dv > 1) _gprof.vframeJumps++; }
		}
		_gprof.vframePrev = vframe;
		_gprof.frames++;

		if ((_gprof.frames % 120) == 0) {
			double wallSec = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - _gprof.wallStart).count();
			double prodFps = _gprof.frames / (wallSec > 0 ? wallSec : 1);
			double vframeSpan = (double)(vframe - _gprof.vframeFirst);
			double avgJump = _gprof.frames > 1 ? (double)_gprof.vframeSpanSum / (_gprof.frames - 1) : 0;
			double f = _gprof.frames;
			printf("[GPROF] produced=%llu wall=%.1fs PRODUCE_FPS=%.1f | "
			       "vframe %llu..%u span=%.0f AVG_VFRAME_PER_PRODUCE=%.2f jumps>1=%llu\n",
				(unsigned long long)_gprof.frames, wallSec, prodFps,
				(unsigned long long)_gprof.vframeFirst, vframe, vframeSpan, avgJump,
				(unsigned long long)_gprof.vframeJumps);
			printf("[GPROF] avg us/frame: splat=%.0f tails=%.0f render=%.0f stage=%.0f "
			       "bodyta=%.0f decode=%.0f hud=%.0f | TOTAL=%.0f\n",
				_gprof.splat/f, _gprof.tails/f, _gprof.render/f, _gprof.stage/f,
				_gprof.bodyta/f, _gprof.decode/f, _gprof.hud/f, _gprof.total/f);
			fflush(stdout);
		}
	}
}

// WS thread: connect 7212, msg1=ZCST prefix (seed), msg N = FRMx (apply+render).
static void gstaClientRun(std::string host, int port)
{
	printf("[GSTA] Connecting to %s:%d (replica-live wire)...\n", host.c_str(), port); fflush(stdout);
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
	struct addrinfo hints = {}, *res = nullptr;
	hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
	char portBuf[16]; snprintf(portBuf, sizeof(portBuf), "%d", port);
	if (getaddrinfo(host.c_str(), portBuf, &hints, &res) != 0 || !res) {
		printf("[GSTA] getaddrinfo('%s:%d') failed\n", host.c_str(), port); return;
	}
	int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) { printf("[GSTA] socket() failed\n"); freeaddrinfo(res); return; }
	if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
		printf("[GSTA] connect() failed: %s\n", strerror(errno));
		mc_closesocket(fd); freeaddrinfo(res); return;
	}
	freeaddrinfo(res);
	int one = 1; mc_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
	if (!wsHandshake(fd, host.c_str(), port)) {
		printf("[GSTA] WebSocket handshake failed\n"); mc_closesocket(fd); return;
	}
	printf("[GSTA] WebSocket handshake OK  --  awaiting ZCST static prefix\n"); fflush(stdout);

	MirrorDecompressor decomp;
	decomp.init(40 * 1024 * 1024);   // prefix ~27MB uncompressed + headroom

	std::vector<uint8_t> frame;
	bool prefixSeen = false;
	while (true) {
		if (!wsReadFrame(fd, frame)) { printf("[GSTA] connection lost\n"); break; }
		if (frame.empty()) continue;                              // ping/text
		size_t dn = 0;
		const uint8_t* dd = decomp.decompress(frame.data(), frame.size(), dn);
		if (!dd || dn < 12) continue;

		if (!prefixSeen) {
			if (gstaApplyPrefix(dd, dn)) {
				prefixSeen = true;
				_gstaSeeded.store(true, std::memory_order_release);
				printf("[GSTA] seeded; streaming frames\n"); fflush(stdout);
			}
			continue;
		}
		if (gle32(dd) == GSTA_FRMX_MAGIC) gstaApplyFrame(dd, dn);
	}
	mc_closesocket(fd); decomp.destroy();
}

static void initGstaClient()
{
	_isClient   = true;     // run the mirror render loop in mainui.cpp
	_gstaMode   = true;
	_useWebSocket = false;  // NOT the TA-mirror WS path

	const char* host = std::getenv("MAPLECAST_SERVER_HOST");
	if (!host || !*host) host = "127.0.0.1";
	int port = 7212;
	if (const char* pe = std::getenv("MAPLECAST_GSTA_PORT")) { int v = atoi(pe); if (v>0 && v<65536) port = v; }

	printf("[GSTA] === NATIVE GSTA CLIENT === ws://%s:%d/ (render_frame -> flycast renderer)\n", host, port);
	memwatch::unprotect();

	if (!_decodeTaAlloced) { _decodeTaCtx[0].Alloc(); _decodeTaCtx[1].Alloc(); _decodeTaAlloced = true; }
	_decodeIdx = 0;

	_gstaThread = std::thread(gstaClientRun, std::string(host), port);
	_gstaThread.detach();

	// input sink still goes to the headless server (7100) so the user can drive the match.
	int audioPort = 7203;
	if (const char* a = std::getenv("MAPLECAST_AUDIO_WS_PORT")) audioPort = atoi(a);
	maplecast_audio_client::init(host, audioPort);
}

// ---- render-thread consumer: drain the last GSTA frame, feed renderer->Process ----
// Same contract as clientReceive(): fills `rc`, sets vramDirty, returns true if a frame
// was applied. Called from mainui.cpp's mirror render loop when gstaModeActive().
bool clientReceiveGsta(rend_context& rc, bool& vramDirty)
{
	vramDirty = false;
	if (!_gstaMode) return false;
	if (!_gstaReady.load(std::memory_order_acquire)) return false;

	static GstaFrame local;
	{
		std::lock_guard<std::mutex> lk(_gstaMtx);
		local = std::move(_gstaFrame);
		_gstaReady.store(false, std::memory_order_release);
	}
	if (local.ta.empty()) return false;

	// PROFILE (render-consume side = what the user SEES): vframe stride between two
	// CONSUMED frames. >1 means the render thread skipped a wire frame (the WS thread
	// produced it then overwrote it before this thread drained it) -> visible judder.
	if (_gprof.on) {
		uint64_t prev = _gprof.rVframePrev.load(std::memory_order_relaxed);
		uint64_t cn   = _gprof.rConsumed.fetch_add(1, std::memory_order_relaxed);
		if (cn > 0) {
			int64_t dv = (int64_t)local.vframe - (int64_t)prev;
			if (dv > 0) { _gprof.rVframeSpan.fetch_add((uint64_t)dv, std::memory_order_relaxed);
				if (dv > 1) _gprof.rVframeJumps.fetch_add(1, std::memory_order_relaxed); }
		}
		_gprof.rVframePrev.store(local.vframe, std::memory_order_relaxed);
		if (((cn+1) % 120) == 0) {
			uint64_t span = _gprof.rVframeSpan.load(std::memory_order_relaxed);
			uint64_t jumps = _gprof.rVframeJumps.load(std::memory_order_relaxed);
			printf("[GPROF-RENDER] consumed=%llu RENDER_VFRAME_PER_CONSUME=%.3f jumps>1=%llu produceDropped=%llu\n",
				(unsigned long long)(cn+1), cn>0 ? (double)span/cn : 0.0,
				(unsigned long long)jumps, (unsigned long long)_gprof.produceDropped);
			fflush(stdout);
		}
	}

	// Apply THIS frame's staged body tiles into vram[] HERE (render thread), paired with
	// its TA — so the textures the renderer samples are exactly the ones decoded for this
	// frame's quads. (The WS thread no longer writes the shared vram[] mid-flight, which
	// raced the render thread and put the wrong part under each TCW = Cable fragmentation.)
	if (!local.tiles.empty()) {
		for (const auto& tw : local.tiles) {
			if ((size_t)tw.vaddr + 512 > VRAM_SIZE) continue;
			VramLockedWriteOffset(tw.vaddr);
			memcpy(&vram[tw.vaddr], tw.bytes, 512);
		}
	}

	TA_context& ctx = _decodeTaCtx[_decodeIdx];
	_decodeIdx ^= 1;
	uint8_t* taDst = ctx.tad.thd_root;
	size_t taSize = local.ta.size();
	memcpy(taDst, local.ta.data(), taSize);

	ctx.rend.Clear();
	ctx.tad.Clear();
	ctx.tad.thd_data = taDst + taSize;

	// Framebuffer geometry: MVC2 renders at 640x480 internally; the engine's TA
	// VERTICES are already in screen pixels (render_frame Ax/Ay), so the renderer
	// just needs a 640x480 target scaled by the client's RenderResolution.
	uint32_t serverW = 640, serverH = 480;
	if (config::RenderResolution > 480) {
		float scale = config::RenderResolution / 480.f;
		serverW = (uint32_t)(serverW * scale); serverH = (uint32_t)(serverH * scale);
	}
	ctx.rend.framebufferWidth  = serverW;
	ctx.rend.framebufferHeight = serverH;
	ctx.rend.clearFramebuffer  = true;
	ctx.rend.fZ_max = 1.0f;

	// The gold-standard clientReceive() restores a battery of PVR regs from the wire
	// pvr_snapshot before Process(). The GSTA path has no snapshot AND the SH4 is OFF,
	// so these PVR-reg globals may be zero. Seed the tile-clip + framebuffer-control
	// regs the rasterizer needs for a normal 640x480 frame. CRITICAL: the render
	// viewport size is derived as (tile_x_num+1)*32 x (tile_y_num+1)*32
	// (dx11_renderer.cpp:1264). A zero TA_GLOB_TILE_CLIP -> 32x32 target -> everything
	// clipped away (a SECOND blank-screen cause beyond the ParaType=0 PCW bug).
	ctx.rend.ta_GLOB_TILE_CLIP.full = 0;
	ctx.rend.ta_GLOB_TILE_CLIP.tile_x_num = (serverW / 32) - 1;   // 640 -> 19
	ctx.rend.ta_GLOB_TILE_CLIP.tile_y_num = (serverH / 32) - 1;   // 480 -> 14
	ctx.rend.scaler_ctl.full        = SCALER_CTL.full;
	ctx.rend.fb_X_CLIP.full         = 0;
	ctx.rend.fb_X_CLIP.min          = 0; ctx.rend.fb_X_CLIP.max = serverW - 1;
	ctx.rend.fb_Y_CLIP.full         = 0;
	ctx.rend.fb_Y_CLIP.min          = 0; ctx.rend.fb_Y_CLIP.max = serverH - 1;
	ctx.rend.fb_W_LINESTRIDE        = FB_W_LINESTRIDE.full;
	ctx.rend.fb_W_SOF1              = FB_W_SOF1;
	ctx.rend.fb_W_CTRL.full         = FB_W_CTRL.full;
	ctx.rend.fog_clamp_min.full     = FOG_CLAMP_MIN.full;
	ctx.rend.fog_clamp_max.full     = FOG_CLAMP_MAX.full;
	ctx.rend.isRTT = false;

	vramDirty = local.vramDirty;
	if (vramDirty && renderer) renderer->resetTextureCache = true;
	if (local.palDirty && renderer) {
		pal_needs_update = true; palette_update();
		renderer->updatePalette = true; renderer->updateFogTable = true;
	}

	if (renderer) {
		renderer->Process(&ctx);
		// LIVE render-debug (control-WS): per-TA-LIST isolation. Clearing a parsed list's
		// PolyParam vector before Render() suppresses that whole list (OP / PT / TR) — lets the
		// user isolate which list carries which geometry. Defaults on (no-op) when all enabled.
		{
			auto& _D = ::gsta_render_debug::g();
			if (_D.listOpaqueOn.load(std::memory_order_relaxed) == 0) ctx.rend.global_param_op.clear();
			if (_D.listPunchOn .load(std::memory_order_relaxed) == 0) ctx.rend.global_param_pt.clear();
			if (_D.listTransOn .load(std::memory_order_relaxed) == 0) ctx.rend.global_param_tr.clear();
		}
		rc = ctx.rend;
	}

	// Diagnostic: confirm ta_parse produced actual geometry (op/pt/tr poly params +
	// verts). All-zero here means the TA framing is still malformed.
	{
		static uint64_t _pn = 0;
		if ((_pn++ % 120) == 0)
			printf("[GSTA] parsed: verts=%zu op=%zu pt=%zu tr=%zu (taSize=%zu)\n",
				ctx.rend.verts.size(), ctx.rend.global_param_op.size(),
				ctx.rend.global_param_pt.size(), ctx.rend.global_param_tr.size(), taSize);
	}

	// DIAG (MAPLECAST_DUMP_TR_EXTENTS=1): dump flycast's ACTUAL RUNTIME PolyParam
	// screen extents per list, computed from ctx.rend.verts[first..first+count]. Pins
	// whether flycast's runtime parser produces LARGE extents for the injected list-2
	// parcels (a runtime-vs-TaTypeLut divergence — the wedge would be a PARSE bug) or
	// SMALL extents matching our two byte-faithful models (the wedge is a RENDER bug).
	// Read-only; A/B POLY3D=1 vs =0 on a held frame (mock_hold_server.mjs).
	if (const char* de = std::getenv("MAPLECAST_DUMP_TR_EXTENTS"); de && *de && *de != '0') {
		// Value "1" -> stdout (lost for the GUI client); any other value -> that file path.
		FILE* tf = (strcmp(de, "1") == 0) ? stdout : fopen(de, "a");
		if (!tf) tf = stdout;
		auto dumpList = [&](const char* nm, const std::vector<PolyParam>& L) {
			const auto& V = ctx.rend.verts;
			int nbig = 0; float maxspan = 0.f; int bx0=0,by0=0,bx1=0,by1=0, boff=0; uint32_t btcw=0, bpcw=0; size_t bcnt=0;
			for (const auto& pp : L) {
				if (pp.count < 3) continue;
				float mnx=1e30f,mxx=-1e30f,mny=1e30f,mxy=-1e30f;
				for (u32 i = pp.first; i < pp.first + pp.count && i < V.size(); i++) {
					float x = V[i].x, y = V[i].y;
					if (x<mnx)mnx=x; if(x>mxx)mxx=x; if(y<mny)mny=y; if(y>mxy)mxy=y;
				}
				float span = (mxx-mnx) > (mxy-mny) ? (mxx-mnx) : (mxy-mny);
				if (span > 500.f) {
					nbig++;
					fprintf(tf, "[TREXT] %s BIG pp pcw=%08x tcw=%08x first=%u count=%u bbox=[%d,%d,%d,%d] span=%.0f\n",
						nm, pp.pcw.full, pp.tcw.full, pp.first, pp.count, (int)mnx,(int)mny,(int)mxx,(int)mxy, span);
				}
				if (span > maxspan) { maxspan=span; bx0=(int)mnx;by0=(int)mny;bx1=(int)mxx;by1=(int)mxy; btcw=pp.tcw.full; bpcw=pp.pcw.full; bcnt=pp.count; }
			}
			fprintf(tf, "[TREXT] %s: polys=%zu big(span>500)=%d maxspan=%.0f (pcw=%08x tcw=%08x count=%zu bbox=[%d,%d,%d,%d])\n",
				nm, L.size(), nbig, maxspan, bpcw, btcw, bcnt, bx0,by0,bx1,by1);
		};
		dumpList("OP", ctx.rend.global_param_op);
		dumpList("TR", ctx.rend.global_param_tr);
		if (tf != stdout) fclose(tf);
	}
	return true;
}
#else // !MAPLECAST_GSTA_CLIENT_BUILD
// Headless / server build: stub the GSTA-client entry points so mainui.cpp links.
// There is no GSTA reconstruction client here, so gstaModeActive() is always false
// (the normal clientReceive() path runs). This fixes the headless link break
// (unresolved gstaModeActive/clientReceiveGsta from mainui.cpp) that left the
// build-headless-win binary undefined.
bool gstaModeActive() { return false; }
bool clientReceiveGsta(rend_context&, bool&) { return false; }
#endif // MAPLECAST_GSTA_CLIENT_BUILD

// !!! THIS FUNCTION IS THE GOLD STANDARD  --  KEEP IT THAT WAY !!!
//
// Three other implementations parse the same wire format and MUST stay aligned
// with this one (which is the desktop flycast mirror client, the only one that
// has been correct end-to-end since day one):
//
//   1. packages/renderer/src/wasm_bridge.cpp renderer_frame()  (king.html WASM)
//   2. core/network/maplecast_wasm_bridge.cpp mirror_render_frame()  (emulator.html WASM)
//   3. relay/src/fanout.rs (the Rust VPS relay  --  parses dirty pages for its SYNC cache)
//
// When fixing a rendering bug in either browser client, the fix is almost
// always "make it look like clientReceive()". Five bugs we already paid for:
//
//   (A) Decompressor sized too small  --  use 16MB shared between SYNC and frames.
//   (B) Skipping dirty-pages walk while waiting for first keyframe  --  DON'T.
//       Walk pages even when you can't render the TA buffer yet.
//   (C) VramLockedWriteOffset MUST be called BEFORE memcpy into VRAM.
//   (D) Don't truncate prevTA when taSize shrinks  --  only grow.
//   (E) renderer->resetTextureCache MUST be set whenever any VRAM page is dirty.
//
// All five bugs manifest as broken character select / loading screens while
// in-match looks fine. If you only test in-match, you will not catch them.
//
// See docs/ARCHITECTURE.md "Mirror Wire Format  --  Rules of the Road" for the
// canonical list of rules all four parsers must obey.
bool clientReceive(rend_context& rc, bool& vramDirty)
{
	vramDirty = false;
	if (!_isClient) return false;
	int64_t t0 = _clientNowUs();

	if (_useWebSocket)
	{
		// Pipelined: background thread already decoded TA + staged dirty pages.
		// Take a local snapshot of _decoded under the mutex so the producer
		// can't std::move() a new vector into it while we're iterating pages.
		static DecodedFrame df_local;
		{
			std::lock_guard<std::mutex> lock(_decodedMtx);
			if (!_decodedReady.load(std::memory_order_relaxed)) return false;
			df_local = std::move(_decoded);
			_decoded = DecodedFrame{};  // reset so producer's next merge starts empty
			_decodedReady.store(false, std::memory_order_relaxed);
		}

		DecodedFrame& df = df_local;
		TA_context& ctx = _decodeTaCtx[df.taBufferIdx];
		uint8_t* taDst = ctx.tad.thd_root;

		// === CLIENT-SIDE TA DUMP  --  pair with the server-side dump in serverPublish() ===
		// MAPLECAST_DUMP_TA=1 â†’ write the received TA buffer to
		// /tmp/ta-dumps-client/frame_NNNNNN.bin so we can byte-diff it against
		// the server's /tmp/ta-dumps/frame_NNNNNN.bin for the same frame.
		// Both should be byte-identical if the wire is faithful.
		{
			static bool _dumpInit = false;
			static bool _dumpEnabled = false;
			static std::string _dumpDir;
			if (!_dumpInit) {
				const char* e = std::getenv("MAPLECAST_DUMP_TA");
				_dumpEnabled = (e && *e && *e != '0');
				if (_dumpEnabled) {
					const char* d = std::getenv("MAPLECAST_DUMP_TA_DIR");
					_dumpDir = (d && *d) ? d : "/tmp/ta-dumps-client";
#ifdef _WIN32
					int rc = _mkdir(_dumpDir.c_str());
#else
					int rc = mkdir(_dumpDir.c_str(), 0755);
#endif
					printf("[TA-DUMP] client enabled — writing %s/frame_NNNNNN.bin (mkdir rc=%d, errno=%d)\n",
					       _dumpDir.c_str(), rc, errno);
					fflush(stdout);
				}
				_dumpInit = true;
			}
			if (_dumpEnabled && df.taSize > 0) {
				char path[512];
				snprintf(path, sizeof(path), "%s/frame_%06u.bin", _dumpDir.c_str(), df.frameNum);
				FILE* f = fopen(path, "wb");
				if (f) {
					fwrite(taDst, 1, df.taSize, f);
					fclose(f);
				} else {
					static int _warnedFopen = 0;
					if (_warnedFopen++ < 3)
						printf("[TA-DUMP] client fopen(%s) failed: errno=%d\n", path, errno);
				}
			}
		}

		// Apply dirty pages to emulator memory (must happen on render thread).
		// Also track whether ANY PVR-regs page was dirty this frame  --  used
		// below to gate the palette/fog re-upload on actual state changes
		// instead of doing it unconditionally every frame.
		bool pvrRegsDirty = false;
		for (uint32_t d = 0; d < df.dirtyCount; d++) {
			size_t pageOff = df.pages[d].pageIdx * MEM_PAGE_SIZE;
			uint8_t rid = df.pages[d].regionId;

			if (rid == 0 && pageOff + MEM_PAGE_SIZE <= 16 * 1024 * 1024)
				memcpy(&mem_b[pageOff], df.pages[d].data, MEM_PAGE_SIZE);
			else if (rid == 1 && pageOff + MEM_PAGE_SIZE <= VRAM_SIZE) {
				// Unprotect BEFORE writing  --  texture cache may have mprotect'd this page
				VramLockedWriteOffset(pageOff);
				memcpy(&vram[pageOff], df.pages[d].data, MEM_PAGE_SIZE);
				vramDirty = true;
			}
			else if (rid == 2 && pageOff + MEM_PAGE_SIZE <= 2 * 1024 * 1024)
				memcpy(&aica::aica_ram[pageOff], df.pages[d].data, MEM_PAGE_SIZE);
			else if (rid == 3 && pageOff + MEM_PAGE_SIZE <= (size_t)pvr_RegSize) {
				memcpy(pvr_regs + pageOff, df.pages[d].data, MEM_PAGE_SIZE);
				pvrRegsDirty = true;
			}
		}

		// DIAG (MAPLECAST_DUMP_GSTA_VRAM=<dir>): one-shot dump of the REAL engine's
		// body texture VRAM band [0x80000..0xE0000], keyed by this client's frameNum,
		// to byte-diff against the GSTA-decoded VRAM at each Cable wide-part TCW.
		{
			static int _rvN = 0;
			const char* rvd = std::getenv("MAPLECAST_DUMP_GSTA_VRAM");
			if (rvd && *rvd && _rvN < 4000 && df.frameNum > 0) {
				_rvN++;
				char rvp[512]; snprintf(rvp, sizeof(rvp), "%s/real_vram_%u.bin", rvd, df.frameNum);
				FILE* rf = fopen(rvp, "wb");
				if (rf) { fwrite(&vram[0x400000], 1, 0x80000, rf); fclose(rf); }
				pal_needs_update = true; palette_update();
				char rpp[512]; snprintf(rpp, sizeof(rpp), "%s/real_pal_%u.bin", rvd, df.frameNum);
				FILE* rpf = fopen(rpp, "wb");
				if (rpf) { fwrite(palette32_ram, 4, 1024, rpf); fclose(rpf); }
			}
		}

		// E2E latency probe  --  complete if visual change detected.
		// Zero-cost when no probe is pending (single atomic load).
		if (vramDirty)
			maplecast_input_sink::onVisualChange();

		// Render  --  TA data already decoded in ctx.tad.thd_root by background thread
		if (df.taSize > 0) {
			ctx.rend.Clear();
			ctx.tad.Clear();
			ctx.tad.thd_data = taDst + df.taSize;

			TA_GLOB_TILE_CLIP.full = df.pvr_snapshot[0];
			SCALER_CTL.full = df.pvr_snapshot[1];
			FB_X_CLIP.full = df.pvr_snapshot[2];
			FB_Y_CLIP.full = df.pvr_snapshot[3];
			FB_W_LINESTRIDE.full = df.pvr_snapshot[4];
			FB_W_SOF1 = df.pvr_snapshot[5];
			FB_W_CTRL.full = df.pvr_snapshot[6];
			FOG_CLAMP_MIN.full = df.pvr_snapshot[7];
			FOG_CLAMP_MAX.full = df.pvr_snapshot[8];

			ctx.rend.isRTT = df.pvr_snapshot[13] != 0;
			ctx.rend.fb_W_SOF1 = df.pvr_snapshot[5];
			ctx.rend.fb_W_CTRL.full = df.pvr_snapshot[6];
			ctx.rend.ta_GLOB_TILE_CLIP.full = df.pvr_snapshot[0];
			ctx.rend.scaler_ctl.full = df.pvr_snapshot[1];
			ctx.rend.fb_X_CLIP.full = df.pvr_snapshot[2];
			ctx.rend.fb_Y_CLIP.full = df.pvr_snapshot[3];
			ctx.rend.fb_W_LINESTRIDE = df.pvr_snapshot[4];
			ctx.rend.fog_clamp_min.full = df.pvr_snapshot[7];
			ctx.rend.fog_clamp_max.full = df.pvr_snapshot[8];
			{
				// Server sends its native framebuffer dimensions. Scale
				// by the client's RenderResolution so the local renderer
				// draws at the client's chosen upscale factor.
				u32 serverW = df.pvr_snapshot[9];
				u32 serverH = df.pvr_snapshot[10];
				if (config::RenderResolution > 480 && serverH > 0) {
					float scale = config::RenderResolution / 480.f;
					serverW = (u32)(serverW * scale);
					serverH = (u32)(serverH * scale);
				}
				ctx.rend.framebufferWidth = serverW;
				ctx.rend.framebufferHeight = serverH;
			}
			ctx.rend.clearFramebuffer = df.pvr_snapshot[11] != 0;
			float fz; memcpy(&fz, &df.pvr_snapshot[12], 4);
			ctx.rend.fZ_max = fz;

			if (vramDirty) renderer->resetTextureCache = true;

			// Gate palette + fog re-upload on actual PVR-regs changes
			// instead of running unconditionally every frame. Profiling
			// showed render=18100Âµs avg=15587Âµs per frame on a 3090 with
			// no vsync, which dropped the mirror client to ~30 fps. The
			// overwhelming majority of that cost was palette_update() +
			// renderer->updatePalette + renderer->updateFogTable running
			// EVERY frame when the palette/fog had not actually changed.
			//
			// Palette RAM and fog LUT both live in the PVR regs region
			// (rid=3). If no PVR-regs page was dirty this frame, the
			// palette and fog are the same bytes as last frame  --  there
			// is nothing to re-upload. The browser WASM client has an
			// analogous path but doesn't touch these flags every frame,
			// which is why the browser runs smoothly on the same feed.
			if (pvrRegsDirty) {
				::pal_needs_update = true;
				palette_update();
				renderer->updatePalette = true;
				renderer->updateFogTable = true;
			}

			renderer->Process(&ctx);
			rc = ctx.rend;
		}

		int64_t t1 = _clientNowUs();
		static int64_t totalUs = 0;
		static uint32_t count = 0;
		const int64_t thisDecodeUs = t1 - t0;
		totalUs += thisDecodeUs;
		count++;
		if (df.frameNum % 600 == 0)
			printf("[MIRROR] Client frame %u | dirty=%u pages | render=%lldÂµs avg=%lldÂµs | WS-PIPELINE\n",
				df.frameNum, df.dirtyCount, (long long)thisDecodeUs, (long long)(totalUs / count));

		// Publish debug-overlay telemetry atomics for the WS path.
		_clientLastDecodeUs.store(thisDecodeUs, std::memory_order_relaxed);
		{
			const int64_t prev = _clientDecodeEmaUs.load(std::memory_order_relaxed);
			_clientDecodeEmaUs.store(prev + ((thisDecodeUs - prev) >> 4),
			                         std::memory_order_relaxed);
		}
		// Tele-0.5/0.10: max-since-last-report + frame counter, used by
		// the HTTP-POST stats reporter that pushes to /api/telemetry
		// every 1s.
		{
			int64_t cur = _clientDecodeMaxUs.load(std::memory_order_relaxed);
			while (thisDecodeUs > cur
			    && !_clientDecodeMaxUs.compare_exchange_weak(cur, thisDecodeUs,
			        std::memory_order_relaxed)) {}
		}
		_clientFramesDecoded.fetch_add(1, std::memory_order_relaxed);
		_clientLastDirtyPages.store(df.dirtyCount, std::memory_order_relaxed);
		_clientLastTaSize.store(df.taSize, std::memory_order_relaxed);
		_clientLastVramDirty.store(vramDirty, std::memory_order_relaxed);

		return df.taSize > 0;
	}

	// === SHM path ===
	uint8_t* src = nullptr;
	{
		if (!_shmPtr) return false;
		RingHeader* hdr = (RingHeader*)_shmPtr;
		uint64_t serverFrames = hdr->frame_count;
		if (serverFrames == _clientFrameCount) return false;
		__sync_synchronize();
		uint64_t offset = hdr->latest_offset;
		uint32_t totalSize = hdr->latest_size;
		if (totalSize == 0 || offset + totalSize > RING_SIZE) return false;
		src = _shmPtr + RING_START + offset;
	}

	// === OPTIMIZED CLIENT DECODE  --  zero-copy into TA context, fused checksum ===
	//
	// Decode directly into flycast's TA buffer (clientCtx.tad.thd_root).
	// No intermediate std::vector. Checksum computed during decode, not after.
	// One read of the network data, one write to the TA buffer. Done.

	static TA_context clientCtx;
	static bool ctxAlloced = false;
	if (!ctxAlloced) { clientCtx.Alloc(); ctxAlloced = true; }

	uint8_t* taDst = clientCtx.tad.thd_root;  // decode target  --  flycast's own buffer

	uint32_t frameSize; memcpy(&frameSize, src, 4); src += 4;
	uint32_t frameNum; memcpy(&frameNum, src, 4); src += 4;

	// PVR registers  --  read directly into stack, write to hardware + rend_context later
	uint32_t pvr_snapshot[16];
	memcpy(pvr_snapshot, src, sizeof(pvr_snapshot)); src += sizeof(pvr_snapshot);

	uint32_t taSize; memcpy(&taSize, src, 4); src += 4;
	uint32_t deltaPayloadSize; memcpy(&deltaPayloadSize, src, 4); src += 4;

	static bool clientHasFullFrame = false;
	uint32_t clientChecksum = 0;

	if (deltaPayloadSize == taSize)
	{
		// Keyframe: copy directly into TA buffer + compute checksum in one pass
		uint32_t i = 0;
		for (; i + 3 < taSize; i += 4) {
			memcpy(taDst + i, src + i, 4);
			uint32_t w; memcpy(&w, src + i, 4);
			clientChecksum ^= w;
		}
		for (; i < taSize; i++) taDst[i] = src[i];
		src += taSize;
		clientHasFullFrame = true;
	}
	else if (!clientHasFullFrame)
	{
		src += deltaPayloadSize;
		src += 4;  // skip checksum
		return false;
	}
	else
	{
		// Delta decode: apply runs directly into TA buffer
		// taDst already holds previous frame's data (we decode in-place)
		uint8_t* dd = src;
		uint8_t* de = src + deltaPayloadSize;

		while (dd + 4 <= de) {
			uint32_t off; memcpy(&off, dd, 4); dd += 4;
			if (off == 0xFFFFFFFF) break;
			uint16_t runLen; memcpy(&runLen, dd, 2); dd += 2;
			if (off + runLen <= taSize && dd + runLen <= de)
				memcpy(taDst + off, dd, runLen);
			dd += runLen;
		}
		src += deltaPayloadSize;

		// Checksum the full TA buffer after delta apply
		for (uint32_t i = 0; i + 3 < taSize; i += 4) {
			uint32_t w; memcpy(&w, taDst + i, 4);
			clientChecksum ^= w;
		}
	}

	// Verify checksum
	uint32_t serverChecksum; memcpy(&serverChecksum, src, 4); src += 4;
	static uint32_t checksumFails = 0;
	static uint32_t checksumTotal = 0;
	checksumTotal++;
	if (clientChecksum != serverChecksum) {
		checksumFails++;
		if (checksumFails <= 10 || checksumFails % 100 == 0)
			printf("[DELTA] CHECKSUM MISMATCH frame %u (fail %u/%u)\n",
				frameNum, checksumFails, checksumTotal);
	}

	// Memory diffs  --  apply dirty pages to emulator memory. Track whether
	// any PVR-regs page was dirty so we can gate palette/fog re-upload
	// below (same optimization as the WS path above).
	bool pvrRegsDirty = false;
	uint32_t dirtyPages; memcpy(&dirtyPages, src, 4); src += 4;
	for (uint32_t d = 0; d < dirtyPages; d++) {
		uint8_t regionId = *src++;
		uint32_t pageIdx; memcpy(&pageIdx, src, 4); src += 4;
		size_t pageOff = pageIdx * MEM_PAGE_SIZE;

		if (regionId == 0 && pageOff + MEM_PAGE_SIZE <= 16 * 1024 * 1024)
			memcpy(&mem_b[pageOff], src, MEM_PAGE_SIZE);
		else if (regionId == 1 && pageOff + MEM_PAGE_SIZE <= VRAM_SIZE) {
			// Unprotect BEFORE writing  --  texture cache may have mprotect'd this page
			VramLockedWriteOffset(pageOff);
			memcpy(&vram[pageOff], src, MEM_PAGE_SIZE);
			vramDirty = true;
		}
		else if (regionId == 2 && pageOff + MEM_PAGE_SIZE <= 2 * 1024 * 1024)
			memcpy(&aica::aica_ram[pageOff], src, MEM_PAGE_SIZE);
		else if (regionId == 3 && pageOff + MEM_PAGE_SIZE <= (size_t)pvr_RegSize) {
			memcpy(pvr_regs + pageOff, src, MEM_PAGE_SIZE);
			pvrRegsDirty = true;
		}
		src += MEM_PAGE_SIZE;
	}

	// Build TA context  --  data is already in taDst, no copy needed
	if (taSize > 0) {
		clientCtx.rend.Clear();
		clientCtx.tad.Clear();
		// thd_root already has the data  --  just set the end pointer
		clientCtx.tad.thd_data = taDst + taSize;

		TA_GLOB_TILE_CLIP.full = pvr_snapshot[0];
		SCALER_CTL.full = pvr_snapshot[1];
		FB_X_CLIP.full = pvr_snapshot[2];
		FB_Y_CLIP.full = pvr_snapshot[3];
		FB_W_LINESTRIDE.full = pvr_snapshot[4];
		FB_W_SOF1 = pvr_snapshot[5];
		FB_W_CTRL.full = pvr_snapshot[6];
		FOG_CLAMP_MIN.full = pvr_snapshot[7];
		FOG_CLAMP_MAX.full = pvr_snapshot[8];

		clientCtx.rend.isRTT = pvr_snapshot[13] != 0;
		clientCtx.rend.fb_W_SOF1 = pvr_snapshot[5];
		clientCtx.rend.fb_W_CTRL.full = pvr_snapshot[6];
		clientCtx.rend.ta_GLOB_TILE_CLIP.full = pvr_snapshot[0];
		clientCtx.rend.scaler_ctl.full = pvr_snapshot[1];
		clientCtx.rend.fb_X_CLIP.full = pvr_snapshot[2];
		clientCtx.rend.fb_Y_CLIP.full = pvr_snapshot[3];
		clientCtx.rend.fb_W_LINESTRIDE = pvr_snapshot[4];
		clientCtx.rend.fog_clamp_min.full = pvr_snapshot[7];
		clientCtx.rend.fog_clamp_max.full = pvr_snapshot[8];
		{
			u32 serverW = pvr_snapshot[9];
			u32 serverH = pvr_snapshot[10];
			if (config::RenderResolution > 480 && serverH > 0) {
				float scale = config::RenderResolution / 480.f;
				serverW = (u32)(serverW * scale);
				serverH = (u32)(serverH * scale);
			}
			clientCtx.rend.framebufferWidth = serverW;
			clientCtx.rend.framebufferHeight = serverH;
		}
		clientCtx.rend.clearFramebuffer = pvr_snapshot[11] != 0;
		float fz; memcpy(&fz, &pvr_snapshot[12], 4);
		clientCtx.rend.fZ_max = fz;

		if (vramDirty)
			renderer->resetTextureCache = true;

		// Gate palette + fog re-upload on actual PVR-regs changes
		// (same fix as the WS path above). Palette RAM and fog LUT
		// both live in PVR regs; if none of those pages changed this
		// frame, there is nothing to re-upload. Running these every
		// frame unconditionally was the root cause of the ~18 ms decode
		// budget on the client render thread.
		if (pvrRegsDirty) {
			::pal_needs_update = true;
			palette_update();
			renderer->updatePalette = true;
			renderer->updateFogTable = true;
		}

		renderer->Process(&clientCtx);
		rc = clientCtx.rend;
	}

	if (!_useWebSocket) {
		RingHeader* hdr = (RingHeader*)_shmPtr;
		_clientFrameCount = hdr->frame_count;

		// Check VRAM every 60 frames  --  reset texture cache if drifted
		if (frameNum % 60 == 0)
		{
			uint64_t clientHash = fastVramHash();
			uint64_t serverHash = hdr->server_vram_hash;
			if (clientHash != serverHash)
				renderer->resetTextureCache = true;
		}
	}

	int64_t t1 = _clientNowUs();

	static int64_t totalDecodeUs = 0;
	static uint32_t decodeCount = 0;
	const int64_t thisDecodeUs = t1 - t0;
	totalDecodeUs += thisDecodeUs;
	decodeCount++;

	// Publish to the debug-overlay telemetry atomics. EMA on decode time
	// uses the same 1/16 alpha shape as the arrival interval EMA.
	_clientLastDecodeUs.store(thisDecodeUs, std::memory_order_relaxed);
	{
		const int64_t prev = _clientDecodeEmaUs.load(std::memory_order_relaxed);
		_clientDecodeEmaUs.store(prev + ((thisDecodeUs - prev) >> 4),
		                         std::memory_order_relaxed);
	}
	_clientLastDirtyPages.store(dirtyPages, std::memory_order_relaxed);
	_clientLastTaSize.store(taSize, std::memory_order_relaxed);
	_clientLastVramDirty.store(vramDirty, std::memory_order_relaxed);

	if (frameNum % 600 == 0)
		printf("[MIRROR] Client frame %u | delta=%u bytes | dirty=%u pages | decode=%lldÂµs avg=%lldÂµs | %s\n",
			frameNum, deltaPayloadSize, dirtyPages,
			(long long)thisDecodeUs, (long long)(totalDecodeUs / decodeCount),
			_useWebSocket ? "WS" : "SHM");

	return taSize > 0;
}

}  // namespace maplecast_mirror
