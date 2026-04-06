/*
	MapleCast Mirror — shared memory rend_context streaming between two flycast instances.

	Server (MAPLECAST_MIRROR_SERVER=1):
	  Every frame: writes rend_context to shared memory file
	  The game runs normally. Zero overhead — just a memcpy.

	Client (MAPLECAST_MIRROR_CLIENT=1):
	  Every frame: reads rend_context from shared memory, renders it
	  Skips its own CPU/TA. Just renders what the server computed.

	Both instances share /dev/shm/maplecast_mirror (mmap'd)
*/
#pragma once

#include <cstdint>

struct rend_context;
struct TA_context;

namespace maplecast_mirror
{
void initServer();
void initClient();
bool isServer();
bool isClient();

// Server: write this frame's TA context to shared memory
void serverPublish(TA_context* ctx);

// Client: read the latest rend_context from shared memory into rc
// Returns true if a new frame is available. Sets vramDirty if VRAM pages changed.
bool clientReceive(rend_context& rc, bool& vramDirty);

// Mark VRAM pages as dirty so the next serverPublish() streams them.
// Called from DMA paths (Ch2 DMA, PVR DMA, TAWriteSQ 64-bit, ELAN texture
// DMA, YUV converter) which memcpy directly into vram[] and bypass both the
// page-protect SIGSEGV handler and the shadow-copy memcmp diff.
// `offset` and `size` are in VRAM bytes (0..VRAM_SIZE).
// No-op when the mirror server isn't running.
void markVramDirty(uint32_t offset, uint32_t size);

// Force a fresh full SYNC broadcast on the next serverPublish() call.
// Used by:
//   - SB_SFRES soft reset (player presses A+B+X+Y+Start on a Dreamcast pad)
//   - dc_reset(true) hard reset / boot
//   - Anything else that knows the renderer state is about to be invalidated
// The publish path serializes the SYNC build/broadcast on the render thread
// to avoid races with VRAM mid-update.
void requestSyncBroadcast();
}
