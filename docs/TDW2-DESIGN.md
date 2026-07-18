# TDW2 — the loss-tolerant, ultra-thin geometry wire

The datagram-native successor to the streaming-zstd TDW1 wire. Design goal: each
per-frame geometry update is **independently decodable** (survives datagram loss),
**tiny** (≤ the current ~540 B/frame), and **loss-tolerant in state** (a lost frame
never corrupts the dictionary or VRAM). Bulk state (SYNC, dict snapshot, pages)
rides the reliable QUIC stream with zstd; per-frame geometry rides datagrams with
**semantic** coding, no stateful compressor.

Grounded in the two expert reviews (2026-07-18): raw inner ≈ 18 KB/frame
(~8.6 Mbps); streaming zstd → 540 B by LZ-matching the **~92% byte-identical**
prior-frame ref array; INDEP (per-frame zstd reset) → 6052 B because it throws
that temporal match away. The whole game is *removing temporal redundancy without
a stateful window.*

## Frame structure — I/P (keyframe/delta), like a video codec
- **Keyframe (I):** full geometry state, on the RELIABLE stream, every N frames
  (start N≈60 = 1/s; tune). Independent. Carries any new dict blocks + pages.
- **Delta (P):** coded against the **last keyframe** (never the prior delta), on a
  DATAGRAM. Losing a P never breaks later Ps (they rebase on the I). Losing an I
  costs ≤ N frames until the next I. Header carries `keyframeId` so a P names its
  base; a P whose base is unknown is skipped, not fatal.

This alone gives loss-tolerance. The tricks below make each P *tiny*.

---

## The trick bag (ranked by impact)

### 1. Factor out the camera — the biggest math win
The 44 B/vertex today are **screen corners** = `M · world_pos` (M = the per-frame
camera M1·M2, already shipped once in TDW1 bit3). When the camera pans/zooms, every
screen corner changes → the "92% churn is positions" finding. But the **world
positions barely change** (a standing fighter's world pos is constant while the
camera moves).

**Send world/model positions + the one shared camera matrix; the client reprojects**
— exactly what the local-stage path already does (`stage.rs` reprojects the bake
through M1·M2). Camera motion becomes a single 132 B matrix instead of churning
every vertex. A panning-camera neutral frame collapses from thousands of changed
bytes to ~0. This is the single highest-leverage change; RE cost = extract the
pre-`ftrv` positions (the Frame Oracle / render_replica work already reads them).

### 2. Lattice-quantize positions
MVC2 positions sit on a **coarse lattice** (measured: X in steps of 5/3, Y in
480/224 — `docs/TA-DICT-WIRE-PLAN.md`). A screen coord is ~9 bits X × ~8 bits Y ≈
**2 bytes** vs 44 B of raw floats. And a sprite is a rectangle → send **anchor +
size (2 coords)** not 4 corners (8 coords) → another 4×. Pure math, lossless at
pixel granularity (the gate proves it).

### 3. Content-hash dictionary IDs — kills the state-corruption class
Today dict IDs are **insertion order**, so a lost "news" frame permanently desyncs
the id space. Make IDs = **content hash** (FNV/xxh of the block). Now IDs are
order-independent: a client that missed a block just doesn't have that hash yet,
requests it (or gets it on the next keyframe) — no id-space corruption, ever.
Bonus: the same hash across sessions ⇒ **persistent disk cache** (a returning
player ships ~no dict). This is the queued TDW2 protocol rev
(`docs/TDW-PROTOCOL.md:114`). It's what makes new-block frames *datagram-safe*.

### 4. Sparse ref delta
A P-frame's refs = **only the changed (slot → ref) pairs** vs the keyframe. With
~92% identical, that's ~8% of ~2700 blocks ≈ 200 entries. Encode as a
changed-bitmap (2700/8 ≈ 340 B, itself mostly-zero → RLE to tens of bytes) + the
changed refs, or a sparse index list. Delta-code the ref *values* too (adjacent
parts often reference adjacent dict IDs → small varints).

### 5. Dead-reckoning prediction — the "send almost nothing" trick
Positions move smoothly (a fighter drifts a few px/frame). Run the **same
constant-velocity predictor on both ends**: client extrapolates `x(t) ≈ x(t₀) +
v·Δt`; server sends only the **residual** (actual − predicted). Smooth motion ⇒
residuals ≈ 0 ⇒ a byte or two per moving object. This is entity extrapolation
from netcode, applied to sprite corners. Rebased per keyframe so it's loss-tolerant.

### 6. Columnar / structure-of-arrays
Group homogeneous fields (all ref-indices, then all X, then all Y) before delta +
quantize. Homogeneous columns delta-code and pack far better than interleaved
records. (The bandwidth expert flagged this for split-position too.)

### 7. Forward error correction for keyframes
Keyframes are the one critical loss point. Because the wire is **so thin**, we can
afford real redundancy: **fountain/rateless codes (LT/Raptor)** — the client
decodes the keyframe once it has *enough* packets, regardless of *which* were lost,
with no retransmit round-trip. Or simple Reed-Solomon (K data + M parity, recover
any K). Keyframes become loss-proof for pennies of bandwidth. (P-frames need no FEC
— they self-heal on the next frame.)

### 8. Trained static entropy coder (only if still needed)
If after 1–6 the residuals aren't tiny enough, a **static range/arithmetic coder
with a shipped-once trained frequency table** squeezes the entropy *without* a
stateful window (unlike zstd streaming). Likely unnecessary once camera-factoring +
dead-reckoning land — the residual is near-zero.

---

## What stays on zstd + the reliable stream (genuine bulk)
- **Connect SYNC** (8 MB VRAM → 0.5 MB): real texture data, no semantic shortcut.
- **Dictionary snapshot / keyframe dict** (content-hash blocks): compress the block
  bytes.
- **VRAM pages** (texture deltas): cumulative, must be reliable + compress well.

zstd earns its keep on bulk; it just doesn't belong on the per-frame hot path.

---

## The convergence worth naming
Stack tricks 1 + 3 + 5 and TDW2 becomes: *ship the objects' world positions (mostly
unchanged), let the client reproject through the shared camera and extrapolate
motion, reference geometry by content hash.* That is **within arm's reach of the
GSTA semantic wire** (ship game-state, reconstruct client-side) — the theoretical
floor. TDW2 gets ~all of GSTA's thinness while still rendering from real TA
(no sprite-machine dependency). Keep that convergence in view; don't reinvent it.

---

## Build sequence — gated, one variable at a time
The experts' mandate: **every step validated on a frozen `.mcrec` replay + the
reference decoder, byte-exact / pixel-exact, with scripted datagram-drop injection.
NEVER a live A/B.** Loss-recovery is a temporal symptom — the gate must diff
consecutive frames of the QUIC client framebuffer vs the engine mirror over an
induced-loss link.

- **G0 — the gate itself (build FIRST).** Frozen `.mcrec` replay server + a
  drop-injecting bridge (`tc netem` / an in-bridge drop-1-in-N) + the pixel-diff
  harness (client framebuffer vs engine mirror). Nothing below earns sign-off
  without it. Also re-measures the honest jitter (S0 from the review).
- **S1 — I/P frame structure** (keyframe reliable, delta datagram, keyframeId
  rebase). Gate: induced loss on P-frames → skips cleanly, never desyncs; loss on
  I → recovers ≤ N frames.
- **S2 — semantic reliability split** (server flags new-block/page frames → reliable
  stream; pure geometry → datagram). Gate: drop-injection shows zero dict/VRAM
  corruption.
- **S3 — camera-factored world positions + lattice quantization** (trick 1+2). Gate:
  pixel-exact vs mirror; measure B/frame drop, especially on a camera-pan segment.
- **S4 — sparse ref delta + dead-reckoning residuals** (trick 4+5). Gate: pixel-exact;
  B/frame → target ≤ 540.
- **S5 — content-hash dict IDs** (trick 3): new-block frames become datagram-safe;
  persistent cache. Gate: news-loss no longer corrupts.
- **S6 — FEC on keyframes** (trick 7), only if G0's loss measurement warrants.

Expected endpoint: independent per-frame deltas at **≤ the current 0.3 Mbps**,
**loss-tolerant**, on QUIC datagrams — the correct low-jitter wire, provably.
