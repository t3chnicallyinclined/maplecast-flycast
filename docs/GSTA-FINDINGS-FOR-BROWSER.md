# GSTA native-client findings → WebGPU / sprite-client (browser) port notes

Running notes: measured render findings from the native GSTA client work (`build/flycast.exe`
GSTA mode, render_frame → flycast's own renderer) that apply to the **browser** render-replica
(`web/render-replica/replay.html` + `web/webgpu/*`). The browser is where the original garble
(HUD, bodies, bars) lives. Every item below is CONFIRMED-BY-MEASUREMENT against real flycast
(A/B TA diff / engine VRAM+palette diff / ASMTRACE) unless tagged.

Keep appending as the stage/z-order work lands.

---

## ★ THE BIG LEVER — switch the browser body drawer to render_frame
The browser defaults to the **EMITTER** (`window.__bodyMode='emitter'`, `sprite-client.mjs
buildEmitterDrawList`) — unvalidated flip/decode/roster, the source of the body/HUD garble.
The GSTA work has now **hardened `render_frame` + `body_decoder.mjs` into a pixel-perfect path**
(verified through flycast's own `twop`). The browser already has this path wired:
`replay.html?bodymode=render_frame`. **Switching the default to render_frame inherits ALL the
fixes below for free** — this is the single biggest browser win. (Wire-cost: render_frame needs
the rectab/idxtab dynamic regions, which the replica-live wire already ships.)

---

## Shared-code fixes (ALREADY in the browser's render_frame path)
These landed in files the browser render_frame path uses (`web/render-replica/body_decoder.mjs`,
the transpile in `tools/render-replica-poc/gen_*.c`). render_frame-mode browser gets them now;
the EMITTER path does NOT (see next section).

- **Palette bank PRESERVE** (`gen_submit_params.c finalize_body`, commit `f2e81a82f`): do NOT
  override the resident rectab TCW PalSelect with the static formula `16*(char_pair+1)+8*player_side`.
  The engine allocates banks dynamically (`loc_8c124bd8`) and uses ODD siblings (17,25); the static
  formula only makes even banks → Cable-all-blue. Preserve the wire'd bank.
- **Carve multi-row pitch** (`body_decoder.mjs ensureBodyTextures`, commit `90533abc4`): per-tile
  pitch `m = W/cols = H/rows ∈ {8,16,32}`, NOT a hardcoded 32. Fixed-32 over-steps for m<32 parts
  → grey rows. (re_kb/42)
- **Carve non-square** (`body_decoder.mjs`, commit `1ee855655`): the native-chunk (twTile) carve is
  correct ONLY for SQUARE grids (`Tw==Th`). Non-square parts (e.g. 64×128 2×4) must fall back to the
  linear-slice carve. Gate the chunk path on `Tw==Th`. (re_kb/44, corrects re_kb/43)
- **texU mirror = `facing XOR 0x4000`** (`render_frame.c`), NOT `0x4000`-alone (measured 35% vs 1.46%
  U-mismatch) and NOT `!facing`. (re_kb/24, vindicated by A/B diff)

## EMITTER-specific (only if the browser keeps the emitter instead of the lever above)
The emitter (`sprite-client.mjs` / `sprite-gpu.mjs`) has its OWN palette/carve/facing handling and
did NOT get the shared fixes. If the emitter stays the default, port each fix above into it:
palette-bank preserve, the m-carve, the non-square Tw==Th gate, facing XOR 0x4000.

## Z-ORDER / LISTS  — likely the browser HUD/bars garble  [IN PROGRESS, native]
Native `gstaEmitSpriteTA` forced ALL quads into one Translucent list, discarding the engine's
per-object OPAQUE/PUNCH-THROUGH/TRANSLUCENT structure. Bodies ARE genuinely TR (ListType=2,
TSP `0x949004D2`) — confirmed. The fix in progress: emit each object in its real engine list (from
the PCW ParaType/ListType) so OP→PT→TR draw in order with correct depth.
**Browser equivalent:** `buildHudTA` flattens every HUD-quad z to 1.0 and draw order = submit order
→ the para4 red backing covers the para5 team-color fill (the "all-red bars"). The per-object
list/z fix should port directly.

**CONFIRMED list assignments (native z-order pass, commit `ed11835b2`, re_kb/45):**
- **STAGE = OPAQUE** — ListType 0, ParaType 4 (Polygon). PCW high byte `0x80`.
- **BODIES = TRANSLUCENT** — ListType 2, ParaType 5 (Sprite). `gen_submit_params finalize_body`
  sets `PCW |= 0x02000000` (bit 25). TSP `0x949004D2`.
- flycast `ta_handle_cmd` (`core/hw/pvr/ta.cpp:222-249`) latches `pcw.ListType` on the first param of
  each list, resets on End_Of_List; the renderer draws **OP→PT→TR**.
- Native emit order that works: `[stage OP polys][EOL][body TR sprites]` → stage behind bodies.
  Live: `op=930 pt=0 tr=87` (was `op=0`). Browser: emit each object in its real PCW list + a proper
  EOL between lists, instead of one flat TR list with z=1.0.

## STAGE / BACKGROUND  [native FIRST VERSION landed — commit `ed11835b2`, re_kb/45]
Native `gsta_stage.cpp` is a faithful C++ PORT of `web/webgpu/stage-client.mjs _buildFromTA`
(loads `STGxx_ta.json`, reads `stage_id`@`0x8C289638` + camera M2@`0x8C2D6AD8`/M1@`0x8C2D6B18` from
the seeded RAM, re-projects world-authored meshes through the live camera, emits OP-list polys).
Stage verts 1881/1893 = 99.4% vs engine; bottom edge exact. NO wire gap (stage_id + camera already
shipped; other stages just need the OFFLINE `bake_stage_from_ta.py` per-stage TA bake).

**★ Browser-applicable bug found:** the native port inherited a **floor-to-white intensity override**
from `stage-client.mjs` that washed the dark grid bright and hid the blue floor — **disproven by the
engine-TA ground-truth raster** and replaced with the bake's real **per-vertex RGB**. The browser
`stage-client.mjs` very likely STILL has this floor-to-white override — check `_buildFromTA` and use
per-vertex colour, validated against `_stage_gt/GROUNDTRUTH_engine_ta_STG0B.png` (the engine TA bake).
**Open (native, also browser):** lower blue-floor deck stripes render too dark (deck bottom-row
per-vertex RGB ~0 modulates near-black; engine shows lit blue stripes) — recheck per-vertex colour on
mesh0 bottom rows vs the engine TA.

## META-LESSON — a hand-rolled validator can lie
`tools/render-replica-poc/_validate_all_multi.mjs` reported false `diff=0` because its reference
assumed single-blob storage (the native non-square carve was "self-consistent under the wrong
model"). It was only caught by diffing through **flycast's REAL `twop`** (`core/rend/texconv.cpp
ConvertTwiddlePal4`). **Validate browser decode against flycast's own twop / the engine VRAM, never
a hand-authored reference.** Same applies to `web/webgpu-test.html` DIFF tooling.

## NOT applicable to the browser
- **Texture↔TA threading race** (commit `b82834447`): native-only (WS thread decoded into shared
  `vram[]` while the render thread sampled a prior frame). The browser applies tiles synchronously
  per frame, so it never had this. Listed so it isn't mistakenly "ported."
