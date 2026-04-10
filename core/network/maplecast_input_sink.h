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
#include <cstdint>

namespace maplecast_input_sink
{
	bool init(const char* host, int slot = 0);
	void shutdown();
	bool active();

	struct Stats {
		uint64_t packetsSent;
		uint64_t buttonChanges;
		uint64_t triggerChanges;
		uint32_t sendRateHz;
		int64_t  lastSendUs;
		// E2E latency probe
		double   e2eLastMs;         // last measured button-to-visual latency
		double   e2eEmaMs;          // exponential moving average
		double   e2eMinMs;          // session minimum
		double   e2eMaxMs;          // session maximum
		uint64_t e2eProbes;         // number of completed probes
	};
	Stats getStats();

	// Called by the mirror WS thread when a TA frame with visual changes
	// arrives. Completes any pending E2E probe. Zero-cost if no probe is
	// pending (single atomic load).
	void onVisualChange();
}
