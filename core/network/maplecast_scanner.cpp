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

// Load discovered animation states from visual_cache directory
static void loadDiscoveredStates(const char* cacheDir)
{
	for (int i = 0; i < TOTAL_CHARACTERS; i++)
		_validStates[i].clear();

	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		"ls %s/char_* 2>/dev/null | sed 's/.*char_\\([0-9]*\\)_anim\\([0-9a-f]*\\)_.*/\\1 \\2/' | sort -u",
		cacheDir);

	FILE* p = popen(cmd, "r");
	if (!p) return;

	int charId;
	char animHex[16];
	int totalLoaded = 0;
	while (fscanf(p, "%d %s", &charId, animHex) == 2)
	{
		if (charId >= 0 && charId < TOTAL_CHARACTERS)
		{
			int animState = (int)strtol(animHex, nullptr, 16);
			_validStates[charId].insert(animState);
			totalLoaded++;
		}
	}
	pclose(p);

	int charsWithStates = 0;
	for (int i = 0; i < TOTAL_CHARACTERS; i++)
		if (!_validStates[i].empty()) charsWithStates++;

	printf("[scanner] loaded %d known states for %d characters from cache\n",
		totalLoaded, charsWithStates);
}

// Iterator for targeted scan through known states
static std::vector<int> _currentCharStates;
static int _stateIdx = 0;

static void loadNextCharStates()
{
	_currentCharStates.clear();
	_stateIdx = 0;

	// Find next character with known states
	while (_charId < TOTAL_CHARACTERS)
	{
		if (!_validStates[_charId].empty())
		{
			_currentCharStates.assign(_validStates[_charId].begin(),
				_validStates[_charId].end());
			printf("[scanner] char %d: %zu known states to scan\n",
				_charId, _currentCharStates.size());
			return;
		}
		_charId++;
	}
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
	_stateIdx = 0;

	// Load known states from cache for targeted/full scan
	loadDiscoveredStates("visual_cache");

	// For targeted/full mode, start with known states (safe, no crashes)
	if (mode == ScanMode::FullScan || mode == ScanMode::Targeted)
	{
		loadNextCharStates();
		if (!_currentCharStates.empty())
			_animState = _currentCharStates[0];
	}

	_active = true;

	const char* modeStr = "unknown";
	switch (mode) {
		case ScanMode::Discovery: modeStr = "DISCOVERY (sweep, may crash on invalid states)"; break;
		case ScanMode::Targeted: modeStr = "TARGETED (known states only, safe)"; break;
		case ScanMode::FullScan: modeStr = "FULL SCAN (known states for all characters, safe)"; break;
		default: break;
	}

	printf("[scanner] === BRUTE FORCE SCANNER STARTED ===\n");
	printf("[scanner] Mode: %s\n", modeStr);
	printf("[scanner] Starting from character %d\n", startCharId);
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

	if (_mode == ScanMode::Discovery)
	{
		// Discovery: sweep all 16-bit states (DANGEROUS — may crash)
		_animState++;
		if (_animState > 0xFFFF)
		{
			printf("[scanner] Character %d complete: %zu valid states found\n",
				_charId, _validStates[_charId].size());
			_charsCompleted++;
			_charId++;
			_animState = 0;
			if (_charId >= TOTAL_CHARACTERS) { stop(); return; }
		}
		if (_animState % 1000 == 0 && _animState > 0)
			printf("[scanner] char %d: sweep 0x%04x...\n", _charId, _animState);
	}
	else if (_mode == ScanMode::FullScan || _mode == ScanMode::Targeted)
	{
		// Safe: only scan KNOWN valid states from gameplay discovery
		_stateIdx++;
		if (_stateIdx >= (int)_currentCharStates.size())
		{
			// Done with this character
			printf("[scanner] Character %d complete: scanned %zu known states\n",
				_charId, _currentCharStates.size());
			_charsCompleted++;
			_charId++;

			if (_charId >= TOTAL_CHARACTERS)
			{
				printf("[scanner] === ALL CHARACTERS COMPLETE ===\n");
				printf("[scanner] Total: %d states, %d frames captured\n",
					_totalStatesFound, _totalFramesCaptured);
				stop();
				return;
			}

			loadNextCharStates();
			if (_currentCharStates.empty())
			{
				// No known states for remaining characters — skip to next
				_charId++;
				while (_charId < TOTAL_CHARACTERS)
				{
					loadNextCharStates();
					if (!_currentCharStates.empty()) break;
					_charId++;
				}
				if (_charId >= TOTAL_CHARACTERS) { stop(); return; }
			}
		}

		if (_stateIdx < (int)_currentCharStates.size())
			_animState = _currentCharStates[_stateIdx];
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
