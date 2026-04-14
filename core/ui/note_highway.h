/*
	Note Highway — DDR / Guitar Hero style scrolling-input widget.

	When a combo trainer session is active (user loaded a .mccombo),
	this widget shows upcoming inputs as notes falling toward a "hit
	zone" at the bottom of the screen. The player presses each button
	as its note crosses the line; accuracy is scored PERFECT / GREAT /
	GOOD / MISS based on frame proximity.

	The data source is the currently-loaded replay's input log
	(via replay_reader). We read ahead N frames from the current
	emulator frame and render each input change as a colored note
	on a 6-lane highway (one lane per face button + LP, HP, PP/KK).

	This is the Phase 8 crown feature — replay + deterministic
	playback unlocked it. The game plays the combo visually; the
	learner has to match it with their own inputs.
*/
#pragma once

#include <cstdint>

namespace note_highway
{
	// Drawing entry point. Call between ImGui::NewFrame() and
	// ImGui::Render() while the HUD is rendered. Cheap no-op when no
	// combo is loaded (single atomic check returns early).
	void draw();

	// Enable/disable. Also wired to a keybind in sdl.cpp.
	void toggle();
	bool isActive();

	// Register a "note hit" from the input sink — any button change.
	// Scoring compares to the scheduled note at the player's current
	// frame. Updates the running score (PERFECT/GREAT/GOOD/MISS counts).
	void onPlayerInput(uint16_t buttons, uint8_t lt, uint8_t rt);

	struct Score {
		uint32_t perfect;
		uint32_t great;
		uint32_t good;
		uint32_t miss;
		uint32_t streak;
		uint32_t best_streak;
		uint32_t total_notes;
	};
	Score currentScore();
	void resetScore();
}
