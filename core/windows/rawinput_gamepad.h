/*
	MapleCast Raw Input / XInput direct gamepad bypass — Windows.
	See rawinput_gamepad.cpp for design. Linux uses maplecast_evdev_input.h
	for the equivalent bypass via /dev/input/event*.
*/
#pragma once

#ifdef _WIN32

namespace maplecast_rawinput
{
	// Initialize. Returns true if XInput / Raw Input bypass is active.
	// Reads MAPLECAST_RAWINPUT env var: "1" or "xinput" enables the XInput
	// fast path; "raw" / "all" reserved for future Raw Input fallback.
	// Returns false if env var unset (caller falls back to SDL).
	bool init();
	void shutdown();
	bool active();

	// Standalone wizard entry point. Called from winmain.cpp very early
	// (before flycast_init) when MAPLECAST_BUTTON_WIZARD=1 is set, so
	// the wizard runs even without MAPLECAST_MIRROR_CLIENT. Returns true
	// on success (mapping saved); the caller then std::exit(0).
	bool runWizardStandalone();

	// True when the XInput thread has loaded a user-defined button mapping
	// (from the wizard). When true, SDL's gamepad input pipeline should be
	// fully suppressed so its (possibly-default-broken) mapping doesn't
	// double-fire DreamcastKeys against the user's chosen ones.
	bool userMappingActive();
}

#endif // _WIN32
