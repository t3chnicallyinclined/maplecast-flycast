# MapleCast Render — Changelog

Chronological log of the whole-sprite reconstruction client. `?v=` = the `sprite-client.mjs` cache-bust in `webgpu-test.html`. Newest first.

## 2026-06-08 (session: Frame Oracle, RE cockpit, palettes-all-chars, Storm electric)

### THE FRAME ORACLE — live JIT hook on the production dynarec (the breakthrough)
- Proved the exact PC-hook can run **live** WITHOUT a GDB trap (which patches a trap opcode → recompile/trap/SMC = expensive + determinism-risky). Instead: a **compile-time block-entry `GenCall`** in `core/rec-x64/rec_x64.cpp` `BlockCompiler::compile()` (after `sub(rsp,STACK_ALIGN)`, before `regalloc.DoAlloc`) — one cheap native call per hooked block entry, **READ-ONLY** (reads `Sh4cntx.r[]` + `addrspace::read*`), determinism-safe, perf-trivial. `core/hw/sh4/dyna/decoder.cpp` forces a block boundary at hooked PCs mid-block. Gated `MAPLECAST_FRAME_ORACLE_HOOK` (default OFF → byte-stock).
- **PC-ALIAS:** MVC2 renders from P0 (`0x0C03093C`); marvelous2 labels are P1 (`0x8C03093C`); same low 28 bits. `mc_isHookedPC` masks `pc & 0x1FFFFFFF`.
- **`loc_8c03093c` confirmed = per-object PER-FRAME render** (150,541 fires / 22197-frame match). Reads `r4=node` → writes world→screen transform to **screen_x/y @+0xE0/+0xE4** (authoritative GPU-placement anchor) = OBJ_BEGIN.
- **`loc_8c033e90` confirmed = LOAD-TIME part-atlas DECODE**, NOT per-frame. Fires once at match load (frame ~2568); dumps ~1190 parts/char into decomp buffer `0x0CE60000` (texptr walks it, palptr=0, no screen coords). Dispatched from cell-processor jump table `loc_8c033d78`. Gated separately `MAPLECAST_FRAME_ORACLE_DECODE` (off). Useful as the part-atlas catalog, NOT placement. 16-byte internal quad: `w<<3@+0,h<<3@+2,attr@+4,texptr(r8)@+8,palptr(r12)@+C` (bank03 9275–9284, cursor r14). Display list lands in RAM ~`0x0C56xxxx` then bulk-DMA'd to TA — NOT the TA FIFO.
- **Per-frame SCREEN quads (the working capture):** recovered POST-walk from the parsed TA in `serverPublish` (`ta_parse`→`rc.verts`), attributed to the nearest OBJ_BEGIN object's authoritative `screen_xy` (160px). Output `/dev/shm/mc_oracle_hook.jsonl` per object: `node, sprite_id, screen_xy, scale, facing, tex_src, screen_quads[{x,y,w,h,u,v,z,vram_addr,tcw,fmt,tex_wh,blend}]`. In-match gated (`0x8C289624`). Validated: 1105 frames, 2173 objects, ~8.4 quads/obj, real x/y at object positions. (Attract demo = 2 bodies → ~18% attribution; full match populates more; unmatched → frame `unassigned[]`.)
- **Files:** `core/network/maplecast_oracle_hook.cpp/.h`, `core/rec-x64/rec_x64.cpp`, `core/hw/sh4/dyna/decoder.cpp`, `core/network/maplecast_mirror.cpp` (flush ~1762). Analyzers `_oracle/oracle_layers.py` (Z-cutoff), `oracle_live.py`, `oracle_attribute.py` (ROM-derived jsonl gitignored). Commits on `feat/state-replica-client`, latest `3899a16ac`. Prod binary md5 `b3a0eca914583d07e879c4582f3cc393`; stock backup `flycast.bak-20260608-193357` md5 `25cbc8aa…`. ENABLED on prod (gated OFF).

### RE COCKPIT — client-side ETL layer tool (`?v=4`, `pvr2-renderer.mjs ?v=4`)
- Toggle-able `window._reMode` panel. The browser already parses the TA (`FrameDecoder`/`PVR2Renderer`; `rc.verts {x,y,z,u,v}`, `PolyParam tcw`/blend) AND has GSTA objects. Classifies each TA poly by **Z-cutoff** (operator-validated ~0.0091 floor isolates chars from stage/bg — `pvr2-renderer.mjs` floor-cut; depth separates char from stage where size/blend/texture can't), **blend** (additive=EFFECT, `[4,5]`=CHARACTER), and **proximity** to GSTA objects.
- Shows layer breakdown, **Z-histogram** (drag the cutoff), per-object parts, **MISSES worklist**, layer-isolation toggles (`DBG.reHide` bitmask on the render).

### Storm electric effect now RENDERS (the long chase)
- Fixed by the effects-atlas reshape (`tools/reshape_fx_atlas.py` → `fx_atlas.json` `sprites{}` map, 17 effect sids) + OBJS effect-routing (`o.isEffect` → `FX_CID`, additive blend). Validates the EFFECT layer (additive `[4,1]`).

### UI — forced whole-sprite + hide OBJ-CALIBRATION (`?v=49`)
- asm/emitter/STAF toggles hidden; render hard-pinned to whole-sprite. **OBJ CALIBRATION panel hidden** (it covered the RE COCKPIT panel).

---

## 2026-06-08 (earlier: whole-sprite render, palettes, projectiles)

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
