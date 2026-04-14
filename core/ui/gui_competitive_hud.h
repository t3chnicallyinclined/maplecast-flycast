/*
	Competitive HUD — always-on diagnostic overlay for the native client.

	Renders 3 small windows in a corner during gameplay:
	  • NETWORK   — server name, RTT, jitter, loss%, network grade S/A/B/C/F
	  • LATENCY   — input → wire / wire → pixel / E2E (button-to-display)
	  • INPUT     — recent gamepad inputs scrolling at the screen edge

	Each toggleable independently with F1-F4 (default: NETWORK + LATENCY on,
	INPUT off). Designed to be ALWAYS visible during play — competitive
	players want to see if their network is degrading mid-match.

	Hooked into gui_displayMirrorDebug() in gui.cpp, drawn after the game
	render but before buffer swap. Read-only ImGui windows (no input
	capture) so they don't steal mouse from the gear icon.
*/
#pragma once

namespace gui_competitive_hud
{
	// Drawing entry point. Call between ImGui::NewFrame() and ImGui::Render().
	// Cheap when disabled — single atomic load returns early.
	void draw();

	// Section toggles. Wired to F1-F4 in the SDL key handler.
	void toggleNetwork();
	void toggleLatency();
	void toggleInput();
	void toggleAll();
}
