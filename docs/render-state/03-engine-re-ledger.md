# RENDER-STATE appendix 03 — Engine-side render ledger (SH4 RE)

> Produced 2026-07-08 by the mvc2-sh4-re expert during the RENDER-STATE ledger sweep.
> Finding ids are `re_kb` (`tools/re_kb/NN_*.surql`); PCs are marvelous2 (`bankNN.asm`).

## 1. Char pass — scope of the Phase 2a driver (loc_8c030858 → retPc 0x8C039648)

**CONFIRMED (bank03.asm:22541-22559 + gsta_charpass.cpp):** `loc_8c039632` is the per-frame render-pass dispatcher — walks a pointer table (`loc_8c0396ac`) calling each pass via `jsr @r3` at 0x8C039644; RET_PC 0x8C039648 is that jsr's return. The Phase 2a run executes exactly ONE table entry: the full `loc_8c030858` char-pass routine.

**CONFIRMED driver contents (bank03.asm:1131-1196, re_kb/61):**
- `bsr loc_8c0308c2` **Render_sprites** — slot table 0x8C2895E0, cats 0–4 (0 = bodies, 1–4 = sprite-class satellites/projectiles/capes/sprite effects).
- The **3D/NaomiLib machine passes**: `loc_8c030cc0` + `loc_8c030d12` (gated on GGP+0x98), `loc_8c030d24` (lists 5/6), `loc_8c030d36` (lists 7/8), `loc_8c030d56` (list 9), `loc_8c030dcc` (list 0xB, + conditional `loc_8c030d68` walk of list 0xC head 0x8c287a8c → bank0f.loc_8c0f215e), plus special-mode tail `loc_8c031470` when `*(GGP)+0x2E==1`.
- **NOT in this driver:** lists 0xC/0xD full walks (`loc_8c0307ae`/`loc_8c0307d2`), the stage POL-tree dispatcher `loc_8c03223e` (re_kb/26), and the HUD root `loc_8c03012c` (re_kb/57). Separate render closures, exactly as 483511fef states.

**Nuance for the splicer (INFERRED):** the 37× cat11 stage set-piece nodes live on list 0xB (re_kb/61 census) — their draws are INSIDE the char-pass driver while the main STGxx POL tree is NOT. **Double-draw risk** if char-pass TA is spliced with `gsta_stage.cpp` output uncritically; needs a one-frame TCW-band audit before the Phase 2a-live splice.

## 2. The 3D object machine — INSIDE the Phase 2a TA; the re-implementation campaign is DEAD

- CONFIRMED: the 3D list walks (5–9, 0xB) execute inside `loc_8c030858`, so the single POL drawer `bank12.loc_8c129cc0` (re_kb/64) runs during the Phase 2a driver execution.
- CONFIRMED (gsta_charpass.cpp `h_sqCapture`): `doSqWrite` is hooked for the whole run and appends EVERY SQ flush — slot0 TA-direct (cls 0x10) AND slot2/4 deferred P2-RAM (cls 0xAC, the TR effects per re_kb/65). No class filter → the TR electric/hit effects the old P3D capture dropped are captured by construction.
- CONFIRMED (483511fef): byte gate closed — "the missing electric/hit effects are IN this TA."

**Therefore:** hitsparks (3D lists 7/8, re_kb/63) and the 5-node list-7 cast flashes (`loc_8c103ba4`) are covered by Phase 2a. Textures need no new work: TCWs are embedded in the Effect-Poly models (re_kb/64) and resolve against the shipped full 8MB VRAM prefix (re_kb/54).

**Stays OPEN under Phase 2a (483511fef NEXT):** deferred-parcel **splice order** vs OP list (capture is execution-ordered; the engine drains slot2/4 later — drain routine untraced, re_kb/64 OPEN, now only an ordering question); **native-parcel palette repoint** (re_kb/62 incl. its OPEN m-2 parity mechanism); **VRAM texture coherence**; the frozen live pixel gate.

**Supersessions:** `typhoon_hitspark_class_open` (61) → resolved by 63. `tr_effect_capture_fix_spec` (65) + commit 12bb53a57 (OP-prefix P3D injection) → superseded as render path by 483511fef; retain as capture/diagnostic tooling only.

## 3. HUD — Phase 2b, the one remaining engine closure

**Closure location (re_kb/57):** root `bank03.loc_8c03012c` → `bank0f.loc_8c0f048e` → 6× `loc_8c0f04c4` (panel: `loc_8c0f05f4` portrait, `loc_8c0f0824` name plate, `loc_8c0f08b0`, `loc_8c0f0a1a`) + 2× `loc_8c0f06ec` (life bars) + `loc_8c0f0fdc` (segmented meter, METER_MAX=144 @ loc_8C0F113C, wave fn `bank11.loc_8c11E860`); allocator `bank04.loc_8c044f12` (list 0x0B); emit `bank12.loc_8c1294c8/bc`; ROM tables `bank15.loc_8c15FFB0` (team gouraud, exact bytes in `source:hud_team_colour_table`) + `bank16.loc_8c1601C0/loc_8c160220`. Counters 0x8C289BD0..BD6 advanced by `loc_8c03015c`.

**PROVEN byte-matched (P2a):** re_kb/59 — life fill / chip / P2 fill / meter fill / baked portrait all PASS vs the HUDQ oracle (chip pcw 0x808c002d tsp 0x20080440; exact 0xFE-alpha loc_8c15FFB0 stops). Predicate re_kb/58: **loc_8c0f06ec writes NO x/y/uv** (fills a model record; corners come from the downstream transform) — a bar transpile was correctly rejected. Portraits: CLOSED authoritative via `loc_8c161fec` col-major table + `loc_8c13b7d4` inverse + DM01POL UVs (re_kb/35).

**Supersession chain:** 27 flat → angled-layout → real-art → garble-fix → real_ta/HUDQ (34) → [36 garble sub-chain: detwiddle claim REFUTED; para5 capture-is-garbage supersedes the 36f re-tile recipe] → re_kb/54 verdict + 55: **HUDQ pass-through DROPPED as renderer; HUDQ retained only as the byte-gate ORACLE (57)**.

**OPEN for Phase 2b:**
- The HUD closure itself un-run/un-transpiled — Phase 2b = run it the char-pass way.
- `hud_meter_segments_vram_gap` (36): fmt=5 PAL4 meter-segment art VRAM 0x440000–0x460000 ZEROED in the shipped prefix — server prefix coverage gap, unfixed.
- `hud_para5_expansion_capture_fix` (36g): implemented_pending_live_verify (run `hud_plane_conformance.mjs`, expect ~32/32). Only matters if names stay HUDQ-sourced.
- Meter breathing counters 0x8C289BD0..BD6 not on wire (trivial 8B or client-reproduce).
- **CONFLICT to fix in the ledger:** re_kb/33 (grounded, Oracle-read) says win count = `char+0x540 num_wins`, already inside the shipped char_str region; re_kb/55's "win-stars UNDRAWN, missing wire field" clause is stale — mark superseded by 33. Win stars are drawable today.
- `replica_live_hud_real_ta_open_multistrip` (34) + `hud_meter_capture_completeness` (36) — moot if HUDQ becomes oracle-only; close-as-obsolete when Phase 2b lands.

## 4. Stage / background

**PROVEN:** transform = XMTRX = M1(0x8C2D6B18)·M2(0x8C2D6AD8), same as bodies (26b). Control words + textures grounded in engine TA (26d/26c-deck; 99.989% raster identity; deck TCW 0x0809FC00 vertex-type-5 colour fix). Props: 26e per-node matrices was REOPENED by 31 (cross-frame re-projection explodes) and re-closed by the per-mesh world-authored gate (31). Native client: OP/EOL/TR z-order (45, commit ed11835b2); floor MARGIN-cull fix (47).

**OPEN:** only STG0B has an engine-TA bake; every other stage_id falls to POL-rip deck-only (31/45). Work = per-stage live capture (MAPLECAST_DUMP_RAM) + `bake_stage_from_ta.py` + complete the stage_id→STGxx map (only 0x11→STG0B confirmed; 0x0B works by coincidence). Under Phase 2a: the list-0xB set-piece overlap (§1).

## 5. Camera + remaining wire-field gaps

- **Camera (39):** zoom/pan enters exclusively through M2 (rebuilt per frame by `loc_8c1216c0` from 0x8C2D6918/0x8C2D69D8); M1 constant viewport; cam_mat ships both; body recompute 4.3e-5px max vs +0xE0/E4 over 1000 frames. OPEN (validation-only): live moving-match witness never captured.
- **33:** hit-flash = palette swap via char+0x12E → `loc_8c035162`/`loc_8c035000` → PALETTE_RAM; rides the on-change pvr tail. Wire adds shipped: game-mode block 0x8C26828C, battle state 0x8C2895F0, win count char+0x540.
- **51/53:** `gsta_motion_blocks_body_idxtab` FIXED (0x85xxx cull). `gsta_super_flash_paramtype_gap` (53) SUPERSEDED by 54: there is NO engine fullscreen flash quad — bigwhite was our own reconstruction garbling. Still nominally OPEN: `gsta_body_part_transposition` (51) and `gsta_motion_blocks_residual_bands` (53) — **both render_frame-reconstruction defects, mooted under the Phase 2a flag**; mark "obsolete-if-2a-default".

## 6. GSTA wire read-set — PROVEN state

- **Complete for fixed-address resident state:** re_kb/25 — stale-seed universe ZERO bytes outside the 15 dyn regions across two live captures; every chased pointer covered.
- **Sufficient for the REAL driver:** 483511fef --drop-scratch — arena/tiledesc/idxtab/rectab = genuine driver-entry inputs (588 misses when trimmed); charpass runs in-place on `_gstaRam` and closes the byte gate. Entry context: embedded 512B `kEntryCtx` proven frame-invariant for the tested frames (CONFIRMED for those; INFERRED for all).
- **On-change GFX chain:** 18 static-once gap → 19 true-extent `{base,len}` tail → 32 Storm-scramble (GFX2 self-modifies per anim sub-frame ⇒ fresh ship) → 50 super-effect overtile RESOLVED (objpool GFX + arena coherence). 20: local-ROM overlay correct as mechanism; the residual scramble was the tile-storage decode model, CLOSED (whole-part contiguous +0x200 stepping).
- **Palette:** on-change 32KB pvr tail (25b) — carries hit-flash for free (33).
- **Still OPEN on the wire:** `replica_live_satellite_gfx_residency` (29) — `collectFreshGfx` skips cat 1–4 nodes (only cat==0 bodies get fresh GFX); never yet triggered live but unfixed. Plus the Phase 2a-live items (§2).

## Dead campaigns — do NOT resume

1. **Client 3D-machine re-implementation** — 61's discriminator plan, 63's gate-consequence (c), 64's transpile-prep (`loc_8c129cc0` port). Dead: the real driver emits it inside the Phase 2a TA, byte-gated. 61/63/64/65 remain valid as grounding/diagnostics.
2. **P3D capture + OP-prefix injection** (65, commit 12bb53a57, MAPLECAST_P3D_CLS widen) — replaced for the char pass by the charpass TA. Keep only as measurement tool.
3. **render_frame/gstaDecodeBodies hand-assembly patch stack** — 0x85xxx cull (51), bit15 scale-walker index repair + idxtab post-pass remap (50), residual-band cull plans (53) — replaced under the flag; retained only while the flag defaults OFF.
4. **HUDQ-as-renderer** (34 + the 36 client-decode saga) — dropped per 54/55; HUDQ survives strictly as the Phase 2b byte-gate oracle (57/58/59). Don't debug HUDQ rendering further.
5. **HUD-routine transpile (gen_hud_*.c plan in 57)** — falsified for the bars by 58 (`loc_8c0f06ec` is coordinate-less); Phase 2b should run the real closure, not transpile leaf builders.
6. **Effects-atlas / effect-binding heuristics** (40, 10_replica_live_effects) — sprite-class effects proven cat1–4 body-walker objects (63); 3D-class effects ride the char-pass TA; atlas-key guessing obsolete.
7. **BTCW per-frame entry→palsel map hook** (62's proposed fix) — under Phase 2a the driver executes the palsel patch stores itself; 62's value is explaining the arena snapshot's palsel carry (m-2 parity stays open only if a live discrepancy resurfaces).
8. **Per-prop NaomiLib matrix-stack capture** (26 deferred recipe) — closed by TA world un-projection (26e) + the 31 per-mesh gate; never capture the matrix stack.

Key sources: `core/network/gsta_charpass.cpp`, `tools/re_kb/{61,63,64,65,57,58,59,31,25,26,33,39,50,51,53,54,55,62}_*.surql`, `_marv_re/build/bank03.asm:1131-1196, 22541-22559`.
