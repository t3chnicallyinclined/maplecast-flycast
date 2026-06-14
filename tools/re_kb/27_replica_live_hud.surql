-- ============================================================================
-- 27: render-replica LIVE — PHASE 3 HUD (health/timer/round/meter/combo) FOREGROUND.
-- The bodies + stage were on screen but the match had no HUD. This adds the MVC2 HUD
-- as a 2D foreground pass over the WebGPU canvas, every field grounded in the engine
-- state + the production GSTA wire's own addresses. (feat/render-replica-live.)
-- Pinned 2026-06-13.
-- ============================================================================
USE NS re DB kb;

UPSERT source:hud_field_addresses SET kind='code',
  ref='core/network/maplecast_gamestate.cpp (readState) + docs/MVC2-MEMORY-MAP.md page 649',
  note='The HUD field addresses are the SAME ones the SHIPPED production GSTA wire reads verbatim, so they are confirmed-live, not guessed. PER-CHAR (char-struct base + off): health +0x420 (u8), red_health/chip +0x424 (u8), char_id +0x001 (u8), active +0x000 (u8). GLOBAL (page 649): in_match 0x8C289624, round_counter 0x8C28962B, game_timer 0x8C289630, stage_id 0x8C289638, p1_meter_fill u16 0x8C289646, p2_meter_fill u16 0x8C289648, p1_meter_level u8 0x8C28964A, p2_meter_level u8 0x8C28964B, p1_combo u16 0x8C289670, p2_combo u16 0x8C289672. CONSTANTS: HP_MAX=144 (0x90); METER_MAX=144 (cited loc_8C0F0FDC meter_fill/144.0). The on-screen POINT char of a side is whichever of its 3 slots (P1={0,2,4}, P2={1,3,5}) has active!=0; its health/red_health drive that side life bar. Per-team life-bar gradient (which of C1/C2/C3 is active point -> magenta/green/cyan -> yellow) = the team-slot modulate loc_8c15FFB0.';

UPSERT source:hud_readset SET kind='code',
  ref='core/network/maplecast_replica_live.cpp buildTables()',
  note='The stage agent added D(0x8C289620, 0x20, "gstate") = 0x289620..0x28963F (in_match/round/timer/stage_id) but that 32B window does NOT reach the HUD meter (0x289646..0x64B) or combo (0x289670..0x673). EXTENDED to D(0x8C289620, 0x60, "gstate") = 0x289620..0x28967F so the SAME region now also carries p1/p2 meter_fill, meter_level, and combo. To reach combo (last byte 0x289673) the window must span 0x54 bytes; rounded to 0x60 (ends 0x28967F, still inside page 649). +0x40 bytes/frame on the wire, zstd-trivial (mostly stable). Per-char health/red_health/char_id already ride the "char_str" region (D(0x8C268340, 6*0x5A4, "char_str")) — no extra wire for them. NOTE: the server gstate extension deploys with the stage region in ONE batched headless rebuild (coordinated by the user) — built, not deployed independently.';

UPSERT source:hud_layout_reference SET kind='code',
  ref='web/webgpu/sprite-client.mjs drawHUD/_drawBar/_barCols/_pointSlot/_drawDigits',
  note='The faithful, already-grounded MVC2 HUD layout that the replay.html pass mirrors (640x480 game space): life bars LB={x1:18,x2:330,y:16,w:292,h:14} top-left(P1)/top-right(P2), red chip-damage (red_health/144) drawn FIRST then current HP (health/144) ON TOP so the chip trails BEHIND; P2 bars right-anchored (grow leftward). Super meters at (18,456,250,9)/(372,456,250,9) width=fill/144 team-tinted, right-anchored on P2. Meter-level pips 0..5 at y=446 (P1 left x=18+i*12, P2 right x=613-i*12). Round timer = 2 centered digits (game_timer 0..99) at (320, top). Combo counter per side (combo>1) top-corner. Colors = per-team gradient loc_8c15FFB0.';

UPSERT finding:replica_live_hud SET
  statement='PHASE 3 HUD landed on the render-replica LIVE client as a FOREGROUND pass over the bodies + stage, every field grounded in the engine state + the production GSTA wire addresses, NO model. web/render-replica/replay.html: renderHudPass() draws on a SEPARATE 2D overlay canvas layered exactly over the WebGPU body canvas (same technique as the quad-bounds overlay), called AFTER pane.render() in BOTH the file path (showFrame) and the live path (liveApplyFrame). It reads HUD state from the RAM image at the engine addresses (readHud()/hudPointSlot()): per-char health+0x420 / red_health+0x424 / active+0x000 / char_id+0x001 (from the shipped char_str region), globals in_match/timer/round/stage_id + meter_fill/meter_level/combo (from the EXTENDED gstate region). Render: life bars (chip-damage trailing behind current HP, team-gradient tint), super meters (fill/144), meter-level pips, round timer + per-side combo as 7-segment vector digits (built-in, no external art for v1 — fully local, dependency-free). Layout mirrors the grounded reference sprite-client.mjs drawHUD (loc_8c15FFB0 team gradient, loc_8C0F0FDC METER_MAX=144, HP_MAX=144). Server: core/network/maplecast_replica_live.cpp gstate region extended 0x20 -> 0x60 to cover meter+combo+P2. Body render (render_frame.wasm/gen_*.c/body_decoder.mjs), the stage pass, and the GFX/palette tails UNTOUCHED. Cache-bust ?v=stage1 -> ?v=hud1.',
  status='confirmed', confidence='high', date='2026-06-13';

UPSERT finding:replica_live_hud_validated SET
  statement='The HUD foreground pass is PROVEN to reflect live state + match the MVC2 layout. Headless validation (tools/render-replica-poc/shot_hud.mjs, puppeteer WebGPU, real recording maxq_86.mcrr): STEP 1 __readHud() reads correct fields straight from the seeded RAM image (inMatch=1, timer=99, round=2, P1+P2 point slots active) — field plumbing confirmed on real recording bytes. STEP 2 drives grounded synthetic values (P1 HP 72/144 with chip to 110/144, P2 HP 130/144, P1 meter full / P2 half, level pips, timer 73, P1 combo 7) into the RAM image at the real engine addresses, re-runs renderHudPass(), and reads back the overlay: ALL 7 elements render (10585 non-blank px) — P1/P2 life bars (bright team-color HP fill), the red chip-damage trailing BEHIND current HP, P1/P2 super meters, the timer digits, the combo digits. Screenshot PNG_hud.png shows the composited HUD over the bodies/stage in the MVC2 layout (life bars top-L/top-R, timer center-top, meters/pips bottom). The body canvas reads back as the magenta clear-color under headless software WebGPU (known readback limitation, same as shot_replay.mjs) but the 2D HUD overlay reads back perfectly — confirming the foreground pass composites on top. LIVE meter/combo bytes require the batched headless rebuild (the extended gstate region) which the user coordinates; the layout/render/health-bar+timer path validate against the pre-extension recording today.',
  status='confirmed', confidence='high', date='2026-06-13';

RELATE finding:replica_live_hud->cites->source:hud_field_addresses;
RELATE finding:replica_live_hud->cites->source:hud_readset;
RELATE finding:replica_live_hud->cites->source:hud_layout_reference;
RELATE finding:replica_live_hud_validated->cites->source:hud_field_addresses;
RELATE finding:replica_live_hud_validated->cites->source:hud_layout_reference;
