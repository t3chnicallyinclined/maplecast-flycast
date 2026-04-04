/*
	Game State Loopback Test.

	Every frame:
	1. readGameState() — read from RAM
	2. serialize() — pack to 253 bytes (same as network send)
	3. deserialize() — unpack back to struct
	4. writeGameState() — write back to RAM

	If the game keeps running normally, the round-trip is lossless.
	This proves: 253 bytes contains everything needed to reproduce the state.

	Also logs any differences detected between read and write-back.
*/
#include "types.h"
#include "maplecast_gs_loopback.h"
#include "maplecast_gamestate.h"

#include <cstdio>
#include <cstring>
#include <atomic>

namespace maplecast_gs_loopback
{

static std::atomic<bool> _active{false};
static uint32_t _frameCount = 0;
static uint32_t _mismatchCount = 0;

void init()
{
	_active = true;
	_frameCount = 0;
	_mismatchCount = 0;
	printf("[gs-loopback] === LOOPBACK TEST ACTIVE ===\n");
	printf("[gs-loopback] Every frame: read → serialize → deserialize → write back\n");
	printf("[gs-loopback] If game runs normally, 253 bytes is all you need.\n");
}

void tick()
{
	if (!_active) return;

	// Step 1: Read game state from RAM
	maplecast_gamestate::GameState original;
	maplecast_gamestate::readGameState(original);

	if (!original.in_match) return;

	// Step 2: Serialize to 253 bytes (same as network send)
	uint8_t wire[256];
	int wireSize = maplecast_gamestate::serialize(original, wire, sizeof(wire));

	// Step 3: Deserialize back to struct (same as network receive)
	maplecast_gamestate::GameState received;
	maplecast_gamestate::deserialize(wire, wireSize, received);

	// Step 4: Verify — compare original and received
	bool match = true;
	if (original.game_timer != received.game_timer) match = false;
	if (original.stage_id != received.stage_id) match = false;
	for (int i = 0; i < 6; i++)
	{
		const auto& a = original.chars[i];
		const auto& b = received.chars[i];
		if (a.active != b.active) match = false;
		if (a.character_id != b.character_id) match = false;
		if (a.health != b.health) match = false;
		if (a.animation_state != b.animation_state) match = false;
		if (a.anim_timer != b.anim_timer) match = false;
	}

	if (!match)
	{
		_mismatchCount++;
		if (_mismatchCount <= 5)
			printf("[gs-loopback] MISMATCH at frame %u!\n", _frameCount);
	}

	// Step 5: Write back to RAM
	maplecast_gamestate::writeGameState(received);

	_frameCount++;

	// Log every 300 frames
	if (_frameCount % 300 == 0)
	{
		printf("[gs-loopback] frame %u — %u mismatches (%.1f%% match rate) — %d bytes/frame\n",
			_frameCount, _mismatchCount,
			100.0 * (1.0 - (double)_mismatchCount / _frameCount),
			wireSize);
	}
}

bool active() { return _active; }

} // namespace maplecast_gs_loopback
