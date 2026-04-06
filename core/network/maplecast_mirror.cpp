/*
	MapleCast Mirror v3 — stream TA command buffers + memory diffs.

	Instead of streaming pre-parsed rend_context (which loses texture resolution),
	stream the RAW TA command buffer. The client runs ta_parse() on it, which
	builds rend_context AND resolves textures from VRAM — exactly like flycast
	normally works.

	Server: each frame, captures the TA command buffer + PVR registers + memory diffs
	Client: loads server sync state, then applies diffs + feeds TA commands to renderer
*/
#include "types.h"
#include "maplecast_mirror.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/aica/aica_if.h"
#include "rend/gles/gles.h"
#include "rend/TexCache.h"
#include "serialize.h"
#include "emulator.h"
#include "hw/mem/mem_watch.h"
#include "maplecast_ws_server.h"
#include "maplecast_compress.h"
#include "rend/texconv.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>
#include <mutex>
#include <thread>
#include <deque>
#include <chrono>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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

static bool openShm(bool create)
{
	if (create) shm_unlink(SHM_NAME);
	_shmFd = shm_open(SHM_NAME, create ? (O_CREAT | O_RDWR) : O_RDWR, 0666);
	if (_shmFd < 0) { printf("[MIRROR] shm_open failed\n"); return false; }
	if (create) ftruncate(_shmFd, SHM_SIZE);
	_shmPtr = (uint8_t*)mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, _shmFd, 0);
	if (_shmPtr == MAP_FAILED) { _shmPtr = nullptr; return false; }
	if (create) memset(_shmPtr, 0, SHM_SIZE);
	return true;
}

static void initRegions()
{
	_numRegions = 0;

	// SKIP RAM — renderer doesn't read from main RAM
	// SKIP ARAM — audio RAM not needed for rendering
	// ONLY diff VRAM (textures) and PVR regs (palette, fog, hardware state)

	_shadowVRAM = (uint8_t*)malloc(VRAM_SIZE);
	memcpy(_shadowVRAM, &vram[0], VRAM_SIZE);
	_regions[_numRegions++] = { &vram[0], _shadowVRAM, VRAM_SIZE, 1, "VRAM" };

	// PVR registers: 32KB — palette RAM, FOG_TABLE, ISP_FEED_CFG
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

// Double-buffered TA for zero-copy delta (replaces std::vector prevTA)
static uint8_t* _taBuf[2] = { nullptr, nullptr };
static uint32_t _taBufSize[2] = { 0, 0 };
static int _taCur = 0;
static bool _taHasPrev = false;
static MirrorCompressor _compressor;

void initServer()
{
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

	// Start lightweight WebSocket server — no CUDA, no NVENC
	int wsPort = 7200;
	const char* portEnv = std::getenv("MAPLECAST_SERVER_PORT");
	if (portEnv) wsPort = std::atoi(portEnv);
	maplecast_ws::init(wsPort);

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
	// loadstate re-protects VRAM — unprotect so our memcpy patches work
	memwatch::unprotect();
	printf("[MIRROR] Loaded server sync state: %.1f MB\n", size / (1024.0*1024.0));
}

static void initClientWebSocket();  // forward declaration

void initClient()
{
	// Use WebSocket if MAPLECAST_SERVER_HOST is set, or shm_open fails
	if (std::getenv("MAPLECAST_SERVER_HOST") || !openShm(false)) {
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
		// Direct memory copy instead of emu.loadstate — avoids corrupting scheduler/interrupt state
		uint8_t* snap = _shmPtr + HEADER_SIZE;
		size_t off = 0;
		memcpy(&mem_b[0], snap + off, 16 * 1024 * 1024); off += 16 * 1024 * 1024;
		memcpy(&vram[0], snap + off, VRAM_SIZE); off += VRAM_SIZE;
		memcpy(&aica::aica_ram[0], snap + off, 2 * 1024 * 1024);
		// Also copy PVR regs from the server's current state
		// (they're diffed per-frame anyway, but this gives us a clean start)

		memwatch::unprotect();
		renderer->resetTextureCache = true;
		pal_needs_update = true;
		palette_update();
		renderer->updatePalette = true;
		renderer->updateFogTable = true;

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

// Double-buffered TA contexts — background decodes into one, render reads the other
static TA_context _decodeTaCtx[2];
static bool _decodeTaAlloced = false;
static int _decodeIdx = 0;  // which buffer background thread writes to
static bool _decodeHasFullFrame = false;

// Decoded frame metadata — written by background thread, read by render thread
struct DecodedFrame {
	uint32_t frameNum;
	uint32_t pvr_snapshot[16];
	uint32_t taSize;
	int taBufferIdx;  // which _decodeTaCtx[] has the TA data
	uint32_t dirtyCount;
	struct { uint8_t regionId; uint32_t pageIdx; uint8_t data[4096]; } pages[128];
	bool vramDirty;
};
static DecodedFrame _decoded;
static std::atomic<bool> _decodedReady{false};

// Raw TCP WebSocket client — bypasses websocketpp/asio resolver entirely
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
	if (send(fd, req, len, 0) != len) return false;

	// Read HTTP response
	char resp[1024];
	int total = 0;
	while (total < (int)sizeof(resp) - 1) {
		int n = recv(fd, resp + total, 1, 0);
		if (n <= 0) return false;
		total += n;
		if (total >= 4 && memcmp(resp + total - 4, "\r\n\r\n", 4) == 0) break;
	}
	resp[total] = 0;
	return strstr(resp, "101") != nullptr;
}

static bool wsReadFrame(int fd, std::vector<uint8_t>& out)
{
	// Read WebSocket frame header (2 bytes min)
	uint8_t hdr[2];
	if (recv(fd, hdr, 2, MSG_WAITALL) != 2) return false;

	bool fin = (hdr[0] & 0x80) != 0;
	int opcode = hdr[0] & 0x0F;
	bool masked = (hdr[1] & 0x80) != 0;
	uint64_t payloadLen = hdr[1] & 0x7F;

	if (payloadLen == 126) {
		uint8_t ext[2];
		if (recv(fd, ext, 2, MSG_WAITALL) != 2) return false;
		payloadLen = (ext[0] << 8) | ext[1];
	} else if (payloadLen == 127) {
		uint8_t ext[8];
		if (recv(fd, ext, 8, MSG_WAITALL) != 8) return false;
		payloadLen = 0;
		for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | ext[i];
	}

	// Skip mask key if present (server→client should not be masked)
	if (masked) {
		uint8_t mask[4];
		if (recv(fd, mask, 4, MSG_WAITALL) != 4) return false;
	}

	// Read payload
	out.resize(payloadLen);
	size_t read = 0;
	while (read < payloadLen) {
		ssize_t n = recv(fd, out.data() + read, payloadLen - read, 0);
		if (n <= 0) return false;
		read += n;
	}

	// Handle close/ping/text
	if (opcode == 0x8) return false;  // close
	if (opcode == 0x9) { out.clear(); return true; }  // ping — ignore
	if (opcode == 0x1) { out.clear(); return true; }  // text (JSON status) — ignore

	return fin && opcode == 0x2;  // binary frame
}

static void wsClientRun(std::string host, int port)
{
	printf("[MIRROR-WS] Connecting to %s:%d...\n", host.c_str(), port); fflush(stdout);

	_wsFd = socket(AF_INET, SOCK_STREAM, 0);
	if (_wsFd < 0) { printf("[MIRROR-WS] socket() failed\n"); return; }

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

	if (connect(_wsFd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		printf("[MIRROR-WS] connect() failed: %s\n", strerror(errno));
		close(_wsFd); _wsFd = -1; return;
	}
	printf("[MIRROR-WS] TCP connected\n"); fflush(stdout);

	if (!wsHandshake(_wsFd, host.c_str(), port)) {
		printf("[MIRROR-WS] WebSocket handshake failed\n");
		close(_wsFd); _wsFd = -1; return;
	}
	printf("[MIRROR-WS] WebSocket handshake OK — waiting for initial sync\n"); fflush(stdout);

	if (!_decodeTaAlloced) {
		_decodeTaCtx[0].Alloc();
		_decodeTaCtx[1].Alloc();
		_decodeTaAlloced = true;
	}
	_decodeIdx = 0;

	// zstd decompressor — reused across all frames
	MirrorDecompressor decomp;
	decomp.init(16 * 1024 * 1024);  // 16MB covers SYNC + worst-case frames

	// Wait for initial SYNC message (VRAM + PVR regs)
	bool synced = false;
	std::vector<uint8_t> frame;
	while (!synced) {
		if (!wsReadFrame(_wsFd, frame)) {
			printf("[MIRROR-WS] Connection lost waiting for sync\n"); fflush(stdout);
			close(_wsFd); _wsFd = -1; decomp.destroy(); return;
		}
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
			}
			// Unprotect VRAM so per-frame memcpy patches work (nvmem page protection)
			memwatch::unprotect();
			// NOTE: renderer cache/palette updates happen on render thread in clientReceive()

			synced = true;
			printf("[MIRROR-WS] Initial sync received: %.1f MB (%.1f MB compressed) — VRAM + PVR loaded\n",
				decompSize / (1024.0 * 1024.0), frame.size() / (1024.0 * 1024.0));
			fflush(stdout);
		}
	}

	while (true) {
		if (!wsReadFrame(_wsFd, frame)) {
			printf("[MIRROR-WS] Connection lost\n"); fflush(stdout);
			break;
		}
		// Decompress if needed
		size_t decompSize = 0;
		const uint8_t* decompData = decomp.decompress(frame.data(), frame.size(), decompSize);
		if (decompSize < 80) continue;

		const uint8_t* src = decompData;
		uint32_t frameSize; memcpy(&frameSize, src, 4); src += 4;
		uint32_t frameNum; memcpy(&frameNum, src, 4); src += 4;

		uint32_t pvr_snap[16];
		memcpy(pvr_snap, src, sizeof(pvr_snap)); src += sizeof(pvr_snap);

		uint32_t taSize; memcpy(&taSize, src, 4); src += 4;
		uint32_t deltaPayloadSize; memcpy(&deltaPayloadSize, src, 4); src += 4;

		// Sanity check — TA buffers are ~50-300KB, never megabytes
		if (taSize > 512 * 1024 || deltaPayloadSize > 512 * 1024 ||
		    frameSize > decompSize) {
			printf("[MIRROR-WS] BAD FRAME: taSize=%u delta=%u frameSize=%u bufSize=%zu — skipping\n",
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

		// Skip checksum — TCP guarantees data integrity, checksum was for shm race detection
		// (commented out, not deleted — can re-enable for debugging)
		// uint32_t serverChecksum; memcpy(&serverChecksum, src, 4);
		src += 4;

		// Stage dirty pages — copy page data so render thread can apply safely
		uint32_t dirtyCount; memcpy(&dirtyCount, src, 4); src += 4;
		if (dirtyCount > 128) dirtyCount = 128;

		DecodedFrame df;
		df.frameNum = frameNum;
		memcpy(df.pvr_snapshot, pvr_snap, sizeof(pvr_snap));
		df.taSize = taSize;
		df.dirtyCount = dirtyCount;
		df.vramDirty = false;

		for (uint32_t d = 0; d < dirtyCount; d++) {
			df.pages[d].regionId = *src++;
			memcpy(&df.pages[d].pageIdx, src, 4); src += 4;
			memcpy(df.pages[d].data, src, MEM_PAGE_SIZE); src += MEM_PAGE_SIZE;
			if (df.pages[d].regionId == 1) df.vramDirty = true;
		}

		// Publish — render thread picks it up
		// Store which TA buffer index this frame was decoded into
		df.taBufferIdx = _decodeIdx;
		_decoded = df;
		_decodedReady.store(true, std::memory_order_release);

		// Swap to other buffer for next frame's decode
		// Render thread reads buffer [df.taBufferIdx], we write to the other one
		_decodeIdx = 1 - _decodeIdx;

		uint32_t n = _wsFramesReceived.fetch_add(1, std::memory_order_relaxed);
		if (n == 0) printf("[MIRROR-WS] First frame decoded\n");
		if (n > 0 && n % 300 == 0) printf("[MIRROR-WS] %u frames decoded\n", n);
	}

	close(_wsFd); _wsFd = -1;
	decomp.destroy();
}

static void initClientWebSocket()
{
	_isClient = true;
	_useWebSocket = true;

	const char* host = std::getenv("MAPLECAST_SERVER_HOST");
	if (!host) host = "127.0.0.1";
	const char* portStr = std::getenv("MAPLECAST_SERVER_PORT");
	int port = portStr ? std::atoi(portStr) : 7200;

	printf("[MIRROR] === CLIENT MODE (WebSocket) === ws://%s:%d/\n", host, port);

	// Unprotect VRAM BEFORE spawning WS thread — the thread will memcpy into
	// VRAM pages during SYNC and per-frame diffs. Without this, nvmem page
	// protection causes SIGSEGV on the first VRAM write.
	memwatch::unprotect();

	std::string hostStr(host);
	_wsThread = std::thread(wsClientRun, hostStr, port);
	_wsThread.detach();
}

bool isServer() { return _isServer; }
bool isClient() { return _isClient; }

// ==================== SERVER: publish TA commands + memory diffs ====================

void serverPublish(TA_context* ctx)
{
	if (!_isServer || !_shmPtr || !ctx) return;
	auto publishStart = std::chrono::high_resolution_clock::now();
	rend_context& rc = ctx->rend;
	// DON'T skip RTT frames — MVC2 renders character sprites via render-to-texture!

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
	// These set up the rend_context hardware params
	uint32_t pvr_snapshot[16];
	pvr_snapshot[0] = TA_GLOB_TILE_CLIP.full;
	pvr_snapshot[1] = SCALER_CTL.full;
	pvr_snapshot[2] = FB_X_CLIP.full;
	pvr_snapshot[3] = FB_Y_CLIP.full;
	pvr_snapshot[4] = FB_W_LINESTRIDE.full;
	pvr_snapshot[5] = FB_W_SOF1;
	pvr_snapshot[6] = FB_W_CTRL.full;
	pvr_snapshot[7] = FOG_CLAMP_MIN.full;
	pvr_snapshot[8] = FOG_CLAMP_MAX.full;
	pvr_snapshot[9] = rc.framebufferWidth;
	pvr_snapshot[10] = rc.framebufferHeight;
	pvr_snapshot[11] = rc.clearFramebuffer ? 1 : 0;
	float fz = rc.fZ_max;
	memcpy(&pvr_snapshot[12], &fz, 4);
	pvr_snapshot[13] = rc.isRTT ? 1 : 0;
	memcpy(dst, pvr_snapshot, sizeof(pvr_snapshot)); dst += sizeof(pvr_snapshot);

	// === Raw TA command buffer — double-buffered delta ===
	uint32_t taSize = (uint32_t)(ctx->tad.thd_data - ctx->tad.thd_root);
	uint8_t* taData = ctx->tad.thd_root;

	int cur = _taCur;
	int prev = 1 - cur;
	uint8_t* prevData = _taBuf[prev];
	uint32_t prevSize = _taBufSize[prev];

	// Copy current TA into double buffer
	memcpy(_taBuf[cur], taData, taSize);
	_taBufSize[cur] = taSize;

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

			uint16_t runLen = (uint16_t)(i - runStart);
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

	// Checksum disabled — client skips it, TCP guarantees integrity
	// uint32_t taChecksum = 0;
	// for (uint32_t i = 0; i < taSize; i += 4)
	// 	taChecksum ^= *(uint32_t*)(taData + i);
	uint32_t taChecksum = 0;  // placeholder — client expects 4 bytes here
	memcpy(dst, &taChecksum, 4); dst += 4;

	// === Memory diffs ===
	uint32_t totalDirty = 0;
	uint8_t* dirtyCountPtr = dst;
	dst += 4;

	// VRAM + PVR regs: memcmp against shadow copies
	for (int r = 0; r < _numRegions; r++) {
		MemRegion& reg = _regions[r];
		size_t numPages = reg.size / MEM_PAGE_SIZE;
		for (size_t p = 0; p < numPages; p++) {
			size_t off = p * MEM_PAGE_SIZE;
			if (memcmp(reg.ptr + off, reg.shadow + off, MEM_PAGE_SIZE) != 0) {
				if ((size_t)(dst - dstStart) + 5 + MEM_PAGE_SIZE > RING_SIZE / 3)
					goto done_diff;
				*dst++ = reg.id;
				uint32_t pi = (uint32_t)p;
				memcpy(dst, &pi, 4); dst += 4;
				memcpy(dst, reg.ptr + off, MEM_PAGE_SIZE); dst += MEM_PAGE_SIZE;
				memcpy(reg.shadow + off, reg.ptr + off, MEM_PAGE_SIZE);
				totalDirty++;
			}
		}
	}
done_diff:
	memcpy(dirtyCountPtr, &totalDirty, 4);

	// Patch frame size
	uint32_t totalSize = (uint32_t)(dst - dstStart);
	uint32_t frameSizeVal = totalSize - 4;
	memcpy(dstStart, &frameSizeVal, 4);

	__sync_synchronize();
	hdr->latest_offset = writePos;
	hdr->latest_size = totalSize;
	hdr->write_pos = writePos + totalSize;
	hdr->frame_count++;

	// Compress + broadcast over WebSocket to browser clients
	uint64_t compressUs = 0;
	uint32_t compressedSize = totalSize;
	if (maplecast_ws::active())
	{
		size_t compSize = 0;
		const uint8_t* compData = _compressor.compress(dstStart, totalSize, compSize, compressUs);
		maplecast_ws::broadcastBinary(compData, compSize);
		compressedSize = (uint32_t)compSize;
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
		hdr->sync_ready = 1;
		printf("[MIRROR] Client requested sync — fresh state + TA reset\n");
	}

	// Brain snapshot disabled — was 26MB memcpy every 30 frames (~5ms stall)
	// Only needed for shm client initial sync. WebSocket clients use save state instead.
	// if (frameNum % 30 == 0)
	// {
	// 	uint8_t* snap = _shmPtr + HEADER_SIZE;
	// 	size_t off = 0;
	// 	memcpy(snap + off, &mem_b[0], 16 * 1024 * 1024); off += 16 * 1024 * 1024;
	// 	memcpy(snap + off, &vram[0], VRAM_SIZE); off += VRAM_SIZE;
	// 	memcpy(snap + off, &aica::aica_ram[0], 2 * 1024 * 1024);
	// }

	// VRAM hash disabled — only used by shm client for drift detection
	// hdr->server_vram_hash = fastVramHash();

	// Audit disabled — reduced to VRAM+PVR only

	if (frameNum % 600 == 0)
		printf("[MIRROR] Server frame %u | TA=%u bytes | %u dirty pages | %u->%u bytes (%.1fx) zstd %luus\n",
			frameNum, taSize, totalDirty, totalSize, compressedSize,
			compressedSize > 0 ? (double)totalSize / compressedSize : 0.0, compressUs);
}

// ==================== CLIENT: receive TA commands + diffs, run ta_parse ====================

static int64_t _clientNowUs() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

bool clientReceive(rend_context& rc, bool& vramDirty)
{
	vramDirty = false;
	if (!_isClient) return false;
	int64_t t0 = _clientNowUs();

	if (_useWebSocket)
	{
		// Pipelined: background thread already decoded TA + staged dirty pages
		if (!_decodedReady.load(std::memory_order_acquire)) return false;
		_decodedReady.store(false, std::memory_order_relaxed);

		DecodedFrame& df = _decoded;
		TA_context& ctx = _decodeTaCtx[df.taBufferIdx];
		uint8_t* taDst = ctx.tad.thd_root;

		// Apply dirty pages to emulator memory (must happen on render thread)
		for (uint32_t d = 0; d < df.dirtyCount; d++) {
			size_t pageOff = df.pages[d].pageIdx * MEM_PAGE_SIZE;
			uint8_t rid = df.pages[d].regionId;

			if (rid == 0 && pageOff + MEM_PAGE_SIZE <= 16 * 1024 * 1024)
				memcpy(&mem_b[pageOff], df.pages[d].data, MEM_PAGE_SIZE);
			else if (rid == 1 && pageOff + MEM_PAGE_SIZE <= VRAM_SIZE) {
				// Unprotect BEFORE writing — texture cache may have mprotect'd this page
				VramLockedWriteOffset(pageOff);
				memcpy(&vram[pageOff], df.pages[d].data, MEM_PAGE_SIZE);
				vramDirty = true;
			}
			else if (rid == 2 && pageOff + MEM_PAGE_SIZE <= 2 * 1024 * 1024)
				memcpy(&aica::aica_ram[pageOff], df.pages[d].data, MEM_PAGE_SIZE);
			else if (rid == 3 && pageOff + MEM_PAGE_SIZE <= (size_t)pvr_RegSize)
				memcpy(pvr_regs + pageOff, df.pages[d].data, MEM_PAGE_SIZE);
		}

		// Render — TA data already decoded in ctx.tad.thd_root by background thread
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
			ctx.rend.framebufferWidth = df.pvr_snapshot[9];
			ctx.rend.framebufferHeight = df.pvr_snapshot[10];
			ctx.rend.clearFramebuffer = df.pvr_snapshot[11] != 0;
			float fz; memcpy(&fz, &df.pvr_snapshot[12], 4);
			ctx.rend.fZ_max = fz;

			if (vramDirty) renderer->resetTextureCache = true;
			::pal_needs_update = true;
			palette_update();
			renderer->updatePalette = true;
			renderer->updateFogTable = true;

			renderer->Process(&ctx);
			rc = ctx.rend;
		}

		int64_t t1 = _clientNowUs();
		static int64_t totalUs = 0;
		static uint32_t count = 0;
		totalUs += (t1 - t0);
		count++;
		if (df.frameNum % 600 == 0)
			printf("[MIRROR] Client frame %u | dirty=%u pages | render=%lldµs avg=%lldµs | WS-PIPELINE\n",
				df.frameNum, df.dirtyCount, (long long)(t1 - t0), (long long)(totalUs / count));

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

	// === OPTIMIZED CLIENT DECODE — zero-copy into TA context, fused checksum ===
	//
	// Decode directly into flycast's TA buffer (clientCtx.tad.thd_root).
	// No intermediate std::vector. Checksum computed during decode, not after.
	// One read of the network data, one write to the TA buffer. Done.

	static TA_context clientCtx;
	static bool ctxAlloced = false;
	if (!ctxAlloced) { clientCtx.Alloc(); ctxAlloced = true; }

	uint8_t* taDst = clientCtx.tad.thd_root;  // decode target — flycast's own buffer

	uint32_t frameSize; memcpy(&frameSize, src, 4); src += 4;
	uint32_t frameNum; memcpy(&frameNum, src, 4); src += 4;

	// PVR registers — read directly into stack, write to hardware + rend_context later
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

	// Memory diffs — apply dirty pages to emulator memory
	uint32_t dirtyPages; memcpy(&dirtyPages, src, 4); src += 4;
	for (uint32_t d = 0; d < dirtyPages; d++) {
		uint8_t regionId = *src++;
		uint32_t pageIdx; memcpy(&pageIdx, src, 4); src += 4;
		size_t pageOff = pageIdx * MEM_PAGE_SIZE;

		if (regionId == 0 && pageOff + MEM_PAGE_SIZE <= 16 * 1024 * 1024)
			memcpy(&mem_b[pageOff], src, MEM_PAGE_SIZE);
		else if (regionId == 1 && pageOff + MEM_PAGE_SIZE <= VRAM_SIZE) {
			// Unprotect BEFORE writing — texture cache may have mprotect'd this page
			VramLockedWriteOffset(pageOff);
			memcpy(&vram[pageOff], src, MEM_PAGE_SIZE);
			vramDirty = true;
		}
		else if (regionId == 2 && pageOff + MEM_PAGE_SIZE <= 2 * 1024 * 1024)
			memcpy(&aica::aica_ram[pageOff], src, MEM_PAGE_SIZE);
		else if (regionId == 3 && pageOff + MEM_PAGE_SIZE <= (size_t)pvr_RegSize)
			memcpy(pvr_regs + pageOff, src, MEM_PAGE_SIZE);
		src += MEM_PAGE_SIZE;
	}

	// Build TA context — data is already in taDst, no copy needed
	if (taSize > 0) {
		clientCtx.rend.Clear();
		clientCtx.tad.Clear();
		// thd_root already has the data — just set the end pointer
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
		clientCtx.rend.framebufferWidth = pvr_snapshot[9];
		clientCtx.rend.framebufferHeight = pvr_snapshot[10];
		clientCtx.rend.clearFramebuffer = pvr_snapshot[11] != 0;
		float fz; memcpy(&fz, &pvr_snapshot[12], 4);
		clientCtx.rend.fZ_max = fz;

		if (vramDirty)
			renderer->resetTextureCache = true;

		::pal_needs_update = true;
		palette_update();
		renderer->updatePalette = true;
		renderer->updateFogTable = true;

		renderer->Process(&clientCtx);
		rc = clientCtx.rend;
	}

	if (!_useWebSocket) {
		RingHeader* hdr = (RingHeader*)_shmPtr;
		_clientFrameCount = hdr->frame_count;

		// Check VRAM every 60 frames — reset texture cache if drifted
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
	totalDecodeUs += (t1 - t0);
	decodeCount++;

	if (frameNum % 600 == 0)
		printf("[MIRROR] Client frame %u | delta=%u bytes | dirty=%u pages | decode=%lldµs avg=%lldµs | %s\n",
			frameNum, deltaPayloadSize, dirtyPages,
			(long long)(t1 - t0), (long long)(totalDecodeUs / decodeCount),
			_useWebSocket ? "WS" : "SHM");

	return taSize > 0;
}

}  // namespace maplecast_mirror
