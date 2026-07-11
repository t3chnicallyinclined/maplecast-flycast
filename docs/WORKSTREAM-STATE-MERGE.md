# WORKSTREAM — State-Wire Consolidation (fold `/replica-live` into `/ws`)

> **Locked 2026-07-10** by a 3-expert panel (flycast-internals · senior-re-generalist · gsta-verification-harness).
> Direction: KEEP render_frame (`?bodysrc=wasm`) + `bodytex=local` (the sprite machine was rejected on
> fidelity, re_kb/74). Fold the `/replica-live` :7212 state feed INTO the main `/ws` ZCS2 wire — one socket,
> one vframe, delta-coded. Motto: overkill; ludicrous low latency + low bandwidth at every step; every claim MEASURED.
>
> **SCOPE (user, 2026-07-10): this is a CLEAN + OPTIMIZE workstream, NOT a fidelity fix.** The current render
> (render_frame + bodytex=local) is liked as-is. Priority = consolidate the two sockets into one, shrink the
> wire, cut latency — **without regressing a single pixel of the current render.** The Storm forward/back garble,
> the down-flicker, and the Sentinel rocket are a SEPARATE, DEFERRED "get-it-perfect" fidelity track (bottom of
> doc). The merge MAY fix the garble as a byproduct; we do NOT gate on that. **The one hard gate for every phase
> here: PIXEL-NEUTRAL — the optimization must not change what render_frame draws.** (So the temporal garble-gone
> gate P2b is deferred to the fidelity track; P2's acceptance is the frozen-frame pixel-neutral diff.)

## North star
Browser holds ONE WebSocket. Each frame = ONE ZCST packet, ONE `vframe`: char-stripped TA **+** render_frame
STATE, delta-coded. Pairing garble becomes structurally impossible; wire drops ~300KB→~0.4KB; button→pixel
drops by up to one full frame (16.67 ms) for free.

---

## What the panel CHANGED from the draft (do not skip)

1. **The delta's "byte-exact" proves the CODEC, not the plan.** A byte-run delta is lossless by construction;
   re_kb/73's 0-mismatch/9600-frames says nothing about pixels, merge coherency, or the garble. Never cite it
   for P2/P3/garble.
2. **"One vframe ⇒ no garble" is UNPROVEN — and there is an independent render_frame bug that survives any merge**
   (Sentinel rocket, single misplaced part, 13% of frames, emit-order — re_kb/74). The drift-class garble the
   ring-widen `5f66d5641` fixed is a *different* garble from the still-open Storm forward/back. **Assign the
   symptom to a layer BEFORE building P2** (Phase 0 bisection).
3. **The two-socket drift has a CONFIRMED structural cause:** `/replica-live` :7212 has **no `TCP_NODELAY`**
   (`replica_live.cpp:1460-1476`, vs `ws_server.cpp:1713-1716` which sets it) AND a different network path
   (nginx→loopback direct) AND a different drop policy than `/ws` (relay `SendQueue`). Body(V) and wire(V-1)
   ride different Nagle/queue regimes → drift. The merge removes this *by construction*.
4. **"same vframe ≠ same instant."** The `/replica-live` capture is deliberately multi-instant (char-pass
   snapshot pre-QueueRender vs bulk regions later; both stamp the same non-incrementing vframe). MEASURED
   2026-07-02 regression when mis-timed ("shatters moving bodies"). The merge must carry the char-pass snapshot
   machinery intact — call `serializeStateInner` from serverPublish right after `mc_replicaSnapshotCharPassTables()`
   (`mirror.cpp:2275`), which is STRICTLY MORE coherent than today's feed (fixes a latent 1-frame idxtab/rectab lag).
5. **Latency numbers were on the WRONG wire.** streaming-zstd 6.87→1.93 & hash-gate 56.9% were measured on the
   TA-mirror pixel wire, not the state wire. Need a real E2E button→pixel probe (none exists), one lever at a time.
6. **`savestate_load` is HARD-DISABLED** (`maplecast_control_ws.cpp:308`). Freeze source = `mock_hold_server.mjs`
   (replay one vframe). Caveat: replay can't reproduce live two-socket drift → the garble gate needs a LIVE dual capture.

---

## PHASE 0 — Free server-only wins (hours, zero client/relay change)

**0a — PAGEGATE (free bandwidth win, already built).** Flip `MAPLECAST_PAGEGATE=1` (`mirror.cpp:2645-2687`).
Full-page memcmp (STRONGER than an 8-byte hash — do NOT weaken it) drops forced-dirty pages byte-identical to
the client's shadow. Kills 56.9% of forced page ships (47% of raw bytes), server-only, wire-transparent.
Gate: live pixel diff unchanged over consecutive frames (PIXEL-NEUTRAL) + PAGEGATE telemetry shows forcedEqual skipped.

**0b — NODELAY on :7212 (one line, pure latency).** Add `no_delay(true)` init handler to `replica_live.cpp:~1463`
(copy `ws_server.cpp:1713-1716`). Removes up to ~40 ms of Nagle coalescing on small body frames on the current
two-socket path — also stops the current path from lying to our latency baseline while we build P1/P2. Gate: pixel-neutral.

**Gate to leave Phase 0:** both wins live-pixel-neutral over consecutive frames.
(The garble layer-assignment / pairDrift bisection moved to the DEFERRED fidelity track — not on this critical path.)

---

## PHASE 1 — Delta the state on `/replica-live` (contained, gated `MAPLECAST_STATE_DELTA`, default OFF)

Refactor `captureFrame` (`replica_live.cpp:842-951`) into pure `serializeStateInner(u32 vframe, vector<u8>& out)`
(no publish). Add keyframe-on-connect + byte-run delta of the ~294 KB dynamic span (`_dynTotal` @ `:515`; objpool
118KB + rectab 65KB + efxtmpl 86KB dominate and are near-static → ~150 changed B/frame). **Reuse the run encoder
at `mirror.cpp:2534-2569` VERBATIM** (u16 runLen clamp + 8-byte gap-merge — do NOT fork). Keep compress on the WS
thread (`:706`). **Force a keyframe whenever the dyn-region table SHAPE changes** (count/len) — a delta vs a
differently-shaped keyframe is corruption (regression bug #1 class).

**Gates (all exact-zero, over the FULL corpus — Phase-0 tooling below):**
- `delta_roundtrip.mjs`: `mismatchBytes == 0` AND `shapeChanges == keyframesEmitted`, on EVERY capture.
- `ta_md5.mjs raw` vs `reconstructed`: `diff` empty (render invariance).
- ≥3 frozen-frame pixel diffs (super-freeze, mid-combo, transition) via `mock_hold_server` → `diff_png_regions --tol 0`: `diffPixels 0, maxΔ 0, SSIM 1.0`.
**Corpus (NOT steady play):** match-load, round-start, KO+win-pose, tag-in, tag-out, super-freeze onset+release,
launcher/zoom, double-super, transition-to-results. **NOTE:** the fixed-region delta stays ~150 B, but the
VARIABLE tails (GFX ~1.3MB on tag-in, palette 32KB, PL3D, efxtmpl 86KB on super onset) produce multi-hundred-KB
frames — size the send buffer + off-thread compress for the p99/max, don't average them away.
Risk: LOW (loopback, gated; lossless can only mis-SIZE, not corrupt render). Rollback: flip the gate.

## PHASE 2 — Merge into `/ws` (four-parser; relay needs NO change)

- **Server:** call `serializeStateInner` from serverPublish **right after `mc_replicaSnapshotCharPassTables()`
  (`mirror.cpp:2275`)** — most-coherent instant. Append the `"STAT"` section **between `:2718` (dirtyCount patch)
  and `:2831` (frameSize patch)**, INSIDE the zstd inner payload (free window-sharing; relay forwards opaque).
- **Wire layout** (self-describing, older clients skip):
  `"STAT"(4) + flags u32 (bit0 KEY / bit1 DELTA) + vframe u32 + innerLen u32 + [KEY: serializeStateInner payload | DELTA: run-encoded vs last KEY]`.
- **Four parsers:** emit (mirror.cpp); **relay = NO change** (forwards ZCST opaque, `apply_dirty_pages` stops at
  page list — `protocol.rs:284-334`, `fanout.rs:283`); skip (both wasm bridges already stop after dirty pages —
  `wasm_bridge.cpp:529`, `maplecast_wasm_bridge.cpp:212`); extract+apply (`packages/renderer/src/wasm_bridge.cpp`
  + `web/webgpu-test.html`).
- **Client:** DELETE the `_bodyMerge` ring + the second socket. Move GFX/palette mark-shipped to
  unconditional-after-broadcast (drop the drop-old dance `replica_live.cpp:963-971`).
- **Bootstrap:** the relay's cached SYNC carries VRAM+PVR only (no main RAM / no STATE). Deliver the one-time
  static prefix (16MB RAM + GFX) in the STATE KEY or a companion one-shot on connect; a late-joiner picks up the
  next STATE KEY within the keyframe interval.
- **Coherency test (mandatory + negative control):** dump `vframe` in `serializeStateInner` and the char-pass
  `snapshotCharPassSlots`; assert equality over 600 frames. Negative control: deliberately move tiledesc read to
  serverPublish and confirm the KNOWN 2026-07-02 shatter reappears (proves the instrument isn't blind).
- **Ride the PVR-atomic-snapshot discipline** (regression bug #5) — STATE reads the same render-thread-owned arrays.
- **Drop landmine:** STATE rides inside the `LegacyDelta` frame; a relay drop under backpressure drops that
  frame's STATE delta → client MUST treat a `vframe` gap as "wait for next STATE KEY," never apply a stale delta.
  Relay auto-requests SYNC on drop (`fanout.rs:845`); aligned keyframe re-bases within ≤1s.

**Gate (this track):** P2a merge-is-PIXEL-NEUTRAL on frozen frames (`ta_md5` diff empty + `diff_png_regions --tol 0`
every region 0/0/1.0) — the merge must not move one pixel of the current render. Keep `/replica-live` standalone
alive as the rollback path THROUGH P2. Risk: MEDIUM.
*(DEFERRED to fidelity track: P2b "garble is gone" temporal gate — pair_skew / diff_temporal / positive control.
We ship the consolidation on pixel-neutrality alone; whether it also fixes the garble is verified later.)*

## PHASE 3 — Ludicrous low latency (measure each lever ALONE)

- **Compress off the render thread:** serverPublish hands `{dstStart,totalSize}` snapshot to a single-slot
  drop-old compressor thread (today it's inline @ `mirror.cpp:2920`). Removes `compressUs` jitter from the 16.67 ms budget.
- **Streaming zstd:** bind STATE KEY to `forceKeyframe||streamStart` (`mirror.cpp:2524/2944`); drop `_zResetEvery`
  300→60 (join ≤1s). Measure the ratio cost in `_bwlab` before committing.
- **Client:** decompress+apply STATE in the WS worker (off main thread); present-ASAP rAF (one socket = no pairing wait).
- **CHARSTRIP_PAGES** (`mirror.cpp:3051`, drops 84.1% of page bytes — the body VRAM the local decoder makes dead
  weight): enable ONLY after `bodytex=local` is the client default AND a consecutive-frame live pixel diff confirms
  local decode covers blocks {0x82,0x83,0x88,0x89}.
- **TCP_NODELAY:** already on `/ws`, relay, native client, audio; :7212 fixed in Phase 0.
- **Gates:** P3a lossless (`ta_md5` diff empty + `diff_png_regions --tol 0` + hash-gate soak `hashMismatches 0 / ≥100k`).
  P3b `e2e_probe.mjs` button→pixel median/p95/max + 3-stage split + Mbps, BEFORE/AFTER each lever alone. Floor: p95 no regress, Mbps drops. Risk: MEDIUM.

## PHASE 4 — Retire `/replica-live` standalone → gate to offline-validation-only. Risk LOW.
Only after the merged path holds the live consecutive-framebuffer pixel gate over a sustained run (retiring earlier removes rollback).

## DEFERRED — Fidelity track ("get it perfect", AFTER the clean/optimize track above)
Not on this workstream's critical path (user: "don't worry about the garble now… I like where it is").
Revisit once the wire is consolidated + optimized:
- **Storm forward/back garble + down-flicker** — run the pairDrift bisection (garble vs `BODY.pairDrift`, + the
  NODELAY-on-:7212 A/B) to assign it to pairing (fixed by the merge) vs render_frame emit-order (independent). The
  merge may have already fixed it — verify with the temporal garble-gone gate (pair_skew / diff_temporal + positive control).
- **Sentinel rocket single misplaced part** (render_frame emit-order divergence, 177/1359 frames) — the cleanest
  live probe of emit-order vs pairing.
- Verification: consecutive `MAPLECAST_GSTA_SHOT` framebuffers over a Storm forward→back reversal + crouch, diffed
  vs the engine mirror on the same vframes. A single still will call a flickering render "clean."

---

## Gate/tooling build list (Phase 0 — none exist yet; no phase ships without its gate green)
1. `ta_md5.mjs` — per-frame TA md5 from a `.mcrr` (reuse `emit_live_ta.mjs`/`_tx_detect.mjs` RAM-splat loader + `render_frame_node`).
2. `delta_roundtrip.mjs` — encode→decode→memcmp + keyframe-on-shape-change assert.
3. `diff_png_regions.mjs` — region masks (HUD/body/effect/stage bboxes) + SSIM + strict `--tol 0`.
4. `GSTA_SHOT` vframe stamp — `mainui.cpp:~211`, put `mirrorCtx.frameNum` in the filename (A/B must be vframe-matched).
5. `pair_skew.mjs` — from a live dual capture, `skew=bodyVframe−taFrameNum` distribution.
6. `diff_temporal.mjs` — consecutive-frame + per-part X-jump detector (garbleFrames, flickerMax).
7. `gate_check.mjs` — orchestrator: capture→(encode/merge)→replay→diff → one PASS/FAIL + exit code.
8. `e2e_probe.mjs` — button→pixel (super-freeze luma-spike stimulus, 3-stage decomposition), the FIRST E2E number on this path.

## Quantified latency wins (INFERRED — confirm with `e2e_probe`)
- Remove :7212 Nagle: up to ~40 ms tail on small body frames.
- Remove body/wire pairing: up to 16.67 ms (one frame) off the worst case — the single largest win, free from the merge.
- Compress off render thread: `compressUs` jitter off every frame's critical path.
- PAGEGATE (56.9% forced pages) + CHARSTRIP_PAGES (84.1% page bytes) → smaller frames → lower serialization + relay-queue tail.

## The acceptance gate for THIS track (never skip): PIXEL-NEUTRAL
Every clean/optimize phase must prove it did not change what render_frame draws — offline `ta_md5` diff empty +
`diff_png_regions --tol 0` (every region 0/0/SSIM 1.0) on frozen frames, PLUS a consecutive-frame live pixel diff
vs the pre-change build (the melt/flicker class is only visible in consecutive LIVE pixels — a single still hides it).
We are NOT trying to improve the render here; we are trying to NOT touch it while making the wire smaller and faster.
(Improving it = the deferred fidelity track.)
