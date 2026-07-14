# INSANE-LATENCY-PLAN — Predictive Netcode for MvC2 (native client)

> Branch `feat/predictive-netcode`, off `f6c535cd2` (jitter buffer + `/replica-live`
> NODELAY live on prod). This doc is the checkpoint: the plan itself is versioned so we
> can always come back to "how we saw it now." Grounded by workflow `w2869wi5g`
> (4 parallel code audits → synthesis → adversarial critique). Every claim below is
> tied to code; `EXISTS` = already built (cited), `NEW` = must build.

## North star

Console-grade MvC2 netcode on the native Rust/wgpu client:

- **Latency:** collapse visible press-to-pixel from **~39 ms today** (browser: ~22 ms
  press→submit ema, probe `ce4352c82`, + one 16.67 ms MvC2 frame + compositor) toward
  **~22 ms on LAN** (Tier 1 removes the MvC2 internal frame), then to **~0 added frames
  locally** for the ROM-having player (Tier 3 computes the frame on the client instead
  of shipping it).
- **Bandwidth:** from the **~3 Mbps** render wire (~6 Mbps triple-super spike) to
  **~0.02 Mbps inputs-only** (~2.4 KB/s tape + 24 B/60 f checksum + one-time ~10 MB
  join), proven in lockstep-mirror `bc16af338` (9121/9121 frames byte-matched).

## Why us: the three assets almost no streaming setup has

1. **SH4 is byte-deterministic** across machines and OSes (validated 2026-05-07,
   `reference_determinism_validated`). Same state + inputs → bit-identical result.
2. **The full game state is already on the client** — `/replica-live` streams the 16 MB
   RAM image every frame (`native-client/src/replica.rs`).
3. **We can run SH4 logic off that state, ROM-free** — `render_frame.c` transpiles SH4
   render routines to C and runs them on the client with the emulator OFF
   (`ee3e60065`, `native-client/src/main.rs:334-369`).

## Latency budget (measured, per stage)

Browser baseline (E2E probe `ce4352c82`, n≈820): ~22 ms ema to render-**submit**;
VISIBLE press→pixel ~39 ms once the +16.67 ms MvC2 frame + compositor scanout are added
(neither is in the probe).

| # | Stage | Cost | Attacked by |
|---|-------|------|-------------|
| 1 | input poll | ≤1 ms | — |
| 2 | uplink + vblank-latch beat | ~10–30 ms | QUIC input (browser) |
| 3 | **MvC2 internal frame** | **+16.67 ms** (measured 8× byte-identical) | **Tier 1 / Tier 2 / Tier 3** |
| 4 | emulate | ~2–5 ms | — |
| 5 | server latch→publish | ~1.7 ms | — |
| 6 | downlink RTT/2 | ~5–10 ms | Tier 3 (local compute) |
| 7 | client decode/parse | ~2.4–3.1 ms | Tier 3 |
| 8 | present | native AutoNoVsync ~0 (`main.rs:78-81`) | D1 immediate-present |
| 9 | compositor scanout | 16.7–33 ms (not in probe) | — |

Native path is **inferred** (no probe exists yet): direct UDP:7100 input (no WS/relay
hop) + AutoNoVsync ≈ 10–15 ms over console on LAN. **Building the native E2E probe is a
Tier-1 deliverable — we do not claim any shave we have not measured.**

---

## Tier 1 — Server run-ahead (+1 speculative frame). GO. No ROM. The spine's first move.

**Goal:** ship the frame that already contains the response to the just-pressed input,
so the client loses the 16.67 ms MvC2-internal frame with **zero client code change** —
and make that +1 reach the native client's bodies *cleanly*, not by last-write-wins luck.

**Target:** −16.67 ms VISIBLE press-to-pixel. Native LAN visible ~22 ms; over the
internet an honest ~15 % competitive edge, not a collapse. Bandwidth neutral on `/ws`;
on `:7212` the gate *removes* today's up-to-3-captures/tick waste.

**EXISTS (do not rebuild):**
- Full 3-leg run-ahead cycle, the path prod runs: leg1 hidden→N + `saveFrame(N)`; leg2
  suppressed lookahead→N+1; leg3 PUBLISH N+1; then `rend_wait_render_idle()` +
  `rewindToFrame(N, lightweight)`. `core/emulator.cpp:1744-1880`.
- +1 offset REAL on the state wire (`f9a22b6de`, `[STATEVF-PUB] 0→+1 100%`).
- Rewind = the shelved-GGPO rollback RING repurposed: 10×40 MB arena
  (`maplecast_rollback.h:47`, `.cpp:132`), memwatch page tracker.
- Dirty-page rewind byte-exact gate-proven (`62c53017f`): ~137 pages / ~550 KB,
  12.9→1.9 ms; gate 0 diffs / 1800+ rewinds.
- Short-leg3 (`99f0cfb1b`): halts leg3 the instant N+1 ships; preview 4.1→2.0 ms.
- `/ws` TA wire IS run-ahead-gated (`mirror.cpp:2263` early-returns on `mc_hiddenLeg`).

**NEW — the single load-bearing change + its correctness prereqs (per the critique):**

1. **Gate the `:7212` capture with the CONTEXT-STAMPED hidden-leg flag** (NOT the global
   `suppressActive()` atomic — it stays `false` across leg3's multiple STARTRENDERs and
   races the between-tick `setSuppressPublish`). `onRenderFrame` already *receives* `ctxv`
   and ignores it. Cast it and read `rend.mc_hiddenLeg`, exactly as `serverPublish` does
   (`mirror.cpp:2263`), right after `if(!_armed) return;` (`replica_live.cpp` ~1421).
   → only leg3 (N+1) captures+publishes; kills the 3×/tick heavy capture + the race.
2. **`MAPLECAST_RA_SHORT_LEG3` is a Tier-1 CORRECTNESS prereq, not an fps flag.** leg3
   sets `suppress=false` *before* `runInternal` (`emulator.cpp:1789`) and restores it at
   1809 — so across leg3 both the N+1 publish SR *and* any N+2-building SR have
   `mc_hiddenLeg==false`; with drop-old publish the LAST wins → an N+2-partial can clobber
   N+1. `raArmPublishStop` (gated on `_raShortLeg3`) halts leg3 at exactly the N+1 publish
   SR. Must be ON.
3. **Add a `vframe==N+1` dedup in `captureFrame`** (belt-and-suspenders): a second
   non-hidden SR can never overwrite the shipped frame.
4. **Default-on** `MAPLECAST_RA_DIRTY_REWIND` + `MAPLECAST_RA_SHORT_LEG3` (or bake into
   `headless.env`).
5. **Cap memory BEFORE arming:** 400 MB eager ring (RING_DEPTH=10×40 MB, allocated up
   front) + ~322 MB base + the *unbounded* `:7212` write queue vs `MemoryMax=1G`
   (`maplecast-headless.service:70`) ≈ 722 MB before backpressure. **Depth-1 run-ahead
   does not need 10 slots** — drop RING_DEPTH to the actual rewind depth + bound the
   write queue first.

**Steps (validate on the LOCAL RIG first — headless↔native, `project_local_rig` — so +1
correctness is proven independent of the prod-fps question):**
1. Land the context-stamped `:7212` gate + `short-leg3` default + `vframe` dedup. Rebuild
   headless. Exercise on the local rig (headless server ↔ native client) → confirm bodies
   are cleanly +1 and only one capture/tick.
2. Build the native E2E press→present probe: seq'd input echo in the FRMx/STAT tail +
   client submit/present stamps (`input.rs` measures only 0xFE input RTT today).
3. Prove the *full 3-leg* tick fits 16.67 ms on the actual authoritative box (re-run
   `MAPLECAST_RUNAHEAD_MEASURE` with dirty-rewind ON on prod 2-vCPU EPYC — prior 1.80 ms
   was the *save* leg only), OR provision a dedicated fast-Ryzen node (OVH 9700X
   5.5 ms/tick proven).
4. Cap memory (ring depth + write queue) → arm: `MAPLECAST_RUNAHEAD=1` + the two flags
   (requires `MAPLECAST_REPLICA_LIVE=1` + `MAPLECAST_STATE_MERGE`). Run the `DUMP_TA`
   authoritative-track determinism gate (must stay byte-identical); exercise native
   `request_sync`/reconnect against the rewind's `rend_wait_render_idle` barrier (crash
   guard `9184dc3ae` was browser-only validated).
5. Measure with the probe on LAN + internet; record the honest LAN shave and internet
   fraction. The SUFFICIENT live-pixel gate named in `f9a22b6de` has no recorded pass —
   do not claim delivery until the probe + pixel gate pass.

**Risks:** native bodies not cleanly +1 until the gate lands; run-ahead + native is
UNTESTED E2E; prod is 2-vCPU no-GPU EPYC while validated 60 fps ticks were desktop Ryzens.

---

## Tier 2 — ROM-less local-response prediction. DEMOTED to an optional, browser-only
## parallel track (a native ROM-having client should skip to Tier 3).

The full SH4 game-step transpile is **NOT feasible** (audit conclusion). Tier 2 is only a
small, hand-written approximation of the LOCAL player's *obvious* actions.

**Scope (hard guard):** predict ONLY walk (Δpos from `x_velocity` +0x5c), jump-start, and
basic-attack anim-start (set +0x144 sprite_id / +0x142 anim_timer the way anim-start
`loc_8c034e8c` would). **NEVER** special/super/throw motion. Render via the existing
`render_frame` path; reconcile/cosmetically-snap the LOCAL char on each authoritative
`/replica-live` frame.

**Target:** hide ≤1 frame of local-action latency for that subset, on ROM-less clients
(browser). Bandwidth neutral. Value is narrow — only clients that *cannot* run the ROM
truly need it. Not on the critical path.

---

## Tier 3 — Full lockstep + client-side rollback. The dramatic collapse. ROM-having.
## NO-GO today — gated on the determinism spike.

**Goal:** the ROM-having native client computes each frame locally from the input tape,
predicts the opponent, rolls back on the authoritative entry → local input ~0 added
frames, opponent latency hidden, and the render wire deleted (inputs-only ~0.02 Mbps).
Simultaneously the lowest-latency AND lowest-bandwidth path.

**EXISTS:** lockstep-mirror headless↔headless MEASURED 9121/9121 checksums, 0 resyncs,
stable 8-frame lag, ~2.4 KB/s (`bc16af338`, `core/network/maplecast_lockstep.{h,cpp}`).
Input wire: 16 B `TapeEntry` over UDP 7101 (`maplecast_input_server.h:417-476`).

**⚠ THE RISKIEST ASSUMPTION IN THIS ENTIRE PLAN:** that the SH4 step stays byte-
deterministic under **INDEPENDENT client prediction**. The only live cross-instance gate
(7103 confHash) is **RED under input: 34/1065 frames diverge** (`581abc8b4`). The
9121/9121 proof was **stall-based** — both instances consuming the *same* authoritative
tape with *zero* independent prediction — so it is NOT evidence for this assumption. The
plan even notes raw input latch matches every frame yet chars diverge ~20 frames into a
hold; if inputs are byte-identical AND the latch matches AND chars still diverge, that is
by definition core nondeterminism in the step, not an input-phase bug. **If this is a
genuine determinism gap, Tier 3 is dead** and the plan degrades to Tier 1's honest LAN
win.

**Sequencing (corrected by the critique): turn the confHash gate GREEN first, as a pure
research spike on the EXISTING C++ predict-live stack (`b339c40dc`), with NO shipping-
client changes — before the architecture fork.**
1. Unify the predictor/ring frame counters into one canonical game-frame (split counters
   make the falsification test misleading — do this before the phase test).
2. Route the client head through the server's `maplecast-latch` input phase; run
   `MAPLECAST_SUBHASH_LOG`. **Falsification:** if chars still diverge with matched latch
   phase + byte-identical inputs, the input-phase hypothesis is wrong → re-open
   `DC-SERIALIZE-AUDIT §2.1` parser statics (genuine determinism gap).
3. Turn the 7103 confHash gate GREEN over a multi-minute sustained-input match. **This is
   NO-GO until green.**
4. THEN decide the architecture fork: FFI-embed the C++ headless SH4 core into the Rust
   native-client, vs ship the headless binary as the player client.
5. Wire opponent predict-then-correct on the now-correct single timeline.

**Gaps this tier must close as first-class deliverables (not footnotes):**
- **Rollback visual artifacts:** `render_frame` re-renders the WHOLE scene (6 chars +
  screen-wide supers) on a rollback, not just the opponent. Need an interpolation/hold
  smoothing strategy.
- **Mispredict frequency/depth: unquantified.** MvC2 at speed (magic series, dashes) =
  rapid input churn = high rollback rate. Instrument the existing predict-live loop to
  *measure* rollback count + depth over real play before claiming "~0 added frames."
- **Input-delay dial:** "~0 added frames" is the maximum-misprediction/maximum-artifact
  config. Every shipping rollback exposes an input-delay knob (a few frames) to trade a
  little local latency for far fewer rollbacks. Pick the operating point with data.
- **Client hardware budget:** per-frame save + on-arrival multi-frame re-sim
  (1.9 ms/frame × depth ≈ 11–15 ms at internet depth) must fit 16.67 ms on the *player's*
  CPU alongside wgpu render. Set a floor.
- **Two-authority reconciliation contract:** server stays authoritative AND the client
  computes locally — state the exact rule for disagreement (server tape wins; when/how
  the client snaps; is the local sim ever authoritative for the local char?).
- **ROM legal/product model:** the entire dramatic-collapse tier is gated on how each
  user supplies MvC2 + whether a public product shipping a Dreamcast core expecting a user
  ROM is viable. This is a product decision, not a footnote.
- **Desync recovery cost:** recovery = the ~10 MB zstd join re-seed = a visible hitch;
  estimate forced mid-match re-seed frequency under sustained play.

---

## Stacking wins (independent, cheap — do alongside)

- **A2 run-ahead (Tier 1) = −16.67 ms visible.** Biggest single lever.
- **Input-over-QUIC (C4, ~90 % built):** WebTransport for INPUT only, decoupled from the
  video stream (~−20–40 ms p99 on lossy links). *Browser* win — native already has
  UDP:7100.
- **Banked/live:** D1 immediate-present default (−8 ms avg/−15 ms p95); B3 zstd L9→L3
  (0.313→0.065 ms/frame); `TCP_NODELAY` on `:7212` (done, `f6c535cd2`).
- **nginx `proxy_buffering off` for `/replica-live` + commit the config to git** (the
  block lives only on prod, unverified — buffering-on holds frames + adds jitter, and a
  redeploy silently regresses it).
- **Bound the `:7212` per-conn write queue + idle WS ping** (queue was UNBOUNDED → slow
  client grows RAM toward the 1 GB bounce; no keepalive vs nginx 60 s reap).
- **Present-at-T fair-frame sync** (WORKSTREAM-FAIRFRAME, `?present=sched`): a fairness
  DELAY, not a raw cut — tournament opt-in only, never default. Composes with A2.

## The spine

**Tier 1 → (Tier-3 determinism spike) → Tier 3.** Tier 2 is an optional browser-only
parallel track. Run-ahead is the concrete win we can ship without the ROM decision; the
determinism spike de-risks the collapse *before* we pay for the architecture fork.

## Doc reconciliation (fix in the Tier-1 PR so the next engineer isn't misled)

- `docs/HANDOFF-RUNAHEAD-A2.md` still reads "offset 0 / ZERO benefit" — superseded by the
  3-leg fix (`f9a22b6de`).
- `maplecast_player.{h,cpp}` carry a "SHELVED, DO NOT add features" banner while being
  load-bearing for the July lockstep+predict work.
