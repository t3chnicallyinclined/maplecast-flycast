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
#include "maplecast_lookup_test.h"
#include "maplecast_stream.h"
#include "rend/texconv.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

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

	// Re-snapshot shadows to match the sync state we just saved
	// This way, the first diff a client sees covers ALL changes since sync point
	for (int i = 0; i < _numRegions; i++)
		memcpy(_regions[i].shadow, _regions[i].ptr, _regions[i].size);

	printf("[MIRROR] === SERVER MODE === streaming TA commands + memory diffs\n");
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

void initClient()
{
	if (!openShm(false)) return;
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

	// === Raw TA command buffer ===
	uint32_t taSize = (uint32_t)(ctx->tad.thd_data - ctx->tad.thd_root);
	uint8_t* taData = ctx->tad.thd_root;

	// Delta encode TA commands against previous frame
	// Format: originalSize(u32) + deltaPayloadSize(u32) + XOR delta (only changed bytes)
	// If sizes differ or no previous frame: send full TA (deltaPayloadSize == originalSize)
	static std::vector<uint8_t> prevTA;
	static uint64_t totalDeltaPayload = 0;
	static uint64_t totalTABytes = 0;
	static uint32_t deltaFrames = 0;

	memcpy(dst, &taSize, 4); dst += 4;  // original size

	// Send full keyframe every 60 frames (1 second) for sync recovery
	bool forceKeyframe = (frameNum % 60 == 0);
	bool canDelta = !prevTA.empty() && taSize > 0 && !forceKeyframe;

	if (canDelta)
	{
		// XOR delta: write only non-zero runs
		// Format: [offset(u32) + len(u16) + data(len)] repeated, terminated by offset=0xFFFFFFFF
		uint8_t* deltaStart = dst;
		dst += 4;  // placeholder for deltaPayloadSize

		uint32_t prevSize = (uint32_t)prevTA.size();
		uint32_t commonSize = std::min(taSize, prevSize);

		uint32_t i = 0;
		while (i < taSize)
		{
			// Skip unchanged bytes (only within common range)
			while (i < commonSize && taData[i] == prevTA[i]) i++;
			if (i >= taSize) break;

			// Found a changed byte — find the run length
			uint32_t runStart = i;
			while (i < taSize && (i - runStart) < 65535 &&
				   (i >= commonSize || taData[i] != prevTA[i])) i++;
			// Include a few trailing bytes to avoid tiny gaps between runs
			if (i < taSize) {
				uint32_t gapEnd = std::min(i + 8, taSize);
				bool moreChanges = false;
				for (uint32_t j = i; j < gapEnd; j++)
					if (j >= commonSize || taData[j] != prevTA[j]) { moreChanges = true; break; }
				if (moreChanges)
					while (i < gapEnd) i++;
			}

			uint16_t runLen = (uint16_t)(i - runStart);
			memcpy(dst, &runStart, 4); dst += 4;
			memcpy(dst, &runLen, 2); dst += 2;
			memcpy(dst, taData + runStart, runLen); dst += runLen;
		}
		// Terminator
		uint32_t term = 0xFFFFFFFF;
		memcpy(dst, &term, 4); dst += 4;

		uint32_t deltaPayloadSize = (uint32_t)(dst - deltaStart - 4);
		memcpy(deltaStart, &deltaPayloadSize, 4);

		totalDeltaPayload += deltaPayloadSize;
		totalTABytes += taSize;
		deltaFrames++;

		if (frameNum % 600 == 0)  // log every 10 seconds instead of every second
		{
			float avgDelta = (float)totalDeltaPayload / deltaFrames;
			float avgTA = (float)totalTABytes / deltaFrames;
			printf("[MIRROR] TA DELTA: %.1f KB / %.1f KB (%.1f%%) | stream: %.1f MB/s\n",
				avgDelta / 1024.0, avgTA / 1024.0,
				avgDelta * 100.0 / avgTA, avgDelta * 60.0 / 1024.0 / 1024.0);
		}
	}
	else
	{
		// Send full TA (first frame or size changed)
		uint32_t deltaPayloadSize = taSize;  // full = same as original
		memcpy(dst, &deltaPayloadSize, 4); dst += 4;
		if (taSize > 0) { memcpy(dst, taData, taSize); dst += taSize; }
	}

	// Compute checksum of full TA for client verification
	uint32_t taChecksum = 0;
	for (uint32_t i = 0; i < taSize; i += 4)
		taChecksum ^= *(uint32_t*)(taData + i);
	memcpy(dst, &taChecksum, 4); dst += 4;

	prevTA.assign(taData, taData + taSize);

	// === Memory diffs ===
	uint32_t totalDirty = 0;
	uint8_t* dirtyCountPtr = dst;
	dst += 4;

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
		// Reset TA delta so next frame is sent as full (client has no prevTA)
		prevTA.clear();
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
	if (!_isClient || !_shmPtr) return false;

	RingHeader* hdr = (RingHeader*)_shmPtr;
	uint64_t serverFrames = hdr->frame_count;
	if (serverFrames == _clientFrameCount) return false;

	__sync_synchronize();
	uint64_t offset = hdr->latest_offset;
	uint32_t totalSize = hdr->latest_size;
	if (totalSize == 0 || offset + totalSize > RING_SIZE) return false;

	uint8_t* src = _shmPtr + RING_START + offset;

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

		// Build lookup cache from streamed TA commands (client VRAM is frozen = textures stable)
		if (maplecast_lookup_test::active())
			maplecast_lookup_test::addToCache(&clientCtx);

		// Copy the built rend_context out
		rc = clientCtx.rend;

		// Debug removed for clean performance
	}

	_clientFrameCount = serverFrames;

	// Check VRAM every 60 frames — reset texture cache if drifted
	if (frameNum % 60 == 0)
	{
		uint64_t clientHash = fastVramHash();
		uint64_t serverHash = hdr->server_vram_hash;
		if (clientHash != serverHash)
			renderer->resetTextureCache = true;
	}

	if (frameNum % 600 == 0)
		printf("[MIRROR] Client frame %u | delta=%u bytes | dirty=%u pages\n",
			frameNum, deltaPayloadSize, dirtyPages);

	return taSize > 0;
}

}  // namespace maplecast_mirror
