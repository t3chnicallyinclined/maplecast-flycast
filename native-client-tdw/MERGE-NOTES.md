# native-client-tdw → native-client merge notes (2026-07-15)

This folder is a CLONE of native-client/ taken 2026-07-14 (~2:07 PM state) that
became the test vehicle for the TDW one-protocol wire (docs/TA-DICT-WIRE-PLAN.md).
Everything below is user-verified live against the local TDW server
(`_run_srv_tadict.bat` — MAPLECAST_TDW_ONLY decommission mode). Server side is
committed on feat/predictive-netcode (16c5e3445, e0292946d, 9c44938fc, c53ca006c).

## New modules
- `src/tdw.rs` — TDW1/TDWS decode: streaming zstd (zstd-safe DStream, ZCS2-style
  seq/epoch/desync discipline), content dictionary, TA reassembly, in-band
  camera (envelope bit3: stage_id+M2+M1), PVR reg snapshot (bit5, 64B), page
  section (bit4, legacy layout verbatim), self-locating E2EP tail.
- `src/stage.rs` — local stage: loads `.mcstg` (tools/stage_bake_to_native.py),
  re-projects world-authored meshes per frame via col-major M1·M2 rows{0,1,3}
  (bake_stage_from_ta.py proven math) with FULL GUARD-VOLUME clipping
  (Sutherland-Hodgman pre-divide: w>=1e-4 AND |X|,|Y|<=4096·w — near-clip alone
  produced |x|-in-millions verts the ±100k sanity then ate = the air-HVB
  missing-floor bug). Static (non-world-authored) props draw baked screen pos.
  Emits engine-word PolyParams; textures resolve via the normal TCW path.
- `src/hud.rs` — byte-matched synthetic HUD (HUDQ-oracle constants, re_kb/58/59).
  DORMANT: MC_HUD=synth only — keep-rule v4 ships the REAL HUD on the wire
  (measured 2.7% churn). Retain for a future ultra-thin spectator tier.
- `src/settings.rs` — F2 display panel (resolution presets/fullscreen/vsync/
  jitter/render toggles) → request atomics; main loop applies + persists
  "w h fs vsync" to maplecast-display.txt next to the exe.

## Modified
- `src/net.rs` — routes TDW1/TDWS; players mode (`MC_TDW=players`) applies TDW1
  directly (pages→apply_page_section, TA→fd.tdw_ta, camera/pvr→fd, E2EP→
  debug.e2e_echo, wire_frame tick) and **skips ZCS2 entirely (no zstd decode)**;
  gate/render dev modes unchanged. Per-leg bandwidth breakdown into debug.
- `src/frame.rs` — `apply_page_section()` factored out (returns count);
  `tdw_ta`/`tdw_cam`/`gsta` fields; `mark_tdw_frame()`; `replace_ta()`.
- `src/main.rs` — mod decls; STAGE_BAKE OnceLock (MC_STAGE env); render path:
  parse `fd.tdw_ta` when present, stage.append + hud gate; DebugWin::render
  generalized to a ui-closure (third window reuses it); F2 handling + settings
  window arm + display-request apply block in AboutToWait; window defaults
  640x480 PHYSICAL, remembers last size (MC_WINDOW env > persisted > default).
- `src/debug.rs` — per-leg Mbps rows, TDW panel (mode/synced/dict/eq/ne/pairing/
  pages/stage strips), settings atomics.
- `src/input.rs` — **directions fix your tree wants too**: left stick +
  Axis::DPadX/Y hat mapping (was Button::DPad* only — DirectInput pads never
  fired it); pad-name logging.

## Env / launch
`_run_native_tdw.bat`: MAPLECAST_WS=ws://127.0.0.1:7200, MAPLECAST_REPLICA →
localhost (avoid prod), MAPLECAST_INPUT_HOST=127.0.0.1 (input.rs DEFAULTS TO
nobd.net — always override for local), MC_TDW=players, MC_STAGE=<mcstg path>.

## Verified numbers (local rig)
press→present 16-20 ms (E2E probe, at the game's 1-frame floor); TDW wire
0.02-0.36 Mbps steady / ~1.2 super peaks (+pages after fold ~0.7 warmup);
byte-equality ~95k frames 0 mismatches (gate mode vs legacy chain);
render ~324 fps; wire 57.9-61.7 fps; 0 drops.

## Known opens (do not lose)
- D1: ground blinks during Sentinel drones super (F1 stage-strips row
  discriminates projection vs overlay-cover).
- D2: stray white "snowball" sprites near drones/effects (suspect stale
  double-buffer parity tiles or untex-TR overlay blend).
- Game wedge: pause→character-change→versus-loading hang (server-side, static
  TA — likely autoload-savestate/GD-ROM state, PRE-EXISTING, not wire).
- Reserve-row HUD tint selector (hud.rs, re_kb/58 open) — only matters for the
  synth tier.

## 2026-07-15 — input-follows-server hardening (merge these too)
- `input.rs`: retarget no longer latches the server index on a FAILED
  `sock.connect` (DNS blip / empty host) — it retries every tick; on success
  it clears `send_times`/RTT EMA (stale samples belonged to the old server)
  and forces an immediate state packet. Loud startup warn when
  `MAPLECAST_INPUT_HOST` pins input (cmd sessions keep env vars from OLD bats
  that set it — the prime suspect for "wire switched, input didn't").
- `debug.rs`: new `input_pinned: AtomicBool`.
- `settings.rs`: Servers panel always shows `input -> host:7100`; orange
  "⚠ input PINNED" when the env pin is active.
- Context: user hit "swapped servers but input stayed" live on 2026-07-15.
  See docs/STATE-HANDOFF-PLAN.md for the server-transfer feature this feeds.

## 2026-07-15 — live state transfer (server hand-off) client flow
- `debug.rs`: `transfer_server: AtomicU64` + `migrate_status: Mutex<String>`.
- `settings.rs`: per-row "transfer ▶" button (hands the LIVE GAME to that
  node vs "connect" which just views its own game) + status line.
- `net.rs`: Message::Text handling (was silently dropped) — parses
  "migrated"/"migrate_failed" replies; transfer poll sends
  {"type":"migrate","dest":ws_dest,"key":MC_FLEET_KEY}. `ws_dest()` derives
  host[:port] from the ws:// URL (the dest the SOURCE SERVER dials — NOT the
  input host; multi-instance boxes differ).
- `input.rs`: directory input hosts may carry "host:port" for non-7100
  instances (multi-instance boxes).
- Server side is commit bc511da98; protocol in docs/STATE-HANDOFF-PLAN.md.

## 2026-07-15 late — one-button connect + tdw subscription (merge too)
- `settings.rs`: connect/transfer merged into ONE "connect ▶" per row —
  connecting CARRIES the game (transfer_server), falls back to a fresh
  connect on migrate_failed or dead wire (user decision: "you would only do
  both").
- `net.rs`: sends {"type":"subscribe","mode":"tdw"} after request_sync in
  players mode — the server sheds legacy legs per-connection (commit
  e3cd0a11c) instead of the client paying multi-Mbps to drop them;
  `pending_transfer` tracks the target for the fallback switch; dead-wire
  fallback in the reconnect loop.
- Bat gotcha: env values containing `|` need `set "VAR=a|b"` quoting.

## 2026-07-16 — remote-first auto-connect + local-play mode
- `main.rs` probe thread: auto-closest EXCLUDES loopback entries and
  force-switches OFF a local default at startup ("closest" = the fleet;
  local play is explicit). Probe + input handle host:port entries.
- `main.rs`: local-play manager thread — panel-chosen ROM, spawns/stops
  build-headless-win\flycast.exe (gold TDW_ONLY env) as a child, logs to
  _srv_localplay.log, auto-reaps, switches to entry 0 on start.
  MC_LOCAL_SERVER_EXE overrides the exe path.
- `debug.rs`: local_rom / local_play_cmd / local_srv_on.
- `settings.rs`: "Local play" section (ROM text field + start/stop).

## 2026-07-16 late — tabbed side panel
- ONE resizable panel window (debug/servers/display tabs) replaces the two
  fixed windows; docked to the game window's right edge and follows it
  (Moved/Resized -> dock_panel). F1 = Debug tab, F2 = Servers tab; every tab
  body is in a ScrollArea so text is always reachable.
- settings.rs rewritten: panel() + servers_body/display_body; debug.rs ui()
  -> body(ui,d); DebugState.active_tab; settings_open now unused.

## 2026-07-16 — auto-closest self-correction
- One-shot pick latched whatever answered during a fleet-restart window
  (user got dfw @43ms while main @4.8ms was mid-restart). Now re-evaluates
  every sweep for the first 15 sweeps (~30s), switches only when >5ms better
  or active down/local, and stands down permanently once the user picks a
  server manually (debug.manual_server, set by connect ▶).

## 2026-07-16 — startup server picker (auto-connect retired to MC_AUTO=1)
- Startup = the panel on the Servers tab as a PICKER: live pings + signal
  bars (rtt_bars: ▂▄▆█ tiers 15/35/70ms), "Where do you want to play?"
  heading, net thread WAITS for the user's pick (pre-connect transfer click
  converts to a plain switch). MC_AUTO=1 restores probe placement (now with
  two-sweep confirmation + 10ms hysteresis + one-move budget + dead-active
  failover after the flapping incident). Local play sets manual_server.
- active_tab starts at 1 (servers); loop-top pending-switch consumption in
  net.rs handles pre-connection placement.

## 2026-07-17 — Track A latency (client display path) — see docs/OPTIMIZATION-ROADMAP.md
- main.rs: desired_maximum_frame_latency 2 -> 1 (A2, one fewer buffered present;
  safe with the continuous-redraw loop).
- main.rs: MC_FULLSCREEN=1 forces borderless-fullscreen at launch (A1) so the
  surface reaches DXGI independent-flip and bypasses the DWM compositor
  (~12-16ms, the single biggest removable button-to-photon chunk). The F2
  display toggle does the same live. VALIDATE WITH PresentMon
  (HardwareIndependentFlip vs Composed) — the E2EP probe is blind below present().
- NEXT (A3/A4): event-driven render wake + DXGI waitable swapchain; shrink the
  FrameDecoder lock hold across parse+upload (main.rs:192-234).
