# Sprite Bake Harness — Plan & Verification

> Branch `feat/rom-asset-probe`. Follows the live state-probe that validated the
> ROM-asset client at the RAM level (see docs/MATCH-DATA-PLATFORM.md Tele-0 and
> the `MAPLECAST_STATELOG` probe in `serverPublish`).

## Goal

Produce — and **verify** — a stable mapping `(char_id, sprite_id) → composited
character image (RGBA)`. This single artifact:

1. **Proves the last unproven theory link** ("Layer B"): that a given
   `(char_id, sprite_id)` renders identical pixels every time it occurs.
2. **Is the client asset pack** the ROM-asset renderer preloads.

We use **the emulator as the decompressor** — flycast already decodes MVC2's
sprites to pixels deterministically, so we never touch the unsolved GFX RLE
codec. We capture what the game draws, keyed by the `sprite_id` it reports.

## What we already proved (don't re-litigate)

- `sprite_id` (RAM +0x144) is a **bounded, stable, global per-character sprite
  index** — 90 unique for Ryu / 110 for Cable over 33s, ~20× reuse, same
  `sprite_id` reused across many `anim_states`.
- It is **streamed directly** in the 253-byte blob — the client does not derive
  it. Render model: `draw sprite[char_id][sprite_id]` flipped by `facing`,
  positioned by `screen_x/y`, colored by `palette`.
- Bandwidth target: ~80 B/frame ≈ 5 KB/s vs ~4 Mbps today (~800×).

## Host & hook

- **Host:** the standard GPU build `build/flycast.exe` running the ROM directly
  (NOT mirror-client mode). Full SH4 → `maplecast_gamestate::readGameState()`
  gives `sprite_id`; the GL renderer gives pixels. One process.
- **Hook:** `core/hw/pvr/Renderer_if.cpp::render()`, gated by `MAPLECAST_BAKE`,
  placed AFTER `renderer->Render()` so the framebuffer holds the drawn frame.
  Same call site family as the existing `serverPublish` / `MAPLECAST_DUMP_TA`
  hooks (lines ~217-240) — proven-safe location.

## The four sub-problems

| # | Problem | Status / approach |
|---|---------|-------------------|
| 1 | **KEY** — get `(char_id, sprite_id, facing, palette, screen_x/y)` per active char/frame | **SOLVED.** `readGameState()`, already validated by the probe. |
| 2 | **CAPTURE** — get pixels each frame | flycast has `glReadPixels`/screenshot paths in `core/rend/gles/`. Read the framebuffer (or an offscreen FBO) post-Render. |
| 3 | **ISOLATE** — separate the character from stage + HUD, with alpha | **The hard one.** See below. Deferred past P0. |
| 4 | **DRIVE** — make the game display every `sprite_id` | organic play + scripted move inputs; training mode for clean backgrounds; `.mcrec` replays for repeatability. See below. |

### Sub-problem 3 (ISOLATE) — options, ranked

Characters are translucent textured polygons over an opaque stage, plus a HUD
(per docs/ARCHITECTURE.md). Candidate isolation methods:

- **(a) Translucent-pass-only render to a cleared transparent FBO + crop by
  `screen_x/y` bbox.** Most promising. Excludes the opaque stage; the bbox crop
  excludes the fixed-position HUD. *Uncertainties to verify empirically:* which
  pass(es) the character lands in, and whether shadows / super flashes / the
  other character contaminate. Flag, don't assume.
- **(b) Dual-background alpha recovery** — render the char over black and over
  white, recover true alpha (`alpha = 1-(C_white-C_black)`). Gold standard for
  translucent edges. Needs background suppression; pairs well with (a).
- **(c) Flat-background force** via RAM/stage write, then chroma-key. Fragile;
  fallback only.

**Decision: defer all of this to P1.** P0 verification does NOT need isolation
(see below), so we prove the theory first and only pay for isolation once the
core mapping is confirmed.

### Sub-problem 4 (DRIVE) — coverage strategy

- **P0/P1:** organic + scripted play through one character's moveset. Good
  enough to hit the ~hundreds of `sprite_id`s a character actually uses.
- **P3 (full roster):** scripted input macros per character (every normal /
  special / super / getup), driven via the existing input-injection path, or
  RAM-forced animation stepping if the engine cooperates. `.mcrec` replays make
  runs repeatable for regression.

## Phases

### P0 — Verify Layer B (cheapest, no isolation) ← do this first
**Question:** does the same `(char_id, sprite_id)` produce identical pixels?
**Method that sidesteps isolation:** training mode, one character, **fixed
position + static background**, capture a full-frame crop at the character bbox
during a **looping idle/animation**. The same `sprite_id` recurs at the same
position over the same background, so the crops must be **byte-identical**.
**Pass:** ≥2 occurrences of each looped `sprite_id` → identical crop hashes.
**Build:** `MAPLECAST_BAKE` hook that, per frame, reads `sprite_id` and writes
`bake/<char>/<sprite_id>__<frameseq>.png` (+ a hash). Analyzer groups by
`sprite_id` and checks intra-group hash equality. Reuses statelog patterns.

### P1 — Single-character bake with isolation
Add translucent-FBO isolation (3a). Produce a Ryu atlas `sprite_id → RGBA PNG`.
**Pass:** captured `sprite_id` set ⊇ the statelog `sprite_id` set for Ryu, and
each `sprite_id` resolves to one stable image. Empirically resolve the 3a
uncertainties (pass selection, contamination).

### P2 — Alpha quality
If 3a edges are dirty, add dual-background recovery (3b). Clean, premultiplied
alpha suitable for compositing.

### P3 — Full roster automation
Drive all 56 characters through full movesets; bake the complete atlas. Measure
total size (informs preload-all vs lazy-per-character-in-match).

### P4 — ROM-asset client
JS/WebGPU renderer: preload atlas, consume the existing 253-byte state, draw
`sprite[char_id][sprite_id]` by `facing`/`screen_x,y`/`palette`.

## End-to-end verification (the killer check)

We have a **byte-perfect TA-mirror reference** (commit 466d72d54). So once P4
exists we can render a recorded match two ways — (1) the ground-truth mirror,
(2) the ROM-asset client from atlas + 253B state — and **diff them
quantitatively**. That closes the entire theory: if the ROM-asset render matches
the mirror within tolerance, the ~800× bandwidth model is proven, not asserted.

## Open decisions (resolve as we hit them)

1. Atlas granularity: composited-frame-per-`sprite_id` (chosen — matches the
   validated model) vs piece atlas + assembly (rejected — needs the unreliable
   PLDAT assembly decode).
2. Isolation method final choice — pending P1 empirical results.
3. Preload-everything vs lazy-per-character — pending P3 size numbers.
4. Multi-sprite characters: `sprite_id` is one value/char (the engine expands it
   into parts internally), so we bake the composited result — no per-part logic.
