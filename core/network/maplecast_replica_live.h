/*
	MapleCast Render-Replica LIVE feed (Phase 4c) — see maplecast_replica_live.cpp.

	A GATED, determinism-safe, READ-ONLY WebSocket that streams the render
	read-set (the MCRR format of docs/RENDER-REPLICA-RECORDING-FORMAT.md) off the
	LIVE prod headless so a browser can run the transpiled render_frame() against
	the moving game — the SH4 stays authoritative; the stream never writes guest
	state. This is the live-wire sibling of the file-based MAPLECAST_REPLICA_RECORD
	hook (which the experiment branch added on the SAME hook point).

	Gating + safety contract (load-bearing — this runs on the live service):
	  * env MAPLECAST_REPLICA_LIVE=1  ⇒  a loopback WS server starts on port 7212
	    (override MAPLECAST_REPLICA_LIVE_PORT). UNSET ⇒ no thread, no capture, the
	    onRenderFrame() call returns on its first instruction ⇒ byte-identical to
	    today's prod binary (zero overhead, zero behavioral change).
	  * The per-frame capture is READ-ONLY: it memcpy's OUT of guest RAM/VRAM/PVR
	    via addrspace reads + the resident arrays — never writes guest memory, never
	    raises an exception. Exactly like the always-on Oracle hook it piggybacks on.
	  * The capture runs on the SH4/render thread but does ONLY the ~58 KB dynamic
	    memcpy + a buffer hand-off; the zstd compress + socket write happen on the
	    WS thread. The 16.67 ms frame budget is never blocked on I/O.

	Hook point: maplecast_oracle_hook.cpp mc_oracle_charPassCapture() (the
	STARTRENDER pre-QueueRender CHARACTER pass, the Phase 4a recording point). We
	do NOT add a new SH4-side hook.

	Protocol (binary frames; the client reuses its MCRR parser):
	  * On CONNECT → ONE "STATIC PREFIX" message = the MCRR header (32B) + STATIC
	    region table + DYNAMIC region table + STATIC payload (VRAM 8MB + PVR 32KB +
	    16MB area-3 RAM backdrop), zstd-compressed inside a ZCST envelope.
	  * Per RENDERED in-match frame, if a client is connected → ONE "FRAME RECORD"
	    message = "FRMx"(u32 LE) + vframe(u32 LE) + taSize(u32 LE = 0) + the dynamic
	    regions' raw bytes in region-table order. (MVP: raw, no dirty-diff.)
*/
#pragma once

namespace maplecast_replica_live
{

// One-time init from env MAPLECAST_REPLICA_LIVE. No-op (and the module stays
// completely inert) when the env var is unset. Called once from emulator init,
// next to the other maplecast WS servers. Safe to call repeatedly.
void init();

// Render-thread per-frame entry, called from mc_oracle_charPassCapture(). When
// the module is disabled OR no client is connected it returns immediately
// (membership is a single relaxed atomic load — the OFF/idle path is free).
// `ctxv` is the just-completed frame's TA_context* (forward-declared void* to
// avoid pulling ta_ctx.h into the oracle-hook include set); currently unused for
// the dynamic payload (taSize=0) but kept for symmetry with the file recorder
// and a future TA-ground-truth extension.
void onRenderFrame(void* ctxv);

// Clean shutdown (stop the WS thread). Called from emulator term.
void shutdown();

// True when the gate is armed (the env var was set at init). Diagnostic only.
bool enabled();

// True when armed AND at least one loopback WS client is connected. The caller uses
// this to skip the per-pass character-pass discrimination (a ta_parse) entirely when
// no client is listening — preserving the zero-overhead-when-idle hot-path contract.
bool hasClients();

}
