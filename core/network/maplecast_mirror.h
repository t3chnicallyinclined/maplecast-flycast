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
#include <cstddef>
#include <string>
#include "maplecast_gamestate.h"

struct rend_context;
struct TA_context;

namespace maplecast_mirror
{
void initServer();
void initClient();
// Start only the WS mirror stream receiver (for TA correction) without
// setting isClient mode (which disables the GUI and SH4). Used by
// maplecast_replica to get VRAM/PVR correction alongside a running SH4.
void startMirrorStream(const char* host, int port);
bool isServer();
bool isClient();
// True if MapleCast is active in ANY mode (server, client, or local with MAPLECAST=1).
// Use this instead of isClient() to gate features that should work everywhere.
inline bool isActive() { return isServer() || isClient(); }

// Phase 2: hub-discovery's runner-up server, used by the input-sink as a
// hot-standby UDP target for failover. Empty string if no backup
// available (single-server hub or hub-discovery disabled).
const std::string& clientBackupServerHost();

// True iff MAPLECAST_HEADLESS=1 was set in the environment at startup.
// When headless, flycast boots without creating an SDL window, without
// an OpenGL/Vulkan context, and without any imgui driver. The norend
// renderer is wired in instead, and serverPublish() still runs the exact
// same CPU-only path. Wire bytes are guaranteed byte-identical to the
// GPU-backed build (enforced by the MAPLECAST_DUMP_TA determinism rig).
// Checked once at startup; subsequent env changes are ignored.
bool isHeadless();

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

// Build the full DC save state via dc_serialize into a freshly malloc'd
// buffer. Caller must free() it. Returns nullptr on failure. This is the
// same data serverSaveSync() writes to disk.
uint8_t* buildFullSaveState(size_t& outSize);

// Run serverSaveSync() (writes /dev/shm/maplecast_sync.state) then read
// the file back and broadcast it to all WS clients wrapped in a "SAVE"
// envelope. Triggered by SIGUSR1.
void doForcedSaveStateBroadcast();

// Set a flag that serverPublish() drains on the next frame to broadcast
// the full save state to all connected WS clients. Safe to call from any
// thread (atomic). Used by the SIGUSR1 handler to manually trigger a
// full-state push for debugging.
void requestFullSaveStateBroadcast();

// Phase A — read-only accessors for the input latch path (called from
// ggpo::getLocalInput at vblank time) and the status JSON broadcaster.
// Both are cheap atomic loads with acquire ordering — safe to call from
// any thread, no locking, no shm header touching. Updated once per frame
// at the bottom of serverPublish() under release ordering.
//
// currentFrame()    — monotonic frame counter, mirrors hdr->frame_count.
//                     Returns 0 before the first frame is published.
// lastLatchTimeUs() — CLOCK_MONOTONIC microseconds at the moment the most
//                     recent serverPublish() committed. Returns 0 before
//                     the first frame is published.
// framePeriodUs()   — exponential moving average of (publish_n - publish_{n-1})
//                     over the last ~16 frames. Used by the frame_phase block
//                     in status JSON for browser-side phase-aligned send
//                     scheduling. Returns ~16670 µs default before the EMA
//                     has had a chance to converge.
uint64_t currentFrame();
int64_t  lastLatchTimeUs();
int64_t  framePeriodUs();

// Client-side telemetry snapshot for the ImGui debug overlay. All values
// are atomic loads — the snapshot may mix a trailing and leading edge of
// one frame's updates, but that's fine for a once-per-frame overlay.
struct ClientStats {
	bool     wsConnected;
	uint64_t frameCount;            // current client frame
	uint64_t packetsReceived;       // total WS frames received
	uint64_t bytesReceived;         // total WS payload bytes received
	int64_t  lastDecodeUs;          // last clientReceive() decode cost
	int64_t  decodeEmaUs;           // EMA of decode cost
	uint32_t lastDirtyPages;        // dirty page count on last applied frame
	uint32_t lastTaSize;            // TA buffer size on last applied frame
	bool     lastVramDirty;         // did the last frame touch VRAM
	int64_t  lastArrivalUs;         // steady_clock µs of last WS frame
	int64_t  arrivalEmaUs;          // EMA of video-WS arrival interval
	int64_t  arrivalMaxUs;          // peak video-WS arrival interval since last reset
};
ClientStats getClientStats();

// Reset the arrivalMaxUs peak watermark. All other counters keep running.
void resetClientStatsPeaks();

// Game state from server (for overlay/HUD). Returns false if no state received yet.
bool getClientGameState(maplecast_gamestate::GameState& out);

// Force the video WS client to drop its current connection. The receive
// thread sees the close and the existing reconnect loop picks it up.
// Used by the debug overlay's "Reconnect Video" button.
void requestClientVideoReconnect();
}
