/*
	MapleCast WASM Bridge — exports for browser JS to feed delta frames
	into the flycast renderer.

	JS calls _mirror_render_frame(ptr, size) with the binary delta frame data.
	This function decodes the frame and renders it through flycast's real renderer.
*/
#ifdef __EMSCRIPTEN__

#include "types.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/aica/aica_if.h"
#include "rend/gles/gles.h"
#include "rend/TexCache.h"
#include "rend/texconv.h"
#include "hw/mem/mem_watch.h"
#include "emulator.h"
#include "maplecast_compress.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <emscripten.h>

extern Renderer* renderer;
extern bool pal_needs_update;

static TA_context _wasmCtx;
static bool _wasmCtxAlloced = false;
static std::vector<uint8_t> _wasmPrevTA;
static bool _wasmInitialized = false;
static MirrorDecompressor _decompressor;
static bool _decompressorInit = false;

extern "C" {

// Apply SYNC data from server — writes VRAM + PVR regs directly
// Format: "SYNC" (4) + vramSize (4) + vram data + pvrSize (4) + pvr data
EMSCRIPTEN_KEEPALIVE
int mirror_apply_sync(uint8_t* data, int size)
{
	if (size < 8) return 0;

	// Decompress if ZCST-compressed
	if (!_decompressorInit) { _decompressor.init(16 * 1024 * 1024); _decompressorInit = true; }
	size_t decompSize = 0;
	const uint8_t* decompData = _decompressor.decompress(data, size, decompSize);
	if (decompSize < 12) return 0;

	const uint8_t* src = decompData;

	// Skip "SYNC" magic
	if (memcmp(src, "SYNC", 4) != 0) return 0;
	src += 4;

	uint32_t vramSize; memcpy(&vramSize, src, 4); src += 4;
	if (vramSize > VRAM_SIZE) vramSize = VRAM_SIZE;
	memcpy(&vram[0], src, vramSize); src += vramSize;

	uint32_t pvrSize; memcpy(&pvrSize, src, 4); src += 4;
	if (pvrSize > (uint32_t)pvr_RegSize) pvrSize = pvr_RegSize;
	memcpy(pvr_regs, src, pvrSize);

	// Force texture cache reset so renderer picks up new VRAM
	if (renderer) {
		renderer->resetTextureCache = true;
		renderer->updatePalette = true;
		renderer->updateFogTable = true;
	}
	pal_needs_update = true;
	palette_update();

	printf("[WASM-MIRROR] SYNC applied: VRAM=%u PVR=%u bytes\n", vramSize, pvrSize);
	return 1;
}

// Initialize the mirror renderer — call after save state is loaded
EMSCRIPTEN_KEEPALIVE
int mirror_init()
{
	if (!_wasmCtxAlloced) {
		_wasmCtx.Alloc();
		_wasmCtxAlloced = true;
	}
	memwatch::unprotect();
	_wasmInitialized = true;
	printf("[WASM-MIRROR] Initialized\n");
	return 1;
}

// Receive and render a delta frame
// data format: frameSize(4) + frameNum(4) + pvr_snapshot(64) + taOrigSize(4) + taDeltaSize(4) + deltaData + checksum(4) + dirtyPages
EMSCRIPTEN_KEEPALIVE
int mirror_render_frame(uint8_t* data, int size)
{
	if (!_wasmInitialized || !renderer || size < 12) return 0;

	// Decompress if ZCST-compressed
	if (!_decompressorInit) { _decompressor.init(512 * 1024); _decompressorInit = true; }
	size_t decompSize = 0;
	const uint8_t* decompData = _decompressor.decompress(data, size, decompSize);
	if (decompSize < 80) return 0;

	const uint8_t* src = decompData;

	// Frame header
	uint32_t frameSize; memcpy(&frameSize, src, 4); src += 4;
	uint32_t frameNum; memcpy(&frameNum, src, 4); src += 4;

	// PVR registers
	uint32_t pvr_snapshot[16];
	memcpy(pvr_snapshot, src, sizeof(pvr_snapshot)); src += sizeof(pvr_snapshot);

	// TA commands (delta encoded)
	uint32_t taSize; memcpy(&taSize, src, 4); src += 4;
	uint32_t deltaPayloadSize; memcpy(&deltaPayloadSize, src, 4); src += 4;

	if (deltaPayloadSize == taSize)
	{
		// Full frame
		_wasmPrevTA.assign(src, src + taSize);
		src += taSize;
	}
	else
	{
		// Delta decode
		if (_wasmPrevTA.empty()) {
			src += deltaPayloadSize;
			// Skip checksum
			src += 4;
			return 0; // Need full frame first
		}
		if (_wasmPrevTA.size() < taSize)
			_wasmPrevTA.resize(taSize, 0);
		else if (_wasmPrevTA.size() > taSize)
			_wasmPrevTA.resize(taSize);

		const uint8_t* deltaData = src;
		const uint8_t* deltaEnd = src + deltaPayloadSize;
		while (deltaData + 4 <= deltaEnd)
		{
			uint32_t offset;
			memcpy(&offset, deltaData, 4); deltaData += 4;
			if (offset == 0xFFFFFFFF) break;
			uint16_t runLen;
			memcpy(&runLen, deltaData, 2); deltaData += 2;
			if (offset + runLen <= taSize && deltaData + runLen <= deltaEnd)
				memcpy(_wasmPrevTA.data() + offset, deltaData, runLen);
			deltaData += runLen;
		}
		src += deltaPayloadSize;
	}

	// Skip checksum
	src += 4;

	// Apply memory diffs (VRAM, PVR regs)
	uint32_t dirtyPages; memcpy(&dirtyPages, src, 4); src += 4;
	for (uint32_t d = 0; d < dirtyPages; d++)
	{
		uint8_t regionId = *src++;
		uint32_t pageIdx; memcpy(&pageIdx, src, 4); src += 4;
		size_t pageOff = pageIdx * 4096;

		if (regionId == 1 && pageOff + 4096 <= VRAM_SIZE)
		{
			memcpy(&vram[pageOff], src, 4096);
			VramLockedWriteOffset(pageOff);
		}
		else if (regionId == 3 && pageOff + 4096 <= (size_t)pvr_RegSize)
		{
			memcpy(pvr_regs + pageOff, src, 4096);
		}
		src += 4096;
	}

	// Build TA context
	_wasmCtx.rend.Clear();
	_wasmCtx.tad.Clear();

	memcpy(_wasmCtx.tad.thd_root, _wasmPrevTA.data(), taSize);
	_wasmCtx.tad.thd_data = _wasmCtx.tad.thd_root + taSize;

	// Set PVR registers
	TA_GLOB_TILE_CLIP.full = pvr_snapshot[0];
	SCALER_CTL.full = pvr_snapshot[1];
	FB_X_CLIP.full = pvr_snapshot[2];
	FB_Y_CLIP.full = pvr_snapshot[3];
	FB_W_LINESTRIDE.full = pvr_snapshot[4];
	FB_W_SOF1 = pvr_snapshot[5];
	FB_W_CTRL.full = pvr_snapshot[6];
	FOG_CLAMP_MIN.full = pvr_snapshot[7];
	FOG_CLAMP_MAX.full = pvr_snapshot[8];

	_wasmCtx.rend.isRTT = pvr_snapshot[13] != 0;
	_wasmCtx.rend.fb_W_SOF1 = pvr_snapshot[5];
	_wasmCtx.rend.fb_W_CTRL.full = pvr_snapshot[6];
	_wasmCtx.rend.ta_GLOB_TILE_CLIP.full = pvr_snapshot[0];
	_wasmCtx.rend.scaler_ctl.full = pvr_snapshot[1];
	_wasmCtx.rend.fb_X_CLIP.full = pvr_snapshot[2];
	_wasmCtx.rend.fb_Y_CLIP.full = pvr_snapshot[3];
	_wasmCtx.rend.fb_W_LINESTRIDE = pvr_snapshot[4];
	_wasmCtx.rend.fog_clamp_min.full = pvr_snapshot[7];
	_wasmCtx.rend.fog_clamp_max.full = pvr_snapshot[8];
	_wasmCtx.rend.framebufferWidth = pvr_snapshot[9];
	_wasmCtx.rend.framebufferHeight = pvr_snapshot[10];
	_wasmCtx.rend.clearFramebuffer = pvr_snapshot[11] != 0;
	float fz; memcpy(&fz, &pvr_snapshot[12], 4);
	_wasmCtx.rend.fZ_max = fz;

	// Palette update
	pal_needs_update = true;
	palette_update();
	renderer->updatePalette = true;
	renderer->updateFogTable = true;

	// Process + Render
	renderer->Process(&_wasmCtx);
	bool isScreen = renderer->Render();
	if (isScreen)
		renderer->Present();

	// Tell RetroArch to present the frame to the canvas
	extern void mirror_present_frame();
	mirror_present_frame();

	return 1;
}

// Get the current frame count for sync
EMSCRIPTEN_KEEPALIVE
int mirror_get_frame_count()
{
	return _wasmInitialized ? 1 : 0;
}

}  // extern "C"

#endif  // __EMSCRIPTEN__
