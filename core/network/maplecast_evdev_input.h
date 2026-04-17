/*
	MapleCast Direct Evdev Input — bypass SDL for fighting game sticks.

	Reads /dev/input/eventN directly on a dedicated SCHED_FIFO thread,
	bypassing SDL's event queue which adds ~1-3ms of batching latency.

	SDL still handles the window and non-gamepad input. This thread ONLY
	handles the fight stick — it detects joystick devices automatically
	and reads button/axis events as fast as the kernel delivers them.

	Enable with MAPLECAST_EVDEV_INPUT=1 (auto-detects the first joystick).
	Or specify a device: MAPLECAST_EVDEV_DEVICE=/dev/input/event14

	The evdev events are translated to DC button bits using the same
	mapping as the evdev_gamepad.h default mapping, then written to
	the input sink's _buttons + sendState() — same path as SDL, just
	faster delivery.
*/
#pragma once

namespace maplecast_evdev_input
{
	// Start the evdev input thread. Returns true if a device was found.
	bool init();
	void shutdown();
	bool active();
}
