# RENDER-STATE appendix 09 — CHARACTER FLIP on the ZCS2 client: scouted plan

> Produced 2026-07-09 by the character-flip scout. Template: the shipped Phase-3a stage strip
> (appendix 08). Thesis CONFIRMED: this is assembly of proven pieces, not invention.

## Corrections to prior framing (measured)
- In-match, characters ride **TR-SPRITE (paraType 5, list lt==2)** — 82.3 quads/frame, 5.5% of
  the TA + 21.3% of changed bytes. PT is ~empty in-match.
- **TRAP (same as stage/HUD-opaque): the HUD character-NAME letters are ALSO TR-sprite para5**
  (pcw a2000009, FONT.BIN TCWs). Strip must be TCW-ALLOWLISTED (char/effect decoded-GFX VRAM
  set) or HUD names vanish.
- **Do NOT strip TR polys (paraType 4)** — those are the 3D-machine parcels (sparks/flashes)
  that render_frame does NOT reproduce (they ride the PL3D injection path).

## The pieces (all exist)
1. **render_frame.wasm in the browser** (web/render-replica/render_frame.{mjs,wasm};
   glue tools/render-replica-poc/wasm_entry_frame.c): export `render_frame_ta(ram16, out, cap)`
   emits ALL bodies + cat1-4 satellites/effects as paraType-5 sprite TA with REAL resident TCWs,
   byte-exact. ta-parser.mjs parses it directly (sprite param :142-156, sprite vert :159-171).
   Driving template: replay.html ensureWasm :988 + transpiledTA :989-1019.
2. **State feed** = replica-live wire (7212): prefix = ZCST MCRR (16MB RAM + 8MB VRAM + tables),
   per-frame FRMx = ~58KB raw dyn regions (zstd-trivial steady-state) + GFX/palette/BTCW/PL3D
   tails. Client template: replay.html parsePrefix :548 / seedFrom :2041 / liveApplyFrame :2254.
   **NOT zero server work on prod:** needs MAPLECAST_REPLICA_LIVE=1 in the unit env (present)
   AND a new nginx `location /replica-live` proxy to 127.0.0.1:7212 (ABSENT from prod nginx —
   DEPLOYMENT.md:343 block has no such location).
3. **Textures: NO body_decoder needed on this client.** ZCS2 keeps D.vram live via dirty pages;
   render_frame's real TCWs resolve through the existing TextureManager (the CHARQ path proved
   this pattern, webgpu-test.html:716-719). body_decoder is only for ROM-less pure-state clients.

## Blockers ranked
1. **Two-socket frame pairing** (make-or-break): state (7212 FRMx vframe) vs ZCS2 (frame counter)
   arrive with independent jitter — need a small match buffer keyed on frame counters + policy on
   mismatch. (The stage avoided this by riding the camera INSIDE ZCS2 — consider the same:
   a compact state tail inside ZCS2 instead of a second socket, ~58KB raw/frame is too big raw
   but the DYNAMIC subset render_frame actually reads may compress fine in the shared stream —
   evaluate before building the two-socket pairing.)
2. HUD-name TCW allowlist correctness (offline build + gate).
3. Browser seed download (multi-MB prefix, connect stall) — only if two-socket route chosen.
4. Satellite GFX residency edge (re_kb/29) — low risk on the mirror wire (DMA force-dirty).
5. Palette skew between state pose and ZCS2 palette page (one-frame flash class).
6. Byte-pair verifier doesn't apply to stripped frames (same as stage) — pixel A/B is the gate.

## Implementation sketch
- Server: `taStripCharSprites` beside taStripStage (same FSM walk): drop TR-list para5 quads with
  TCW ∈ char/effect allowlist; ZCS2-inner only; header flag bit4; env MAPLECAST_CHARSTRIP=measure|1.
- Client: state feed (per blocker-1 decision) → 16MB ram image → render_frame_ta per frame →
  `window._bodyMerge(g, bodyTA)` modeled on _stageMerge (parse, append to g.translucent with
  vert shift) AFTER _stageMerge so bodies composite over stage, under HUD.
- Gate: frozen + side-by-side pixel A/B; acceptance on a MOVING match capture.
- Projected wire: stage-strip + char-strip ≈ the own-HUD endgame minus HUD — **~0.3-0.5 Mbps**.

## Shipped meanwhile (this session, after appendix 08)
- Strip gate HYSTERESIS (in_match byte flaps during round transitions — was THE green flicker;
  engage @60 stable frames, release @180; 2ea6e5df8).
- Camera rides INSIDE the ZCS2 frame (flags bit3, 132B; separate CAMM removed — it raced the
  worker decode; 23c2d2d1e).
- fillBGP skipped when local stage merged (BGP z-fought re-projected far geometry; 540dac213).
- ZCS2 = default wire: page defaults ON, relay `{"type":"subscribe","mode":"zcs2"}` sheds legacy
  deltas per-client (SYNC passes; auto-restore on desync; b67794fd1). Worker decode + keyframe-
  aligned epochs (78b2f4c6b).

## ModNao research addendum (2026-07-09)
- **Already mined**: tools/rip_stage.py IS a ModNao port (github.com/rob2d/modnao, cited in its
  header) — NLOBJPUT decoder, strip winding, twiddle, color conv all extracted long ago.
- **Cannot complete the disc-only 3b bake**: the blockers aren't in the disc data — per-prop
  world transforms are RUNTIME matrix-stack state (bank12 loc_8c122fd0) and real PVR words come
  only from live TA. ModNao renders with meshBasicMaterial (no PVR synthesis, no camera model).
  3b stays: one MAPLECAST_DUMP_RAM capture per stage + bake_stage_from_ta.py (scriptable sweep).
- **Residual value to lift (format facts only)**: VQ + LZSS texture branches
  (loadTextureFileWorker.ts), wrapping-flag table (getTextureWrappingFlags.ts → TSP clamp/flip),
  mesh textureControlValue + specular fields, and mvc2StageAttribMappings.ts = the full
  17-stage name map for tools/stage_id_map.json (only 0x11→STG0B confirmed today).
- **⚠ LICENSE**: ModNao relicensed 2026-05-25 to a custom non-commercial license with a
  no-AI-analysis clause. Existing port predates + attributes; ANY further borrowing = format
  facts (offsets/bit meanings), never verbatim code; MapleCast stays non-commercial or gets
  written permission.
