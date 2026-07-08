# RENDER-STATE appendix 05 — Gate catalog + Phase 2a live A/B protocol

> Produced 2026-07-08 by the gsta-verification-harness expert during the RENDER-STATE ledger sweep.
> CONFIRMED = verified in tree/logs that day. INFERRED = commit message / memory record only.
> `<R>` = repo root `c:\Users\trist\projects\maplecast-flycast\`.

---

## 1. GATE CATALOG

### G1. Transpile-path offline chain (run.cmd steps 1–8)
- **File:** `<R>\tools\render-replica-poc\run.cmd` (CONFIRMED read)
- **Compares:** transpiled SH4 render (gen_leaf/gen_walker/gen_render_object/render_frame) outputs — leaf FP values, per-tile screenX/Y, per-object PCW/ISP/TSP/TCW, whole-frame body TA — against ASMTRACE, engine TA corners, and engine PNG via `render_ta.mjs` + `diff_png.mjs --tol 0`.
- **Ground truth:** OFFLINE frozen prod capture `<R>\_ryu_capture\mc_ram_dump.bin` / `mc_vram_dump.bin` / `mc_pvr_regs.bin`. **Offline.**
- **Last known:** README "GO" table: leaf 21/21 bit-exact; transform 18/18; walker numeric 9/9 tiles 0.00px w/ negative control; Phase-1 9/9 params byte-exact + 0 diff pixels.
- **Command:** `<R>\tools\render-replica-poc\run.cmd` — Windows-runnable (auto-vcvars64, needs Python3+numpy+node).
- **Status:** STANDING for the transpile fallback path.

### G2. texel_gate.cpp — body-texture staging byte gate
- **File:** `<R>\tools\render-replica-poc\texel_gate.cpp` (incl. TEXFIX2 desc-keyed carve)
- **Compares:** (1) CERTIFY: replica-staged 512KB VRAM band [0x400000,0x480000) vs LIVE client dump `gsta_vram_<vf>.bin`; (2) GATE: per scene quad, detwiddled staged tile vs ENGINE mirror band (+parity twin ±0x30000), sampled-window only, classes EXACT/WRONG/ZERO/BOTHZERO per palette; (3) LZSS undershoot diag. Env diagnostics `TEXMAP`/`TEXDESC`/`TEXDUMP=<selhex>`/`TEXCONTENT=<bands>`.
- **Last known:** commit 189544592: "TEXEL 100.0% (WRONG=0 ZERO=0 across 893 quads)" on _live4; 5/5 on out-of-sample _live5 (memory).
- **Command:** `cl /O2 /DTEXFIX2 texel_gate.cpp render_frame.c gen_*.c /Fe:texel_gate.exe` then `texel_gate.exe <ram16.bin> <btcw.bin> <eng_band.bin> [cli_band.bin] [prefix_band.bin]`. Windows-runnable.
- **Status:** STANDING — exercises the transpile/decode staging path; still relevant to the pixel side under native charpass (VRAM texture coherence).

### G3. _gate4.mjs — geometry lockstep remap simulation
- render_frame_node.wasm sprite tcw texaddrs vs native engine TA (frames 90–93). Pre-filter, not proof. ONE-SHOT HISTORICAL. `node _gate4.mjs`.

### G4. _prove_replay.mjs — HUD replay through verbatim deployed code
- Captured HUDQ rendered via `buildHudTA` extracted VERBATIM from `web/render-replica/replay.html`; `--before` monkeypatches pre-fix behavior. Output PNG, human-judged. ONE-SHOT HISTORICAL (browser HUD path; native HUD = Phase 2b).

### G5. audit_seed_stale_scan.py — wire read-set completeness
- Per-frame RAM bytes differing from the MCRR seed OUTSIDE shipped dyn regions (union over all frames). Empty = complete.
- `python <R>\tools\render-replica-poc\audit_seed_stale_scan.py <capture.mcrr>`. STANDING (rerun on any fresh capture when a stray/garble appears). Last known: 0 stale bytes (re_kb/25).

### G6. audit_pointer_targets.py — chased-pointer stability
- idxtab `*0x8C2DAD3C`, rectab `*0x8C2DAD4C`, node GFX1/GFX2 (+0x15C/+0x160), desc base 0x8C1F9F9C vs shipped coverage. STANDING companion to G5.

### G7. audit_replay_probe.mjs — render_frame.wasm truth probe
- Drives the REAL render_frame.wasm over an MCRR, reports chased pointers + per-frame quad Ax spread (stray detector). STANDING pre-filter (geometry only).

### G8. emitter_truth_gate.py — emitter geometry/pixel gate
- Deployed sprite-client.mjs emitter placement vs CHARQ per-part PVR quads; PASS = ≤0.5px AND opaque-pixel agree ≥99%. Last known 0.00px. **HISTORICAL** — emitter deprecated in favor of render_frame-as-drawer (2026-06-15). Keep only if the browser emitter path is revived.

### G9. Step-2 read-set delta — MAPLECAST_READTRACE (live headless, one-shot)
- **File:** `<R>\core\network\mc_readtrace.cpp`
- Every byte the real char-pass driver (0x8C030858→ret 0x8C039648) reads at a live frame, classified vs the shippable-region table; verdict = TRUE-EXTERNAL bytes.
- **Envs:** `MAPLECAST_READTRACE=1`, `MAPLECAST_READTRACE_FRAME=<n>` (default 60), `MAPLECAST_READTRACE_OUT=<path>`, plus `MAPLECAST_READTRACE_STEP3=1` + `MAPLECAST_READTRACE_SEED=<path>` (writes RTSEED02: ctx512+CCN72+RAM16MB), `MAPLECAST_READTRACE_ISOLATE=1`, `MAPLECAST_READTRACE_ENGINE_TA=<path>` (engine SQ char-pass TA dump).
- Last known: TRUE-EXTERNAL=0B (commit 1dc03e611). STANDING — the seed-capture step of the byte gate.
- **Caution (re_kb/52):** do NOT stack readtrace with oracle-hook/CHARQ instrumentation on the Windows headless — stacking caused the 0xC0000005 host crash.

### G10. Step-3 standalone runner — realcore byte gate
- **Files:** `<R>\tools\render-replica-poc\realcore\runner.cpp`, `build_ds.bat`
- flycast's real SH4 interpreter runs the driver from an RTSEED02 on RESIDENT-ONLY RAM (non-resident zeroed); captured SQ/FIFO TA → `ta_out.bin`; exit 0 iff retPc reached. Negative controls: `--drop-chars` (must fire misses), `--drop-scratch` (falsified wire trim), `--min-ctx`, `--ctx-override=`.
- **Last known (artifacts in realcore/):** 4519 parcels (144608 B), miss=0; `engine_ta.bin == runner_ta.bin == ta_ctrl_ds.bin` md5 `be1377d28b3d4bf624c18590dae21ce5`; negative control 0 parcels / 64 misses; `--drop-scratch` 588 misses (arena/idxtab/rectab MUST-STAY).
- **Command:** `build_ds.bat` then `runner.exe <rt_seed.bin> [--drop-chars|--drop-scratch|--min-ctx]`. STANDING.

### G11. Phase 2a in-process byte gate (commit 483511fef)
- **Files:** `<R>\core\network\gsta_charpass.cpp` (+.h), selftest invoked from `core/nullDC.cpp:128-132`; live splice `core/network/maplecast_mirror.cpp:4812-4888`.
- **The md5 compare lives in `selftest_from_env()` (gsta_charpass.cpp:243-287).** Log lines:
  - `[charpass-selftest] ran=%d reached=%d parcels=%zu bytes=%zu md5=%s wall=%.3fms`
  - `[charpass-selftest] EXPECT md5=be1377d28b3d4bf624c18590dae21ce5 (byte gate)`
  - `[charpass-selftest] run_live(EMBEDDED ctx) ... == run() (embedded ctx byte-correct)` (or `!!! MISMATCH`)
  - Writes `charpass_ta.bin`, then exits.
- **LIVE-path log lines (NO md5):** every 120 frames `[charpass] NATIVE TA %zu parcels %.2fms (byte-exact vs engine)` and `[charpass] decode-follows-native: %d/%zu body decode addrs redirected`; fallback warn `[charpass] NATIVE requested but run_live unavailable ... using transpile`; first-frame `[charpass] entry context: EMBEDDED constant`.
- **CRITICAL:** the live "(byte-exact vs engine)" text is an ASSERTION carried from the offline gate — the live path computes NO per-frame md5 and has NO per-frame engine reference. The live byte gate does not exist yet (Missing tooling #2).
- **Command:** `set MAPLECAST_CHARPASS_SELFTEST=<seed2.bin>` then run `<R>\build\flycast.exe`. Requires an RTSEED02 seed (ROM-derived, NOT in tree — regenerate via G9). STANDING.

### G12. Determinism rig — MAPLECAST_DUMP_TA
- Server dump `maplecast_mirror.cpp:~2017` (serverPublish), client dump `:~5690`; procedure `docs/ARCHITECTURE.md` §determinism.
- Per-frame raw TA `frame_NNNNNN.bin` server vs client, byte-diff. **Windows:** set `MAPLECAST_DUMP_TA_DIR` to a native Windows dir (default `/tmp/...` fails silently under MSVC). STANDING — run at end of every phase per CLAUDE.md.

### G13. Savestate-freeze + framebuffer A/B rig — THE acceptance gate
- **Freeze:** control WS 7211 `{"cmd":"savestate_save","slot":N}` WORKS (`maplecast_control_ws.cpp:289-304`); **`savestate_load` is HARD-DISABLED** (`:308-345`). So freeze = save slot, then **restart** headless with `MAPLECAST_HEADLESS_AUTOLOAD=1` (`core/emulator.cpp:825-827`, loads `config::SavestateSlot`, default slot 0). Prior art: `<R>\_ctrl_save_slot0.mjs`, `_ctrl_save_slot1.mjs`.
- **Capture:** `MAPLECAST_GSTA_SHOT=<prefix>` (`core/ui/mainui.cpp:185-216`): dumps `<prefix>_<n>.png` where **n = `_grn`, the client-local render counter — NOT vframe**; knobs `MAPLECAST_GSTA_SHOT_EVERY` (default 30), `_START` (default 120), `_END` (default 600). Works in GSTA mode AND plain-mirror mode.
- **Differ:** `node <R>\tools\render-replica-poc\diff_png.mjs <a> <b> [--out d.png] [--tol N]` — match %, mean/max channel delta, heatmap, exit 0 iff mean ≤ tol.
- STANDING — this is THE gate for any "done" claim.

### G14. Dual recorder — _cap_persist.mjs
- Records 7200 (framed ZCST → `.mirror.zcst`) + 7212 (prefix+FRMx → `.gsta.mcrr`), time-aligned; auto-stops `--after` sec past first FRMx.
- `node <R>\tools\render-replica-poc\_cap_persist.mjs --out <prefix> --hard 30 --after 5`. STANDING corpus producer for G2/G5/G6/G7.

### G15. Autonomous replay + scorecard rig (189544592 era)
- **Replay server:** `mock_replica_live_server.mjs [rec.mcrr] [port=7212] [fps=60] [--zstd]` — replays a captured .mcrr to the native client deterministically.
- **Scorecard:** `gate_check.mjs` — **NOT IN THE TREE** (lived in a prior session's ephemeral scratchpad; ROM-derived frozen refs so not committed). Last results (5/5 on _live5) are memory-only. STANDING in intent, **MISSING in fact — re-land it**.

---

## 2. LIVE A/B PROTOCOL — MAPLECAST_GSTA_NATIVE_CHARPASS=1 (fully-local Windows rig)

Binaries: `_build_headless.bat` → `build-headless-win\flycast.exe`; `_build_client.bat` → `build\flycast.exe`. ROM `C:\roms\roms\mvc2.gdi`. re_kb/52 launch rules: minimal instrumentation on the headless, launch from repo/build dir, log via redirected file.

**Step 0 — one-shot byte gate (per fresh build).**
1. Capture seed+engine TA at a live frame (headless, isolated run):
   `MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 MAPLECAST_REPLICA_LIVE=1 MAPLECAST_HEADLESS_AUTOLOAD=1 MAPLECAST_READTRACE=1 MAPLECAST_READTRACE_FRAME=90 MAPLECAST_READTRACE_STEP3=1 MAPLECAST_READTRACE_SEED=<R>\_ab\rt_seed.bin MAPLECAST_READTRACE_ENGINE_TA=<R>\_ab\engine_ta.bin build-headless-win\flycast.exe "C:\roms\roms\mvc2.gdi"` (native Windows paths mandatory).
2. Client selftest: `MAPLECAST_CHARPASS_SELFTEST=<R>\_ab\rt_seed.bin build\flycast.exe` → PASS = printed md5 == md5(engine_ta.bin) of the SAME capture (the historical constant `be1377d2...` is frame-90-of-the-old-slot-specific), plus the `run_live(EMBEDDED ctx) ... == run()` line.

**Step 1 — freeze the moment.** Play to the target moment, `node <R>\_ctrl_save_slot0.mjs` (7211 savestate_save slot 0). Kill headless. All captures boot from this state via AUTOLOAD (WS savestate_load is disabled — restart IS the freeze mechanism).

**Step 2 — engine ground-truth pixels (plain mirror client).** Restart headless (`_run_srv_gsta.bat`). Launch mirror client (no committed bat — see Missing #3):
```
set MAPLECAST_MIRROR_CLIENT=1
set MAPLECAST_SERVER_HOST=127.0.0.1
set MAPLECAST_SERVER_PORT=7200
set MAPLECAST_GSTA_SHOT=<R>\_ab\mirror
set MAPLECAST_GSTA_SHOT_EVERY=1 & set MAPLECAST_GSTA_SHOT_START=200 & set MAPLECAST_GSTA_SHOT_END=230
build\flycast.exe
```
(MIRROR_CLIENT **without** GSTA_CLIENT = engine ground truth.) Yields 31 consecutive frames of the frozen state.

**Step 3 — B leg (native charpass).** Kill client, restart headless (identical state), run the native-flag client with `MAPLECAST_GSTA_SHOT=<R>\_ab\native`, `_EVERY=1`, `_START=200`, `_END=230`, **plus `MAPLECAST_GSTA_POLY3D=0`** (P3D double-draw workaround, blocker G1 in appendix 02).

**Step 4 — A leg (transpile control).** Same with the flag unset → `<R>\_ab\transpile_*.png`.

**Per-frame pass signal in the native log:**
- MUST see `[charpass] entry context: EMBEDDED constant` once.
- MUST NOT see `NATIVE requested but run_live unavailable ... using transpile` (that means the A/B silently compared transpile vs transpile).
- `[charpass] NATIVE TA N parcels X.XXms` every 120 frames — liveness + wall-clock budget only. **NOT a byte gate.**

**Step 5 — pixel gate (the only pass).**
- Cross-leg: `node diff_png.mjs <R>\_ab\mirror_210.png <R>\_ab\native_210.png --tol 0 --out d_210.png` for every index in both runs. Shot index = client-local render count, NOT vframe — valid ONLY because the state is frozen. If the moment animates, pairing is invalid.
- **Numeric criteria:** char-pass region (bodies+effects) diff pixels = 0 vs mirror. Full-frame 0 NOT expected (HUD = Phase 2b, stage separate). Report full-frame diff % + diff bounding boxes; PASS only if every bbox lies inside named HUD/stage regions. Any body/effect-region diff pixel = FAIL.
- Native must be ≥ transpile: any region where native > transpile diff = regression, FAIL.
- **Flicker gate:** frame-to-frame within the native leg (30 consecutive pairs of a frozen state) — expected 0 diff pixels every pair; any nonzero = temporal bug (decode-follows-native / snapshot-ordering class).

**Step 6 — regression set.** Repeat on ≥3 frozen moments (neutral, mid-super with bit15 effects, post-tag).

### Missing tooling (exact gaps)
1. **`gate_check.mjs` scorecard is not in the repo** — re-land it or the 189544592 standing gate is unreproducible.
2. **No live per-frame TA md5 gate.** Client never hashes `_nta` per vframe; headless has no per-frame engine char-pass md5. Needed: env-gated `[charpass] vf=%u md5=%s` + matching headless line, so the live "byte-exact" claim becomes a measurement.
3. **No mirror-ground-truth launcher bat** — write `_run_cli_mirror_shot.bat`.
4. **`_run_client_shot.bat` is broken as committed** — missing `MAPLECAST_MIRROR_CLIENT=1` (client sits at menu) and uses `MAPLECAST_SERVER_PORT=7212` instead of `MAPLECAST_GSTA_PORT=7212`. Fix before use.
5. **Shot filename uses `_grn`, not vframe** — cross-client pairing on a moving frame impossible; keep the frozen-state constraint or add vframe to the filename.
6. **No .zcst replay server for 7200** (mirror-leg offline replay).
7. **Windows companion dumps hardcoded to /dev/shm** (`maplecast_oracle_hook.cpp:~3545`) — same-frame VRAM+PVR+engine-TA A/B on Windows needs a getenv override + rebuild.
8. **diff_png.mjs is full-frame only** — no `--rect` region mask for the "exclude HUD band" criterion.

---

## 3. STANDING vs ONE-SHOT

**STANDING (run before any future "done" claim):** G13 frozen-frame pixel A/B (THE gate), G11 charpass selftest + G9/G10 seed/runner with negative controls, G12 determinism rig, G2 texel_gate on fresh out-of-sample corpus, G5/G6/G7 wire audits on fresh captures, G14 corpus producer, G15 replay rig (once gate_check re-lands), G1 run.cmd chain (while transpile fallback ships).

**ONE-SHOT / HISTORICAL:** G3, G4, realcore adjudication runs (archived in `realcore/run_*.log`), G8 emitter gate (path deprecated).

**Verdict discipline:** the only live evidence for Phase 2a today is a throttled log line with no measurement in it. Until missing-tooling #2 lands, every live charpass claim must ride the frozen-frame pixel gate — numbers and bounding boxes, not impressions.
