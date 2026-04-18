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
Telemetry getLastTelemetry();
}
