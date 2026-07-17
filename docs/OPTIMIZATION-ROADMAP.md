# MapleCast Optimization Roadmap — RISE-3 (US-East / Vint Hill VA) as main

Companion to [LATENCY-ANALYSIS.md](LATENCY-ANALYSIS.md) (the 66ms button-to-photon
teardown). This sequences the wins for the confirmed hardware: **RISE-3 — Ryzen 9
5900X (12c/24t Zen 3 @ 4.8GHz), up to 128GB, 1-3Gbps + private net, in Vint Hill VA
(US-East).** Vint Hill is ~5-15ms from the US East Coast, so this box is BOTH the
low-latency US-East play anchor AND the compute host — it does not trade geography
for power.

## What the RISE-3 changes vs the 2-vCPU virtio VPS

Bare metal + US-East + 12 fast cores un-parks the levers the VPS made impossible:

| Blocked on virtio | Unlocked on RISE-3 |
|---|---|
| Run-ahead net-negative (no headroom for the 2-3× speculative sim) | **Run-ahead viable** — isolated 4.8GHz core with room to spare |
| CPU governor a hypervisor no-op; can't spare a core to isolate | **isolcpus + nohz_full + governor=performance + C-states off + pinning** → rock-steady frame times |
| 1-4GB → OOM at match load | **128GB** → OOM gone; huge dicts, persistent caches trivial |
| Shared 2 vCPU → one match, contended | **12 cores** → isolated emu core + render/send cores + concurrent rooms |
| Far from users (if EU) | **US-East** → ~5-15ms to East Coast; the propagation floor is small here |

Physics floor for THIS path (Vint Hill → US-East player): ~8.3ms sample phase +
16.7ms sim + ~4-8ms RTT + ~9ms vblank/scanout. Run-ahead + phase-alignment attack
two of those four.

---

## TRACK A — Client display path (FREE, no hardware, biggest single reclaim ~18-27ms)
Do first. Pure `native-client-tdw` code, measurable with **PresentMon** (external,
no probe build needed for the present slice).

- [ ] **A1 · Borderless-fullscreen DXGI independent-flip** — bypass the DWM
  compositor. **~12-16ms**, the single biggest lever. Path exists
  ([main.rs:1057](../native-client-tdw/src/main.rs#L1057)); default launch is a
  composited 640×480 window ([main.rs:876](../native-client-tdw/src/main.rs#L876)).
  Gate: PresentMon must show `HardwareIndependentFlip`, not `Composed`.
- [ ] **A2 · `desired_maximum_frame_latency` 2 → 1** + DXGI waitable swapchain
  ([main.rs:88](../native-client-tdw/src/main.rs#L88)). Drops one buffered present.
  ~3-6ms (part of P2).
- [ ] **A3 · Event-driven render wake** — replace the ~236fps `request_redraw` poll
  ([main.rs:1097](../native-client-tdw/src/main.rs#L1097)) with a net-thread wake on
  decode; kills the 0-4.2ms pickup jitter + ~75% of GPU/power. Pair with A2 or it
  undershoots (do NOT naive present-on-decode — that re-adds a stale frame).
- [ ] **A4 · Shrink the render-thread `FrameDecoder` lock** — held across
  parse+GPU-upload ([main.rs:192-234](../native-client-tdw/src/main.rs#L192)),
  stalling the net thread + same-task recv. Double-buffer / copy-refs-under-lock.
  ~2-3ms.

## TRACK B — Bare-metal host tuning (RISE-3, the jitter foundation run-ahead needs)
Provision-time. These make frame times rock-steady, which is the prerequisite for
run-ahead's prediction + rewind to be reliable.

- [ ] **B1 · Pin the SH4 emu thread to an isolated core** (`isolcpus` + `nohz_full` +
  `sched_setaffinity`) — no timer tick, no other process, no scheduler jitter.
- [ ] **B2 · `governor=performance` + disable C-states** — clock locked at 4.8GHz, no
  wake-from-idle latency. (Impossible on Vultr KVM; trivial on bare metal.)
- [ ] **B3 · IRQ affinity / RPS/XPS** — steer NIC softirqs OFF the emu core.
- [ ] **B4 · Dedicate cores**: emu (isolated) · render/publish · WS send thread · rest
  for rooms / fan-out tier.
- [ ] **B5 · Keep the existing tuning** (SO_BUSY_POLL, SCHED_FIFO, mlockall, DMA
  latency 0, GRO/LRO off) — already applied; verify it carries to the new box.

## TRACK C — Physics-floor attackers (the real latency, needs B done first)
- [ ] **C1 · Run-ahead depth=1, done right** (RA_SHORT_LEG3 + dirty-page rewind).
  **Hides the 16.7ms sim frame** — the largest single latency win. Determinism-PROVEN
  (STATEVF 1177/1177), parked net-negative on the VPS. RISE-3's isolated 4.8GHz core
  gives it the headroom; B1/B2 give it the timing stability. **Requires the Tier-0
  probe (Track E) to measure.**
- [ ] **C2 · Client-side input phase-alignment** — collapse the 8.3ms maple sample
  phase toward ~1ms by pacing sends to land just before the server's vblank. **Newly
  viable BECAUSE the bare-metal box has a PREDICTABLE vblank phase** (locked clock,
  isolated core, no jitter) — this trick is unreliable on a jittery VPS. ~5ms.
- [ ] **C3 · Force-synchronous maple-DMA experiment** (re_kb/75) — does MVC2's
  response move TRIG+2 → TRIG+1? Up to another frame if real; re-prove determinism.
- [ ] **C4 · West-coast edge** — Vint Hill covers US-East; a West node closes the
  ~60ms transcontinental gap for West players (topology is the only RTT lever).

## TRACK D — Transport + bandwidth
- [ ] **D1 · QUIC/WebTransport datagram downstream** for the direct player — kills TCP
  head-of-line blocking (~20-40ms p99 on lossy links). ~90% built in the relay; NOT a
  kernel-bypass problem. The real downstream lever.
- [ ] **D2 · Dedicated send thread** for the competitive TDW conn (CLAUDE.md TODO,
  ws_server.cpp:1836) — ~5ms **jitter**; affordable with 12 cores. Audit
  `_connMutex`/`_queue`/`_relayTree` first.
- [ ] **D3 · Columnar + XOR-residual of the split-position corners** — ~20-40% steady
  wire. Raw-byte XOR only (lossy residual fails the pixel gate).
- [ ] **D4 · Content-hash page IDs + persistent client page cache** — super-spike /
  repeat-super killer; pair with char-select pre-cache. 128GB makes the server-side
  cache free.
- [ ] **D5 · Server publish cleanups** — extend DMA force-dirty to kill the 8MB/frame
  VRAM memcmp (~0.8ms, mandatory pixel gate); gate the redundant legacy TA delta
  behind `!tdwOnly()`.

## TRACK E — The measurement gate (blocks sign-off on C, and the honest number for A)
- [ ] **E1 · Content-anchored button-to-photon probe** — drive a known on-screen
  change from a known input seq, timestamp when it lights the panel (external
  high-FPS camera tap, or a display-side content marker). The E2EP probe stops at
  `present()` and is invariant to sim frame N vs N+1 — it CANNOT see A1's compositor
  win or ANY of Track C. **Nothing in Track C gets signed off without E1.** (Track A
  can lean on PresentMon for the present-path slice in the interim.)

---

## The insane-engineering endgame (stack them)
On the RISE-3, the levers compose. Best-case button-to-photon for a US-East player:

```
  66ms today
  − A1 borderless flip         (~14)
  − A2/A3/A4 present path       (~7)
  − C1 run-ahead (hides sim)    (~16.7)
  − C2 phase-align input        (~5)   [needs bare-metal steady vblank]
  ─────────────────────────────
  ≈ 23ms  →  approaching the propagation + scanout floor
```
That's **sub-25ms button-to-PHOTON to a US-East player on a Dreamcast game over the
internet** — competitive-offline territory. The floor left is ~4-8ms RTT + ~9ms
monitor scanout (a faster/higher-Hz panel chips the latter). Everything above the
floor is engineering we can actually do.

## Execution order
1. **Track A now** (free, client-side, PresentMon-measurable) — start with A1+A2.
2. **Track E1** (the probe) — needed before Track C can be trusted.
3. **Provision RISE-3 → Track B** (isolation foundation).
4. **Track C1 run-ahead + C2 phase-align** (the physics attackers) — the big ones.
5. **Track D** in parallel throughout.
