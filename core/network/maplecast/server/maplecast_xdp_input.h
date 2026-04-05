/*
	MapleCast AF_XDP Input — zero-copy gamepad packet receiver.

	NIC DMAs UDP gamepad packets directly into userspace ring buffer.
	Background thread polls the ring buffer and atomic-stores to kcode[]/lt[]/rt[].
	CMD9 response reads atomics — zero syscalls, zero jitter.

	Like NOBD's ISR reading pre-computed cmd9ReadyW3, but on a PC:
	  NOBD: GPIO → lookup table → cmd9ReadyW3 (1-2µs)
	  This: NIC DMA → ring buffer → kcode[] atomic (0.1-0.5µs)
*/
#pragma once

namespace maplecast_xdp_input
{
	// Initialize AF_XDP socket on interface, filtering UDP port
	// Returns true if XDP is active (native or SKB fallback)
	bool init(const char* ifname, int udp_port);

	// Shutdown XDP socket and detach BPF program
	void shutdown();

	// Is XDP input active?
	bool active();
}
