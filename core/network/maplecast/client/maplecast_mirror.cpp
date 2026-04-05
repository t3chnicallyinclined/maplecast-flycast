/*
	MapleCast Mirror Client — receives delta-encoded TA commands, renders them.

	Per frame:
	  1. Read delta-encoded TA commands from server (via shared memory or WebSocket)
	  2. Apply VRAM + PVR register diffs
	  3. palette_update() — critical for MVC2 paletted sprites
	  4. renderer->Process() — ta_parse builds rend_context + resolves textures
	  5. renderer->Render() + Present()
*/
#include "types.h"
#include "maplecast_mirror.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/aica/aica_if.h"
#include "rend/TexCache.h"
#include "hw/mem/mem_watch.h"
#include "rend/texconv.h"

#include <cstdio>
#include <cstring>
#include <vector>

#ifndef __EMSCRIPTEN__
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

extern Renderer* renderer;
extern bool pal_needs_update;

namespace maplecast_mirror
{

static const size_t HEADER_SIZE = 4096;
static const size_t BRAIN_SIZE = 32 * 1024 * 1024;
static const size_t RING_START = HEADER_SIZE + BRAIN_SIZE;
static const size_t SHM_SIZE = RING_START + 128 * 1024 * 1024;
static const size_t RING_SIZE = SHM_SIZE - RING_START;
static const size_t MEM_PAGE_SIZE = 4096;

static bool _isClient = false;
static uint8_t* _shmPtr = nullptr;

struct RingHeader {
	volatile uint64_t write_pos;
	volatile uint64_t frame_count;
	volatile uint64_t latest_offset;
	volatile uint32_t latest_size;
	volatile uint32_t client_request_sync;
	volatile uint32_t sync_ready;
	volatile uint64_t server_vram_hash;
	uint8_t pad[4096 - 44];
};

static uint64_t _clientFrameCount = 0;

static uint64_t fastVramHash()
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < VRAM_SIZE; i += 64) {
		h ^= vram[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

#ifndef __EMSCRIPTEN__
static const char* SHM_NAME = "/maplecast_mirror";
static int _shmFd = -1;

static bool openShm()
{
	_shmFd = shm_open(SHM_NAME, O_RDWR, 0666);
	if (_shmFd < 0) { printf("[MIRROR] shm_open failed\n"); return false; }
	_shmPtr = (uint8_t*)mmap(nullptr, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, _shmFd, 0);
	if (_shmPtr == MAP_FAILED) { _shmPtr = nullptr; return false; }
	return true;
}

void initClient()
{
	if (!openShm()) return;
	_isClient = true;
	_clientFrameCount = 0;

	RingHeader* hdr = (RingHeader*)_shmPtr;
	hdr->sync_ready = 0;
	hdr->client_request_sync = 1;
	printf("[MIRROR] Requesting fresh sync state from server...\n");

	for (int i = 0; i < 500; i++) {
		if (hdr->sync_ready) break;
		usleep(10000);
	}

	if (hdr->sync_ready) {
		uint8_t* snap = _shmPtr + HEADER_SIZE;
		size_t off = 0;
		memcpy(&mem_b[0], snap + off, 16 * 1024 * 1024); off += 16 * 1024 * 1024;
		memcpy(&vram[0], snap + off, VRAM_SIZE); off += VRAM_SIZE;
		memcpy(&aica::aica_ram[0], snap + off, 2 * 1024 * 1024);

		memwatch::unprotect();
		renderer->resetTextureCache = true;
		pal_needs_update = true;
		palette_update();
		renderer->updatePalette = true;
		renderer->updateFogTable = true;

		_clientFrameCount = hdr->frame_count;
		printf("[MIRROR] === CLIENT MODE === synced at frame %lu\n", _clientFrameCount);
	} else {
		printf("[MIRROR] WARNING: server didn't respond\n");
	}
}
#else
void initClient()
{
	// WASM init handled by wasm_bridge.cpp mirror_init()
	_isClient = true;
}
#endif

bool isClient() { return _isClient; }

bool clientReceive(rend_context& rc, bool& vramDirty)
{
#ifdef __EMSCRIPTEN__
	return false; // WASM uses mirror_render_frame() from wasm_bridge.cpp
#else
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

	uint32_t pvr_snapshot[16];
	memcpy(pvr_snapshot, src, sizeof(pvr_snapshot)); src += sizeof(pvr_snapshot);

	uint32_t taSize; memcpy(&taSize, src, 4); src += 4;
	uint32_t deltaPayloadSize; memcpy(&deltaPayloadSize, src, 4); src += 4;

	static std::vector<uint8_t> clientTA;
	static bool clientHasFullFrame = false;

	if (deltaPayloadSize == taSize)
	{
		clientTA.assign(src, src + taSize);
		src += taSize;
		clientHasFullFrame = true;
	}
	else if (!clientHasFullFrame)
	{
		src += deltaPayloadSize;
		src += 4;
		return false;
	}
	else
	{
		if (clientTA.size() < taSize)
			clientTA.resize(taSize, 0);
		else if (clientTA.size() > taSize)
			clientTA.resize(taSize);

		uint8_t* deltaData = src;
		uint8_t* deltaEnd = src + deltaPayloadSize;

		while (deltaData + 4 <= deltaEnd)
		{
			uint32_t off;
			memcpy(&off, deltaData, 4); deltaData += 4;
			if (off == 0xFFFFFFFF) break;

			uint16_t runLen;
			memcpy(&runLen, deltaData, 2); deltaData += 2;
			if (off + runLen <= taSize && deltaData + runLen <= deltaEnd)
				memcpy(clientTA.data() + off, deltaData, runLen);
			deltaData += runLen;
		}
		src += deltaPayloadSize;
	}

	uint32_t serverChecksum;
	memcpy(&serverChecksum, src, 4); src += 4;

	uint8_t* taData = clientTA.data();
	uint32_t clientChecksum = 0;
	for (uint32_t i = 0; i + 3 < taSize; i += 4)
		clientChecksum ^= *(uint32_t*)(taData + i);

	static uint32_t checksumFails = 0;
	static uint32_t checksumTotal = 0;
	checksumTotal++;
	if (clientChecksum != serverChecksum)
	{
		checksumFails++;
		if (checksumFails <= 10 || checksumFails % 100 == 0)
			printf("[DELTA] CHECKSUM MISMATCH frame %u (fail %u/%u)\n",
				frameNum, checksumFails, checksumTotal);
	}

	// Memory diffs
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

	// Build TA context and render
	if (taSize > 0) {
		static TA_context clientCtx;
		static bool ctxAlloced = false;
		if (!ctxAlloced) { clientCtx.Alloc(); ctxAlloced = true; }

		clientCtx.rend.Clear();
		clientCtx.tad.Clear();

		memcpy(clientCtx.tad.thd_root, taData, taSize);
		clientCtx.tad.thd_data = clientCtx.tad.thd_root + taSize;

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

	_clientFrameCount = serverFrames;

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
#endif
}

}  // namespace maplecast_mirror
