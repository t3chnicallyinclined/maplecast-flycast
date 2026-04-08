/*
	MapleCast Control WebSocket — admin/maintenance command channel.

	A second WebSocket listener bound to LOCALHOST ONLY (127.0.0.1:7211)
	for in-process control commands. Separate from the TA mirror WS on
	port 7210 because:
	  1. Mirror WS is binary-first, high-volume; mixing JSON control
	     would risk corrupting the relay's parser.
	  2. Loopback-bound = zero external attack surface for control commands.
	  3. The /overlord admin panel proxies through the same-VPS relay,
	     which connects to control WS over loopback. No internet exposure.

	Commands are JSON text frames. Each command queues a Command into
	the render thread's command queue, which drains it via
	drainCommandQueue() once per frame at the top of PvrMessageQueue::render().

	This means: dc_savestate() / dc_loadstate() / dc_reset() always run
	on the render thread, never on the WS handler thread. The WS handler
	just enqueues + waits for the result via a stored connection_hdl.

	After any dc_loadstate(), the executor MUST trigger a fresh SYNC
	broadcast (maplecast_mirror::requestSyncBroadcast()) so the mirror's
	per-region shadow buffers realign with the loaded state. See
	docs/ARCHITECTURE.md "Eight bugs we already paid for" #8.

	Wire protocol: see docs/WORKSTREAM-OVERLORD.md "Appendix A — Control WS
	protocol reference".
*/
#pragma once

#include <cstdint>
#include <string>

namespace maplecast_control_ws
{

// Initialize the control WS server. Binds to 127.0.0.1:<port>.
// Returns true on success, false on bind/init failure.
// Default port is 7211 (matches MAPLECAST_CONTROL_PORT env var).
bool init(int port = 7211);

// Stop the control WS server, drop all connections, join the listener
// thread. Safe to call from any thread, idempotent.
void shutdown();

// True iff init() succeeded and shutdown() hasn't been called.
bool active();

// Drain queued commands from the render thread. Called once per frame
// from PvrMessageQueue::render() BEFORE serverPublish(). Each queued
// command is executed synchronously (dc_savestate / dc_loadstate /
// dc_reset etc.) and a reply is pushed back to the originating WS
// client via the stored connection_hdl. Non-blocking — if no commands
// are queued, returns immediately.
//
// MUST be called from the render thread. Calling from any other thread
// is a thread-safety violation (the queue is single-consumer).
void drainCommandQueue();

}
