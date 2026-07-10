# RENDER-STATE.md — the canonical render ledger

> **READ THIS FIRST before touching anything render-related.** This file exists because the
> project repeatedly lost built knowledge: 5+ parallel render implementations, design docs that
> were never tombstoned when superseded, and gates that closed offline while live assembly
> regressed. Every rebuild session must start here, not in the design docs.
>
> Produced 2026-07-08 by a five-expert ledger sweep + a live Phase 2a A/B on the local rig.
> Detail appendices (full expert reports, verbatim): `docs/render-state/01..05`.
> Update discipline: when an implementation is superseded, update §1/§2 AND stamp the loser's
> doc with a 🪦 header (pattern + paste-ready headers in `render-state/04-docs-audit.md`).

---

## 1. The authority ranking (2026-07-08)

Render paths for the MVC2 stream, ranked. Any new session starts at the top.

| Rank | Path | Wire | Gate status | Live status |
|---|---|---|---|---|
| **1** | **Lockstep-mirror client** — client runs full local SH4+ROM from the input/checksum wire (`core/network/maplecast_lockstep.{h,cpp}`, `MAPLECAST_LOCKSTEP=1`, commit `bc16af338`) | **~2.4 KB/s** | bit-exact, **live-proven 9121/9121 checksums** | ACTIVE arc (predict STAGE 0–c + CAPSTONE `3cf92ca83..3cf861b33`, `MAPLECAST_PREDICT_LIVE=1`). Declared successor to the whole render-reconstruction stack. Requires ROM+SH4 on client. |
| **2** | **Phase 2a native char-pass** — MVC2's real render driver run in-process on streamed `_gstaRam` (`gsta_charpass.cpp`, `MAPLECAST_GSTA_NATIVE_CHARPASS=1`, commit `483511fef`) | GSTA ~7 KB/frame | TA byte gate CLOSED offline (md5 == engine, 4519 parcels) | **SHELVED** by `bc16af338` ("superseded by lockstep") mid-arc. First live run 2026-07-08: TA runs in budget (0.95–4.1 ms) but **bodies render invisible** — pixel-side (palette repoint / decode-follows-native / splice) unfinished. Blockers G1–G9 in `render-state/02`. Fallback frontier if lockstep is rejected (thin clients). |
| **3** | **render_frame transpile + lockstep decoders** — the DEFAULT GSTA client render (gsta_render_frame.c + gstaDecodeBodies / body_decoder.mjs) | GSTA ~7 KB/frame | sprite machine CLOSED 5/5 byte-exact vs mirror on out-of-sample capture (`189544592`) | Shipping default (flag off). Known residual opens: body-part transposition (re_kb/51), 3D-machine injection residual ~2%, HUD/stage on separate paths. |
| **4** | **TA mirror** — raw TA delta + dirty pages (`serverPublish`) | **~4.1 Mbps in-match** (zstd; ~12 raw; ~900 Kbps idle) | byte-perfect deterministic wire (466d72d54); THE ground truth for every gate | Production for browsers/spectators. No ROM needed. |
| **5** | Whole-sprite atlas client (`sprite-client.mjs buildDrawList`) | ~15 KB/s class | approximation BY DESIGN (RGB bake, tint flash) | Prod lean path. Never byte-exact; don't use for fidelity work. |
| — | Emitter part-assembly (`buildEmitterDrawList`) | — | geometry 0.00px, **RETIRED as drawer** (`364f9ce1e`) | Browser replay.html default is STILL 'emitter' (replay.html:1811) — standing decision to flip to render_frame is OPEN. |
| — | StafGL, EFCT/TX64/STAF texture channels, VCACHE, HUDQ-as-renderer, live-SH4 state injection | — | — | **DEAD.** Tombstones + reasons in `render-state/01/03/04`. |

**The one-sentence strategic state:** lockstep (input-wire, bit-exact, 2.4 KB/s) won for ROM-holding native clients; the GSTA reconstruction stack (ranks 2–3) remains the path for ROM-less/thin clients; the TA mirror remains the browser/spectator wire and the universal ground truth.

## 2. Subsystem ledger (GSTA reconstruction stack)

| Subsystem | Authoritative impl | Proven | Open |
|---|---|---|---|
| Bodies | rank-2/3 above | texels 100% byte-exact; geometry byte-exact vs ASMTRACE; 9/9 params; **2026-07-09: frame-exact gameplay gates 5664/5664 (band2) + 4800/4800 (band4) node-frames clean (_tx_detect)** | 2a pixel-side; palsel root fix; ~~part transposition (51)~~ RESOLVED 2026-07-09 — TWO mechanisms: sel==0xFF blank-record desc-slot compaction (re_kb/66) + wide-part COLUMN-PAIR-MAJOR tile order (re_kb/68, Sentinel 4×4 block swaps) |
| Satellites/projectiles | gen_render_satellite.c + slot-table walk | in the 5/5 close; <0.005px | ~~satellite GFX residency (re_kb/29)~~ FIXED 2026-07-09 — objpool 0x8C26AA54+0x1D000 in the per-frame read-set, deployed prod (HANDOFF-SATELLITE-READSET) |
| 3D machine (sparks/flashes) | INSIDE the 2a native TA by construction (re_kb/61-65) | byte-gated via 2a | splice order of deferred cls-0xAC parcels; **P3D double-draw when 2a is on (G1)** |
| HUD | Phase 2b — separate closure `loc_8c03012c` (re_kb/57) | bars/portraits byte-matched vs HUDQ oracle (58/59) | run the real closure 2a-style; meter-segment VRAM prefix gap (36); win-stars drawable NOW from char+0x540 (33 supersedes 55) |
| Stage | gsta_stage.cpp + STG0B engine-TA bake | z-order + floor cull fixed (45/47); **2026-07-09: bake HUD-contamination purged (re_kb/67)** — the bake shipped a frozen captured HUD (68/72 meshes) that depth-rejected the live HUD; bake tool now filters HUD_TEX_WORDS | only STG0B baked; stage_id→STGxx map; list-0xB set-piece double-draw audit vs 2a |
| Textures/palettes | native-chunk carve key = **twTileYFirst** (whole-part Y-first tile-twiddle; re_kb/70 CORRECTED — colPairChunk was 4×4-valid but broke 4×8/8×8, roster-gated) + on-change PVR tail | **0 bad / 21312 tiles across all 59 chars** (_zz_roster_carve_gate); 0.00% palette-bank mismatch | SH4 confirm of the live 4×8/8×8 TCW order (mvc2-sh4-re-expert); **VCACHE ref-page misses = permanent stale VRAM on wire path** (frame-decoder.mjs:171; bodytex=local bypasses it) |
| Camera | cam_mat M1·M2 (re_kb/39) | 4.3e-5px over 1000 frames | live moving-match witness (validation only) |
| Wire read-set | 16MB seed + 15 dyn regions + GFX/palette tails | **complete: stale universe = 0 bytes (re_kb/25); drop-scratch falsified trimming** | on-change GFX for cat 1–4 satellites (29) |

## 3. Bandwidth numbers — pinned (cite ONLY these)

- **TA mirror:** ~4.1 Mbps in-match zstd / ~12 Mbps uncompressed / ~900 Kbps idle (ARCHITECTURE.md §compression table, Apr 2026). **2026-07-08 re-measurement on feat/render-replica-live: 6.875 Mbps in-match** (+1.08 Mbps side-channel) — the growth is dominated by DMA force-dirty page re-ships (56.9% of shipped pages byte-identical); split + sub-1-Mbps plan in `render-state/07-bandwidth-lab-results.md` (measured best stack: **0.788 Mbps @ 0.23 ms/frame**).
- **GSTA state wire:** ~7 KB/frame (≈3.4 Mbps at 60fps; PHASE-B shrink never executed).
- **Lockstep wire:** ~2.4 KB/s (bc16af338).
- The "~1.7 MB/s" figure in Jun-7-era docs = unlabeled/uncompressed, do not cite. The "36–88 Mbps" figure = unrecovered measurement conditions, do not cite without re-measuring.

## 4. Gates (details + exact commands: `render-state/05`)

- **THE acceptance gate for any live render claim:** frozen-frame pixel A/B — savestate_save via 7211 + AUTOLOAD restart + `MAPLECAST_GSTA_SHOT` consecutive frames on both legs + `diff_png.mjs` with bounding boxes. Numbers, never impressions.
- Standing: charpass selftest (G11), READTRACE seed + realcore runner w/ negative controls (G9/G10), determinism rig (G12), texel_gate (G2), wire audits (G5/G6/G7), dual recorder (G14).
- **Missing tooling (blocks claims):** `gate_check.mjs` scorecard NOT in tree (re-land it); no live per-frame TA md5 (the live `[charpass] ... byte-exact` log line is an ASSERTION, not a measurement); `_run_client_shot.bat` broken as committed; diff_png has no region mask.

## 5. Live A/B record — Phase 2a, 2026-07-08 (first ever)

Local rig (`_run_srv_gsta.bat` + `_run_gsta_client_native.bat` / `_run_gsta_client_ctrl.bat`):
- Flag ON: native driver engaged every frame, embedded ctx, 0.95–4.1 ms/frame, 135–4540 parcels. **Bodies invisible** (stage/HUD/electric effect render). `decode-follows-native: 0/N redirected` most frames.
- Flag OFF control: **bodies render fully and correctly.**
- Conclusion: 2a's remaining gap is exactly the declared pixel-side (G2 live composite coherence, decode-follows-native correspondence, palette repoint, splice order) — not the TA computation.
- Rig gotcha reproduced twice: **headless server crashes when its GSTA client disconnects** — restart server between client swaps (re_kb/52 class).

## 6. Dead campaigns — do NOT resume (full list + citations: `render-state/03`)

Client 3D-machine re-implementation · P3D capture/OP-prefix injection as a render path · the render_frame hand-assembly patch stack (once 2a/lockstep defaults) · HUDQ-as-renderer · HUD leaf-transpile (bars are coordinate-less) · effects-atlas binding heuristics · BTCW palsel map hook · per-prop matrix-stack capture · live-SH4 state injection · TX64/STAF/VCACHE pixel-shipping · whole-frame TA dedup (Pivot A, 0% hits).

## 7. Solved-bug catalogs

Do not re-solve: facing XOR 0x4000 · full-span vs logical-crop · pen-origin reflection · bit15=scale-dispatch · 2-row-band carve (FINAL) · sprite base color +16 · rectab PalSelect preserve · purple-Cable-is-correct · +0x12C visibility gate · OP-prefix injection legality · desc-keyed carve · char-pass (not HUD-pass) snapshot timing · **sel==0xFF blank records CONSUME desc slots** (engine allocates formula slots + suppresses only the draw; never skip them in a desc rebuild — re_kb/66) · the eight mirror-wire bugs (ARCHITECTURE.md). Full catalogs with file:line: `render-state/01`.
**Diff-tooling gotchas (2026-07-09):** ASMTRACE screen anchor = quad LEFT-edge X or RIGHT-edge X depending on facing (Y = quad max-y always) — compare corner-aware or every moving part shows a phantom width×(5/3) offset (`_diff_sweep` has this artifact; `_tx_detect` is corner-aware). ASMTRACE dies silently when /dev/shm fills (mid-line truncation) — check `df /dev/shm` before trusting a "missing" trace window.

## 8. Doc status

Verdict table for every doc in docs/ + paste-ready tombstone headers: `render-state/04-docs-audit.md`.
Urgent three (actively steering sessions wrong): ROLLBACK-SHELVED.md (un-shelve condition triggered — the predict arc is ACTIVE), ASSEMBLY-DRIVEN-DESIGN.md (disproven blocker stated as fact), the bandwidth-triad docs (§3).
