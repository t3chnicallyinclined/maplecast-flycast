# MapleCast Button-to-Photon Latency Teardown (2026-07-17)

Full-pipeline analysis by a 12-agent engineering pass: 8 parallel per-stage code
traces → synthesized budget → 3 adversarial optimization angles (extreme-latency,
concurrency, bandwidth) + a dedicated kernel-bypass study. Every number is
reasoned from cited code; `physics_floor` = inherent to MVC2 / real networking /
the display, everything else is removable implementation overhead.

## The headline: the number we've been quoting is not button-to-photon

**The measured ~41ms (F1 "press → present") stops at `frame.present()`
([main.rs:247](../native-client-tdw/src/main.rs#L247)) and EXCLUDES the entire
DWM compositor + monitor scanout tail.** Real button-to-photon is **~66ms**.
The E2EP probe is blind to the single biggest *removable* chunk — and worse, it
is provably invariant to whether the sim shows frame N or N+1, so **it cannot
even measure run-ahead.** Every "we improved latency" sign-off against E2EP that
touches the display path or the game clock is measuring the wrong thing. This is
the project's canonical false-win, and it is baked into the only instrument we
have.

## The budget

| | ms |
|---|---:|
| **Total button-to-photon (typical)** | **66.4** |
| Physics floor (irreducible) | 40.0 |
| Removable overhead | 26.4 |
| Best achievable (all removable clawed back) | ~24 |
| Worst (WiFi jitter / super-freeze) | ~120 |

### Per-stage (the serial critical path — one input edge)

| Stage | ms | Floor? | What it is |
|---|---:|:---:|---|
| 1. Capture + UDP send | 2.5 | mostly | Controller USB/XInput report interval (~2ms, up to 8ms on a 125Hz pad) — a *hardware* floor. Only the ~0.5ms poll-sleep ([input.rs:140](../native-client-tdw/src/input.rs#L140)) is ours. **Invisible to E2EP** (stamped after send). |
| 2. Network up → :7100 latch | 3.06 | ✅ | ~98% one-way propagation to NYC. All code work (busy-poll, FIFO 55, mlock, atomic latch) is <60µs. Raw UDP, no Nagle/relay. |
| 3. Maple sample phase | 8.3 | ✅ | MVC2 samples the pad **once per vblank** ([spg.cpp:129](../core/hw/pvr/spg.cpp#L129) → maple_if → single atomic load). Async input waits uniform 0–16.7ms (mean 8.3) for that read. |
| 4. Sim reaction | 16.7 | ✅ | Reads input frame N, renders consequence frame N+1 (lagProbe pinned TRIG+2, 8×). Irreducible without patching the ROM — **only run-ahead hides it.** Super-freeze adds up to ~167ms of *designed* freeze (not counted). |
| 5. Server publish | 1.25 | ✗ | Runs on the render thread (overlaps next SH4 frame). Dominated NOT by the ~300µs TDW encode but by the **8MB VRAM dirty-page memcmp (~0.8ms)** every frame + a redundant legacy TA delta a TDW client never reads. |
| 6. Network down → client | 3.5 | ✅ | ~3ms one-way propagation. Direct flycast :7200, `TCP_NODELAY` set (Nagle disarmed). HoL risk: all writes serialize on one asio `_wsThread`. |
| 7. Client decode | 0.05 | ✗ | Streaming-zstd + dict-ref reassembly, ~50µs (~330µs on page-heavy supers). Nowhere near bandwidth-bound. |
| 8. Render + present → photons | 31 | mostly ✗ | Only ~9ms (vblank quantization + scanout) is floor. The other **~22ms is removable**: ~16ms DWM compositor because we launch a **640×480 window** (AutoNoVsync can't independent-flip in a window), ~2ms poll-not-wake pickup, ~3ms `rebuild()` decode inline in the present path holding the shared lock. **The 41ms measurement stops before this whole tail.** |

**Critical path:** press → HID+poll+UDP → prop up → *maple phase wait* → *sim N→N+1* → publish → prop down → decode → render+present → *DWM compose* → *vblank+scanout* → photons. The four irreducible nodes are 8.3 (sample) + 16.7 (reaction) + 6 (RTT) + ~9 (vblank/scanout) = **40ms floor**. Everything else is overhead.

## Optimization roadmap (ranked, deduped across all three angles)

### Tier 0 — the gate you must build FIRST
**Content-anchored button-to-photon probe.** Nothing below `present()` and nothing
about the game clock is visible to E2EP. Drive a deterministic on-screen change
from a known input seq and timestamp when it actually lights the panel (external
high-FPS camera tap, or PresentMon for the compositor slice). *Without this, every
win below is unfalsifiable — do not sign off on "feels snappier."*

### Tier 1 — CLIENT DISPLAY PATH (~18–27ms, determinism-safe, no hardware needed)
| # | Lever | Save | Effort | Notes |
|---|---|---:|---|---|
| P1 | **Borderless-fullscreen DXGI independent-flip** (bypass DWM) | **12–16ms** | LOW | The single biggest reclaim. Path already exists ([main.rs:1057](../native-client-tdw/src/main.rs#L1057)); default launches composited-windowed ([main.rs:876](../native-client-tdw/src/main.rs#L876)). **Verify with PresentMon** (HardwareIndependentFlip vs Composed) — this is INFERRED. |
| P2 | Event-driven render wake + DXGI waitable swapchain, max_frame_latency 2→1 | 3–6ms | LOW-MED | Replace the ~236fps `request_redraw` poll ([main.rs:1097](../native-client-tdw/src/main.rs#L1097)) with a net-thread wake; drop one buffered present. Pair BOTH or it undershoots. |
| P3 | Shrink render-thread `FrameDecoder` lock / move `rebuild()` decode off present thread | 2–3ms | MED | Lock held across parse+GPU upload ([main.rs:192-234](../native-client-tdw/src/main.rs#L192)) stalls the net thread + same-task recv. Double-buffer the decoder. |

### Tier 2 — attack the physics floor (bigger risk / hardware)
| # | Lever | Save | Notes |
|---|---|---:|---|
| P4 | **Run-ahead depth=1, done right** (RA_SHORT_LEG3 + dirty-page rewind) | **16.7ms** | The ONLY lever on the sim-reaction frame. Determinism-PROVEN (STATEVF 1177/1177). Parked net-negative — the 3-leg cycle + super-frame rewind stall cost more than the frame hid. **Needs the Tier-0 probe to even see the win, and CPU headroom (→ dedicated server).** |
| P5 | Client-side phase alignment of input sends to the server vblank | ~5ms | Attacks the 8.3ms sample phase. Client estimates server vblank phase from frame cadence and paces sends; or reuse the built stamped-input path. Tail-jitter risk. |
| P6 | Force synchronous maple-DMA experiment (collapse the emulator's +1) | up to 16.7ms | re_kb/75 open experiment. Falsify with lagProbe at TRIG 60/120: does response move TRIG+2→+1? Schedule change ⇒ determinism must be re-proven. |
| P7 | Edge input+render node closer to the player (topology) | ~3ms | The only lever on the 6ms RTT. Pillar-5 machinery exists; route the competitive player to the nearest node running the full sim. |
| P8 | Kill the 1ms poll-sleep + 1kHz/raw-HID capture | 2–3ms | Sub-ms/event-wake input; raw-HID reader cuts the controller report interval. |

### Server-side (jitter + drop-avoidance, small mean)
- Dedicated send thread / multi-threaded io_context for the competitive TDW conn (~5ms **jitter**, the CLAUDE.md TODO at ws_server.cpp:1836) — audit `_connMutex`/`_queue`/`_relayTree` first.
- Extend DMA force-dirty to track CPU VRAM writes → kill the 8MB/frame memcmp (~0.8ms). **Mandatory pixel gate** — this is exactly where a "fix" silently ships a different page set.
- Gate the redundant legacy TA delta behind `!tdwOnly()` (~0.05ms, trivial).

## Bandwidth roadmap (lossless only — must re-pass the pixel gate on a frozen replay)
Warm players frame ≈ 0.347 Mbps ≈ 723 B post-zstd. Cost centers: the 44B/moving-vertex split-position floats (dominant in motion), the 132B camera block, the page section (drives 0.9 Mbps super peaks).
1. **Columnar + XOR-residual of the split-position corners** — biggest steady-state win, ~20–40% (0.07–0.15 Mbps). Raw-byte XOR only; a quantized residual FAILS the pixel gate.
2. **Content-hash page IDs + persistent client page cache** — the super-spike / repeat-super killer. Wide hash + verify-on-materialize (collision guard). Pair with char-select pre-cache.
3. Camera XOR-delta (~0.01–0.03 Mbps on pans); strip E2EP tail + PVR-ship-on-change; sub-page dirty-range diffing for palette/flash.
**Process gate:** measure every claim on a FROZEN .mcrec replay (toggle one flag, diff bytes) — NEVER a live A/B across different match moments (a known false-win trap here).

## Rejected — do not re-propose without a gate proving otherwise
- **Sub-frame RAM injection into 0x8C200BA8** — breaks determinism (write not in the recorded input→frame map ⇒ .mcrec/state-sync/migration diverge). MVC2 single-reads it once/vblank anyway.
- **Naive present-on-decode at 60fps** — RE-ADDS a stale frame vs the current spin. The correct form is P2 (wake + waitable swapchain).
- **Client-side game-state speculation** — the client has no local sim; motion extrapolation mispredicts on every hit/block. The only real version is relocating the sim (P7).
- **Kernel bypass (AF_XDP/DPDK/io_uring)** on the virtio VPS — see the kernel study: the in-tree XDP module is dead code that writes the *wrong* latch; virtio kills zero-copy; the path is already busy-polled/FIFO/mlock'd/NODELAY. Sub-µs win inside a 1.7ms server slice of a ~66ms budget. Real downstream lever is QUIC datagrams (semantics, not bypass).

## What this means for hardware
A dedicated bare-metal box (see the Rise/RISE options) does **not** shrink the 40ms
floor's propagation term — that's geography. It unlocks exactly two things on this
list: **P4 run-ahead** (needs an isolated high-clock core with headroom for the
2–3× speculative sim — impossible to make net-positive on a shared 2-vCPU virtio
guest) and the **jitter-control** (isolcpus/nohz_full/governor/pinning) that
run-ahead's prediction accuracy depends on. The Tier-1 client wins (P1–P3, ~18–27ms,
the largest reclaim) are **free** — pure client code, no hardware.

## Recommended order
1. **Build the Tier-0 content-anchored probe.** Non-negotiable — it's the only way to see the biggest wins.
2. **P1 borderless-fullscreen** (verify PresentMon) — ~12–16ms, low effort, code exists.
3. **P2 + P3** client display path — another ~5–9ms.
4. Then the physics attackers: **P4 run-ahead** (with the probe + dedicated-server headroom) and **P7 topology**.
5. Bandwidth track in parallel (columnar split-position + content-hash pages).
