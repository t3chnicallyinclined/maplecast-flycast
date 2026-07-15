/*
	MapleCast WebSocket Server — lightweight binary broadcast.
	No CUDA, no NVENC. Just WebSocket.
*/
#pragma once
#include <cstddef>
#include <cstdint>

namespace maplecast_ws
{
bool init(int port = 7200);
void shutdown();
bool active();
void broadcastBinary(const void* data, size_t size);

// Drain a pending MCSV (mid-match savestate) capture request. MUST be called
// from the EMU THREAD at a frame boundary (i.e. from serverPublish), because
// dc_serialize captures live SH4 register state — taking it from the 1Hz
// status thread mid-execution can snapshot SR.BL=1 (mid-interrupt), which
// crashes replica clients on load with "SH4 exception when blocked". A no-op
// unless checkMatchEnd has requested a capture.
void drainMcsvCapture();

// Live state migration (docs/STATE-HANDOFF-PLAN.md), two halves with DIFFERENT
// thread contracts (learned the hard way on the 2026-07-15 live gate):
//
// drainMigration() — the SOURCE capture: builds + pushes the state when a
//   player's "migrate" message is pending. Called from the serverPublish site
//   (render thread) — dc_serialize from there is the same proven path as
//   drainMcsvCapture / control-WS savestate_save.
//
// applyPendingMigration() — the DESTINATION apply: EMU THREAD with the SH4
//   PAUSED. The STPU receipt arms raArmStepStop() so runInternal() returns at
//   the next true frame boundary; the emu loop then calls this in the same
//   stop-callback-restart slot the rollback rewind / oracle-probe reload use,
//   and must Start()+continue when it returns true. Applying from the render
//   thread swaps RAM under a running SH4 → verify() abort (gate 2, 2026-07-15);
//   a loop-top hook never fires because runInternal doesn't return per frame
//   on a plain server (gate 3).
//
// Both no-op unless MAPLECAST_FLEET_KEY-authenticated work is pending.
void drainMigration();
bool applyPendingMigration();   // true = a pending state was consumed (resume SH4)

// Build a fresh "SYNC" packet from current vram[]/pvr_regs, zstd-compress it
// (ZCST magic), and broadcast to ALL connected clients. Called by the mirror
// server on scene transitions so non-seed clients get a clean state instead
// of trying to limp along with stale textures from missed DMA writes.
void broadcastFreshSync();

// Build a "FSYN" full-state packet — full DC save state via dc_serialize
// (PVR + TA contexts + TA FSM + everything), zstd-compress it, broadcast
// to ALL connected clients. The WASM client deserializes only the PVR
// section it cares about. Used as the heaviest possible scene-change
// fix when a normal SYNC isn't enough.
void broadcastFullSync();

// Build a "SAVE" envelope holding the FULL dc_serialize blob (~26 MB raw,
// ~3-5 MB compressed) and broadcast to ALL connected clients. Triggered
// by SIGUSR1 via maplecast_mirror::requestFullSaveStateBroadcast().
// Debug last-resort: if shipping every byte of state doesn't fix a
// glitch, the bug isn't a missing-state bug.
void broadcastFullSaveState();

// Compress the given pre-built SAVE envelope (already prefixed with
// "SAVE" magic + size header) and broadcast to all WS clients. Used by
// maplecast_mirror::doForcedSaveStateBroadcast() which reads the on-disk
// /dev/shm/maplecast_sync.state file produced by serverSaveSync().
void broadcastSaveStateBytes(const void* data, size_t size);

// Telemetry — updated by mirror publish
struct Telemetry {
	uint32_t frameNum;
	uint32_t taSize;
	uint32_t dirtyPages;
	uint32_t deltaSize;
	uint64_t publishUs;  // time to encode + broadcast one frame
	uint64_t fps;
	uint32_t compressedSize; // wire size after zstd compression
	uint64_t compressUs;     // zstd compression time (microseconds)
};
void updateTelemetry(const Telemetry& t);

// Tele-0.9: broadcast a match-end event JSON to all WS clients on
// in_match 1->0. start_us / end_us are wall-clock microseconds since
// epoch; start_gs / end_gs are the game-state snapshots at each
// transition. Includes a winner inferred from final HP totals.
// Forward-declared so maplecast_mirror.cpp can call without pulling
// the gamestate header into the public ws_server interface.
}
namespace maplecast_gamestate { struct GameState; }
namespace maplecast_ws {
void broadcastMatchEnd(int64_t start_us, int64_t end_us,
                       const maplecast_gamestate::GameState& start_gs,
                       const maplecast_gamestate::GameState& end_gs);
Telemetry getLastTelemetry();
int clientCount();
}
