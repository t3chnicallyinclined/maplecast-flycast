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
}
