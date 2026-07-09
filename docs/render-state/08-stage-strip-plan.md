# RENDER-STATE appendix 08 — Phase 3a stage-strip: scouted implementation plan

> Produced 2026-07-09 by the Phase-3a asset scout. Measured basis: −49.0% on real gameplay
> (docs/TA-WIRE-V2-PLAN.md §Phase 3, _bwlab/STAGE-SHARE-REPORT.md).

## Orienting fact
Two browser wires exist. The replica-live wire (7212) ALREADY renders the stage client-local
(zero geometry shipped). Phase 3a = port that capability onto the **7200 TA-mirror client**.

## Inventory (all exists)
1. **STG0B bake**: `atlas/stages/STG0B_ta.json` + `_tNN.png` (mirrored `web/test-atlas/stages/`),
   produced by `tools/bake_stage_from_ta.py` from `_stage_gt/engine_ta.bin` + `ram.bin`.
   Per-mesh JSON: real PCW/ISP/TSP/TCW + tris with BOTH screen verts and un-projected WORLD
   verts (`(M1·M2)^-1`, round-trip ≤3.6e-6 px) → client re-projects against a live camera.
   96 opaque groups, ~2457 verts, ~27 textures. Only ListType==0 baked.
2. **Browser adapter**: `web/webgpu/stage-client.mjs` `_buildFromTA()` emits EXACTLY the
   PVR2Renderer parsed shape — no renderer change needed. `replay.html:1302-1327`
   `renderStagePass()` = the OP-clear-then-composite pattern + re_kb/31 per-mesh gate
   (only the 3 world-authored deck meshes re-project; 69 local props stay baked-screen).
3. **Native path**: `core/network/gsta_stage.cpp` `gstaStageEmitTA()` — same JSON source,
   emits real TA parcels (forced ListType 0), camera from RAM. Proves the synthesis model.
4. **THE CRUX — camera NOT on the 7200 wire**: `M1@0x8C2D6B18` (constant) +
   `M2@0x8C2D6AD8` (per-frame view·proj, ALL zoom/pan) + `stage_id@0x8C289638` are main-RAM;
   serverPublish ships no main-RAM region. Phase 3a adds a per-frame **CAMM** message
   (M1+M2+stage_id ≈ 200 B) — the one new wire field. Four-parser rule applies.
5. **stage_id map**: only 0x11→STG0B confirmed (`tools/stage_id_map.json`); fallback id&0xFF.
   modelCounts fingerprints for all 17 stages exist; only STG0B has a bake.
6. **⚠ THE TRAP — the opaque list is NOT pure stage: the HUD rides it.** Life-bar fills
   (TCW band 0x80000), bar frames (0x9be00), portraits (0x9deXX), meter fills, name plates
   are ListType-0 polys. A wholesale opaque strip DELETES THE HUD on plain TA-mirror clients.
   **Mitigation: TCW allowlist** — strip only opaque polys whose TCW ∈ the true-stage set
   (buildable offline from STG0B_ta.json's textures[].addr vs the HUD bands;
   stage_share.py already enumerates op-list TCWs).

## Implementation plan
- **Server** (`maplecast_mirror.cpp`): `taStripStage(...)` beside `taCanonicalize` (same FSM
  walk), dropping allowlisted-TCW opaque polys — applied to the **ZCS2 inner only** (legacy
  untouched, per the shadow discipline), new header flag bit. + `CAMM` broadcast
  (read RAM addrs; relay forwards unknown magic verbatim — proven with ZCS2).
  Env `MAPLECAST_STAGESTRIP=measure|1`.
- **Browser** (`webgpu-test.html`): load STG0B bake once; per frame when strip flag set:
  stage OP pass via stage-client.mjs + PVR2Renderer (CAMM camera), then the stripped wire
  TA composites over (loadOp:'load' pattern from replay.html).
- **Gate change**: stripped ZCS2 inner ≠ legacy inner BY DESIGN — the byte-exact pair
  verifier no longer applies to stripped frames. Primary gate becomes the frozen/side-by-side
  PIXEL A/B (stage region + HUD present + bodies identical), plus the non-stage byte spans
  still matching.
- **Risks ranked**: (1) HUD-on-opaque partition correctness; (2) CAMM four-parser rule;
  (3) only STG0B baked (3b sweep: per-stage MAPLECAST_DUMP_RAM capture + bake + id map);
  (4) live camera-motion A/B never witnessed (get a moving-match capture as acceptance);
  (5) OP-clear z/order vs stripped wire expectations.

## Also queued (from live testing 2026-07-09)
- Frame drops during specials on the ZCS2 render path: main-thread fzstd+SoA decode
  (~1.5 ms/msg, worse on burst frames) + relay 16-slot backpressure → desyncs → legacy
  fallback hitch. Fix: move ZCS2 decode into the frame worker. (Panel has a desyncs counter.)
