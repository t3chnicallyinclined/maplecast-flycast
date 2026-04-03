/*
	MapleCast Sprite Learner — builds sprite_id → texture mapping at runtime.

	While the game plays, correlates:
	  - sprite_id from game state (RAM address +0x144 per character)
	  - texture data from Flycast's render pipeline (TA PolyParam)
	  - character screen position to match polygon to character

	Builds a mapping file: {character_id, sprite_id} → texture image data
	The more you play, the more complete the atlas becomes.
*/
#pragma once
#include <cstdint>

namespace maplecast_spritelearn
{

// Call once per frame after rendering, with the current game state
void onFrameRendered();

// How many unique sprite frames have we learned?
int learnedCount();

}
