/*
	MapleCast Mirror Client — receives delta-encoded TA commands, renders them.

	Native: reads from shared memory ring buffer (/dev/shm/maplecast_mirror)
	WASM: receives frames via wasm_bridge.cpp (JS -> mirror_render_frame)
*/
#pragma once

struct rend_context;

namespace maplecast_mirror
{
void initClient();
bool isClient();

// Read the latest frame from server, decode deltas, render.
// Returns true if a new frame was rendered. Sets vramDirty if VRAM pages changed.
bool clientReceive(rend_context& rc, bool& vramDirty);
}
