# ROM-Asset Client — Research, Verification & Handoff

> **Status: THEORY VERIFIED (2026-06-04).** Branch `feat/rom-asset-probe`.
> The core claim — that we can stream MVC2 with a preloaded sprite atlas + a
> tiny per-frame state blob instead of the full TA video stream — is proven at
> every layer, including pixel-level. What remains is build-out, not risk.
>
> This doc is the pick-up point for the next agent. Read it, then skim the
> linked docs/code below.

---

## TL;DR

MapleCast currently streams **TA display-list commands + VRAM diffs** (~4 Mbps)
because the browser is an *asset-blind dumb renderer* — every texture it samples
must be shipped to it. The **ROM-asset client** inverts that: **preload each
character's sprite atlas once, then stream only the ~253-byte game state** the
server already extracts. The client draws `sprite[char_id][sprite_id]` at the
reported position. This is **[Option 6 in STREAMING-OPTIONS.md](STREAMING-OPTIONS.md)**,
now de-risked.

- **Bandwidth:** ~4 Mbps → **~5 KB/s** (~800×), grounded in measured data.
- **No GFX-codec RE needed.** The MVC2 sprite-graphics codec is *publicly
  unsolved* (see [reference_pldat_sprite_format] memory). We sidestep it: **the
  emulator decompresses the sprites for us**, and we bake its output.
- **Deterministic by construction:** the renderer is a pure function of the TA
  buffer (the [byte-perfect mirror](ARCHITECTURE.md#%EF%B8%8F-the-wire-is-deterministic-and-byte-perfect-commit-466d72d54)),
  and we proved `(char_id, sprite_id) → byte-identical pixels`.

---

## The thesis & how each layer was proven

| Claim | Evidence | Tooling |
|---|---|---|
| `sprite_id` (RAM +0x144) is a **bounded, reused** index | 90 unique for Ryu / 110 for Cable over 33s, ~20× reuse; full moveset ≈ few hundred/char | `MAPLECAST_STATELOG` + `scripts/analyze-statelog.py` |
| It's a **stable global** index (not animation-relative) | same `sprite_id` appears under up to 17 different `anim_state`s | same |
| It's **already streamed** — no derivation needed | `sprite_id` is in the 253-byte `GameState` (the GSTA broadcast) | `core/network/maplecast_gamestate.cpp` |
| `(char_id, sprite_id)` → **byte-identical pixels** | **Ryu idle = 100% stable: 6 sprite_ids at 25–91 occurrences each, 1 hash apiece** | `MAPLECAST_BAKE` / `web/webgpu/bake.mjs` |
| Renderer is deterministic from the TA buffer | the byte-perfect mirror (commit `466d72d54`) | `MAPLECAST_DUMP_TA` rig |

Bonus finding: multiple `sprite_id`s sometimes share the **same** pixel hash
(frame reuse) → the atlas can dedupe by hash and shrink further.

### Where the idea came from
The investigation started from `C:\Users\trist\Downloads\dasm_PLDAT\` (a
community disassembler for MVC2's per-character `PLxx_DAT` files). We fully
decoded the **sprite-assembly pipeline** (GFX offset table → EXTRAS part list →
palette) but hit the wall that the **GFX pixel codec is RLE-compressed and not
publicly cracked**. The pivot: don't decode the ROM bytes — let flycast render
them and capture the output, keyed by `sprite_id`. Full PLDAT format notes live
in the **`reference_pldat_sprite_format` memory**.

---

## The tooling (how verification works)

Two env-gated, read-only probes + their analyzers. Both are **inert unless the
env var is set** and neither touches the deterministic wire.

### 1. `MAPLECAST_STATELOG` — per-frame state probe (working-set + indexing)
- **Hook:** [core/network/maplecast_mirror.cpp](../core/network/maplecast_mirror.cpp) `serverPublish()`, before any wire work. Mirrors the existing `MAPLECAST_DUMP_TA` pattern.
- **What:** dumps `char_id, sprite_id, anim_state, anim_timer, screen_x/y, palette, anim_ptr` per active character per frame to a CSV.
- **Analyzer:** [scripts/analyze-statelog.py](../scripts/analyze-statelog.py) — working-set size, reuse, key cardinality.
- **Run:** set `MAPLECAST_STATELOG=<path.csv>`, run a match on the **headless** build, then `python scripts/analyze-statelog.py <path.csv>`.

### 2. `MAPLECAST_BAKE` — C++ pixel-capture probe (GPU build)
- **Hook:** [core/hw/pvr/Renderer_if.cpp](../core/hw/pvr/Renderer_if.cpp) `present()`, after the frame is drawn. Uses the backend-agnostic `Renderer::GetLastFrame()` (RGB top-down, same API as flycast's screenshot).
- **What:** per-slot fixed-box crop, FNV-hashed, logged with position/camera. Captures the full frame (no isolation) — superseded by the WebGPU bake for clean isolation, but kept as a single-process baseline.
- **Analyzer:** [scripts/bake_analyze.py](../scripts/bake_analyze.py) (groups by sprite+position+camera; `--png` converts saved crops).
- **Note:** norend/headless `GetLastFrame()` returns false, so this only does anything on the **GPU build** (`build/flycast.exe`).

### 3. `web/webgpu/bake.mjs` — WebGPU keyed sprite bake (the real harness) ⭐
This is what produced the 100%-stable verification and the clean atlas montage.

- Consumes the server's **GSTA** state broadcast (`'GSTA'` magic + 261-byte
  `GameState`; wire layout = `gamestate.cpp serialize()`). Wired into
  [web/webgpu-test.html](../web/webgpu-test.html): GSTA routed to `BAKE.onGSTA()`
  *before* the TA decoder; `BAKE.onRendered(canvas)` after each `renderFrame`.
- **Isolation:** Bake mode auto-sets **translucent-only** (`drawOpaque=false,
  drawPunch=false`, no `customBg`) → characters on a transparent background, no
  stage/HUD/floorZ-slivers.
- **Crop:** a region centered on the game-reported `screen_x/y`
  (`X/Y_Position_Screen`, RAM 0xE0/0xE4), alpha-tightened to the character.
- **Capture trigger:** **rendered-static** — only records when the crop is
  byte-identical for 3 consecutive frames (the character is visually *held*),
  one sample per held period. This sidesteps the GSTA(20Hz)↔TA(60Hz) skew and
  any animating residual: if anything in the crop moves, it doesn't capture.
- **Output:** live stability % in the panel; **Montage + CSV** button writes a
  contact-sheet PNG (the atlas) + `bake_manifest.csv`.
- **`?ws=ws://localhost:7200`** URL param connects the page straight to a local
  mirror server (skips relay + WebTransport). **`?v=N`** cache-busts the module.

#### How to run the WebGPU bake (local)
1. Start the server (+ a native client for input): `scripts/local-play.ps1`.
2. Serve the web dir: `cd web; python -m http.server 8080`.
3. Open Chrome → `http://localhost:8080/webgpu-test.html?ws=ws://localhost:7200`.
4. **Turn off the in-game hitbox/frame display** (training menu) — its overlay
   lines move every frame and contaminate crops.
5. Get into a training match, check **Bake mode** (auto-isolates), and let the
   character idle. Panel shows `hold=N` climbing + stability %. **Montage + CSV**.

---

## Confounds we hit (so the next agent doesn't re-learn them)

The character renders deterministically; everything *else* in a naive crop does
not. In rough order of how much they hurt:

1. **HUD** — health bars, super-meter bars, and the `2HIT/3HIT` combo counter
   animate and bleed into wide crops. Fixed by translucent-only isolation +
   position-crop, and by **not hitting the dummy** (idle/whiff keeps HUD static).
2. **In-game hitbox display** — thin overlay lines that change every frame. Must
   be turned OFF in the training menu.
3. **`floorZ`/customBg isolation leaves slivers** — a hard depth cut leaves thin
   boundary triangles. Translucent-only (a *list* cut) is cleaner.
4. **The shadow** — a translucent ellipse under the character survives isolation
   and sits at the bottom of the crop. Static while standing (fine), but shifts
   when the character's *height* changes (crouch/jump) → the 2 unstable crouch
   sprites in the verification run. **Crop out the bottom band to fix.**
5. **GSTA↔TA skew** — state is 20Hz, TA is 60Hz; a "settled" `sprite_id` doesn't
   guarantee the rendered frame matches during animation. The **rendered-static**
   trigger sidesteps this (only capture provably-still frames).
6. **WebGPU canvas alpha** — capture needs a transparent background
   (`alphaMode:'premultiplied'` + `clearAlpha=0`). `customBg` sets `clearAlpha=0`;
   translucent-only relies on nothing being drawn in empty areas.

---

## Next steps (build-out, phased)

Mirrors [BAKE-HARNESS-PLAN.md](BAKE-HARNESS-PLAN.md), updated with what's done.

- **[DONE] P0–P1** — isolation + pixel-stability verification. Ryu idle = 100%.
- **P1.5 — polish the bake:** crop out the shadow band; confirm capture during
  *moves* (the rendered-static gate already grabs held move-frames — startup/
  recovery holds; only 1–2-frame transients are missed and can be driven).
- **P2 — alpha quality:** translucent-only already gives transparent edges; if
  premultiplied edges look wrong for compositing, do black/white dual-render
  alpha recovery.
- **P3 — coverage:** script a character through its full moveset (inputs via the
  input-injection path or `.mcrec` replays), then all 56 characters → full atlas.
  Measure total size → decide preload-all vs lazy-per-character-in-match.
- **P4 — the client:** a JS/WebGPU renderer that preloads the atlas and consumes
  the 253-byte state, drawing `sprite[char_id][sprite_id]` by `facing`/
  `screen_x,y`/`palette`. `web/webgpu/bake.mjs` already has all the GSTA parsing
  and the renderer is right there to fork.
- **P5 — the killer end-to-end check:** render a recorded match from the atlas+
  state, diff against the **byte-perfect TA mirror**. If they match within
  tolerance, the ~800× bandwidth model is *proven*, not asserted.

### Stages / projectiles / supers
The atlas covers the **character layer only**. Stages, projectiles, super
flashes, hit sparks, and HUD are separate object systems (not in `PLxx_DAT`).
The **NaomiLib/PVR** path (e.g. **ModNao**, `github.com/rob2d/modnao`) is the
reference decoder for the *stage/model* layer if a full-scene client is ever
wanted. See the `reference_pldat_sprite_format` memory for the format split.

---

## Key files

| File | Role |
|---|---|
| [web/webgpu/bake.mjs](../web/webgpu/bake.mjs) | The bake harness (GSTA parse, isolation, rendered-static capture, atlas export) |
| [web/webgpu-test.html](../web/webgpu-test.html) | WebGPU renderer page; bake hooks + `?ws=`/`?v=` params + bake panel |
| [web/webgpu/pvr2-renderer.mjs](../web/webgpu/pvr2-renderer.mjs) | The renderer; `customBg`/`floorZ` isolation + `drawOpaque/Punch/Trans` list toggles |
| [core/network/maplecast_gamestate.cpp](../core/network/maplecast_gamestate.cpp) | The 253-byte state read/serialize (the GSTA payload) |
| [core/network/maplecast_mirror.cpp](../core/network/maplecast_mirror.cpp) | `serverPublish()` — GSTA broadcast (line ~1708) + `MAPLECAST_STATELOG` hook |
| [core/hw/pvr/Renderer_if.cpp](../core/hw/pvr/Renderer_if.cpp) | `present()` — `MAPLECAST_BAKE` hook |
| [scripts/analyze-statelog.py](../scripts/analyze-statelog.py) | working-set / indexing analysis |
| [scripts/bake_analyze.py](../scripts/bake_analyze.py) | C++-bake pixel-stability analysis |

## Branch commits (feat/rom-asset-probe)
`613f81385` statelog probe · `46517eb7a` bake plan · `e7dc3c89f` C++ bake hook ·
`b38e71a90` WebGPU bake · `cac77a4a8`→`475ad8528` isolation/crop/static-trigger
iterations. (`git log master..HEAD --oneline`.)

## Related docs & context
- [STREAMING-OPTIONS.md](STREAMING-OPTIONS.md) — Option 6 (this), and why TA mirror beats H.264
- [BAKE-HARNESS-PLAN.md](BAKE-HARNESS-PLAN.md) — the phased bake plan
- [ARCHITECTURE.md](ARCHITECTURE.md) — wire format, the byte-perfect determinism guarantee, the four parsers
- [MATCH-DATA-PLATFORM.md](MATCH-DATA-PLATFORM.md) — the Tele-0 per-frame state vision this shares plumbing with; hitbox/attack data also feeds it
- [WEBGPU-RENDERER.md](WEBGPU-RENDERER.md) — the renderer the bake/client builds on
- **Memory:** `reference_pldat_sprite_format` — full PLDAT decode, the unsolved GFX codec, ModNao, and the live-test result
- **External:** `C:\Users\trist\Downloads\dasm_PLDAT\` (PLDAT disassembler + PL00 extract) · `C:\Users\trist\Downloads\MvC2Data - *.csv` (community RAM maps; note `Animation_Value@0x144` = sprite_id "can be increased for sprite rips"; `Hitbox_Count@2C287DDE`, `DAT_HitboxData_PTR@0x170` for exact box crops later)
