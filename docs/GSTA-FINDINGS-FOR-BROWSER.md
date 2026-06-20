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
## STAGE FLOOR/DECK black — ROOT-CAUSED + FIXED  [commit pending, re_kb/47, `finding:gsta_stage_floor_cull_fix`]
The "lower blue-floor deck renders dark/missing" item above was **NOT a per-vertex-colour problem** —
that earlier guess (deck bottom-row RGB ~0) was **wrong**. Per-mesh measurement vs the engine TA found
the real cause:

**The BLUE LOWER-DECK FLOOR (mesh3, texture t02 tcw `0xa0000`) was MARGIN-culled.** It is 2 huge
triangles spanning X ±6822 that cross the visible bottom band (baked screen Y 362..585). The native
emit rejected a whole triangle if ANY vertex left `[-MARGIN(800), 640/480+MARGIN]`, so the giant floor
quad was dropped entirely. The engine instead **submits the full quad and lets the PowerVR guard-band
clip it** (the captured engine TA carries mesh3 at full ±6822 extent). Also `mesh0` (green deck) has 8
grazing/behind-camera verts the engine clamps to `1/w=10` with screen XY ≈ −1.4e7 (`pos[2]==10`
sentinel), and the whole-tri reject killed every strip triangle touching one.

Measured lower-deck band coverage (y 330..410): engine GT **0.41**, native BEFORE **0.03** (93%
missing), native AFTER **0.49–0.58** across 17 frames, rgb now **blue-dominant** `[~40,~78,~148]`
matching engine `[3,18,70]`. All 4 stage bands hue-match the engine (grid green-dom, deck+floor
blue-dom).

**Fix (engine-faithful, applies to the browser too):** drop the whole-tri MARGIN reject. Reject a
vertex ONLY if non-finite or `|screen| > 1e6` (the `1/w=10` sentinel garbage); then keep a triangle
iff its screen **bounding box overlaps** the visible frame (±64px slack). Let the rasterizer clip the
rest. The browser `stage-client.mjs _buildFromTA` has the **same `MARGIN`-based per-vertex cull** —
apply the same change there or its blue floor is culled identically. The "~12 culled props" were these
world-authored deck/floor pieces, **not** the local props (69 props rendered 68/69; the 1 miss is a
deck piece entirely above the frame, engine-culled too).

## STAGE PATH must resolve from the BINARY dir  [commit pending, re_kb/47, `finding:gsta_stage_path_from_binary`]
Native `gstaStageEnsureLoaded` resolved `STGxx_ta.json` only via the env override + cwd-relative bases,
so launching `build/flycast.exe` from `$HOME` silently failed to load the stage. Fixed: prepend the
**executable's own dir** (`GetModuleFileNameA` / `readlink /proc/self/exe`) and `exeDir/../atlas/stages`
etc. to the candidate bases. Browser analogue: resolve stage assets relative to the **module URL**
(`import.meta.url`), not a page-relative path, so the page works regardless of where it's served from.

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
