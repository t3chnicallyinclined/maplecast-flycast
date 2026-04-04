/*
	MapleCast Client Mode — renderer-only playback.

	Replays rend_recording.bin through flycast's real OpenGL renderer
	with pre-loaded textures. No SH4 CPU, no ROM, no emulation.

	MAPLECAST_CLIENT=1 to enable.
*/
#pragma once

namespace maplecast_client
{
bool init();     // load recording + textures, returns false if files not found
bool active();
void renderFrame();  // called each frame instead of emulator render
}
