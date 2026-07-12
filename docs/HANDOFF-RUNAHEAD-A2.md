# HANDOFF: A2 Run-Ahead + the latency campaign

Branch `feat/render-replica-live`, HEAD `97d14426d`. Prod = 149.28.44.118 (nobd.net), 2-vCPU no-GPU.
Dev/build box = AX102 Ryzen 9 7950X3D at tris@65.109.77.178 (holds the ROM + assets).
Memory: `project_a2_runahead_state`, `project_e2e_latency_baseline`, `project_render_checkpoint_2026_07_11`,
`project_fairframe_sync_presentation`. Kill-list: `docs/LATENCY-KILL-LIST-2026-07-12.md`.

## 0. WHAT IS SHIPPED AND LIVE (do not break)
- **Render checkpoint**: stage on wire pixel-perfect, bodies render_frame byte-exact, effects on wire. STAGESTRIP=0 CHARSTRIP=1 STATE_MERGE=1. ~0.6 Mbps steady.
- **Tier-0 latency cuts** (all live on prod): D1 immediate-present default, C1 control_only, B3 zstd 9→3 (0.313→0.065 ms/frame), B4a ZCS2-first, B5 RAW-fallback fix.
- **Measured E2E baseline: ~22ms press→pixel** (n=820), server 1.7ms / client 2.7ms / residual = uplink+vblank beat. E2E probe: server `MAPLECAST_E2E_PROBE=1` + client `?e2e=1`.
- **FAIRFRAME (user idea, synchronized presentation)**: `?present=sched&delta=25` LIVE + user-approved feel. F2 complete (v0.3 frame-ring presenter + F2.1 vsync phase-trim servo). Default path (immediate) untouched. Next phases F3 (server Δ negotiation) → F4 (dual-client fairness proof).
- **D4/D7 client spike-killers** live (gfxTail span-copy, bank-scoped palette invalidation).
- Prod binary has run-ahead but **flag OFF** (`MAPLECAST_RUNAHEAD` absent from /etc/maplecast/headless.env). Safe.

## 1. A2 RUN-AHEAD — FULL STATE

**Goal**: hide MVC2's measured 1-frame internal input lag by publishing frame N+1's render at the wall-clock time we'd publish N → visible reaction ~39ms → ~22ms. Cycle: sim hidden frame N (suppress publish) → save → sim preview frame N+1 (publish it) → rewind to N. Mispredict-free because MVC2's +1 lag means N+1's pixels don't depend on N+1's input.

**PROVEN GOOD:**
- Lightweight rewind (skip per-tick dynarec `bm_Reset`/`ResetCache`) is **byte-perfect** (determinism gate 100%). The 80ms cold-JIT thief is dead.
- **60fps on the AX102**: 11.6ms/tick (hidden 3.2 / save 3.3 / preview 2.7 / rewind 2.3). Rewind is memory-bound — 14ms on a plain box, 2.3ms with the 7950X3D V-cache. Scalars-only surgery NOT needed on good HW.
- 60Hz tick-end pacer (`mc_runaheadArmed` global atomic; audio pacing bypassed under run-ahead) → exactly 60.0fps.
- Cycle **mechanically perfect** (4-point anim trace, 1156/1156): hidden advances 1 frame (A1=A0-1), preview advances to N+1 (A2=A1-1), rewind restores (A3=A1).

**THE BUG (fully pinned, confirmed 3 ways — DUMP_TA, state-wire anim A/B, 4-point trace):**
Run-ahead ships the **CURRENT** frame (N), not N+1. Offset 0 / 100% on BOTH the legacy TA wire AND the 7212 state wire (the one the GSTA client uses). So it delivers **ZERO latency benefit** as built.
**Root cause**: the shipped `captureFrame` (onRenderFrame, maplecast_replica_live.cpp ~1414 / ~782) fires at the FIRST STARTRENDER in the preview leg = the **hidden frame's DEFERRED render pass** (MVC2 submits a frame's render ~1 frame late = the "S1 defer"). At that instant resident state is still frame N; it reads+ships N, then the preview's logic advances to N+1 — too late. `suppress` toggles false before the preview leg, so the deferred hidden render (rend_start_render'd during the preview window) is mis-stamped `mc_hiddenLeg=false` and shipped.
Also confirmed: the guest vf counter (0x8C3496B0) increments at VBLANK, AFTER the STARTRENDER/capture point → vf stamps are unreliable as content labels; always compare STATE CONTENT.

**KEY FILE SITES:**
- Cycle (CORRECT, don't touch mechanics): `core/emulator.cpp` ~1758-1815 (threaded loop). Step-stop at vblank via `raArmStepStop`/`raConsumeStepStop` (Emulator::vblank). Lightweight rewind at ~1793.
- The BUG is capture timing: `core/network/maplecast_replica_live.cpp` `onRenderFrame` (~1414) → `captureFrame` (~782). `serverPublish` mc_hiddenLeg early-return: `core/network/maplecast_mirror.cpp` ~2233. Stamp at `core/hw/pvr/Renderer_if.cpp` rend_start_render ~612.
- Suppress API: `maplecast_mirror::{setSuppressPublish,suppressActive}` (mirror.{h,cpp}).

**FIX DIRECTION (real render-pipeline surgery, next session):** retarget the shipped capture to the PREVIEW frame's content — options: (a) capture resident body state (pos/anim/sprite/screen-anchors) at the preview-leg END (the A2 point in the cycle, guaranteed N+1) and reconcile the render-deposited tables (idxtab/rectab); (b) account for the S1 defer so the preview leg's OWN render (not the hidden's deferred one) is the shipped pass — distinguish them despite the suppress toggle timing; (c) the 3rd-runInternal (sim to N+2 so N+1's render fires) — REJECTED for +50% compute + only helps if the client needs render-deposited fields (it mostly needs resident). Prefer (a). GATE: re-run the state-wire anim A/B; PASS = offset flips 0→+1.

## 2. ARCHITECTURE DECISIONS (from the 3-expert workflow, wf_20a3141b-1d4)
- **B (client computes N+1): DEAD.** Browser has render_frame (pixels from state), no SH4 game tick. Can't extrapolate the reaction.
- **C (separate/2-server run-ahead + shim, the user's idea): NOT justified.** No rewind-race to dodge (publish provably precedes rewind), and it doesn't fix the S1 defer (a MVC2 property, not a process boundary). Held as fallback only.
- **A-prime (in-process fix): recommended** — fix the capture timing (§1). This is the path.

## 3. HOW TO REPRODUCE THE GATE (AX102)
- Match state: `/home/tris/mvc2-rom-test/mvc2_match.state` (captured from a prod live match via control-WS `savestate_save` slot 97 over a raw-WS python client; AX102 pulled it from prod). It's an IDLE stance — pos_x static, but **anim_timer@0x8C268482 (u32 = anim_timer+sprite_id) cycles every frame**, so it works for the offset test.
- Build: on AX102 `cd /home/tris/projects/maplecast-flycast && git fetch origin feat/render-replica-live && git reset --hard origin/... && touch <changed files> && cmake --build build-headless -- -j32` (touch is MANDATORY — stale incremental ships old binary).
- Run: `cp mvc2_match.state mvc2.state; MAPLECAST_MIRROR_SERVER=1 MAPLECAST_HEADLESS_AUTOLOAD=1 MAPLECAST_REPLICA_LIVE=1 MAPLECAST_STATE_MERGE=1 MAPLECAST_STATEVF=1 [MAPLECAST_RUNAHEAD=1] timeout 30 ./build-headless/flycast /home/tris/mvc2-rom-test/mvc2.gdi`. Restore `mvc2.state.orig-bak` after.
- Compare `[STATEVF-PUB] anim=` sequences baseline vs run-ahead → offset. `[STATEVF-STEP]` = the 4-point A0/A1/A2/A3 per-tick trace.
- captureFrame requires `MAPLECAST_REPLICA_LIVE=1` (arms `_armed`) + in-match (0x8C289624 != 0).

## 4. DIAGNOSTIC ENV FLAGS (all default-off, harmless, committed)
- `MAPLECAST_RUNAHEAD=1` — arm the cycle.
- `MAPLECAST_RUNAHEAD_TRACE=1` — [RA-TRACE] SR/PUB per-context {leg,vf}.
- `MAPLECAST_RUNAHEAD_MEASURE=1` — dc_serialize timing (the go/no-go gate; was 1.80ms).
- `MAPLECAST_STATEVF=1` — [STATEVF-PUB] shipped anim + [STATEVF-STEP] 4-point trace + [RUNAHEAD-PROF] per-leg ms.
- `MAPLECAST_BYPASS_AUDIO_PACING=1` — kill the null-audio frame limiter (acquitted as the perf thief).

## 5. NEXT STEPS (priority order)
1. **A2 capture-timing fix** (§1 fix direction (a)) — retarget the shipped state capture to the preview frame; re-run the state-wire anim A/B; PASS = offset +1. THEN determinism gate (DUMP_TA authoritative track) + arm-transition `rqueue==ctx` FinishRender crash guard + super-load stress. THEN hardware/feel-test.
2. **FAIRFRAME F3/F4** — server per-match Δ negotiation from both players' floors → dual-client fairness proof (press→latch distributions identical). This is user-loved and closer to done than A2.
3. Kill-list leftovers: client-side body-quad skip (~3→0.6 Mbps), pre-baked super effects (6MB spike).

## 6. OPERATIONAL DISCIPLINE (persist)
- Prod = 149.28.44.118. Deploy SURGICALLY (file backup + scp one file + md5 verify). NEVER deploy-web.sh. Always push after commit.
- Build on AX102, deploy binary to prod with chmod 755 + md5 verify. `touch` changed files before build (stale-incremental landmine). Verify binary contains the change (`strings | grep`).
- NEVER commit ROMs/savestates/PNG rips (copyrighted). `git add` specific paths only.
- Exact-match discipline: every measured delta vs ground truth is a lead; no dismissed differences.
