/*
	MapleCast WebSocket Server — lightweight binary broadcast.
	No CUDA, no NVENC. Just WebSocket.
*/
#pragma once
#include <cstddef>

namespace maplecast_ws
{
bool init(int port = 7200);
void shutdown();
bool active();
void broadcastBinary(const void* data, size_t size);
}
