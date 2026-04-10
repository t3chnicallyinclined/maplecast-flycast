/*
	MapleCast Input Sink — sends local SDL gamepad events to the server.

	Registers a ButtonListener on the SDL gamepad device. When a button
	is pressed or released, fires a 7-byte UDP packet to the server's
	input port (7100) immediately, inline on the SDL event thread. No
	polling, no kcode[] involvement, no feedback loops.

	Used by the native mirror client (MAPLECAST_MIRROR_CLIENT) to let
	players control the game from a native flycast that only renders
	the TA stream — no local SH4.
*/
#pragma once

namespace maplecast_input_sink
{
	// Start the input sink. Opens a UDP socket to host:7100 and
	// registers the SDL ButtonListener. slot = which DC controller
	// port this client claims (0 or 1).
	bool init(const char* host, int slot = 0);
	void shutdown();
	bool active();
}
