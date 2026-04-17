/*
	MapleCast Input Sink — sends local SDL gamepad events to the server.

	Registers a ButtonListener on the SDL gamepad device. When a button
	is pressed or released, fires a UDP packet to the server's input
	port (7100) immediately, inline on the SDL event thread. No polling,
	no kcode[] involvement, no feedback loops.

	Used by the native mirror client (MAPLECAST_MIRROR_CLIENT) to let
	players control the game from a native flycast that only renders
	the TA stream — no local SH4.

	── Phase 2 (2026-04-14) competitive features ─────────────────────
	• Wire format: 11 bytes [P][C][slot][seq:u32_LE][LT][RT][btn_hi][btn_lo].
	  Server detects 7 vs 11 byte packets — backward compatible.
	• Input redundancy: every packet sent twice. T+0 immediately, then
	  T+1ms (different network jitter window). Server dedups by seq.
	  Bandwidth: 11 × 12000 × 2 ≈ 264 KB/s (negligible).
	• Hot-standby socket to a backup server. setBackupServer() pre-warms
	  a second UDP socket; failover when primary stalls.
	• SCHED_FIFO on the trigger poll thread (graceful degrade if
	  CAP_SYS_NICE missing).
*/
#pragma once
#include <cstdint>

namespace maplecast_input_sink
{
	// Initialize against a primary server.
	bool init(const char* host, int slot = 0);

	// Optional: provide a backup server to keep a hot-standby UDP socket
	// open. If primary fails (no probe-ACK for failoverWindowMs), we
	// silently swap. Pass empty/null host to disable standby.
	void setBackupServer(const char* host);

	void shutdown();
	bool active();

	// Direct update from evdev bypass — writes button/trigger state and
	// immediately sends a UDP packet. Called from the evdev thread, not SDL.
	void directUpdate(uint16_t buttons, uint8_t lt, uint8_t rt);

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
		// Phase 2: redundancy + failover
		uint64_t redundantSends;    // how many "T+1ms" copies we sent
		uint64_t failovers;         // how many times we swapped to standby
		bool     onBackupServer;    // true if currently sending to the standby
		bool     hasBackup;         // true if a backup is configured
	};
	Stats getStats();

	// Called by the mirror WS thread when a TA frame with visual changes
	// arrives. Completes any pending E2E probe. Zero-cost if no probe is
	// pending (single atomic load).
	void onVisualChange();
}
