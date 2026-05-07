# MapleCast Rollback Prediction — Design Doc

> **Goal:** Make competitive online MVC2 over MapleCast feel like local play. Input-to-pixel latency goes from "physical RTT + emulation + render" to "local emulation + render only" — typically **5-8ms perceived** instead of 25-30ms, regardless of network.
>
> **The trick:** run a deterministic SH4 emulator on the client, predict the next frame locally and instantly, and use the server's authoritative input log to validate predictions. On mismatch, rollback and re-simulate (invisibly, in ~ms). This is GGPO-style rollback netcode applied to TA streaming. **No fighting-game streaming setup has this.**

---

## The architectural foundation (Phase 0 — done)

Rollback prediction stands on one empirical claim: **same flycast SH4 binary + same starting savestate + same input log → byte-identical TA buffer output, regardless of which machine runs it.**

We validated this five ways on 2026-05-07:

| Test | Pass | What it proves |
|---|---|---|
| Wire faithfulness | 48/48 | Server emit == client receive |
| Same-machine determinism | 30/30 | Run-to-run reproducibility |
| Cross-Linux-machine | 19/19 | EWR ↔ ORD same SH4 output |
| Cross-OS Linux ↔ Windows | 30/30 | Linux/GCC ↔ Windows/MSVC byte-identical |
| `.mcrec` replay determinism | 16/16 | Recorded inputs replay to identical TA |

**Consequence**: the rollback comparator can compare **inputs only** (~16 bytes/frame), never TA buffers. Same input log = same TA, by construction. Trivial implementation.

Full details + reproducibility methodology in [ARCHITECTURE.md "Cross-machine + cross-OS SH4 determinism"](ARCHITECTURE.md). Phase 0 status tracked in [OPTIMIZATION-PLAN.md](OPTIMIZATION-PLAN.md).

---

## The mental model

```
Server-side (unchanged from today):
  SH4 emulates → TA stream over WS → input log over WS → 253-byte gamestate snapshot
  (no new server-side work for V1 — only what's already shipped)

Client-side (NEW):
  Renderer thread (existing):
    reads VRAM, presents to display
                ▲
                │ writes
                │
  SH4 predictor thread (NEW):
    own VRAM context, runs SH4 in lock-step with server's frame schedule
    inputs:
      P1 = local gamepad (immediate, frame N)
      P2 = predict-as-last-known-server-input
    keeps a ring of N=8 savestates for rollback

  Comparator thread (NEW):
    receives server's authoritative input log (per frame)
    compares against our prediction at frame N
      match    → pop savestate from ring, free
      mismatch → trigger rollback in predictor:
                   - load savestate from frame N
                   - re-emulate forward with corrected P2 input
                   - up to current frame (~8 frames @ ~1ms each = ~8ms)
                   - renderer's next frame is the corrected one

  WS recv thread (existing, role expanded):
    still receives TA stream from server (used as ground truth for sanity)
    receives 253-byte gamestate (comparator uses for periodic divergence check)
    receives input log per frame (NEW use: comparator input)

  Input sink (existing):
    sends local input to server (already does this)
    ALSO injects local input into predictor thread (NEW path)
```

---

## Use case catalog

Every scenario the architecture has to handle, with where it lands in the phase plan.

### A. Cold match start (Phase 1)
Both players queued, hub assigns node N. Both clients connect.
1. Server captures one canonical savestate at "match-start frame"
2. Server sends savestate to both clients in parallel
3. Each client ACKs receipt
4. Server waits for both ACKs (prevents faster client from frame head start)
5. Server broadcasts "begin frame 1"
6. Both predictors start from that savestate, fully synced

**Build first.** Trivially testable — launch two `flycast --client` instances pointed at the same server.

### B. Reconnect, casual mode (Phase 1.5)
Player A drops (server detects UDP silence > 1000ms).
1. Server keeps emulating; treats A as "no input change" (sticky)
2. A reconnects ~5s later
3. Server pushes latest periodic snapshot (already published every ~60 frames by [maplecast_state_sync.cpp](../core/network/maplecast_state_sync.cpp))
4. A loads snapshot, predictor starts from there
5. A sees current state immediately; B never paused

Reuses existing state-sync infrastructure. Small addition on top of Phase 1.

### C. Reconnect, tournament mode (Phase 2)
Same trigger as B, but tournament rules.
1. Server detects disconnect
2. **Server pauses** SH4 emulation
3. Both clients show "Waiting for Player A to reconnect (60s timeout)..."
4. A reconnects within timeout → server pushes current savestate → both ACK → server unpauses → both resume from same point
5. A times out → server forfeits A → B wins

The pause prevents the disconnected player from losing in-flight state advantage and prevents the connected player from getting "free hits."

### D. Spectator join, read-only (Phase 1.5)
Spectator C requests to spectate.
1. Server pushes latest periodic snapshot to C
2. C loads, predictor starts from there
3. C sends no inputs → never causes rollback
4. Predictor for C is just **smooth visual playback** — frames feel local even on 50ms+ link
5. C's predictor stays synced via inbound input log + state snapshots

This is the spectator's killer feature — pixel-perfect, native-resolution, low-latency view that no Twitch stream can match.

### E. Match end + replay sharing (already exists, integrates cleanly)
1. Match concludes (timer, KO, forfeit, etc.)
2. Server finalizes `.mcrec` (savestate + full input log + signed metadata)
3. `.mcrec` uploaded to hub via existing `/hub/api/replays` endpoint
4. Anyone replays from the hub via `replay_reader.cpp`
5. Step D of Phase 0 already validated this path produces byte-identical playback

Nothing new for rollback — but the rollback predictor literally IS the replay player, so they share code naturally.

### F. Pause for time-out or break (Phase 2)
Either player presses pause-key.
1. Server captures savestate, broadcasts pause message
2. Both clients freeze prediction + rendering on last predicted frame
3. On unpause: server pushes savestate (might be unchanged), broadcasts unpause
4. Resume

Standard tournament feature.

### G. Cross-version safety (Phase 3)
Predictor + server must run same flycast commit (cross-version determinism is NOT validated and shouldn't be assumed).
1. Both include `flycast_commit_hash` in match-start handshake
2. Mismatch → server refuses prediction, falls back to "no-prediction mirror mode" for that match
3. Client shows warning: "Server flycast version differs — prediction disabled, latency increased"
4. Match still plays, just at non-predicted latency

### H. Predictor desync — silent failure mode (Phase 2)
Predictor and server diverge despite matching inputs (= flycast determinism bug).
1. Detected by periodic 253-byte gamestate hash comparison (already in `state_sync` infra)
2. Logged + telemetry sent to hub
3. Client snaps to authoritative savestate
4. Prediction resumes from there
5. Hub receives postmortem (the inputs leading up to divergence) for offline analysis

Should be near-zero frequency in practice given Phase 0 results. But the safety net costs almost nothing and protects users from rare flycast bugs.

### I. Server crash mid-match (Phase 3 — out of scope V1)
Hub detects server offline.
- V1: declare match no-contest, both players returned to queue
- V2: hub continuously receives `.mcrec` snapshots from server; on crash, hub spawns a new server with the latest snapshot, players reconnect and resume

Hot-failover is hard. V1 acceptably loses the match.

### J. Network blip but no reconnect needed (Phase 1 — THE CORE FEATURE)
100-300ms hiccup, packets queue up.
1. Predictor keeps predicting locally — **player feels nothing**
2. When wire catches up, comparator processes the backlog
3. If predictions matched: drop the backlog quietly
4. If mismatched: rollback, re-simulate, snap

**This is the use case that makes the whole feature worth building.** Latency hiccups become invisible. The 1-3% of players on flaky WiFi suddenly play normally.

### K. Tournament-grade additions (Phase 3+)
- **Latency parity option**: server artificially delays the lower-RTT player to match the higher-RTT one (both see same E2E latency — required for some tournament rules)
- **Match signing**: ED25519-signed `.mcrec` footer (slot already reserved per [docs/COMPETITIVE-CLIENT.md](COMPETITIVE-CLIENT.md))
- **ROM hash verification**: already in node registration; predictor checks server reports same hash as its own copy
- **Anti-cheat**: server detects impossible inputs (e.g., predictor sent moves that physically couldn't be pressed in a frame — implies modded predictor)

---

## Phase plan

### Phase 1 — Core feature (cold start + invisible rollback)
Builds **use case A + J**. Delivers the user-visible feature: "input feels local, network blips invisible."

Components:
1. **Predictor thread**: spawn alongside mirror client. Don't `CPU stopped`. SH4 runs locally, emits TA into VRAM, renderer reads from local prediction.
2. **Match-start handshake**: simple sync at match begin. Both clients load same savestate, ACK, server says "go."
3. **Comparator**: input-log diffing per frame. Mismatch triggers rollback.
4. **Rollback ring**: 8-frame savestate ring. Save state every frame. Load + re-emulate on mismatch.
5. **Renderer source flip**: in this mode, renderer reads from local SH4's VRAM (not wire VRAM).
6. **Wire stream as comparator-only**: wire TA still received but goes to comparator, not VRAM. Used as periodic sanity check via 253-byte gamestate hash.

New env vars:
- `MAPLECAST_PREDICT=1` — enable rollback prediction (off by default, on under TOURNAMENT_MODE)
- `MAPLECAST_PREDICT_ROLLBACK_DEPTH=8` — frames of history to keep (default 8)
- `MAPLECAST_PREDICT_LOG_DIVERGENCE=1` — verbose logging for debugging

### Phase 1.5 — Mid-match join + reconnect (casual)
Builds **use case B + D**. Reuses existing periodic state-sync infrastructure.

Components:
1. **Periodic snapshot subscription**: client subscribes to server's `state_sync` snapshots (every ~60 frames)
2. **Snapshot-load path**: receiving a fresh snapshot → predictor pauses, loads, resumes from new frame
3. **Spectator-mode integration**: spectator client uses Phase 1.5 path on join
4. **Reconnect flow**: on UDP silence-resume, client requests latest snapshot, syncs

### Phase 2 — Tournament-grade flows
Builds **use case C + F + H**.

Components:
1. **Server-side pause**: pause SH4 emulation on disconnect or pause-button
2. **Pause UI**: client shows "Waiting for Player X..." overlay with countdown
3. **Forfeit timeout**: 60s default, configurable per match
4. **Desync detection + recovery**: gamestate hash check fires every N frames, snap to authoritative on mismatch

### Phase 3+ — Cross-version, hot-failover, tournament features
Builds **use case G + I + K**. Bigger projects, ship after the foundation is proven.

---

## Open design questions

1. **What frame is "match-start"?**
   Options: character-select-confirmed; FIGHT-go; or just `mvc2.state` (current default). For testing, `mvc2.state` works. For real matches, FIGHT-go is most natural — captures right when both players are committed.

2. **Where does the savestate live during the match?**
   Options:
   - (a) In RAM only on the server, served on-demand via state_sync (current pattern)
   - (b) Periodically uploaded to hub, served from there
   - (b) is more resilient (server crash doesn't lose the match-start state) but requires hub bandwidth + storage

3. **How does the predictor handle character-select / pre-match menus?**
   The savestate skips that — match begins after loading. But what if the savestate IS at character-select?
   - Probably: predictor is OFF until the match-start handshake fires; menus run via standard mirror-client (TA from wire, no prediction)
   - Predictor turns ON only when match begins
   - Avoids predicting menu navigation which has its own quirks

4. **Memory budget for rollback ring**
   8 savestates × ~7.5MB compressed = 60 MB extra RAM. Acceptable on a competitive client. Could shrink to 4 savestates if memory-constrained.

5. **What about audio?**
   Audio is currently streamed from server (separate WS on :7203). Predictor ignores audio. Audio still comes from server. Any sync issue is bounded by audio's own buffer (~70ms pre-buffer). Acceptable.

6. **What if the server's flycast is faster than the client's?**
   Predictor must keep up at 60fps + handle rollback budget. If client CPU can't, prediction misses frames and falls back to wire-mode (degraded latency but still functional). Document min-spec.

7. **First-time-prediction-failure UX**
   First time a predictor desyncs, what does the player see? Most likely: 1-frame freeze as snapshot loads. Should be invisible. Worth measuring.

---

## Code structure outline

Files to add or substantially change:

| File | Change |
|---|---|
| `core/network/maplecast_predictor.cpp` (NEW) | Predictor thread, rollback ring, comparator |
| `core/network/maplecast_predictor.h` (NEW) | Public API |
| `core/network/maplecast_mirror.cpp` | If `MAPLECAST_PREDICT`, don't `CPU stopped`; route wire TA to comparator instead of VRAM |
| `core/emulator.cpp` | Init predictor thread when `MAPLECAST_PREDICT` set in mirror-client mode |
| `core/network/maplecast_state_sync.cpp` | Already exists; integrate predictor's snapshot consumer |
| `core/ui/gui_competitive_hud.cpp` | Add prediction stats: rollback rate, divergence events, prediction depth |
| `core/network/replay_reader.cpp` | Reused for rollback re-emulation (already deterministic per Step D) |
| `docs/ROLLBACK-PREDICTION.md` | This doc |

---

## Acceptance criteria for Phase 1

1. **Cold match start works**: two `flycast --client` instances connect, handshake, both render predicted frames identical to server's authoritative TA
2. **Predictor matches server**: 0 rollbacks for ≥1000 frames in single-player practice mode (no P2 input)
3. **Network blip handled**: with simulated 200ms wire delay, predictor keeps rendering at 60fps; comparator catches up cleanly when wire resumes
4. **Rollback works**: artificial input mismatch → rollback fires → predictor re-emulates → re-renders → no visible glitch beyond a 1-frame redraw
5. **HUD shows prediction stats**: F1/F2 overlays display rollback rate, divergence events, average prediction depth
6. **Performance**: rollback re-emulation completes in <8ms for 8-frame depth on a modern CPU (Ryzen 5000+ / Intel 11th gen+)
7. **Cross-OS**: feature works identically with Windows client connecting to Linux server (per Phase 0 cross-OS validation)
8. **Backward compatibility**: with `MAPLECAST_PREDICT=0` or unset, mirror client behaves exactly as today

---

## Why this matters

Other cloud-gaming platforms (Stadia, GeForce Now, Parsec) all use frame-by-frame video transport. They CAN'T do rollback prediction because their wire format is opaque encoded video — there's no way to "re-emulate locally with corrected inputs and snap."

MapleCast's TA-stream wire format makes this possible because:
1. The wire IS the GPU draw commands (deterministic to reproduce)
2. The SH4 emulator is byte-deterministic (Phase 0 validated)
3. The `.mcrec` infrastructure already does deterministic replay (Phase 0 Step D validated)
4. Inputs are tiny (16 bytes/frame) — easy to compare

This combination has never existed for fighting games over the internet. The feature isn't "we wish we could do this" — it's "we have all the prerequisites in production code right now, we just need to wire them together."
