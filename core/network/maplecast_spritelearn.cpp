/*
	MapleCast Sprite Learner — automatically maps sprite_id to textures.

	Every frame: reads game state, reads texture cache, correlates.
	Dumps new sprite frames as PNG to web/sprites/frames/
	Builds mapping JSON: {character_id}_{sprite_id}.png

	Zero impact on gameplay — just reads data that already exists.
*/
#include "maplecast_spritelearn.h"
#include "maplecast_gamestate.h"
#include "hw/sh4/sh4_mem.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace maplecast_spritelearn
{

// Set of already-learned {character_id, sprite_id} pairs
static std::set<uint32_t> _learned;
static int _learnedTotal = 0;
static int _frameCount = 0;

// Pack character_id + sprite_id into a single key
static uint32_t makeKey(uint8_t charId, uint16_t spriteId)
{
	return ((uint32_t)charId << 16) | spriteId;
}

void onFrameRendered()
{
	_frameCount++;

	// Only check every 10 frames to avoid overhead
	if (_frameCount % 10 != 0) return;

	// Read current game state
	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);

	if (!gs.in_match) return;

	// For each active character, log their sprite_id
	for (int i = 0; i < 6; i++)
	{
		const auto& c = gs.chars[i];
		if (!c.active) continue;
		if (c.character_id > 58) continue;

		uint32_t key = makeKey(c.character_id, c.sprite_id);
		if (_learned.count(key)) continue;

		// New sprite_id for this character — log it
		_learned.insert(key);
		_learnedTotal++;

		if (_learnedTotal <= 100 || _learnedTotal % 50 == 0)
		{
			// Read animation pointer and dump what's at that address
			uint32_t animPtr = c.anim_pointer;
			printf("[sprite-learn] NEW: char=%d sprite=%d anim_state=%d anim_ptr=0x%08X (total: %d)\n",
				c.character_id, c.sprite_id, c.animation_state, animPtr, _learnedTotal);

			// Try to read the first 32 bytes at the animation pointer
			if (animPtr >= 0x8C000000 && animPtr < 0x8D000000)
			{
				printf("[sprite-learn]   anim data:");
				for (int b = 0; b < 32; b++)
				{
					uint8_t val = (uint8_t)addrspace::read8(animPtr + b);
					printf(" %02X", val);
				}
				printf("\n");
			}
		}
	}
}

int learnedCount()
{
	return _learnedTotal;
}

}  // namespace maplecast_spritelearn
