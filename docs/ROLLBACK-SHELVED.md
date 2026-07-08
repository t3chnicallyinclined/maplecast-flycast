# Rollback Ring + GGPO-style Predictor — SHELVED 2026-05-09

> **⚠️ STATUS (2026-07-08): UN-SHELVED.** Un-shelve condition #1 (client-side SH4 predictor)
> triggered: the lockstep-mirror client (commit bc16af338) runs a full local SH4 bit-exact,
> and the predict/rollback arc is ACTIVE on feat/render-replica-live (STAGE 0–c + CAPSTONE,
> commits 3cf92ca83..3cf861b33; MAPLECAST_PREDICT_LIVE=1). The "central server, thin clients"
> rationale below no longer describes the native client. NOTE: the link to
> docs/ROLLBACK-RING-DESIGN.md is broken (file never existed in docs/).
> Canonical ledger: docs/RENDER-STATE.md.

The Phase 1 GGPO-style rollback prediction work is **shelved**. The code is
kept intact (don't delete it) but is **off by default** and not exercised
in production. This document records why we shelved it and what stays
useful from the work.

## Decision

After landing the V2 .mcrec replay system and validating SH4 forward
determinism (DUMP_TA: 1189/1189 byte-identical TA frames between two
flycast runs from the same savestate), we revisited whether GGPO-style
rollback netcode actually fits MapleCast's architecture.

**It doesn't, for our current model.**

GGPO solves the latency problem in **peer-to-peer** netcode where each
player runs a **full local game simulation**: it locally predicts the
opponent's next input, runs forward immediately, and rewinds + re-emulates
when the real input arrives. Local-player input lag = 0 frames. Opponent
input lag = network RTT, masked by rollback.

MapleCast is **central authoritative server, thin clients**:
- One SH4 emulator on the VPS, authoritative
- Clients are renderers + input sinks (no local SH4)
- Both players' inputs → UDP → server → frame produced → broadcast to
  both clients
- End-to-end latency: ~10ms button-to-pixel = 0.6 frames at 60 fps,
  already below the human-perception threshold for fighting games

Building rollback into this would require flipping the architecture:
each client runs its own local SH4, predictor, state-sync replicas,
mispredict-and-rewind machinery. We'd gain ~10ms of local-player input
feel — at the cost of:

- Every client now needs full SH4 emulation (kills the thin-client win)
- Desync risk where there was none (single SH4 = zero desync by design)
- Significant ongoing maintenance burden (the determinism work alone
  already cost weeks)
- Complexity that erodes the spectator/replay/mod-friendly properties
  that make MapleCast different from existing emulators

10ms is already a great latency budget. The trade isn't worth it.

## What's still active and useful from the work

These pieces stay in production-ready state, **not** under the rollback
gate:

- **`.mcrec` record/replay system** (V2 discipline) — the whole point of
  the recent work. Records inputs + initial savestate, replays
  deterministically. Used for match archives, replay sharing, AI
  training data, regression testing.

- **`scripts/audit-determinism.sh`** — runs F.2 byte-diff audit (which
  *does* require enabling the rollback ring temporarily, since F.2 lives
  in `maplecast_rollback.cpp`), but the gate exists so it can be invoked
  in CI without leaking into production. The audit is the regression net
  for any future change to `dc_serialize`, so keep running it.

- **Async checkpoint writer** in `replay_writer.cpp` (B.3) — moves
  `dc_savestate` + sidecar fwrite off the renderer thread.

- **`captureFrameToBlob` / `restoreFromBlob`** in `maplecast_rollback.h` —
  byte-blob save/restore helpers that don't require the ring to be
  active. Useful for any future code that wants byte-exact state capture
  outside the replay flow.

- **Deferred-rewind deadlock fix** in `Emulator::vblank()` —
  `rend_cancel_emu_wait()` after `Stop()`. Stays so any future caller
  that requests a rewind from vblank context doesn't deadlock.

## What's gated behind `MAPLECAST_ROLLBACK_RING=1`

These compile in but stay dormant unless the env var is set:

- **Rollback ring** (`maplecast_rollback::init()`) — 40 MB arena,
  page-delta + dc_serialize hybrid storage, 10-slot SPSC ring
- **F.1 round-trip determinism test** (env: `MAPLECAST_ROLLBACK_F1_TEST`)
- **F.2 byte-diff audit** (env: `MAPLECAST_DC_AUDIT`)
- **`maplecast_predictor`** (A.5 comparator + rollback trigger) — only
  init'd when the ring is available, since its mismatch path needs a
  working rollback
- **`MAPLECAST_TEST_ROLLBACK`** — synthetic mispredict harness for
  testing the predictor in isolation

## When it might make sense to un-shelf

Re-enable this line of work if/when:

1. We pivot to a **client-side SH4 predictor** model (the
   `MAPLECAST_HEADLESS=ON` binary running locally on the player's
   machine alongside the server connection). Currently planned but not
   the active product direction.
2. We hit a use case where 10ms isn't enough (offline-feel competitive
   1v1 with sub-frame requirements). Most users won't hit this.
3. F.1's forward-determinism gap (~20-byte cycle drift in mid-execution
   restore) gets fixed independently. Without that, rollback can't be
   byte-perfect across multiple frames anyway.

## How to actually re-enable for experimentation

```bash
MAPLECAST_ROLLBACK_RING=1 \
MAPLECAST_ROLLBACK_F1_TEST="300,5" \
./build-headless-win/flycast.exe "<rom>"
```

The headless server will print `[rollback] init` / `[predictor] active`
banners on startup if the ring is on.

## Related files

- `core/network/maplecast_rollback.{cpp,h}` — the ring itself
- `core/network/maplecast_predictor.{cpp,h}` — A.5 comparator
- `core/emulator.cpp:1248-1262` — env-gated init
- `core/emulator.cpp::Emulator::vblank()` — saveFrame + Stop+restart
  hooks, gated on `maplecast_rollback::active()`
- `docs/ROLLBACK-PREDICTION.md` — original design doc (kept for reference)
- `docs/ROLLBACK-RING-DESIGN.md` — ring architecture
- `scripts/audit-determinism.sh` — F.2 audit invocation
