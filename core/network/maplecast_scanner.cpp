/*
	MapleCast Brute Force Scanner — automated visual state discovery.

	We control the emulator. We have addrspace::write().
	We know every RAM address. We force-write character state,
	let flycast render one frame, capture the TA output.

	Every visual frame of every move of every character.
	168,000 states / 60fps = 47 minutes for the entire game.

	WE ARE INSANE.
*/
#include "types.h"
#include "maplecast_scanner.h"
#include "maplecast_gamestate.h"
#include "hw/sh4/sh4_mem.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>
#include <set>

namespace maplecast_scanner
{

// MVC2 RAM addresses (same as maplecast_gamestate.cpp)
static const uint32_t CHAR_BASE[] = {
	0x8C268340,  // P1 Character 1 (point)
	0x8C2688E4,  // P2 Character 1 (point)
	0x8C268E88,  // P1 Character 2 (assist 1)
	0x8C26942C,  // P2 Character 2 (assist 1)
	0x8C2699D0,  // P1 Character 3 (assist 2)
	0x8C269F74,  // P2 Character 3 (assist 2)
};

static const uint32_t OFF_CHAR_ID       = 0x001;
static const uint32_t OFF_ANIM_STATE    = 0x1D0;
static const uint32_t OFF_ANIM_TIMER    = 0x142;
static const uint32_t OFF_ACTIVE        = 0x000;
static const uint32_t OFF_FACING        = 0x110;
static const uint32_t OFF_POS_X         = 0x034;
static const uint32_t OFF_POS_Y         = 0x038;
static const uint32_t OFF_HEALTH        = 0x420;

static const uint32_t ADDR_IN_MATCH     = 0x8C289624;
static const uint32_t ADDR_TIMER        = 0x8C289630;

// Total MVC2 characters (0-58)
static const int TOTAL_CHARACTERS = 59;

// Scanner state
static std::atomic<bool> _active{false};
static ScanMode _mode = ScanMode::Off;

static int _charId = 0;
static int _animState = 0;
static int _frame = 0;
static int _maxFramesPerState = 60;  // max frames to try per animation state
static int _prevAnimState = -1;      // detect when game resets the animation

static int _totalStatesFound = 0;
static int _totalFramesCaptured = 0;
static int _charsCompleted = 0;

// Discovered valid animation states per character
static std::set<int> _validStates[TOTAL_CHARACTERS];

// Track what we've already scanned
static std::set<uint64_t> _scannedStates;

static void advanceState();

static uint64_t scanKey(int charId, int animState, int frame)
{
	return ((uint64_t)charId << 32) | ((uint64_t)animState << 16) | frame;
}

// Force character state into RAM
static void writeCharState(int slot, int charId, int animState, int frame)
{
	uint32_t base = CHAR_BASE[slot];

	// Keep the game in-match with frozen timer
	addrspace::write8(ADDR_IN_MATCH, 1);
	addrspace::write8(ADDR_TIMER, 99);

	// Set character active with full health
	addrspace::write8(base + OFF_ACTIVE, 1);
	addrspace::write8(base + OFF_CHAR_ID, (uint8_t)charId);
	addrspace::write8(base + OFF_HEALTH, 0x90);  // max health

	// Force animation state and frame
	addrspace::write16(base + OFF_ANIM_STATE, (uint16_t)animState);
	addrspace::write16(base + OFF_ANIM_TIMER, (uint16_t)frame);

	// Center character on screen
	float centerX = 0.0f;
	float centerY = 0.0f;
	addrspace::write32(base + OFF_POS_X, *(uint32_t*)&centerX);
	addrspace::write32(base + OFF_POS_Y, *(uint32_t*)&centerY);
	addrspace::write8(base + OFF_FACING, 1);  // face right
}

void start(ScanMode mode, int startCharId)
{
	_mode = mode;
	_charId = startCharId;
	_animState = 0;
	_frame = 0;
	_totalStatesFound = 0;
	_totalFramesCaptured = 0;
	_charsCompleted = 0;
	_scannedStates.clear();
	_active = true;

	const char* modeStr = "unknown";
	switch (mode) {
		case ScanMode::Discovery: modeStr = "DISCOVERY (sweep 0x0000-0xFFFF)"; break;
		case ScanMode::Targeted: modeStr = "TARGETED (known states only)"; break;
		case ScanMode::FullScan: modeStr = "FULL SCAN (all characters)"; break;
		default: break;
	}

	printf("[scanner] === BRUTE FORCE SCANNER STARTED ===\n");
	printf("[scanner] Mode: %s\n", modeStr);
	printf("[scanner] Starting from character %d\n", startCharId);
	printf("[scanner] %d characters × ~300 states × ~10 frames = ~47 minutes\n",
		TOTAL_CHARACTERS);
	printf("[scanner] WE ARE INSANE.\n");
}

void stop()
{
	_active = false;
	_mode = ScanMode::Off;
	printf("[scanner] === STOPPED ===\n");
	printf("[scanner] %d states found, %d frames captured, %d characters completed\n",
		_totalStatesFound, _totalFramesCaptured, _charsCompleted);
}

void tick()
{
	if (!_active) return;

	// Write character state to RAM — the emulator will render this next frame
	writeCharState(0, _charId, _animState, _frame);

	// Check if the game accepted our animation state
	// (invalid states get overwritten by the game engine)
	uint16_t actualAnim = addrspace::read16(CHAR_BASE[0] + OFF_ANIM_STATE);
	uint16_t actualFrame = addrspace::read16(CHAR_BASE[0] + OFF_ANIM_TIMER);

	if (actualAnim == (uint16_t)_animState)
	{
		// State is valid — the game kept it
		uint64_t key = scanKey(_charId, _animState, _frame);
		if (!_scannedStates.count(key))
		{
			_scannedStates.insert(key);
			_totalFramesCaptured++;

			if (_frame == 0)
			{
				_validStates[_charId].insert(_animState);
				_totalStatesFound++;
			}
		}

		// Advance to next frame
		_frame++;

		// Check if animation looped or exceeded max
		if (_frame >= _maxFramesPerState)
		{
			// Done with this state, move to next
			advanceState();
		}
	}
	else
	{
		// Game rejected this animation state — skip it
		advanceState();
	}
}

static void advanceState()
{
	_frame = 0;

	if (_mode == ScanMode::Discovery || _mode == ScanMode::FullScan)
	{
		_animState++;

		// Discovery: sweep through all possible 16-bit animation states
		if (_animState > 0xFFFF)
		{
			// Done with this character
			printf("[scanner] Character %d complete: %zu valid states found\n",
				_charId, _validStates[_charId].size());
			_charsCompleted++;
			_charId++;
			_animState = 0;

			if (_charId >= TOTAL_CHARACTERS)
			{
				printf("[scanner] === ALL CHARACTERS COMPLETE ===\n");
				printf("[scanner] Total: %d states, %d frames\n",
					_totalStatesFound, _totalFramesCaptured);
				stop();
				return;
			}

			printf("[scanner] Starting character %d/%d\n", _charId, TOTAL_CHARACTERS);
		}

		// Progress log every 1000 states
		if (_animState % 1000 == 0 && _animState > 0)
		{
			printf("[scanner] char %d: scanning anim 0x%04x... (%d states found so far)\n",
				_charId, _animState, _totalStatesFound);
		}
	}
	else if (_mode == ScanMode::Targeted)
	{
		// Targeted: only scan known valid states (from previous gameplay)
		// TODO: load valid states from cache directory scan
		_animState++;
		if (_animState > 0xFFFF)
		{
			_charsCompleted++;
			_charId++;
			_animState = 0;
			if (_charId >= TOTAL_CHARACTERS) { stop(); return; }
		}
	}
}

bool active()
{
	return _active.load(std::memory_order_relaxed);
}

ScanProgress getProgress()
{
	ScanProgress p;
	p.currentCharId = _charId;
	p.currentAnimState = _animState;
	p.currentFrame = _frame;
	p.totalStatesFound = _totalStatesFound;
	p.totalFramesCaptured = _totalFramesCaptured;
	p.charactersCompleted = _charsCompleted;
	return p;
}

} // namespace maplecast_scanner
