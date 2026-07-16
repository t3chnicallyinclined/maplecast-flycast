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
#include <vector>
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

// State-replica transport: connect the mirror WS but consume ONLY GSTA/OBJF
// state packets — never apply the server's TA delta or VRAM/PVR SYNC (the local
// SH4 owns the framebuffer). Use this instead of startMirrorStream for the
// state-replica client so the local render isn't clobbered. If vramSync=true,
// the initial SYNC frame is applied once to seed local VRAM from prod's current
// textures, eliminating savestate parity issues.
void startGstaStream(const char* host, int port, bool vramSync = false);

// Switch a running mirror stream from full TA mode to GSTA-only mid-stream.
// Used by state-replica after MCSV is applied: the SH4 takes over rendering,
// so TA delta frames should stop being applied.
void switchToGstaOnly();

// Enable or disable the _isClient flag at runtime.
// State-replica sets true during phase-1 (no-ROM wait) so clientReceive()
// renders the TA stream; sets false after MCSV is applied so the SH4 takes over.
void setClientRendering(bool enabled);

// Re-apply the most recently received SYNC frame's VRAM + PVR registers.
// Call this immediately after dc_loadstate_from_memory() — the loadstate
// overwrites VRAM with match-start textures, which may be missing characters
// loaded later. The saved SYNC has the server's current texture state.
void reapplyLastSyncVram();

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

// Native GSTA client (feat/render-replica-live): true when MAPLECAST_GSTA_CLIENT
// mode is active (connected to the replica-live 7212 wire, rendering the GSTA
// state through flycast's own renderer via the transpiled render_frame). When
// true, the mirror render loop calls clientReceiveGsta() instead of clientReceive().
bool gstaModeActive();
bool clientReceiveGsta(rend_context& rc, bool& vramDirty);

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
// TDW joiner service: queue a TDWS dict snapshot + stream restart on the next
// publish. Join-path only — independent of the SYNC broadcast machinery (its
// kill switches / rate limits must never gate a joiner's dictionary).
void requestTdwSnapshot();
// A2 run-ahead: suppress serverPublish for the hidden authoritative frame (emu loop sets/clears).
void setSuppressPublish(bool v);
bool suppressActive();
// Read the per-STARTRENDER context-stamped hidden-leg flag (race-free vs live suppressActive()).
// ctxv is the TA_context*; null-safe. Gates the :7212 /replica-live capture to leg3 (N+1) only.
bool ctxIsHiddenLeg(void* ctxv);
void raArmStepStop();
bool raConsumeStepStop();
void raArmPublishStop();      // A2 short-leg3: halt leg3 right after the N+1 publish SR
bool raConsumePublishStop();
uint32_t currentGuestVf();
// A2 ADAPTIVE run-ahead (super-drop fix): the previous PUBLISHED frame's uncompressed TA
// delta size (super ~480-520KB vs ~150KB neutral) — the emu-thread gate reads this to skip
// run-ahead's one-shot leg3 on heavy/super frames (where its publish misses -> the tick ships
// nothing -> 35-45fps). Plus a read+reset counter of real (non-hidden) publishes/window = the
// proof metric (must climb 35-45 -> 60 during supers once adaptive is on).
uint32_t lastPublishedTaSize();
// A2 ADAPTIVE v2: the FULL inner-frame payload (VRAM dirty pages + TA + STM2) — the CORRECT
// heaviness signal for the run-ahead gate (balloons 150->480KB+ on supers; TA size does not).
uint32_t lastPublishedTotalSize();
uint32_t consumePublishBroadcasts();

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

// Mid-match join: true when the server has sent an MCSV savestate blob that
// hasn't been consumed yet. takePendingSaveState() moves the blob into `out`
// and clears the flag; call it from the emu thread (frameInject).
bool hasPendingSaveState();
bool takePendingSaveState(std::vector<uint8_t>& out);

// Full object pool from server (OBJF) for the state-replica inject. Copies up
// to maxObjs into out, returns the count (0 if none received yet).
int getClientObjects(maplecast_gamestate::ObjectState* out, int maxObjs);

// Force the video WS client to drop its current connection. The receive
// thread sees the close and the existing reconnect loop picks it up.
// Used by the debug overlay's "Reconnect Video" button.
void requestClientVideoReconnect();
}
