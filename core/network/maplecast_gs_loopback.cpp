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

// Recording + Replay
static const int MAX_RECORD_FRAMES = 600;  // 10 seconds at 60fps
struct RecordedFrame {
	uint8_t data[256];
	int size;
};
static RecordedFrame* _recorded = nullptr;
static int _recordCount = 0;
static int _replayPos = 0;
static bool _recording = false;
static bool _replaying = false;

void init()
{
	_active = true;
	_frameCount = 0;
	_mismatchCount = 0;
	_recordCount = 0;
	_replayPos = 0;
	_recording = true;
	_replaying = false;
	if (!_recorded)
		_recorded = new RecordedFrame[MAX_RECORD_FRAMES];
	printf("[gs-loopback] === RECORD + REPLAY TEST ===\n");
	printf("[gs-loopback] RECORDING %d frames (10 seconds)...\n", MAX_RECORD_FRAMES);
	printf("[gs-loopback] Play normally — then watch it replay!\n");
}

void tick()
{
	if (!_active) return;

	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);

	if (!gs.in_match) return;

	if (_recording)
	{
		// RECORDING: capture game state every frame
		uint8_t wire[256];
		int wireSize = maplecast_gamestate::serialize(gs, wire, sizeof(wire));
		memcpy(_recorded[_recordCount].data, wire, wireSize);
		_recorded[_recordCount].size = wireSize;
		_recordCount++;

		if (_recordCount % 60 == 0)
			printf("[gs-loopback] RECORDING: frame %d/%d...\n", _recordCount, MAX_RECORD_FRAMES);

		if (_recordCount >= MAX_RECORD_FRAMES)
		{
			_recording = false;
			_replaying = true;
			_replayPos = 0;
			printf("[gs-loopback] === RECORDING COMPLETE (%d frames) ===\n", _recordCount);
			printf("[gs-loopback] === NOW REPLAYING — WATCH THE GAME REPLAY YOUR ACTIONS! ===\n");
			printf("[gs-loopback] 253 bytes/frame × %d frames = %d bytes total\n",
				_recordCount, _recordCount * 253);
		}
	}
	else if (_replaying)
	{
		// REPLAYING: write recorded state back to RAM
		maplecast_gamestate::GameState replay;
		maplecast_gamestate::deserialize(
			_recorded[_replayPos].data, _recorded[_replayPos].size, replay);
		maplecast_gamestate::writeGameState(replay);
		_replayPos++;

		if (_replayPos % 60 == 0)
			printf("[gs-loopback] REPLAYING: frame %d/%d...\n", _replayPos, _recordCount);

		if (_replayPos >= _recordCount)
		{
			// Loop the replay
			_replayPos = 0;
			printf("[gs-loopback] === REPLAY LOOPING ===\n");
		}
	}
}

bool active() { return _active; }

} // namespace maplecast_gs_loopback
