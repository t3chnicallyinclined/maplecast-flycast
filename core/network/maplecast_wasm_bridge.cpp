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
static bool _wasmHasFullFrame = false;
static bool _wasmInitialized = false;
static MirrorDecompressor _decompressor;
static bool _decompressorInit = false;
static const size_t MEM_PAGE_SIZE = 4096;

extern "C" {

// One decompressor shared by SYNC and per-frame paths. Sized for the worst case
// (~8MB SYNC payload). Matches the desktop client which uses a single 16MB
// decompressor for both SYNC and frames in maplecast_mirror.cpp wsClientRun().
static void ensureDecompressor()
{
	if (!_decompressorInit) {
		_decompressor.init(16 * 1024 * 1024);
		_decompressorInit = true;
	}
}

// Apply SYNC data from server — writes VRAM + PVR regs directly
// Format: "SYNC" (4) + vramSize (4) + vram data + pvrSize (4) + pvr data
EMSCRIPTEN_KEEPALIVE
int mirror_apply_sync(uint8_t* data, int size)
{
	if (size < 8) return 0;

	ensureDecompressor();
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

	// SYNC replaces all VRAM — any cached prior TA buffer is now stale.
	// Force a wait for the next keyframe before rendering again.
	_wasmHasFullFrame = false;

	// Force texture cache reset so renderer picks up new VRAM
	if (renderer) {
		renderer->resetTextureCache = true;
		renderer->updatePalette = true;
		renderer->updateFogTable = true;
	}
	pal_needs_update = true;
	palette_update();

	// Unprotect VRAM so per-frame memcpy patches work — matches desktop wsClientRun()
	memwatch::unprotect();

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
//
// !!! FRAGILE — KEEP IN LOCKSTEP WITH:
//   - core/network/maplecast_mirror.cpp clientReceive()  (desktop client)
//   - packages/renderer/src/wasm_bridge.cpp renderer_frame()  (king.html)
//   - core/network/maplecast_mirror.cpp serverPublish()  (the producer)
//
// All four files implement one wire-format contract. Editing one without the
// others = silent rendering bugs that ONLY show up on scene transitions
// (character select, loading screens) because in-match VRAM is stable enough
// to mask most decode errors. See packages/renderer/src/wasm_bridge.cpp for
// the long-form list of bugs A-E we already paid for.
//
// NOTE: This bridge is the EmulatorJS-bundled version, loaded by web/emulator.html.
// It is NOT what nobd.net king.html uses (that's the standalone renderer in
// packages/renderer/). Both must be fixed when the wire format changes.
//
// Build pipeline: edit here → sync to ~/projects/flycast-wasm/upstream/source/
// → emmake make → bash upstream/link-ubuntu.sh → 7z package → deploy →
// bump report timestamp.
//
// See docs/ARCHITECTURE.md "Mirror Wire Format — Rules of the Road" for the
// canonical list of rules all four parsers must obey.
EMSCRIPTEN_KEEPALIVE
int mirror_render_frame(uint8_t* data, int size)
{
	if (!_wasmInitialized || !renderer || size < 12) return 0;

	// Decompress if ZCST-compressed
	ensureDecompressor();
	size_t decompSize = 0;
	const uint8_t* decompData = _decompressor.decompress(data, size, decompSize);
	if (decompSize < 80) return 0;

	// Decode directly into flycast's TA buffer (zero-copy, matches desktop client)
	uint8_t* taDst = _wasmCtx.tad.thd_root;

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

	bool skipRender = false;

	if (deltaPayloadSize == taSize)
	{
		// Keyframe — copy directly into TA buffer
		if (taSize > 0) memcpy(taDst, src, taSize);
		src += taSize;
		_wasmHasFullFrame = true;
	}
	else if (!_wasmHasFullFrame)
	{
		// Delta arrived before any keyframe — must still walk past delta + checksum
		// + dirty pages so the parser stays aligned, but we cannot render this frame.
		src += deltaPayloadSize;
		skipRender = true;
	}
	else
	{
		// Delta decode in-place into TA buffer
		const uint8_t* dd = src;
		const uint8_t* de = src + deltaPayloadSize;
		while (dd + 4 <= de)
		{
			uint32_t offset; memcpy(&offset, dd, 4); dd += 4;
			if (offset == 0xFFFFFFFF) break;
			uint16_t runLen; memcpy(&runLen, dd, 2); dd += 2;
			if (offset + runLen <= taSize && dd + runLen <= de)
				memcpy(taDst + offset, dd, runLen);
			dd += runLen;
		}
		src += deltaPayloadSize;
	}

	// Skip checksum
	src += 4;

	// Apply memory diffs — must process even when skipRender, so dirty pages are
	// not lost during the delta-before-keyframe window.
	bool vramDirty = false;
	uint32_t dirtyPages; memcpy(&dirtyPages, src, 4); src += 4;
	for (uint32_t d = 0; d < dirtyPages; d++)
	{
		uint8_t regionId = *src++;
		uint32_t pageIdx; memcpy(&pageIdx, src, 4); src += 4;
		size_t pageOff = pageIdx * MEM_PAGE_SIZE;

		if (regionId == 0 && pageOff + MEM_PAGE_SIZE <= 16 * 1024 * 1024)
		{
			memcpy(&mem_b[pageOff], src, MEM_PAGE_SIZE);
		}
		else if (regionId == 1 && pageOff + MEM_PAGE_SIZE <= VRAM_SIZE)
		{
			// Unprotect BEFORE writing — texture cache may have mprotect'd this page
			VramLockedWriteOffset(pageOff);
			memcpy(&vram[pageOff], src, MEM_PAGE_SIZE);
			vramDirty = true;
		}
		else if (regionId == 2 && pageOff + MEM_PAGE_SIZE <= 2 * 1024 * 1024)
		{
			memcpy(&aica::aica_ram[pageOff], src, MEM_PAGE_SIZE);
		}
		else if (regionId == 3 && pageOff + MEM_PAGE_SIZE <= (size_t)pvr_RegSize)
		{
			memcpy(pvr_regs + pageOff, src, MEM_PAGE_SIZE);
		}
		src += MEM_PAGE_SIZE;
	}

	if (skipRender || taSize == 0) return 0;

	// Build TA context — data is already in taDst, no copy needed
	_wasmCtx.rend.Clear();
	_wasmCtx.tad.Clear();
	_wasmCtx.tad.thd_data = taDst + taSize;

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

	// Force texture cache rebuild whenever VRAM moved — without this, a scene
	// transition (e.g. character select) keeps showing stale textures from the
	// previous scene because the cache was populated from old VRAM bytes.
	if (vramDirty)
		renderer->resetTextureCache = true;

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
