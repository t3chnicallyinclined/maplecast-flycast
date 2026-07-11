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

#include <cstdint>   // uint8_t — currentStatePayload()
#include <cstddef>   // size_t

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

// STATE-MERGE (fold /replica-live into the main ZCS2/ZCST wire — one stream, no second socket).
// Exposes THIS frame's just-built body-state payload (the captureFrame FRMx record, assembled
// coherently at the char pass) so serverPublish can append it as a self-describing "STAT" section
// to its frame buffer. Because the server compresses the SAME buffer into BOTH the legacy ZCST and
// the ZCS2 streaming wire, the body then rides whichever wire the client renders — same packet,
// same vframe (no two-socket pairing drift). The payload IS a complete FRMx record (its own header
// carries the vframe), so the browser's existing _bodyApplyFrame parses it unchanged. Valid on the
// render thread AFTER captureFrame has run this frame; returns false when disabled / no frame built.
// The returned pointer is stable for the duration of the current serverPublish (same render thread,
// double-buffer only swaps on the next frame). Copy it out promptly.
bool currentStatePayload(const uint8_t*& payload, size_t& len);

// CHARACTER-PASS table snapshot (re_kb/50 idxtab effect-range fix). Called from
// mc_oracle_charPassCapture() ONLY on the CHARACTER pass (isCharacterPass), where the
// idxtab/rectab effect entries are LIVE (written by the char-pass submit) before the
// HUD pass reverts them. Read-only side-snapshot of the 2 table regions into side
// buffers; captureFrame ships these instead of the live (HUD-pass-stale) RAM. Free
// when disabled / no client. SCOPED — not the onRenderFrame pass-gate.
void snapshotCharPassTables();

// TILEDESC CHARACTER-PASS SNAPSHOT (2026-07-02). The per-frame tile-descriptor table
// 0x8C1F9F9C is reset+rebuilt from index 0 at the START of each render pass, so it must be
// captured at the CHARACTER pass STARTRENDER (before the HUD pass rebuilds it) — NOT at
// serverPublish (where snapshotCharPassTables fires). Called from mc_oracle_charPassCapture
// gated on the realBody char-pass discriminator. captureFrame ships this tiledesc snapshot.
void snapshotCharPassTiledesc();

// WALK-INSTANT TILEDESC SNAPSHOT (2026-07-02, finding:tiledesc_walk_instant_snapshot). Snapshots
// the tiledesc @0x8C1F9F9C at the BODY-WALKER ENTRY (loc_8c0344d4, PC 0x8C0344D4), where its
// per-record byte1 is the AUTHORITATIVE pre-consumption count (== +0xDC budget; TDTILE ground
// truth). At STARTRENDER the byte1 is already inflated (re-seed). Called from the oracle hook on
// the FIRST char-body walk of each video frame. When active (default, MAPLECAST_TILEDESC_WALKSNAP),
// it OWNS the tiledesc snapshot and snapshotCharPassTiledesc becomes a no-op (it fires later/stale).
void snapshotWalkInstantTiledesc();

// FIRST-BODY idxtab WINDOW char-pass snapshot (2026-07-03, finding:hud_clobbers_first_body_idxtab).
// The first char body (+0xDC=0) resolves idxtab[arena_base+0..+ntiles0); the HUD pass's own first
// object reuses the SAME low arena indices (0x8C1F9D98 reset per pass) and clobbers that window by
// the time captureFrame ships the HUD-pass idxtab -> Storm scrambled, Cable (+0xDC=25) fine. This
// snapshots ONLY body0's idxtab window at the CHARACTER-pass STARTRENDER (body0's idxtab written,
// HUD not yet run) for captureFrame to overlay. rectab stays STARTRENDER. Gated
// MAPLECAST_IDXTAB_CHARSNAP (default OFF, A/B). Called from mc_oracle_charPassCapture.
void snapshotCharPassIdxtabBody0Window();

// CHAR-PASS GFX2 SNAPSHOT (Storm under-tile fix, re_kb/60 finding:storm_shipped_descriptor_tear).
// The engine SELF-MODIFIES the GFX2 cell-record dispatch head GFX2[(sid&0x7FFF)*4] in place per
// animation sub-frame; that mutation is live only during the CHARACTER pass and is reverted before
// captureFrame's live GFX2 read at serverPublish, so the wire froze GFX2 and the client walker
// (render_frame rebuild_tile_grid) computed a stale tile count (Storm ~24-38 vs engine ~49-53).
// This snapshots each active body's GFX2 region at the CHARACTER-pass STARTRENDER (like the tiledesc
// snapshot); collectFreshGfx/captureFrame source GFX2 from it so the LIVE head + referenced records
// ship fresh every frame the head changes. Default ON; MAPLECAST_GFX2_CHARSNAP=0 A/B-disables.
// Called from mc_oracle_charPassCapture on the realBody-gated char pass.
void snapshotCharPassGfx2();

// SLOT-TABLE + OBJPOOL CHAR-PASS SNAPSHOT (finding:replica_live_slot_objpool_snapshot_coherency).
// Snapshots slot_cnt(0x8C2895E0)/slot_ptr(0x8C287DE0)/objpool(0x8C26AA54) at the render-walk instant
// (loc_8c0308c2) so the shipped display list and the objpool node+0x12C visibility bit are coherent
// with the engine's char-pass render-walk — a removed satellite's stale +0x12C cannot leak past a
// non-coherent count (phantom-cape span-balloon fix). captureFrame ships these instead of live RAM.
// Default ON (MAPLECAST_SLOT_CHARSNAP=0 A/B-disables); must stay coherent with the tiledesc snapshot.
// Called from mc_oracle_charPassCapture on the realBody-gated char pass.
void snapshotCharPassSlots();

// Clean shutdown (stop the WS thread). Called from emulator term.
void shutdown();

// True when the gate is armed (the env var was set at init). Diagnostic only.
bool enabled();

// True when armed AND at least one loopback WS client is connected. The caller uses
// this to skip the per-pass character-pass discrimination (a ta_parse) entirely when
// no client is listening — preserving the zero-overhead-when-idle hot-path contract.
bool hasClients();

}
