/*
	MapleCast TA Capture — extracts UV coordinates from the Tile Accelerator
	display list to identify which sprite frame is being drawn per character.

	Called after rendering. Matches TA polygons to character screen positions,
	extracts UV coords, adds to game state packet.
*/
#pragma once
#include <cstdint>

namespace maplecast_ta_capture
{

// Per-character sprite frame info extracted from TA display list
struct SpriteFrame {
	float u0, v0, u1, v1;   // UV bounding box in texture space [0,1]
	uint16_t tex_width;       // texture dimensions
	uint16_t tex_height;
	uint32_t tex_addr;        // DC VRAM address of texture
	bool found;               // true if a matching polygon was found
};

// Called from render pipeline before Present() to save the render context
void setRendContext(void* ctx);  // actually rend_context*, void to avoid include

// Call after rendering to extract UV data for active characters
void captureFrame(const float screen_x[6], const float screen_y[6], const bool active[6]);

// Get the captured sprite frame info for a character slot
const SpriteFrame& getFrame(int slot);

}
