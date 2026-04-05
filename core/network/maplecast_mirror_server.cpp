/*
	MapleCast Mirror Server — delta-encode TA commands + memory diffs, broadcast over WebSocket.

	Optimizations:
	  - TA delta: fused copy + diff + checksum in one pass over ~140KB
	  - VRAM diff: memwatch page-fault tracking, only dirty pages sent
	  - PVR regs: memcmp 8 pages (32KB, negligible)
*/
#include "types.h"
#include "maplecast_mirror.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_mem.h"
#include "rend/gles/gles.h"
#include "rend/TexCache.h"
#include "hw/mem/mem_watch.h"
#include "maplecast_stream.h"
#include "rend/texconv.h"

#include <cstdio>
#include <cstring>
#include <vector>

extern Renderer* renderer;

namespace maplecast_mirror
{

static const size_t MEM_PAGE_SIZE = 4096;
static const size_t FRAME_BUF_SIZE = 512 * 1024;
static uint8_t _frameBuf[FRAME_BUF_SIZE];

static bool _isServer = false;
static uint32_t _frameCount = 0;

static uint8_t* _taBuf[2] = { nullptr, nullptr };
static uint32_t _taBufSize[2] = { 0, 0 };
static int _taCur = 0;
static bool _taHasPrev = false;

static uint8_t* _shadowPVR = nullptr;

void initServer()
{
	_isServer = true;
	_frameCount = 0;
	_taCur = 0;
	_taHasPrev = false;

	for (int i = 0; i < 2; i++) {
		_taBuf[i] = (uint8_t*)malloc(256 * 1024);
		_taBufSize[i] = 0;
	}

	_shadowPVR = (uint8_t*)malloc(pvr_RegSize);
	memcpy(_shadowPVR, pvr_regs, pvr_RegSize);

	memwatch::mirrorActive = true;
	memwatch::vramWatcher.protect();

	printf("[MIRROR] === SERVER MODE === WebSocket-only, memwatch VRAM tracking\n");
}

bool isServer() { return _isServer; }

// Client stubs — server build doesn't use these
void initClient() {}
bool isClient() { return false; }
bool clientReceive(rend_context&, bool&) { return false; }

void serverPublish(TA_context* ctx)
{
	if (!_isServer || !ctx) return;
	rend_context& rc = ctx->rend;

	uint8_t* dst = _frameBuf;
	uint8_t* dstStart = dst;

	dst += 4;
	_frameCount++;
	uint32_t frameNum = _frameCount;
	memcpy(dst, &frameNum, 4); dst += 4;

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

	uint32_t taSize = (uint32_t)(ctx->tad.thd_data - ctx->tad.thd_root);
	uint8_t* taData = ctx->tad.thd_root;

	int cur = _taCur;
	int prev = 1 - cur;
	uint8_t* curBuf = _taBuf[cur];
	uint8_t* prevData = _taBuf[prev];
	uint32_t prevSize = _taBufSize[prev];

	static uint64_t totalDeltaPayload = 0;
	static uint64_t totalTABytes = 0;
	static uint32_t deltaFrames = 0;

	memcpy(dst, &taSize, 4); dst += 4;

	bool forceKeyframe = (frameNum % 60 == 0);
	bool canDelta = _taHasPrev && taSize > 0 && !forceKeyframe;
	uint32_t taChecksum = 0;

	if (canDelta)
	{
		uint8_t* deltaStart = dst;
		dst += 4;
		uint32_t commonSize = std::min(taSize, prevSize);

		uint32_t i = 0;
		while (i < taSize)
		{
			while (i < commonSize) {
				uint32_t word; memcpy(&word, taData + i, 4);
				taChecksum ^= word;
				memcpy(curBuf + i, taData + i, 4);
				if (*(uint32_t*)(taData + i) != *(uint32_t*)(prevData + i)) break;
				i += 4;
			}
			if (i >= commonSize) {
				while (i + 3 < taSize) {
					uint32_t word; memcpy(&word, taData + i, 4);
					taChecksum ^= word;
					memcpy(curBuf + i, taData + i, 4);
					i += 4;
				}
				while (i < taSize) { curBuf[i] = taData[i]; i++; }
				break;
			}

			uint32_t runStart = i;
			while (i < taSize && (i - runStart) < 65535 &&
				   (i >= commonSize || *(uint32_t*)(taData + (i & ~3)) != *(uint32_t*)(prevData + (i & ~3)))) {
				if ((i & 3) == 0 && i + 3 < taSize) {
					uint32_t word; memcpy(&word, taData + i, 4);
					taChecksum ^= word;
					memcpy(curBuf + i, taData + i, 4);
					i += 4;
				} else { curBuf[i] = taData[i]; i++; }
			}
			if (i < taSize) {
				uint32_t gapEnd = std::min(i + 8, taSize);
				bool more = false;
				for (uint32_t j = i; j < gapEnd; j++)
					if (j >= commonSize || taData[j] != prevData[j]) { more = true; break; }
				if (more) {
					while (i < gapEnd) {
						if ((i & 3) == 0 && i + 3 < taSize) {
							uint32_t word; memcpy(&word, taData + i, 4);
							taChecksum ^= word;
							memcpy(curBuf + i, taData + i, 4);
							i += 4;
						} else { curBuf[i] = taData[i]; i++; }
					}
				}
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
		for (uint32_t i = 0; i + 3 < taSize; i += 4) {
			uint32_t word; memcpy(&word, taData + i, 4);
			taChecksum ^= word;
			memcpy(curBuf + i, taData + i, 4);
		}
		uint32_t aligned = taSize & ~3;
		for (uint32_t i = aligned; i < taSize; i++) curBuf[i] = taData[i];
		if (taSize > 0) { memcpy(dst, taData, taSize); dst += taSize; }
	}

	_taBufSize[cur] = taSize;
	_taCur = prev;
	_taHasPrev = true;

	memcpy(dst, &taChecksum, 4); dst += 4;

	// Memory diffs
	uint32_t totalDirty = 0;
	uint8_t* dirtyCountPtr = dst;
	dst += 4;

	memwatch::PageMap vramDirty;
	memwatch::vramWatcher.getPages(vramDirty);

	for (const auto& pair : vramDirty) {
		uint32_t pageOff = pair.first;
		if (pageOff + MEM_PAGE_SIZE > VRAM_SIZE) continue;
		if ((size_t)(dst - dstStart) + 5 + MEM_PAGE_SIZE > FRAME_BUF_SIZE) break;
		*dst++ = 1;
		uint32_t pageIdx = pageOff / MEM_PAGE_SIZE;
		memcpy(dst, &pageIdx, 4); dst += 4;
		memcpy(dst, &vram[pageOff], MEM_PAGE_SIZE); dst += MEM_PAGE_SIZE;
		totalDirty++;
	}

	memwatch::vramWatcher.protect();

	{
		size_t numPages = (size_t)pvr_RegSize / MEM_PAGE_SIZE;
		for (size_t p = 0; p < numPages; p++) {
			size_t off = p * MEM_PAGE_SIZE;
			if (memcmp(pvr_regs + off, _shadowPVR + off, MEM_PAGE_SIZE) != 0) {
				if ((size_t)(dst - dstStart) + 5 + MEM_PAGE_SIZE > FRAME_BUF_SIZE) break;
				*dst++ = 3;
				uint32_t pi = (uint32_t)p;
				memcpy(dst, &pi, 4); dst += 4;
				memcpy(dst, pvr_regs + off, MEM_PAGE_SIZE); dst += MEM_PAGE_SIZE;
				memcpy(_shadowPVR + off, pvr_regs + off, MEM_PAGE_SIZE);
				totalDirty++;
			}
		}
	}

	memcpy(dirtyCountPtr, &totalDirty, 4);

	uint32_t totalSize = (uint32_t)(dst - dstStart);
	uint32_t frameSizeVal = totalSize - 4;
	memcpy(dstStart, &frameSizeVal, 4);

	if (maplecast_stream::active())
		maplecast_stream::broadcastBinary(dstStart, totalSize);

	if (frameNum % 600 == 0)
		printf("[MIRROR] Server frame %u | TA=%u bytes | %u dirty pages | %u bytes sent\n",
			frameNum, taSize, totalDirty, totalSize);
}

}  // namespace maplecast_mirror
