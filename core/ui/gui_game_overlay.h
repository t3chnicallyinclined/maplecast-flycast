/*
	Game Data Overlay — MVC2 state + input display for the native client.

	Three layers (each independently toggleable):
	  F5 — GAME DATA: health bars, meter, combo, character names
	  F6 — INPUT:     current button state + 60-frame scrolling history
	  F7 — reserved for hitbox visualization (Phase 8+)

	Input history is the precursor to the DDR/Guitar Hero combo trainer:
	the same scrolling timeline, just showing YOUR inputs instead of
	target inputs. When a .mccombo is loaded, the note_highway takes
	over with target notes + scoring.

	Data source: maplecast_mirror::getClientGameState() for game data,
	local kcode[] snapshot for input display.
*/
#pragma once
#include <cstdint>

namespace gui_game_overlay
{
	void draw();
	void toggleGameData();
	void toggleInput();

	// Record a button state snapshot. Called from the input sink's
	// onButton callback so we capture every edge, not just per-frame polls.
	void recordInput(uint16_t buttons, uint8_t lt, uint8_t rt);
}
