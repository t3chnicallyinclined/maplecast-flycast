// ============================================================================
// WASM_BRIDGE.CPP — Standalone WASM mirror renderer for King of Marvel
//
// No RetroArch. No libretro. No EmulatorJS. Pure WebGL2.
// Receives TA commands from the server, renders pixel-perfect 3D.
//
// Exported API:
//   renderer_init(w, h)    — create WebGL2 context + OpenGL renderer
//   renderer_sync(ptr, sz) — apply initial VRAM + PVR state
//   renderer_frame(ptr, sz)— decode delta frame, render immediately
//   renderer_resize(w, h)  — handle canvas resize
//   renderer_destroy()     — cleanup
//
// OVERKILL IS NECESSARY. EVERY MICROSECOND COUNTS.
// ============================================================================

#ifdef __EMSCRIPTEN__

#include "types.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/Renderer_if.h"
#include "rend/gles/gles.h"
#include "rend/gles/postprocess.h"
#include <glsm/glsm.h>
#include "rend/TexCache.h"
#include "rend/texconv.h"
#include "rend/transform_matrix.h"
#include "cfg/option.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <emscripten.h>

// External: renderer pointer lives in stubs.cpp
extern Renderer* renderer;
extern bool pal_needs_update;

// External: GL context management from wasm_gl_context.cpp
extern bool wasm_gl_init(int width, int height);
extern void wasm_gl_resize(int width, int height);
extern void wasm_gl_destroy();

// External: FillBGP — fills background polygon from VRAM. CRITICAL FIX.
// This was missing from the original WASM bridge — background polygon
// was never initialized on the client, causing rendering artifacts.
extern void FillBGP(TA_context* ctx);

// External: create the OpenGL renderer
extern Renderer* rend_GLES2();

// ============================================================================
// Blit postProcessor FBO to canvas via postProcessor.render()
// This handles Y-flip, shift, and PowerVR2 filtering correctly —
// exactly how the native client's renderLastFrame() path works.
// ============================================================================

extern gl_ctx gl;
extern PostProcessor postProcessor;

// ============================================================================
// Static state — pre-allocated, zero malloc in hot path
// ============================================================================

static TA_context _ctx;
static bool _ctxAlloced = false;
static std::vector<uint8_t> _prevTA;       // Previous frame's TA buffer (for delta decode)
static bool _initialized = false;
static uint32_t _frameCount = 0;

// ============================================================================
// renderer_init — Create WebGL2 context and initialize the GLES renderer
// ============================================================================

extern "C" {

EMSCRIPTEN_KEEPALIVE
int renderer_init(int width, int height)
{
    if (_initialized) return 1;

    // Create WebGL2 context (this calls initSettings() which sets VRAM_SIZE)
    if (!wasm_gl_init(width, height)) {
        printf("[renderer] WebGL2 init failed\n");
        return 0;
    }

    // Allocate VRAM AFTER initSettings() sets platform.vram_size
    vram.alloc(VRAM_SIZE);
    printf("[renderer] VRAM allocated: %u bytes\n", VRAM_SIZE);

    // Allocate TA context (one-time, 8MB buffer)
    if (!_ctxAlloced) {
        _ctx.Alloc();
        _ctxAlloced = true;
    }

    // Create the OpenGL ES renderer
    renderer = rend_GLES2();
    if (!renderer) {
        printf("[renderer] Failed to create OpenGL renderer\n");
        return 0;
    }

    // Match the native mirror client's config overrides EXACTLY
    // (from core/emulator.cpp lines 1054-1061)
    config::PerPixelLayers.override(4);
    config::UseMipmaps.override(false);
    config::Fog.override(false);
    config::ModifierVolumes.override(false);
    config::ThreadedRendering.override(false);
    // EmulateFramebuffer stays FALSE (default) — the native client uses false.
    // Setting it to true causes writeFramebufferToVRAM() glReadPixels round-trip
    // which is completely wrong for a mirror renderer.

    // Initialize the renderer (compiles shaders, creates FBOs, etc.)
    if (!renderer->Init()) {
        printf("[renderer] Renderer Init() failed\n");
        delete renderer;
        renderer = nullptr;
        return 0;
    }

    _initialized = true;
    printf("[renderer] Initialized: %dx%d, WebGL2 + flycast GLES\n", width, height);
    return 1;
}

// ============================================================================
// renderer_sync — Apply initial SYNC (full VRAM + PVR register snapshot)
//
// Format: "SYNC"(4) + vramSize(4) + vram[N] + pvrSize(4) + pvr[N]
// ============================================================================

EMSCRIPTEN_KEEPALIVE
int renderer_sync(uint8_t* data, int size)
{
    if (!_initialized || !renderer || size < 12) return 0;
    uint8_t* src = data;

    // Verify magic
    if (memcmp(src, "SYNC", 4) != 0) return 0;
    src += 4;

    // VRAM
    uint32_t vramSize;
    memcpy(&vramSize, src, 4); src += 4;
    if (vramSize > VRAM_SIZE) vramSize = VRAM_SIZE;
    memcpy(&vram[0], src, vramSize);
    src += vramSize;

    // PVR registers
    uint32_t pvrSize;
    memcpy(&pvrSize, src, 4); src += 4;
    if (pvrSize > (uint32_t)pvr_RegSize) pvrSize = pvr_RegSize;
    memcpy(pvr_regs, src, pvrSize);

    // Force full cache rebuild
    renderer->resetTextureCache = true;
    renderer->updatePalette = true;
    renderer->updateFogTable = true;
    pal_needs_update = true;
    palette_update();

    // Clear previous TA buffer (force keyframe on next frame)
    _prevTA.clear();

    printf("[renderer] SYNC applied: VRAM=%u PVR=%u resetTexCache=%d\n",
        vramSize, pvrSize, renderer ? renderer->resetTextureCache : -1);
    return 1;
}

// ============================================================================
// renderer_frame — Decode delta frame and render IMMEDIATELY
//
// Push-driven: called from JS on WebSocket/DataChannel message.
// No requestAnimationFrame loop. No buffering. Render on arrival.
//
// Wire format:
//   frameSize(4) + frameNum(4) + pvr_snapshot[16](64) +
//   taSize(4) + deltaPayloadSize(4) + deltaData(var) + checksum(4) +
//   dirtyPageCount(4) + [regionId(1) + pageIdx(4) + data(4096)] * N
// ============================================================================

EMSCRIPTEN_KEEPALIVE
int renderer_info()
{
    return (_initialized ? 100 : 0) + (renderer ? 10 : 0) + (_ctxAlloced ? 1 : 0);
}

EMSCRIPTEN_KEEPALIVE
int renderer_frame(uint8_t* data, int size)
{
    if (!_initialized) return -1;
    if (!renderer) return -2;
    if (size < 80) return -3;

    uint8_t* src = data;

    // ---- Header ----
    uint32_t frameSize; memcpy(&frameSize, src, 4); src += 4;
    uint32_t frameNum;  memcpy(&frameNum,  src, 4); src += 4;

    // ---- PVR register snapshot (16 x u32 = 64 bytes) ----
    uint32_t pvr_snapshot[16];
    memcpy(pvr_snapshot, src, 64); src += 64;

    // ---- TA command buffer (delta encoded) ----
    uint32_t taSize;           memcpy(&taSize,           src, 4); src += 4;
    uint32_t deltaPayloadSize; memcpy(&deltaPayloadSize, src, 4); src += 4;

    if (deltaPayloadSize == taSize) {
        // Keyframe — full TA buffer
        _prevTA.assign(src, src + taSize);
        src += taSize;
        printf("[renderer] KEYFRAME received: frame=%u taSize=%u\n", frameNum, taSize);
    } else {
        // Delta frame — apply runs to previous buffer
        if (_prevTA.empty()) {
            // No previous keyframe — skip until we get one
            return -10;  // waiting for keyframe
        }
        // Resize to match server's TA size
        _prevTA.resize(taSize, 0);

        // Apply delta runs: (offset:4, runLen:2, data:N)*, sentinel 0xFFFFFFFF
        uint8_t* deltaData = src;
        uint8_t* deltaEnd  = src + deltaPayloadSize;
        while (deltaData + 4 <= deltaEnd) {
            uint32_t offset;
            memcpy(&offset, deltaData, 4); deltaData += 4;
            if (offset == 0xFFFFFFFF) break;

            uint16_t runLen;
            memcpy(&runLen, deltaData, 2); deltaData += 2;

            if (offset + runLen <= taSize && deltaData + runLen <= deltaEnd) {
                memcpy(_prevTA.data() + offset, deltaData, runLen);
            }
            deltaData += runLen;
        }
        src += deltaPayloadSize;
    }

    // ---- Skip checksum (4 bytes) ----
    src += 4;

    // ---- Apply dirty memory pages ----
    uint32_t dirtyPages;
    memcpy(&dirtyPages, src, 4); src += 4;
    bool vramDirty = false;

    for (uint32_t d = 0; d < dirtyPages; d++) {
        uint8_t regionId = *src++;
        uint32_t pageIdx;
        memcpy(&pageIdx, src, 4); src += 4;
        size_t pageOff = (size_t)pageIdx * 4096;

        if (regionId == 1 && pageOff + 4096 <= VRAM_SIZE) {
            // VRAM page — copy + invalidate texture cache for this region
            memcpy(&vram[pageOff], src, 4096);
            VramLockedWriteOffset(pageOff);
            vramDirty = true;
        } else if (regionId == 3 && pageOff + 4096 <= (size_t)pvr_RegSize) {
            // PVR register page
            memcpy(pvr_regs + pageOff, src, 4096);
        }
        src += 4096;
    }

    // CRITICAL: Reset texture cache when VRAM changes — without this,
    // health bars, animations, sprites won't update because the GPU
    // texture cache still has stale data from previous frames.
    if (vramDirty) renderer->resetTextureCache = true;

    // Debug: log palette state on first rendered frame
    if (_frameCount == 0) {
        printf("[renderer] PAL_RAM_CTRL=%u PALETTE_RAM[0]=0x%08x TEXT_CONTROL=0x%08x\n",
            PAL_RAM_CTRL, PALETTE_RAM[0], TEXT_CONTROL);
        printf("[renderer] palette16[0]=0x%04x palette32[0]=0x%08x\n",
            palette16_ram[0], palette32_ram[0]);
    }

    // ---- Build TA context ----
    _ctx.rend.Clear();
    _ctx.tad.Clear();

    // Copy TA command buffer
    memcpy(_ctx.tad.thd_root, _prevTA.data(), taSize);
    _ctx.tad.thd_data = _ctx.tad.thd_root + taSize;

    // Apply PVR snapshot to hardware registers (renderer reads these directly)
    TA_GLOB_TILE_CLIP.full  = pvr_snapshot[0];
    SCALER_CTL.full         = pvr_snapshot[1];
    FB_X_CLIP.full          = pvr_snapshot[2];
    FB_Y_CLIP.full          = pvr_snapshot[3];
    FB_W_LINESTRIDE.full    = pvr_snapshot[4];
    FB_W_SOF1               = pvr_snapshot[5];
    FB_W_CTRL.full          = pvr_snapshot[6];
    FOG_CLAMP_MIN.full      = pvr_snapshot[7];
    FOG_CLAMP_MAX.full      = pvr_snapshot[8];

    // Apply PVR snapshot to rend_context (ta_parse reads these)
    _ctx.rend.ta_GLOB_TILE_CLIP.full = pvr_snapshot[0];
    _ctx.rend.scaler_ctl.full        = pvr_snapshot[1];
    _ctx.rend.fb_X_CLIP.full         = pvr_snapshot[2];
    _ctx.rend.fb_Y_CLIP.full         = pvr_snapshot[3];
    _ctx.rend.fb_W_LINESTRIDE        = pvr_snapshot[4];
    _ctx.rend.fb_W_SOF1              = pvr_snapshot[5];
    _ctx.rend.fb_W_CTRL.full         = pvr_snapshot[6];
    _ctx.rend.fog_clamp_min.full     = pvr_snapshot[7];
    _ctx.rend.fog_clamp_max.full     = pvr_snapshot[8];
    // Compute framebuffer dimensions from PVR registers, scaled to OUR
    // render resolution — NOT the server's. The server may be running at
    // 6x resolution (2880x2160) but we render at native 480p.
    // This matches what Renderer_if.cpp line 184 does on the server.
    {
        int fbW, fbH;
        getScaledFramebufferSize(_ctx.rend, fbW, fbH);
        _ctx.rend.framebufferWidth = fbW;
        _ctx.rend.framebufferHeight = fbH;
        if (_frameCount == 0)
            printf("[renderer] Computed FB: %dx%d (server sent %ux%u)\n",
                fbW, fbH, pvr_snapshot[9], pvr_snapshot[10]);
    }
    _ctx.rend.clearFramebuffer       = pvr_snapshot[11] != 0;
    float fz; memcpy(&fz, &pvr_snapshot[12], 4);
    _ctx.rend.fZ_max                 = fz;
    _ctx.rend.isRTT                  = pvr_snapshot[13] != 0;

    // ---- CRITICAL FIX: Fill background polygon ----
    // FillBGP reads the background polygon strip from VRAM at the address
    // specified by ISP_BACKGND_T. The original WASM bridge SKIPPED this,
    // causing incorrect/missing background rendering. The server calls it
    // in rend_start_render(), but the client never did.
    FillBGP(&_ctx);

    // ---- Palette + fog update ----
    pal_needs_update = true;
    palette_update();
    renderer->updatePalette = true;
    renderer->updateFogTable = true;

    // ---- RENDER ----
    // Bind GLSM state — this is what RetroArch does before calling the core.
    // GLSM tracks GL state internally. STATE_BIND restores the core's GL state.
    // Without this, the GL state is corrupted and textures render as garbage.
    glsm_ctl(GLSM_CTL_STATE_BIND, nullptr);

    renderer->Process(&_ctx);        // Parse TA commands → vertex/polygon lists
    bool isScreen = renderer->Render();  // WebGL2 draw calls
    if (isScreen)
        renderer->Present();

    // Blit the postProcessor FBO to FBO 0 (canvas) with proper Y-flip
    postProcessor.render(0);

    // Unbind GLSM state — restores "frontend" GL state.
    // This is what RetroArch does after the core returns from retro_run().
    glsm_ctl(GLSM_CTL_STATE_UNBIND, nullptr);

    _frameCount++;
    FrameCount++;  // Global frame counter for texture cache cleanup

    return 1;
}

// ============================================================================
// renderer_resize — Handle canvas resize
// ============================================================================

EMSCRIPTEN_KEEPALIVE
void renderer_resize(int width, int height)
{
    wasm_gl_resize(width, height);
    if (renderer) {
        // Renderer will pick up new dimensions on next Render()
        settings.display.width = width;
        settings.display.height = height;
    }
}

// ============================================================================
// renderer_destroy — Cleanup
// ============================================================================

EMSCRIPTEN_KEEPALIVE
void renderer_destroy()
{
    if (renderer) {
        renderer->Term();
        delete renderer;
        renderer = nullptr;
    }
    wasm_gl_destroy();
    _initialized = false;
    _prevTA.clear();
    printf("[renderer] Destroyed\n");
}

} // extern "C"

// ============================================================================
// mirror_present_frame — No-op in standalone mode (no RetroArch to notify)
// The canvas is already updated by renderer->Present()
// ============================================================================

extern "C" void mirror_present_frame() {
    // In standalone mode, Present() already swaps the WebGL backbuffer.
    // Nothing to do here.
}

#endif // __EMSCRIPTEN__
