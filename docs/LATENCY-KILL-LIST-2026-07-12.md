# Latency Kill List — Button-to-Pixel (2026-07-12)

Four-expert audit of the full input→pixel path on the shipping checkpoint config
(docs/RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md). Every item carries the segment report's
file:line citations — this is the ranked synthesis. E2E today ≈ ~80ms browser button-to-pixel;
the game's own floor is ~33ms (frame latch + MVC2's measured 1-frame internal lag).

## The path (measured/attributed)

```
press → poll(≤1ms) → /play WSS → nginx → :7210 → UDP :7100 → atomic latch
  → [WAIT 0-16.7ms for vblank latch — browser phase-align lands ~3-5ms]
  → SH4 latches; MVC2 acts +1 frame (16.7ms, ROM-internal, measured)
  → [A1: ~9-12ms audio-pacing sleep currently sits latch→publish!]
  → frame emulates (~2-5ms) → STARTRENDER → serverPublish
  → [B: diff ~1ms + legacy compress + ZCS2 z9 compress ~0.6-1ms]
  → loopback → relay → nginx TLS → internet (~RTT/2≈10ms, median tight)
  → worker fzstd (pipelined) → main-thread decode/parse/merge (~3-8ms)
  → [D1: rAF wait ~8ms avg] → WebGPU submit → [D2: compositor 16.7-33ms INFERRED]
```

## MASTER RANKED LIST

| # | Cut | Est. saved | Effort/Risk | Where |
|---|-----|-----------|-------------|-------|
| 1 | **A2 run-ahead depth=1** — MVC2 internal lag is exactly 1f (measured); running the visible frame with the just-latched input is mispredict-free. Needs per-frame savestate + 2× emulate on 2-vCPU — MEASURE dc_serialize first | **16.7ms** | HIGH | server emu loop |
| 2 | **A1 frame-delay pacing** — headless is paced by NULL-AUDIO 11.61ms sleeps, NOT vblank; ~14ms wall sits between latch and publish (confirmed by code comment). Move the sleep BEFORE the latch (gate exists: MAPLECAST_BYPASS_AUDIO_PACING); sleep→latch→burst→publish | **9-12ms** | MED (auto-margin + DUMP_TA determinism gate) | audiobackend_null.cpp, maple_if.cpp:48 |
| 3 | **D1 immediate present** — DBG.immediateRender exists, default OFF (webgpu-test.html:2558); vsync path waits ~8ms avg AND drops frames (latest-wins). Flip default; radio = opt-out | **~8ms avg / ~15ms p95** | TRIVIAL | client |
| 4 | **D2 compositor overlay promotion** — opaque canvas + fullscreen → DirectComposition hardware overlay. WebGPU has no desynchronized mode (spec). MEASURE with PresentMon first; 120/240Hz display = free halving | **0-16ms** | LOW (measure-first) | pvr2-renderer init |
| 5 | **B1 un-hitch the 60s periodic SYNC** — every 3600 frames: inline 8MB memcpy + zstd-3 of 8.4MB + direct send ON THE PUBLISH THREAD = ~30-70ms stall/min for every viewer; SH4 blocks behind it. NO_SCENE_SYNC does NOT gate it. Load-bearing for relay joiner cache → move build+compress off-thread, keep bookkeeping ordered | **kills a 30-70ms hitch/min** | MED | maplecast_mirror.cpp:2795-2836, ws_server.cpp:1853 |
| 6 | **B5 silent RAW fallback** — legacy compressor dst buffer is 256KB but scene transitions ship 1.2-2.2MB inner → zstd overflow → SILENTLY broadcasts multi-MB RAW frames (relay marks Critical, undroppable) = episodic 10-100ms spikes at round starts. Size the buffer + LOG the fallback | **kills 10-100ms episodic spikes** | LOW | maplecast_mirror.cpp:898, compress.h:96-104 |
| 7 | **B3 ZSTREAM level 9→3** (keep windowLog=24 — the WINDOW does the stage dedup, not the level). Lab-measured: z3=0.110ms vs z9=0.472ms/frame; bandwidth 1.47→1.93 Mbps | **0.3-0.7ms/frame** | ZERO (env var) | headless.env |
| 8 | **C1 control_only for players** — the /play input socket receives the FULL video broadcast + SYNCs the player page silently discards = a second ~3-6Mbps ghost stream on the player's downlink. ONE LINE (server support exists, ws_server.cpp:1134) | **p99 jitter for players** | TRIVIAL | ws-connection.mjs onopen |
| 9 | **B4 legacy ZCST leg** — (a) reorder ZCS2-first (~0.1-0.3ms earlier delivery, no data dep); (b) delete the relay's dead per-frame decompress+apply (cache it writes is NEVER read — confirmed) + fixes the ZCS2→apply_dirty_pages misparse lottery (C3); (c) demand-gate legacy compress on legacy-client count; endgame: migrate king.html to ZCS2, retire ZCST | **0.3-0.8ms + relay CPU** | LOW→MED | mirror.cpp:3011, fanout.rs:217-283 |
| 10 | **B2 VRAM diff memcmp** — 16MB memory traffic/frame (8MB live+shadow reads) ≈ 0.8-1.5ms before both compress legs. Hash-gate per page, =measure mode first (PAGEGATE pattern) | **0.5-1.2ms** | MED (stale-texture risk) | mirror.cpp:2677-2725 |
| 11 | **D3-D7 client main-thread** — meter the unmetered mid-segment (STM2+body+btex, 1-3ms, NO HUD number counts it); kill the _gfxTail 16MB heap copy (2-6ms tag-in spikes); persistent texture bind groups (texBGs.clear() premise stale, 0.3-1.5ms); elide applyFrame input copy (dupes 6MB super frames); palette-bank-scoped invalidation (hit-flash decode storms) | **3-8ms + spikes** | LOW-MED | webgpu-test.html, pvr2-renderer, texture-manager |
| 12 | **C4 input over WT datagram** — ~90% built (gamepad.mjs prefers it; relay webtransport.rs forwards to :7100); dead only because wtUrl:null was set for VIDEO reasons. Verify prod WT listener | **20-40ms p99 on lossy links** | MED (ops) | webgpu-test.html:2620 |
| 13 | **C5 relay staleness cap** — Critical backlog cap = 64MB ≈ 170s of stale frames before pressure; add >250ms-backlog disconnect | tail hygiene | LOW | fanout.rs:467 |
| 14 | **A3/A4/C7/B7 micro** — phase-align margin 3-5→1-2ms (after A1); ACK-after-store; WS-input direct injectInput() (skip loopback UDP hop); NODELAY on 7212 + relay upstream | ~2-3ms + µs | TRIVIAL | various |

## THE E2E PROBE (build FIRST — segment C specced it; nothing today measures press→present)
- Wire A: /play input 4→8B (+seq u32); input server stores lastClientSeq at latch.
- Wire B: ZCS2 trailing 'E2EP' section: frameNum + t_latch_us + t_publish_us + latchedClientSeq[2] (~32B).
- Client: log t_send per input, t_onmessage/t_decoded/t_submit/t_present per frame; clock-sync via 1Hz control-WS ping (min-RTT median).
- Number: t_present(first frame with latchedSeq ≥ seq) − t_send(seq), decomposed into 5 segments that MUST sum to the direct measurement ±2ms.
- Gates: MAPLECAST_E2E_PROBE=1 / ?e2e=1. Every cut ships with a before/after run.

## Landmines found (fix/avoid regardless)
- AF_XDP input module is DEAD CODE (zero call sites; would break input if wired — writes legacy kcode[] not _slotInputAtomic). OPTIMIZATION-PLAN's "✅" is stale.
- MAPLECAST_LATENCY_PARITY deliberately delays the faster player (off; don't enable while measuring).
- Relay ZCS2→apply_dirty_pages misparse (see #9b) — correctness, not just perf.
- :7212 still broadcasts per-frame bodies nobody consumes post-STM2 (C2) — server-side gate when STATE_MERGE=1.

## Opening salvo (recommended order)
1. **Tier 0 (this week, near-zero risk):** E2E probe → D1 flip → C1 one-liner → B3 env A/B → B5 buffer+log → B4a reorder.
2. **Tier 1 (measured, gated):** B1 off-thread SYNC → A1 frame-delay pacing (DUMP_TA gate) → D2 PresentMon measure → D3/D4 client shaves.
3. **Tier 2 (the big swing):** A2 run-ahead depth=1 (after dc_serialize timing proves it fits) → B2 hash-gated diff → C4 WT input → D10 worker/OffscreenCanvas.

Ceiling math: Tier 0+1 ≈ 25-40ms off; +A2 ≈ another 16.7ms → **~80ms → ~30-40ms browser button-to-pixel**, approaching the native client's 10ms + display.

## MEASURED BASELINE (2026-07-12, probe live — commit ce4352c82)
press-ema ~21-24ms = residual 10-30 (uplink + vblank-wait beat) + server 1.7 (latch->publish) + client 2.4-3.1 (recv->submit).
Caveats: +16.7ms MVC2 internal frame to VISIBLE response (~39ms); compositor scanout not included (tPresent=submit).
**A1 FALSIFIED** (est. 9-12ms; measured latch->publish = 1.7ms — the pacing slack already sits publish->next-latch).
Re-ranked: A2 run-ahead (16.7ms, THE lever) > B1 SYNC un-hitch (p99 ~90ms spikes) > D2 compositor (PresentMon).
Probe lesson: first run read min 0.7ms (below physical floor) — internal-vs-client seq-space bug; always gate on the floor.

## A2 GATE RESULT (2026-07-12, prod): **GO**
MAPLECAST_RUNAHEAD_MEASURE on prod: dc_serialize avg=1.80ms max=15.28ms(first-call artifact) size=26.7MB.
Budget: save 1.8 + 2x emulate (~4-10) + load (~2) ~= 8-14ms < 16.67ms. Run-ahead depth=1 FITS.
Implementation plan (next session): per tick T — latch i_T; emulate frame T HIDDEN (suppress publish/
audio/tape/publishFrameTick); dc_serialize -> S; emulate T+1 with repeated i_T and PUBLISH (pixels of
T+1 are fully determined by i_T — MVC2 acts +1 — so this is mispredict-free); dc_loadstate(S). Gates:
MAPLECAST_RUNAHEAD=1 default OFF; DUMP_TA determinism rig must stay byte-identical vs no-runahead on
the AUTHORITATIVE track; .mcrec/tape/lockstep assume 1 frame/tick — suppress on the hidden+preview legs.
Payoff: visible response ~39ms -> ~22ms; the E2E press number keeps its value but the presented frame
now CONTAINS the response.

## B1 DEPRIORITIZED (2026-07-12, measured): the periodic SYNC compress is **7ms live**
(`[MIRROR] 60s periodic resilience SYNC` -> `8.0MB -> 0.6MB (14.3x) in 7ms`), not the estimated
25-60ms — mostly-static VRAM compresses far faster than generic-throughput math. Total inline cost
~7-10ms once/60s = sub-frame blip. Off-threading it (builder thread + deferred epoch/shadow/VCACHE
bookkeeping, ordering-critical) is now bad ROI vs risk. Revisit only if a probe shows user-visible
hitches at the 60s marks. F2.1 + D4 + D7 SHIPPED 2026-07-12 (phase-trim servo, gfxTail span-copy,
bank-scoped palette invalidation).
