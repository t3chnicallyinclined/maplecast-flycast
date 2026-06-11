# Running Flycast's REAL Render in the Browser — Strategic Scope

**Date:** 2026-06-08
**Status:** Read-only RE + design. No build, no prod, no code change.
**Question:** Is running flycast's *actual* render path in the browser — instead of
the hand-rolled JS sprite client (`web/webgpu/sprite-client.mjs`) — **easier AND
more accurate** than continuing the reconstruct-from-state effort? The ROM
constraint is LIFTED for testing (the browser may load the ROM).

> **Confidence key:** **[C] = confirmed** by a file/build I read in-tree; **[I] =
> inferred** from those files + memory; **[?] = unknown / needs the cheap test.**

---

## TL;DR / Recommendation

- **A full flycast WASM that runs the SH4 CPU + the real renderer ALREADY EXISTS**
  in this project's orbit — the EmulatorJS "Path B" core (`flycast-wasm` sibling
  repo). It already loads the MVC2 CHD, boots the game, runs it CPU-first, then
  enters mirror mode. It has a **WASM SH4 JIT** (`rec_wasm.cpp`, SHIL→WASM, 51/70
  ops native). **[C]** (`docs/WASM-BUILD-GUIDE.md`, `web/emulator.html`,
  `core/network/maplecast_wasm_bridge.cpp`).
- That means the "moonshot" path (B) — *load the ROM in-browser, join via a
  savestate, replay the server's input stream so the client's own SH4 renders
  itself* — is **not greenfield**. The two missing pieces are (1) does that core
  hit **playable fps** for MVC2, and (2) wiring the **savestate-join + input-replay**
  harness that already exists natively (`.mcrec` / `replay_reader`) into the wasm
  core. Both are **[?]** today.
- **The dead-end (`project_state_replica_injection_deadend`) does NOT apply to
  (A) or (B).** It killed *partial GSTA injection into a live SH4*. Both (A) the
  standalone emitter and (B) full deterministic replay **never inject partial
  state** — (A) runs no SH4 at all; (B) runs the *whole* sim from inputs. **[C]**

**Recommended first cheap test (Test 1 below): load the MVC2 ROM in the existing
`flycast-wasm` core in a browser tab and measure raw SH4+render fps for an
in-match scene.** One afternoon, no new code beyond a timer. If it sustains
~55–60 fps, path (B) can *moot the entire emitter/sprite-client effort* and the
ongoing cell-data RE work. If it's 10–20 fps, (B) is parked and **(A) emitter is
confirmed the path** — keep doing the cell-data work.

---

## 1. What WASM Flycast Exists Today (all [C])

There are **two** browser WASM clients in this project, plus a pure-JS renderer.
Only **one of them runs the SH4 CPU.**

| Client | Runs SH4? | Real flycast renderer? | Consumes | Size | Source |
|---|---|---|---|---|---|
| **`king.html` standalone renderer** | **NO** | **YES** (flycast GLES via WebGL2) | TA mirror wire (delta TA + ZCST) | 831 KB | `packages/renderer/` (in this repo) |
| **`emulator.html` EmulatorJS core** | **YES** (WASM JIT) | YES (same flycast renderer) | MVC2 **CHD ROM** + BIOS, *then* TA mirror | ~3.4 MB (7z) | `~/projects/flycast-wasm/` (sibling repo) |
| **WebGPU pure-JS (`webgpu-test.html`)** | NO | **NO** (reimplemented PVR2 in JS) | TA mirror **or** GSTA sprite state | ~65 KB JS | `web/webgpu/*.mjs` (in this repo) |

### 1a. `packages/renderer` — the 831 KB .wasm is a TA renderer ONLY, no SH4 [C]
- `packages/renderer/CMakeLists.txt` lists ~35 source files: `core/hw/pvr/`
  (TA parser + regs: `ta_vtx`, `ta_ctx`, `pvr_mem`, `pvr_regs`), `core/rend/gles/`
  (the real GLES renderer), `core/rend/` (TexCache, texconv, sorter), zstd decode.
  **No `core/hw/sh4/`, no dynarec, no `emulator.cpp`.**
- `packages/renderer/src/stubs.cpp` provides ~40 no-op stubs (mprotect, holly_intc,
  `rec_*` JIT, scheduler) *specifically to avoid pulling in the rest of the
  emulator* (`docs/WASM-BUILD-GUIDE.md` §"Build inputs"). Header comment in
  `wasm_bridge.cpp`: *"No RetroArch. No libretro. No EmulatorJS. … Receives TA
  commands from the server."*
- Exports: `renderer_init / renderer_sync / renderer_frame / renderer_resize /
  renderer_destroy`. It **consumes the TA mirror wire**, runs flycast's real
  `renderer->Process()` → `Render()` → WebGL2. Pixel-perfect, **but fed pixels/TA
  from the server** (36–88 Mbps in-match — the bandwidth wall in
  `project_render_pipeline_state`).

### 1b. `emulator.html` EmulatorJS core — the FULL emulator, with a WASM SH4 JIT [C]
- `docs/WASM-BUILD-GUIDE.md` "Path B": *"Full flycast WASM core compiled as a
  libretro archive … Runs MVC2 **CPU-first** and then enters mirror mode."*
- It is built from the **sibling repo `~/projects/flycast-wasm/`** (NOT in this
  tree). The SH4→WASM JIT lives in that repo's patches:
  `upstream/patches/rec_wasm.cpp` ("WASM JIT — SH4 blocks to WASM modules at
  runtime"), `wasm_emit.h` ("SHIL IR to WASM instruction emitter, **51/70 ops
  native**"). So it is a *real* recompiler, not pure interpreter. **[C]**
- It loads the **MVC2 CHD ROM + `dc_boot.bin`/`dc_flash.bin`** in the browser FS,
  boots, runs the game (`EJS_onGameStart`), then auto-calls `_startMirror()` to
  *also* render incoming server TA frames. So today it runs the game **and** can
  mirror — but the mirror is layered on top; the local SH4 run is real.
- `core/network/maplecast_wasm_bridge.cpp` (in THIS repo) is that core's mirror
  bridge — note it `#include`s `hw/sh4/sh4_mem.h`, `hw/aica/aica_if.h`,
  `emulator.h`, `mem_watch.h` (the full-emulator headers), unlike the standalone
  renderer's bridge. **[C]** This is the file that proves Path B is the
  full-emulator core.

### 1c. WebGPU pure-JS client — the CURRENT production whole-sprite client [C]
- `web/webgpu/sprite-client.mjs` + `sprite-gpu.mjs` + `pvr2-renderer.mjs`.
  This is the hand-rolled client the operator wants to compare against. It
  **reimplements** PVR2 rasterization in JS and **guesses** render properties
  (z / anchor / blend) — the source of the ongoing projectile-anchor /
  white-wash / part-remap bugs catalogued in `docs/HANDOFF-2026-06-08.md`.
- It consumes the lean **GSTA** state (~262–328 B/frame, ~0.05 Mbps) and the
  baked whole-sprite atlases. This is the architecture in
  `project_render_pipeline_state`. It is **live on prod** today.

**Bottom line for §1:** A full flycast-in-the-browser (SH4 + render) **is not
hypothetical — it's the EmulatorJS Path B core, already built and loading MVC2.**
The only open question is its *speed* when running the game itself (not just
mirroring). [C] that it exists; [?] its standalone-run fps.

---

## 2. The Concrete Paths to Render MVC2 in-Browser via Flycast's Real Code

### Path (A) — Emitter → TA → existing renderer (the current pivot, "reconstruct-from-state")
Port MVC2's SH4 sprite-assembly emitter (`loc_8c033e90` family) to C++/JS,
fed by **GSTA + ROM part data read directly**, producing a TA buffer that the
existing renderer (`pvr2-renderer.mjs` in JS, or the 831 KB `renderer.wasm`)
rasterizes. Because the *rasterizer* is flycast's, z/blend/sort come out correct
by construction — only the *geometry/UV emit* is reimplemented.

- **Build effort:** **Medium-high, and partially DONE.** `buildEmitterDrawList`
  (sprite-client.mjs) already ports the geometry math (flip/CPS-scale/anchor —
  *verified correct by the RE expert, do not edit*). **The wall is the cell-data
  decode:** the part-index→atlas-slot remap (`gfx2` cell-record `+6` selector).
  `project_render_pipeline_state` (2026-06-08) reports this was just **RESOLVED on
  paper** (bank03.asm:9258-9270: 8-byte cell = `[dx s16][dy s16][PAL u16@+4]
  [GFX-SEL u16@+6]`), pending an extractor fix. **[C/I]**
- **Per-frame cost:** **~0.05 Mbps** wire (GSTA only). Client CPU = emit a few
  hundred quads + rasterize (the 831 KB renderer measures ~700–1090 µs/frame
  rendering full TA, `docs/ARCHITECTURE.md`; the emit add is small). **[C]**
- **Accuracy:** Rasterization inherits flycast (correct). **Geometry/UV/anchor is
  reimplemented → only as correct as the RE.** This is exactly where today's bugs
  live (projectile anchors, part-remap scramble). **Partial inheritance.** **[C]**
- **Open risks:** the cell-data remap (in-progress), per-object screen anchor
  (the Frame Oracle now gives ground truth to *validate* against — but the
  *client* still has to compute it from GSTA, not read it from a live SH4),
  effects/super geometry (deliberately left streamed). **[C]**

### Path (B) — Full flycast WASM + deterministic input-replay (the moonshot — now de-risked)
Compile/run the FULL emulator in wasm (SH4 + render — **the Path B core already
does this** [C]), load the MVC2 ROM in-browser, **join a live match via a server
savestate**, then **replay the server's input stream**. Because SH4 emulation is
**byte-deterministic across machines/OSes** (`reference_determinism_validated`,
validated 2026-05-07), the client's game stays byte-identical to the server's and
**renders itself** locally. Ship **inputs (tiny) + a join savestate** instead of
GSTA or TA.

- **Build effort:** **Lower than it sounds, because the hard parts exist.**
  - Full SH4+render wasm core: **EXISTS** (Path B, `flycast-wasm`). **[C]**
  - Savestate-join + input-replay harness: **EXISTS natively** as the `.mcrec`
    record→replay system (`core/network/replay_reader.{h,cpp}`,
    `replay_writer.*`). `replay_reader.h`: *"decompresses + applies the starting
    savestate, then yields input events frame-by-frame … deterministic SH4
    regenerates byte-perfect identical TA — same pixels as the original match."*
    The **pull-model** input read (2026-05-07 redesign) hooks
    `ggpo::getLocalInput()` per frame — exactly the seam a wasm replayer needs.
    **[C]** Record→replay is **validated end-to-end on native** (Magneto tag-in
    reproduced, `project_step_d_validated`). **[C]**
  - **What's NEW for (B):** (1) compile that replay path into the wasm core; (2) a
    *live* join (ship the current savestate on connect + start streaming
    server-authoritative inputs as a continuous tail, not a finite file); (3)
    periodic re-sync savestates to bound any drift / late-join. **[I]**
- **Per-frame cost:** **Bandwidth = inputs only (~1 KB/s)** + an occasional
  savestate (~6.7 MB compressed to ~0.6 MB via the SYNC-cache ratio, sent on join
  / periodic re-sync). **[C/I]** **Client CPU = run the whole SH4 at 60 fps in
  wasm** — this is the crux **[?]** (see §2-feasibility).
- **Accuracy:** **PERFECT — full inheritance.** It is literally flycast running
  MVC2; pixels are the game's own. No RE, no guessing, no cell-data remap, no
  anchor problem. This is the *only* path that needs zero MVC2 RE. **[C/I]**
- **Open risks:**
  1. **fps of full SH4 in wasm** — *the* gating unknown **[?]**. See below.
  2. **Cross-version determinism** — predictor and server must be the **same
     flycast commit** (`reference_determinism_validated` — not validated across
     versions). A wasm build is a *different toolchain* than the native server;
     determinism is validated cross-**OS** (Linux/GCC ↔ Windows/MSVC) but
     **NOT yet for emscripten/wasm**. **[?] — must be re-validated.**
  3. **Rendered-pixel determinism is NOT guaranteed** — but here it doesn't
     matter: each client renders its *own* deterministic TA, so it shows the
     correct game even if two clients' GPUs differ by a sub-pixel. Determinism is
     load-bearing on the **TA**, which IS guaranteed. **[C]**
  4. **Audio / AICA** in wasm (out of scope for a render test; the mirror never
     shipped audio anyway). **[C]**

#### (B) feasibility — is full Dreamcast/Naomi flycast at 60 fps in a browser realistic? **[?] leaning plausible**
- **Pro:** the WASM JIT is real (51/70 SHIL ops native, `wasm_emit.h`) — not a
  pure interpreter, so it's far faster than naive. **[C]** MVC2 on the *native*
  headless build is only ~12–24% of one core (`docs/ARCHITECTURE.md`), so the SH4
  workload itself is light; wasm-JIT overhead is the multiplier. The render is
  the same WebGL2 path the 831 KB client already runs at 60 fps. **[C]**
- **Con / unknown:** no in-tree measurement of the Path B core running the game
  *standalone* at speed exists — it's documented as "runs CPU-first then mirrors,"
  not "benchmarked at 60 fps." `docs/OPTION6-INSANE-IDEAS.md` #15 frames a *pure*
  SH4-in-browser as a "6–12 month moonshot," but that entry is about a *WebGPU
  compute-shader* recomp, **not** the already-existing emscripten WASM JIT — so it
  overstates the cost for *this* path. **[I]** The honest answer: **unknown until
  measured** — which is exactly Test 1.
- **Prior negative signal:** the native `MAPLECAST_CLIENT_ONLY` mirror-client +
  SH4 combo hit **~5–15 fps in MVC2 match scenes** due to **renderer-thread
  contention** (`project_local_headless_predictor`). That was a *threading*
  problem solved by the headless (no-render) predictor topology — it is **not**
  evidence the SH4 itself is slow. A browser core has the same render-vs-CPU
  tension to watch for. **[C]**

### Path (C) — Hybrids
- **(C1) Render-oracle wasm:** run the Path B core **headless-ish** as a pure
  render oracle (no input, just replay) to *validate* the JS sprite client's
  output frame-by-frame — a browser-side TA-vs-reconstruction differ. Cheap
  byproduct of having the full core; useful even if (B) isn't the shipping path.
  **[I]**
- **(C2) Server renders TA for objects only, client composites:** essentially
  today's mirror but object-scoped — still pays the geometry-bandwidth wall
  (`project_render_pipeline_state`: wire is ~73% geometry). **Not lean. Skip.**
  **[C]**
- **(C3) Stream the chaotic ~5% (supers/aura), reconstruct the rest:** this is
  already the **deliberate hybrid boundary** of the current (A) architecture
  (`project_render_pipeline_state`). Not a new path — it's the status quo. **[C]**

---

## 3. Reconciling With the Injection Dead-End

`project_state_replica_injection_deadend` (2026-06-06, confirmed) says: running a
second SH4, freezing inputs, and **injecting GSTA char fields every frame**
crashes (`SH4 exception when blocked`, illegal instruction). Root cause:
`writeGameState()` overwrites `sprite_id` etc. but **not** the engine-owned
pointer cluster `+0x154–0x184` (cell_data / animations / Dat_GFX*); the bank12
cell processor then reads a half-updated, internally-inconsistent character and
computed-jumps into non-code. **[C]**

**This kills exactly one idea: "inject GSTA + render one frame on a live SH4."**
It does **NOT** affect:
- **(A) standalone emitter** — runs **no SH4 at all**. It reads ROM part data +
  GSTA and emits a TA directly. Nothing to corrupt. **[C]**
- **(B) full deterministic replay** — **never injects partial state.** It runs the
  **whole** simulation forward from a *complete, consistent* savestate using the
  *full* input stream. Every character struct stays internally consistent because
  the *game itself* maintains it. The only state ever applied wholesale is a
  *complete* savestate (consistent by construction) at join / re-sync — never a
  partial per-field overwrite. **[C/I]**

> The dead-end is actually an *argument for (B)*: the lesson was "partial state on
> a running engine is unsurvivable; a whole consistent savestate is fine"
> (`project_state_replica_injection_deadend`: *"Injecting fully-consistent state
> every frame = shipping a whole savestate every frame"*). (B) ships exactly that
> — a whole savestate once, then drives the engine with inputs the way it expects.

---

## 4. Comparison Table

| Path | Build effort | Per-frame cost | Accuracy (inherits flycast raster?) | Key risk |
|---|---|---|---|---|
| **(A) Emitter→TA→renderer** *(current pivot)* | Medium-high; **geometry done**, cell-data remap in progress | **~0.05 Mbps**; client emit+raster ~1 ms | **Partial** — raster inherited (correct z/blend); **geometry/anchor reimplemented** (today's bug surface) | Cell-data part-remap + per-object anchor RE never fully done; effects stay streamed |
| **(B) Full wasm + input-replay** *(moonshot, de-risked)* | **Lower than expected** — full core EXISTS (Path B), replay harness EXISTS (`.mcrec`); new = wasm-replay wiring + live join | **~1 KB/s inputs** + periodic ~0.6 MB savestate; **client CPU = full SH4@60 in wasm [?]** | **PERFECT** — it's the real game; zero RE | **(1) wasm fps [?]**, (2) wasm-build determinism vs native server [?], (3) version pinning |
| **(C1) Render-oracle hybrid** | Low (reuse Path B core) | n/a (validation tool) | Perfect (it's flycast) — used to *check* (A) | Only a tool, not a transport |
| **(C2/C3) Object-TA / stream-the-chaos** | — | C2 still ~Mbps; C3 = status quo | — | C2 not lean; C3 already in (A) |
| **(baseline) JS sprite client** *(prod today)* | Shipped | ~0.05 Mbps | **Reimplemented raster + guessed props** → ongoing bugs | The thing we're trying to beat |

---

## 5. Recommended First Cheap Test

**Test 1 — "Does full flycast-WASM run MVC2 at playable fps in a browser tab?"**
This single measurement decides whether (B) can moot the entire (A) emitter +
cell-data RE effort.

**What to do (no new code beyond an fps counter):**
1. Take the **existing Path B EmulatorJS core** (`~/projects/flycast-wasm/`,
   `emulator.html`) — it already loads the MVC2 CHD + BIOS and boots the game. **[C]**
2. Run it **WITHOUT** `_startMirror()` (don't connect to the server) so it's
   purely the local SH4 driving the local render — i.e. the game running on its
   own in the tab.
3. Sit in an **in-match scene** (the heavy case — both prior fps figures were
   for match scenes) and read fps over ~30 s. EmulatorJS already exposes a frame
   loop; add a `performance.now()` delta counter or use its built-in FPS display.

**Decision:**
- **≥ ~55 fps sustained in-match → (B) is viable.** Greenlight Test 2 (wire the
  `.mcrec`/`replay_reader` join+input path into the wasm core and replay a
  recorded match in-browser — pixels should match the native replay). If Test 2
  passes, **(B) renders MVC2 perfectly from ~1 KB/s of inputs, with ZERO MVC2 RE,
  and the emitter/cell-data work can stop.**
- **~20–50 fps → marginal.** (B) might work for *spectators* (where 1-frame
  judder is tolerable) but not competitive play. Keep (A) for the competitive
  client; consider (B) for watch-back. Use **(C1)** as a render oracle regardless.
- **< ~20 fps → (B) parked.** Confirms **(A) emitter is the path** — continue the
  cell-data remap (`+6` selector fix) and per-object anchor work. The Frame Oracle
  stays the ground-truth validator.

**Why this test first:** it's the **cheapest** possible probe (no new
infrastructure — the core and ROM already load) of the **highest-leverage**
unknown. Every other open question in this doc (cell-data remap, anchor RE, wasm
determinism, live-join protocol) is *downstream* of "is full-wasm fast enough."
If the answer is yes, a large chunk of in-flight RE work becomes optional.

**Secondary (only if Test 1 is borderline):** **Test 1b** — wasm determinism
spot-check: run the wasm core and the native headless server from the **same
savestate + same `.mcrec` inputs**, dump TA via `MAPLECAST_DUMP_TA`
(`Renderer_if.cpp` hook, works in any mode per `reference_determinism_validated`),
and byte-compare. Confirms the wasm toolchain didn't break the determinism that
(B) depends on. **[?] not yet validated for emscripten.**

---

## 6. Confirmed vs Inferred — quick ledger

**Confirmed [C]:**
- 831 KB `renderer.wasm` is TA-renderer-only, no SH4 (`packages/renderer/CMakeLists.txt`, `stubs.cpp`).
- A full flycast wasm with a real SH4 WASM-JIT exists (`flycast-wasm` sibling repo; `docs/WASM-BUILD-GUIDE.md` Path B; `maplecast_wasm_bridge.cpp` includes the full-emulator headers). It loads MVC2 CHD and boots.
- `.mcrec` savestate-join + per-frame input-replay harness exists natively and is validated (`replay_reader.{h,cpp}`, `project_step_d_validated`).
- SH4 is byte-deterministic same-machine / cross-machine / cross-OS (`reference_determinism_validated`).
- The injection dead-end kills only partial-state-into-live-SH4, not (A)/(B).
- The JS sprite client reimplements raster + guesses render props (the bug source).
- Pixel-shipping (mirror/VCACHE) floors at 36–88 Mbps; wire is ~73% geometry.

**Inferred [I]:**
- (B)'s new work is small (wasm-replay wiring + live-join + periodic re-sync) *given* the existing core + harness.
- OPTION6 #15's "6–12 month" cost is for a WebGPU-compute recomp, not the existing emscripten JIT — so it overstates (B)'s effort.

**Unknown — needs the test [?]:**
- **fps of full SH4 + render in the EmulatorJS wasm core for an in-match MVC2 scene** (Test 1 — the decision point).
- Whether the emscripten/wasm flycast build is **TA-deterministic vs the native server** (Test 1b).
- Live-join protocol details (savestate-on-connect + continuous server-input tail + re-sync cadence).

---

*Files cited:* `packages/renderer/CMakeLists.txt`, `packages/renderer/src/wasm_bridge.cpp`,
`packages/renderer/src/stubs.cpp`, `packages/renderer/build.sh`,
`core/network/maplecast_wasm_bridge.cpp`, `web/emulator.html`, `web/king.html`,
`web/webgpu/sprite-client.mjs` / `sprite-gpu.mjs` / `pvr2-renderer.mjs`,
`core/network/replay_reader.{h,cpp}`, `core/network/replay_writer.*`,
`docs/WASM-BUILD-GUIDE.md`, `docs/ARCHITECTURE.md`, `docs/HANDOFF-2026-06-08.md`,
`docs/OPTION6-INSANE-IDEAS.md`.
*Memories:* `project_state_replica_injection_deadend`, `reference_determinism_validated`,
`project_local_headless_predictor`, `project_step_d_validated`, `reference_frame_oracle`,
`project_render_pipeline_state`.
