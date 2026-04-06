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
}
