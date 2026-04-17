/*
	MapleCast Settings Panel — competitive client configuration overlay.

	Toggled with ` (backtick). Separate from:
	  - Tab (flycast's built-in settings)
	  - F1-F3 (competitive HUD diagnostics)

	Sections:
	  GRAPHICS — resolution, filtering, transparency layers, fog, modvol
	  PRESETS  — Performance / Arcade / Max Quality (one-click)
	  NETWORK  — latch policy, direct/relay, player slot (players only)
*/
#pragma once

namespace gui_maplecast_settings
{
	void draw();
	void toggle();
	bool isOpen();
}
