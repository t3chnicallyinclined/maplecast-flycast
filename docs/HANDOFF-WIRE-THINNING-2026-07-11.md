# Wire-Thinning Campaign — Handoff (2026-07-11)

North star: **thinnest possible in-match wire** — ship everything static at character
select, stream only per-frame dynamic state. See memory `project_charselect_precache_thesis`.
Render path = **render_frame** (transpiled SH4), NOT the sprite machine (re_kb/74 verdict, re-verified 2026-07-11: sprite machine renders worse; its wire edge is gone).

## Landed (live on prod, default ON)
- **STM2 size-tolerant delta** — the folded /replica-live body state was keyframing EVERY
  frame (variable payload size defeated `_smPrev.size()!=sn`) → flat ~360KB/frame. Fixed:
  delta the common prefix + append grown tail; never keyframe on a size change. Client
  preserves BODY.stm across resizes.
- **STM2 KEY-defer** — the periodic full reseed landing during a super couldn't dedup the
  novel super assets → 27x zstd wire spike (2KB→59KB). Fixed: defer the KEY when the last
  delta is large (churn); reseed on the next calm frame. Killed the 59KB periodic spike.
- **WIREMON diagnostics** (`MAPLECAST_WIREMON=1`, default OFF; left ON during this campaign):
  per-frame `[WIREMON]` (zcs2 compressed / inner / ratio / taDelta / pages/etc / stm2[KEY|DELTA])
  + `[WIREMON-BLK]` VRAM block histogram. Client mirror: `?wirediag=2` (frame-decoder.mjs).
- **MAPLECAST_NO_SCENE_SYNC=1** — the request-driven full-VRAM SYNC broadcast is gated OFF
  (it was never the super spike; measured). Render survives without it (all-local decode).

## Built but REVERTED (code committed, prod flag OFF)
- **Clean strip** (commit 492c23219): decoupled `MAPLECAST_CHARSTRIP_PAGES` from the TA
  char-strip so it drops the {82,83,88,89} VRAM pages (84% of page bytes) WITHOUT the 5x
  geometry-delta inflation the char-strip caused (removing quads shifts every TA byte).
  Client `?vramoverlay=1` overlays the already-decoded BTEX.lvram tiles into D.vram so the
  kept wire quads texture locally. MEASURED CLEAN: {82,83,88,89} gone, taDelta flat ~3KB,
  wire 0.66 Mbps, bodies correct. REVERTED because: (a) only 9% of the SUPER decompressed
  volume, (b) glitches supers (overlay misses some quads / invalidateAll flicker), (c) with
  CHARSTRIP_PAGES on for all but only ?vramoverlay clients overlaying, the DEFAULT link garbles.
  Keep the code for a future clean re-enable once the overlay covers all quads.

## THE super-spike root cause (MEASURED, definitive)
A triple super's ~16MB is the DECOMPRESSED volume over ~120 frames (~130KB/frame). Decomposed:
- **stm2 (render-STATE) = 84%** (~70KB/frame) ← the spike
- pages/etc = 9% (kept {84,8a} banks)
- taDelta = 5%

The 70KB/frame stm2 is the **effect render-state**: efxtmpl scale arenas (0x8C565000, 7×0x3000)
+ rectab effect-record table (0x10000). FIXED-address regions → the byte-delta is already only
genuinely-changed bytes (no over-shipping). They are FILLED by the game sim (super effect spawn)
and READ by render_frame's scale-walker (replica_live.cpp:490). The client runs render opcodes,
NOT the sim → CANNOT regenerate them → they MUST ship. **This is a genuine render_frame floor for
supers**, and exactly the fidelity-vs-wire tradeoff the sprite machine sidesteps (draws pre-baked
super sprites, no render-state, but renders supers worse). Steady (non-super) wire = 0.6 Mbps.

## Open options for the super floor (user to steer)
1. Accept it — supers are brief (~1-2s) and render correctly; steady wire is excellent.
2. HYBRID — pre-baked composited super EFFECTS (sprite-machine style) spliced into render_frame,
   so the efxtmpl/rectab state never ships. Kills the spike; effects "good enough" not byte-exact.
3. The 3D-effects machine (re_kb/63+64) is the other big arc — impact sparks/cast flashes render
   NOTHING today in either path; drawer-independent.

## NEXT (this session): background/stage from local cache + camera streaming
Render the stage fully from the local baked STGxx cache, streaming only camera (M2/M1 matrices +
stage_id). Infra exists: STAGESTRIP=1 strips stage geometry + ships local (_stageMerge); camera
rides the ZCS2 header (flags bit3, replica: sid @0x8C289638 + 16 matrices @0x8C2D6AD8/0x8C2D6B18).
OPEN (expert-flagged): stage TEXTURES still ride wire VRAM; the merged stage polys carry a
surrogate tcw + no texObj and STAGE.attachDevice/_uploadTextures is never called in webgpu-test.html
— verify the stage is actually rendering local vs wire. Goal: stage textures local too → strip
stage VRAM. See memory `project_stage_decoder`.
