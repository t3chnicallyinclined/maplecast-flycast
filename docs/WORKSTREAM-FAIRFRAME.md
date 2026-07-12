# WORKSTREAM: FAIRFRAME — Synchronized Presentation (Tournament Mode)

> Origin: user idea 2026-07-12 — "what if both clients are reacting to the same exact frame,
> guaranteed, each viewing that frame at that moment." The LAN-cabinet guarantee, over the internet.
> Memory: project_fairframe_sync_presentation. Composes with (does not replace) the latency
> kill-list; opt-in per match — the default stays LatencyFirst + immediate present.

## Goal (falsifiable)

Both players are PRESENTED the same server frame at the same wall-clock instant, and their
press→latch mapping distributions are IDENTICAL (E2E probe on both clients) — competitive fairness
independent of RTT asymmetry. Spectators unaffected.

## Why this architecture is uniquely suited

Server-authoritative single sim (no per-client sims to reconcile — P2P rollback games cannot do
this cheaply). Already built: server frame_phase broadcast + client clock-offset estimator
(gamepad.mjs Phase B), phase-aligned input sends, **E2EP per-frame publish stamps
(frameNum + t_latch_us + t_publish_us, 32B ZCS2 tail — server MAPLECAST_E2E_PROBE=1, client
parser live in webgpu-test.html)**, E2E probe methodology for the fairness proof.

## Mechanism

```
server:  frame N published at t_pub (stamped in the E2EP tail — EXISTS)
client:  T_present(N) = map_to_local(t_pub) + Δ      (Δ = per-match presentation delay)
         buffer decoded frame; present exactly at T_present (not on arrival, not on rAF)
input:   latch already server-aligned → equalized stimulus + equalized latch = fair
```

- `map_to_local`: anchor on min-observed (arrival_local − t_pub) over a sliding window (min =
  fastest delivery ≈ offset + min_one_way; robust, no absolute clock sync needed for pacing).
  Cross-client equality (F3+) refines with the control-WS ping offset (min-RTT median, 1Hz).
- `Δ` (per match) = max over both players of (one-way p95 + decode p95) + margin (~2-3 frames
  typical). The low-RTT player is delayed down to the high-RTT player — the fairness trade,
  which is WHY it is opt-in tournament mode.
- Side benefit even solo: scheduled presents = perfectly even 16.67ms pacing (arrival jitter
  absorbed by the buffer) — may FEEL smoother than arrival-paced.

## Phases

| Phase | What | Gate |
|---|---|---|
| **F0** | Clock foundation: quantify map_to_local stability (min-anchor drift) + offset accuracy over 5+ min | anchor stable ±2ms |
| **F1** | Publish stamps always available: E2EP tail on (env flip — binary already has it) | client sees pubUs per frame |
| **F2** | Client present-at-T scheduler: `?present=sched&delta=<ms>` — buffer + setTimeout-to-target present; SOLO first | pacing histogram: present-interval σ << arrival σ; added latency == Δ − one_way (measured) |
| **F3** | Δ negotiation: server computes per-match Δ from both players' measured RTT/decode (E2EP/ping), announces at match start, re-negotiates on drift | both clients converge on one Δ |
| **F4** | **Fairness proof**: E2E probe on BOTH clients simultaneously → identical press→latch distributions | the falsifiable gate; no proof = not shipped |
| **F5** | Product: tournament-mode toggle in matchmaking; UI badge; spectators keep arrival-paced | — |

## Risks / notes
- setTimeout jitter (~1-4ms): coarse setTimeout to T−4ms then rAF/spin for the last leg.
- Buffer depth: 1-3 frames at realistic Δ — memory trivial; frame drop policy = present newest
  whose T_present ≤ now (latest-wins preserves order because T_present is monotonic).
- Interacts with D1: 'sched' is a THIRD present mode alongside immediate/vsync (radio + URL param).
- A2 run-ahead composes: it cuts shared latency for both players equally; FAIRFRAME equalizes the
  remainder. Together: lowest AND fairest.
- Clock discipline: E2EP pubUs is server CLOCK_MONOTONIC — survives nothing across server restarts;
  re-anchor on epoch/reconnect (reset the min-anchor window).

## Status log
- 2026-07-12: Workstream locked. F1 = env flip (E2EP already in binary b0250090). F2 client
  scheduler v0 implemented behind ?present=sched (see webgpu-test.html).
- 2026-07-12 (v0→v0.2 lessons, all committed): v0 delayed the RENDER → shredded bodies (D.vram is
  shared live state; retained-image presentation is MANDATORY). v0.1 render-now/blit-later via
  setTimeout → smooth-then-judder (59.94 vs 60Hz vsync BEAT; vsync-locking MANDATORY). v0.2 rAF
  presenter → clean + calibrated (slack 47.2ms @Δ=50) but presents SILENTLY stop after seconds:
  NO GPU errors, NO blit warnings ⇒ either decode stalls upstream or the schedule condition goes
  false. NEXT: add a [fairframe] 1Hz heartbeat {pendingBlit?, target-now, wsFrames} to discriminate
  decode-stall vs present-stall — ONE log line answers it. Mode stays opt-in experimental; default
  path untouched.
- 2026-07-12 **F2 COMPLETE + F0 GATE PASSED**: v0.3 frame-ring presenter (5 retained textures,
  per-frame appointments, vsync presenter, PP.blit bindGroup override) VERIFIED live 40+s:
  frames+blits lockstep @60/s, tgtNow 46-50ms steady, anchor drift ±0.5ms (gate ±2ms), no freeze/
  garble. v0.2 starvation root-cause: single latest-wins slot never fires when Δ > frame period.
  User feel: "snappier before" = CORRECT — Δ=50 is the deliberate tournament cushion; default
  (immediate) stays the snappy mode. NEXT: walk Δ down for the per-connection floor, then F3
  (server per-match Δ negotiation) → F4 (dual-client fairness proof).
