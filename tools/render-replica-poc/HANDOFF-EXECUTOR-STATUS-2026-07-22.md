# Thin SH-4 Executor — Status & Handoff (2026-07-22)

Pick-up-cold guide for the transpiled MvC2 game-tick executor. Read this + `HANDOFF-EXECUTOR.md`
+ the `project_thin_sh4_executor_scoped` memory before touching this code.

## Goal
Run MvC2's per-frame game logic **off flycast**, byte-exact, from a resident 16 MB RAM image, so a
browser/native client can drive the game locally (client-side prediction / input-only netcode). The
transpiler (`lift.py`→`codegen.py`→`emit_func.py`→`gen_tick.py`) emits `gen_tick_all.c`; entry =
`tick_entry` (root `loc_8c0358be`, scene-FSM). `render_frame` (amalgam `core/network/gsta_render_frame.c`)
walks the resulting state → body quads.

## THE HEADLINE (2026-07-22)
**The executor is byte-exact for CHARACTER structs but DIVERGES on POOL OBJECTS (effects/projectiles).**
The live "bodies vanish / render decays to 0" symptom is NOT a render bug — it's a **pool-object
game-logic divergence** one layer under the renderer.

### How we got here (all disproven en route — do not re-litigate)
- Live `MC_LOCAL` drive (executor is the engine, seeded once from `/replica-live`) renders, then decays to 0
  over ~10 frames and eventually segfaults.
- Oracle-diff (realcore `runner.exe` chained N frames = flycast ground truth vs `execstep.exe` = executor
  N-tick dump), region-split: **char structs `0x268340` are byte-exact; the tile buffer `0x1FA3xx` diverges.**
- Built a runner **per-part (width,height @ muls.w `0x8C033C06`) + per-object (r4 @ `loc_8c033b0a` entry
  `0x8C033B0A`) logger.** Result: **dims are byte-identical**; flycast processes **47 parts / 6 objects**,
  executor **46** — executor drops **OBJ[2] = `0x8C27D734`** (a pool object, single 64×32 part = the 2
  missing tiles).
- **Both divides proven correct** (`divtest2.exe` unsigned `loc_8c1291dc`, `divtest3.exe` signed
  `loc_8c129128` — the one size-0x20 parts use; incl `1024/512=2`). The record loop, size dispatch, and the
  `loc_8c04508e` pool-list walk are all **faithful transpiles**.
- **TRUE ROOT:** `0x27D734`'s struct **diverges on a SINGLE tick from byte-exact `truth_f2`** (19 bytes):
  `+0x12C` visibility exec=0 vs flycast=1 (→ the walk correctly skips an object the SIM marked invisible);
  `+0x144` sprite 0x352 vs 0x366; `+0xC` next-ptr, `+0x4`, `+0x8` differ.

So: **the render path was never wrong.** A pool-object update routine is mis-transpiled — same class as the
divide (a code path that looks correct and stays latent until a frame exercises it; Test A `f90→f91` never
did, `f2→f3` does). This is a **subsystem gap** (effects/projectiles), not a one-line fix, and there are
likely several such routines.

## Standing tools (built this effort — REUSE THESE)
Build env: `zig cc` at the session scratchpad (`zig-x86_64-windows-0.16.0/zig.exe`); runner via MSVC.
- **realcore ground-truth chain**: `build_leaf_seed.py <entryPC> <ram.bin> <seed>` then
  `runner.exe <seed> --leaf --min-ctx --no-isolate`. Feed each `oracle_ram_out.bin` back as the next seed →
  N frames of flycast ground truth (`truth_f0..f15` already built for the `_ram_f90` line).
- **runner per-part/per-obj logger** (in `realcore/runner.cpp`, `mc_readtrace::onPc`): logs `r8,r9` at PC
  `0x0C033C06` and `r4` at `0x0C033B0A`, prints `FLYCAST tile assembly: N objects, M parts` with the
  object→parts breakdown. **Extend this to log any reg at any PC** — it's the part/object-level oracle.
  Rebuild: `cmd /c "cd /d <realcore> & .\dobuild.bat"` (NOTE: PowerShell mangles `&&` → single `&`; cmd
  won't run a `.bat` from cwd by bare name → `.\dobuild.bat`; ~1 min MSVC build).
- **execstep.exe** `<ram.bin> <N> <out.bin>` — load a RAM image, run `tick_entry` N times, dump 16 MB.
- **region-split diff**: `cmp -l a.bin b.bin | awk '($1-1) < 0xFE0000'` (game region; mask the `0x8CFExxxx`
  scratch stack). Split by struct region to localize.
- **chain_drive.exe** (`chain_render.c` driveN/tickN modes + `gen_tick_all.c` + the 9 render srcs) — offline
  drive+render harness.

## Key facts / addresses
- Char slots: `0x8C268340 + k*0x5A4` (k=0..5). Pool objects: `0x27Dxxx` / `0x26Axxx`, linked list head-table
  near `0x8C287A5C` (`0x287A5C + cat*4`), `node+0xC` = next, `node+0x12C` = visibility gate, `node+0x144` = sprite_id.
- Tile assembler `loc_8c033b0a`; render walker `render_sprites_0308c2` (COUNT `0x8C2895E0` / PTR `0x8C287DE0`,
  16 layers × 0x180). Crash-guard fixes for the LIVE render (separate from this sim bug): clamp the 16 slot
  counts to 0x60, render over a COPY (render_frame writes ~170 sites), reset render-queue counter `0x8C289F80`.
- `loc_8c0348c8` (bit15 per-part-scale twin) is ABSENT from the transpile (0 refs) — a known gap for scaled
  sprites, not this bug.

## ROOT CAUSE FOUND (2026-07-22) — a MISSING transpiled routine
Hooked flycast's memory writes (runner `writeT`, logs the writing PC for any address). On the f2→f3 tick,
flycast writes `0x27D734` via: `0x8C129734` (memclear), **`0x8C044F80`/`0x8C044FD0`/`0x8C044FD4`** (pool-list
INSERTION — writes `+0x08=0`, `+0x0C=0x8C27D564`, splicing it into the active list), `0x8C0E3106`/`0x8C0E3116`
(set `+0x04=1`, `+0x12C=1` visible), `0x8C12960C`/`0x8C129514` (sprite `0x365→0x366`). The executor leaves
`0x27D734` == input — runs NONE of these.

**In `gen_tick_all.c`: `loc_8c0e3106`, `loc_8c0e30bc`, `loc_8c129510/514` ARE transpiled, but `0x8C044F80`,
`0x8C044FD0/FD4`, `0x8C12960C` have 0 refs — NOT transpiled.** Because the pool-INSERTION routine at
`0x8C044F80` isn't in the transpiled set, the runtime `call_addr(c, 0x8c044f80)` hits `mc_unknown_call`
(no-op), the object is never spliced into the active pool list, and the (transpiled) setup routines never
reach it. Classic "worklist missed a fn reached via an indirect `jsr @rN`" — the known crank/indirect-reach gap.

## THE FIX — DONE ✅ (2026-07-22)
Added the object-pool SPAWN chain to `gen_tick.py` `EXTRA_FUNCS` (entry `loc_8c044f12` = the universal
`Obj_Alloc`, reached via `jsr @rN` from the already-transpiled `loc_8c0e3098`; + callees `loc_8c044fa2`
insert-at-head, `loc_8c129728` memset, `loc_8c129560`/`loc_8c129600` copy, `loc_8c034c38` anim-load; + 3
forward-coverage bank14 listop handlers). `python gen_tick.py` → **280 fns, 0 failures.** Rebuilt + oracle diff:
- **`0x27D734` pool object: 19-byte diff → 0. Byte-exact.** (visible, sprite 0x366, links, cell-data all match.)
- **Whole-tick f2→f3 divergence: 109 bytes → 2** (the 2 are tile-arena bookkeeping 0x1F9D7C).
- **Drive-chain game-state divergence (chain executor's own output 14 ticks): 238 bytes → 20** (chars byte-exact;
  2 tile-arena + 18 from a SECOND spawn event at f6→f7, a different pool object — same fix loop applies).

The pool-object/effects subsystem gap is closed for the observed spawn. **The executor is now a byte-exact game
engine** (chars + effects). re_kb upsert candidate: `finding:object_pool_spawn_chain`.

## RENDER DECAY IS A SEPARATE, UNDERSTOOD ISSUE (not a game-logic bug)
`render_frame` over the driven state still decays to 0 by ~frame 10 — BUT it decays IDENTICALLY over flycast's
OWN leaf-oracle chain (`render(truth_f0)=99, f5=35, f10=0` == `render(exec N-tick)`). Reason: the oracle chain +
the drive both run only the **game tick** (`loc_8c0358be`), NOT flycast's full frame (tick **+ render pass**
`loc_8c030858`, which is stubbed). The render pass does per-frame render-state maintenance (resets, e.g. the
render-queue counter `0x8C289F80`; `loc_8c034e8c` anim-load; etc.) that neither chain runs → the render-walk
state degrades. **This is the render-state-rebuild step, orthogonal to game-logic byte-exactness.**

## NEXT
1. **Second spawn (f6→f7, 18 bytes, pool region 0x27D3xx/0x287Axx):** same write-PC-hook loop → likely another
   bank14 listop category (the forward-coverage handlers may already cover it — re-check) or one more fn.
2. **Live render:** run the render pass's state-maintenance each frame in the drive (transpile `loc_8c030858`
   for its side effects, or the specific resets) so `render_frame` over the byte-exact driven state stays stable.
   The game state is correct now, so this is purely render-state plumbing.
3. Then the live `MC_LOCAL` drive renders stably → pure-local drivable game.

## Repo hygiene
`gen_tick_all.c`, `gen_*.c`, `*.wasm`, `_ram_*.bin`, `truth_f*.bin`, `_step*.bin`, `realcore/*.bin` are
DERIVED/ROM-adjacent → **gitignored, never commit.** Tracked changes this effort: `chain_render.c` (driveN/
tickN modes), `shadow_exec_runner.c` (`mc_shadow_compose_proj`), `realcore/runner.cpp` (the per-part/obj
logger). All temporary `gen_tick_all.c` diagnostic edits have been reverted.
