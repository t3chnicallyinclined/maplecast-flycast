# MapleCast Render — Changelog

Chronological log of the whole-sprite reconstruction client. `?v=` = the `sprite-client.mjs` cache-bust in `webgpu-test.html`. Newest first.

## 2026-06-08 (session: whole-sprite render, palettes, projectiles)

### `?v=54` — Object center-anchor + live mode switch
- PATH A's `node+0x178` hotspot proven **degenerate** via `[OBJDIAG]` (constant per-char = body value, or `hasHot=false`). Switched satellite objects to a **center** anchor (`-wG/2,-hG/2`) by default.
- Added `window._objAnchor = 'center' | 'bottom' | 'baked' | 'hot'` for live A/B comparison.
- Object draw no longer trusts `hotDx/hotDy`. (TEMP `[OBJDIAG]` log still present — remove when anchor is settled.)

### `?v=53` — OBJDIAG diagnostic
- Added a one-shot-per-object console log (`cid/sid/hasHot/hot/baked/scr/wh`) to read the real anchor numbers instead of guessing.

### `?v=52` — White-wash fix
- `_applyBodyFx` flash was keyed off `char+0x12d/0x12e` (palette selectors, nonzero in normal play) and overlay off `+0x1a4` (nonzero for normal classes) → every body washed white/blue every frame, masking real hits.
- Flash now fires only on the validated hp-drop window (`sl._flashUntil`); `overlayOn=false`. Atlas confirmed byte-identical to source (not the cause).

### `?v=51` — PATH A: server-side object hotspot (later found degenerate)
- Server (`readAllDrawn`) reads each drawn object's `node+0x178` (Sprite_Extras) → EXTRAS bbox `(min dx, min dy)` → ships `hotDx/hotDy` (s8) per object.
- **OBJS browser wire 9 → 11 B/rec; OBJF replica wire 11 → 13 B/rec.** All 4 parsers updated (`gamestate.cpp/.h`, `maplecast_mirror.cpp`, `sprite-client.mjs onOBJS` auto-detects stride; `frame-decoder.mjs`/wasm/`relay` pass-through).
- Headless rebuilt + deployed on the box (binary md5 `10528f51…` → `6b56560243…`). Commit `2b51eac50` on `feat/state-replica-client`.
- ⚠️ Hotspot is degenerate (body value) — wire shipped but client ignores it as of `?v=54`.

### Palette batch (all 59 chars) — exact indexed palettes
- Generated `PLxx_idx.png` + `PLxx_lut.json` for PL00–PL3A via `tools/rgb_to_indexed.py`. 56 chars pixel-identical (0.0000%); PL05 ~4%, PL1B ~1% (shared-effects colors, nearest-color fallback). Cleared the `_idx/_lut` 404 flood roster-wide.

### `?v=50` + `sprite-gpu.mjs ?v=10` — Exact palettes (indexed + LUT) [#2]
- RGB atlas → indexed (palette-index map) + multi-bank LUT; LUT shader path in `sprite-gpu.mjs` (RGB path kept as fallback). Ryu verified pixel-identical.
- Hit-flash = exact hurt-bank swap from the live PVR palette; skins hooked (`setSkin`). `tools/rgb_to_indexed.py` added.

### Effects atlas reshape [#1]
- `tools/reshape_fx_atlas.py`: `fx_atlas.json` `effects[]` → `sprites{}` map (17 sid keys). Effects now resolve + draw additively over chars. Data-only fix (no client change). Coarse — generic by sid; type-routing is a follow-up.

### `?v=49` — Force whole-sprite + hide parked toggles
- Render tick hard-pins `asm=false/staf=false` → always `buildDrawList`. Hid assembly/emitter/STAF checkboxes + ASM-calibration panel (`#sc_parked`). Fixes the flaky emitter toggle (atlas-reload race) by making it inert.

### `?v=48` — Default whole-sprite + console cleanup
- Default emitter/assembly OFF (parked path was garbling). Skip sentinel object sids (`0x7fff/0xffff/0`); throttle `[effects-miss]` to 1/5s.

### `?v=47` — Object calibration tunable + roster export
- `window._objCfg{dx,dy,scale}` + OBJ-CALIBRATION sliders (residual object nudge). `bake.mjs downloadCharAtlases()` for per-char roster export.

### `?v=46` + `sprite-gpu.mjs ?v=9` — Whole-sprite field build-out
- Applied GSTA enrich to `buildDrawList`: per-char scale (exact), OBJS effect-routing to FX atlas, satellite objects; hit-flash/overlay as additive tints (later corrected in `?v=52`). `sprite-gpu.mjs` gained a `tint` instance attr.

### Infrastructure / RE (this session)
- **DAT decoder fixed** — `tools/rip_pldat_segments.py` (repo `PLxx_DAT.BIN` are stripped PAKs; real anim = `PLxx_TBL.BIN`).
- **EXTRAS decode** — `tools/rip_extras_hotspot.py` (bytes verified vs PL2A; sid→assembly index unsolved).
- **Object screen offset reconciled** — confirmed `+0xE0/+0xE4` (not `+0xC8/+0xCC`); `re-catalog/00-README.md` updated.
- **Expert agent** — `.claude/agents/mvc2-sh4-re-expert.md` now anchors on `re-catalog/` + the one-char-at-a-time method.

### Dead ends (parked — see HANDOFF)
- Part-assembly/emitter port → LZSS shared-scratch-buffer (unrecoverable offline/live).
- Offline EXTRAS hotspot → `sid→assembly` index unsolved.
- PATH A `node+0x178` → degenerate (body value).
