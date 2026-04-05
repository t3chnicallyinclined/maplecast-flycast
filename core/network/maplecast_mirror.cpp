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
#include "maplecast_stream.h"
#include "rend/texconv.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>
#include <mutex>
#include <thread>
#include <deque>
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

	// Handle close/ping
	if (opcode == 0x8) return false;  // close
	if (opcode == 0x9) return true;   // ping — ignore for now
	if (opcode == 0x1) return true;   // text — ignore

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
	printf("[MIRROR-WS] WebSocket handshake OK — receiving frames\n"); fflush(stdout);

	// Read loop
	std::vector<uint8_t> frame;
	while (true) {
		if (!wsReadFrame(_wsFd, frame)) {
			printf("[MIRROR-WS] Connection lost\n"); fflush(stdout);
			break;
		}
		if (frame.size() < 80) continue;  // skip text/small frames

		{
			std::lock_guard<std::mutex> lock(_frameMutex);
			while (_frameQueue.size() >= 2)
				_frameQueue.pop_front();
			_frameQueue.push_back(std::move(frame));
			frame.clear();
		}
		uint32_t n = _wsFramesReceived.fetch_add(1, std::memory_order_relaxed);
		if (n == 0) printf("[MIRROR-WS] First binary frame received\n");
		if (n > 0 && n % 300 == 0) printf("[MIRROR-WS] %u frames received\n", n);
	}

	close(_wsFd); _wsFd = -1;
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

	// Checksum
	uint32_t taChecksum = 0;
	for (uint32_t i = 0; i < taSize; i += 4)
		taChecksum ^= *(uint32_t*)(taData + i);
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

	// Also broadcast over WebSocket to browser clients
	if (maplecast_stream::active())
		maplecast_stream::broadcastBinary(dstStart, totalSize);

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

	// Write full brain snapshot every 30 frames for late-joining clients
	if (frameNum % 30 == 0)
	{
		uint8_t* snap = _shmPtr + HEADER_SIZE;  // brain snapshot area
		size_t off = 0;
		memcpy(snap + off, &mem_b[0], 16 * 1024 * 1024); off += 16 * 1024 * 1024;
		memcpy(snap + off, &vram[0], VRAM_SIZE); off += VRAM_SIZE;
		memcpy(snap + off, &aica::aica_ram[0], 2 * 1024 * 1024);
	}

	// Publish memory hashes for client verification
	hdr->server_vram_hash = fastVramHash();

	// Audit disabled — reduced to VRAM+PVR only

	if (frameNum % 600 == 0)
		printf("[MIRROR] Server frame %u | TA=%u bytes | %u dirty pages\n",
			frameNum, taSize, totalDirty);
}

// ==================== CLIENT: receive TA commands + diffs, run ta_parse ====================

bool clientReceive(rend_context& rc, bool& vramDirty)
{
	vramDirty = false;
	if (!_isClient) return false;

	// Get frame data — either from WebSocket queue or shm
	std::vector<uint8_t> wsFrame;  // holds WebSocket data if used
	uint8_t* src = nullptr;

	if (_useWebSocket)
	{
		std::lock_guard<std::mutex> lock(_frameMutex);
		if (_frameQueue.empty()) return false;
		wsFrame = std::move(_frameQueue.back());
		_frameQueue.clear();
		src = wsFrame.data();
	}
	else
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

	uint32_t frameSize; memcpy(&frameSize, src, 4); src += 4;
	uint32_t frameNum; memcpy(&frameNum, src, 4); src += 4;

	// === PVR registers ===
	uint32_t pvr_snapshot[16];
	memcpy(pvr_snapshot, src, sizeof(pvr_snapshot)); src += sizeof(pvr_snapshot);

	// === TA command buffer (delta encoded) ===
	uint32_t taSize; memcpy(&taSize, src, 4); src += 4;
	uint32_t deltaPayloadSize; memcpy(&deltaPayloadSize, src, 4); src += 4;

	// Client's persistent TA buffer for delta reconstruction
	static std::vector<uint8_t> clientTA;

	static bool clientHasFullFrame = false;

	if (deltaPayloadSize == taSize)
	{
		// Full frame (first frame or size changed)
		clientTA.assign(src, src + taSize);
		src += taSize;
		clientHasFullFrame = true;
	}
	else if (!clientHasFullFrame)
	{
		// Haven't received a full frame yet — skip this delta
		src += deltaPayloadSize;
		// Skip checksum too
		src += 4;
		return false;
	}
	else
	{
		// Delta decode: apply changed runs to previous TA buffer
		if (clientTA.size() < taSize)
			clientTA.resize(taSize, 0);
		else if (clientTA.size() > taSize)
			clientTA.resize(taSize);
		uint8_t* deltaData = src;
		uint8_t* deltaEnd = src + deltaPayloadSize;

		while (deltaData + 4 <= deltaEnd)
		{
			uint32_t offset;
			memcpy(&offset, deltaData, 4); deltaData += 4;
			if (offset == 0xFFFFFFFF) break;  // terminator

			uint16_t runLen;
			memcpy(&runLen, deltaData, 2); deltaData += 2;
			if (offset + runLen <= taSize && deltaData + runLen <= deltaEnd)
				memcpy(clientTA.data() + offset, deltaData, runLen);
			deltaData += runLen;
		}
		src += deltaPayloadSize;
	}

	// Read server's checksum
	uint32_t serverChecksum;
	memcpy(&serverChecksum, src, 4); src += 4;

	uint8_t* taData = clientTA.data();

	// Verify reconstruction
	uint32_t clientChecksum = 0;
	for (uint32_t i = 0; i + 3 < taSize; i += 4)
		clientChecksum ^= *(uint32_t*)(taData + i);

	static uint32_t checksumFails = 0;
	static uint32_t checksumTotal = 0;
	checksumTotal++;
	if (clientChecksum != serverChecksum)
	{
		checksumFails++;
		// Corruption detected — request full frame next time
		// For now, log it
		if (checksumFails <= 10 || checksumFails % 100 == 0)
			printf("[DELTA] CHECKSUM MISMATCH frame %u (fail %u/%u) delta=%s\n",
				frameNum, checksumFails, checksumTotal,
				deltaPayloadSize == taSize ? "FULL" : "DELTA");
	}

	// === Memory diffs ===
	uint32_t dirtyPages; memcpy(&dirtyPages, src, 4); src += 4;

	for (uint32_t d = 0; d < dirtyPages; d++) {
		uint8_t regionId = *src++;
		uint32_t pageIdx; memcpy(&pageIdx, src, 4); src += 4;
		size_t pageOff = pageIdx * MEM_PAGE_SIZE;

		if (regionId == 0 && pageOff + MEM_PAGE_SIZE <= 16 * 1024 * 1024)
			memcpy(&mem_b[pageOff], src, MEM_PAGE_SIZE);
		else if (regionId == 1 && pageOff + MEM_PAGE_SIZE <= VRAM_SIZE) {
			memcpy(&vram[pageOff], src, MEM_PAGE_SIZE);
			VramLockedWriteOffset(pageOff);
			vramDirty = true;
		}
		else if (regionId == 2 && pageOff + MEM_PAGE_SIZE <= 2 * 1024 * 1024)
			memcpy(&aica::aica_ram[pageOff], src, MEM_PAGE_SIZE);
		else if (regionId == 3 && pageOff + MEM_PAGE_SIZE <= (size_t)pvr_RegSize)
			memcpy(pvr_regs + pageOff, src, MEM_PAGE_SIZE);
		src += MEM_PAGE_SIZE;
	}

	// === Build TA context and run ta_parse ===
	if (taSize > 0) {
		// Get or create a TA context
		static TA_context clientCtx;
		static bool ctxAlloced = false;
		if (!ctxAlloced) { clientCtx.Alloc(); ctxAlloced = true; }

		// Reset for new frame
		clientCtx.rend.Clear();
		clientCtx.tad.Clear();

		// Copy TA commands into the context's buffer
		memcpy(clientCtx.tad.thd_root, taData, taSize);
		clientCtx.tad.thd_data = clientCtx.tad.thd_root + taSize;

		// Set PVR register values that rend_start_render normally reads
		TA_GLOB_TILE_CLIP.full = pvr_snapshot[0];
		SCALER_CTL.full = pvr_snapshot[1];
		FB_X_CLIP.full = pvr_snapshot[2];
		FB_Y_CLIP.full = pvr_snapshot[3];
		FB_W_LINESTRIDE.full = pvr_snapshot[4];
		FB_W_SOF1 = pvr_snapshot[5];
		FB_W_CTRL.full = pvr_snapshot[6];
		FOG_CLAMP_MIN.full = pvr_snapshot[7];
		FOG_CLAMP_MAX.full = pvr_snapshot[8];

		// Set up rend_context hardware params (same as rend_start_render)
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

		// If VRAM changed, force texture cache reset so ta_parse re-decodes
		if (vramDirty)
			renderer->resetTextureCache = true;

		// Force palette update — palette_update() converts PALETTE_RAM to palette32_ram
		// Normally called by rend_start_render() which we skip on the client
		::pal_needs_update = true;
		palette_update();

		// Also force palette texture upload
		renderer->updatePalette = true;
		renderer->updateFogTable = true;

		// Run Process — this calls ta_parse which builds rend_context
		// AND resolves textures from VRAM via GetTexture()
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

	if (frameNum % 600 == 0)
		printf("[MIRROR] Client frame %u | delta=%u bytes | dirty=%u pages\n",
			frameNum, deltaPayloadSize, dirtyPages);

	return taSize > 0;
}

}  // namespace maplecast_mirror
