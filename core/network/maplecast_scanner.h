/*
	MapleCast Brute Force Scanner — automated visual state discovery.

	Forces every character into every animation state at every frame,
	lets the emulator render it, captures the TA output.

	Populates the entire visual cache in ~47 minutes unattended.
	Discovery mode: sweep animation_state 0x0000-0xFFFF to find valid states.
	Targeted mode: cycle known states from gameplay discovery.

	Uses addrspace::write() to inject character state directly into
	emulated Dreamcast RAM — same addresses the game state reader uses.
*/
#pragma once
#include <cstdint>

namespace maplecast_scanner
{

enum class ScanMode {
	Off,             // Normal gameplay
	Discovery,       // Sweep all animation states for a character
	Targeted,        // Cycle known states from cache
	FullScan,        // All characters, all states, all frames
};

// Start scanning — takes over P1 character control
void start(ScanMode mode, int startCharId = 0);

// Stop scanning — returns control to normal gameplay
void stop();

// Called every frame from the render pipeline
// Writes character state to RAM, advances to next state after capture
void tick();

// Is scanning active?
bool active();

// Get progress
struct ScanProgress {
	int currentCharId;
	int currentAnimState;
	int currentFrame;
	int totalStatesFound;
	int totalFramesCaptured;
	int charactersCompleted;
};
ScanProgress getProgress();

}
