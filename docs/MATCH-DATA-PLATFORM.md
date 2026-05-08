# MapleCast as a Match-Data Platform

> Captured 2026-05-08 mid-rollback-prediction work. Conceived right after Phase 0 Step D landed.

## The pivot

MapleCast started as "stream a fighting game from a VPS." Phase 0's deterministic-replay work just opened a much bigger surface area:

**Every match a node hosts is a stream of frame-by-frame game state.** Stop recording just inputs — record everything, publish it as events, and we have a decentralized telemetry platform for fighting games.

What you get with that one shift in framing:

- **Leaderboards** — longest combo, most damage in N seconds, win rate by character/matchup, character-pair tier list derived from millions of matches
- **Stats / analytics** — heatmaps of where characters stand on each stage, which buttons get pressed in what % of neutral, optimal-distance histograms, hitstun-vs-blockstun distributions
- **Coaching tools** — "your overhead got blocked 87% of the time when you used it inside this range"
- **AI training data** — massive open dataset of `(state, input) → next-state` tuples. Imitation learning. Offline RL. World-model training. Train an agent to *play* MVC2 well.
- **Live overlays** — broadcast health/meter/position to the browser viewer in real time, no scraping required

The data is uniquely valuable because:
1. SH4 emulation is byte-deterministic, so state is reproducible from inputs alone — every recording is *also* a re-runnable simulation, not just a video
2. Open arcade ROM ecosystem — no IP fight over training data the way there would be with Street Fighter VI / Tekken 8
3. Decentralized node operators accumulate play hours across the network, no central capture infrastructure
4. Free vs. the OCR/capture-card-based telemetry that scene tools currently rely on

## What "all the state" actually means

We already have the memory map (`CLAUDE.md`). Per-frame, accessible at known SH4 addresses:

| What | Where | Bytes |
|---|---|---|
| P1/P2 character struct (×3 chars each) | `0x8C268340` + 0x5A4 stride | ~1.4 KB |
| - active flag, character_id, pos_x/y, screen_x/y, facing | offsets within | 16 |
| - anim_timer, sprite_id, animation_state | | 8 |
| - health, red_health, palette | | 4 |
| Global match state | `0x8C289000` page | 64 |
| - match_sub_state, in_match flag, round_counter, timer, stage_id | | |
| - p1_meter_fill, p1_combo | | |
| Frame counter | `0x8C3496B0` | 4 |

A reasonable telemetry blob is **~256-512 bytes/frame**. At 60Hz that's **15-30 KB/sec, ~5-10 MB for a 5-min match**. Compressible. Negligible compared to the 7.7 MB savestate we already embed.

## Storage + transport: NATS.io

User's instinct is right — **NATS pub/sub is the natural fit**.

### Topology

```
flycast headless (any node) ─publish→ nats://hub.nobd.net:4222
   topic: matches.<server-id>.<match-id>.state.<frame>
   topic: matches.<server-id>.<match-id>.input
   topic: matches.<server-id>.<match-id>.events  (round-end, KO, combo-detected, etc.)

NATS server ─fanout→
   ├─ leaderboard service        (subscribes to *.events)
   ├─ stats aggregator           (subscribes to *.state)
   ├─ live viewer overlay        (subscribes to specific match)
   ├─ training data pipeline     (subscribes to all *.state + *.input, writes to S3)
   └─ archive service            (subscribes to all, writes JetStream durable)
```

### Why NATS specifically

- **Deno/Rust/Go/Node clients** all first-class — easy to write subscribers in any stack
- **JetStream** for durable replay (subscriber crashes? resume from last seen)
- **Subjects + wildcards** map naturally to (server, match, stream) hierarchy
- **Lightweight** — single binary, runs on the hub VPS without a fight
- **Already-in-mind** by user, which means it survives the "would I actually use this" filter

### Why NOT "just stuff it in SurrealDB"

We have SurrealDB on prod for skin storage. But:
- It's a database, not a fanout bus — consumers would have to poll
- Putting 30 KB/sec of telemetry through a DB write path is a different scale of traffic than skin lookups
- A DB is the *archive* layer; pub/sub is the *transport* layer. Need both.

Right architecture: **NATS for live + recent**, **SurrealDB or S3 for archived**. Stats aggregator subscribes to NATS, batches writes to SurrealDB. Pure subscribers (live overlay) just consume from NATS without persisting.

## The .mcrec file's relationship to this

Currently: `.mcrec` = initial savestate + input log.

Future: `.mcrec` becomes one of three artifacts a match produces:
1. `.mcrec` — what we have (deterministic replay)
2. `.mctele` — telemetry stream (state samples per frame, NATS-published live)
3. `.mcevt` — event stream (round end, KO, combo detected, dropped combo, etc. — derived from telemetry by an analyzer)

`.mcrec` stays the source of truth (you can regenerate `.mctele` and `.mcevt` from it by re-running). `.mctele` and `.mcevt` are the *consumable* derivatives.

## Where this intersects rollback prediction

**Periodic state checkpoints** (Phase 0 Step B follow-on, currently scoped) write a fresh savestate every N seconds inside the `.mcrec`. That's basically the same plumbing as periodic state telemetry — we're already going to have a per-frame state read happen.

If we generalize the checkpoint hook now, the same code path emits:
- Full savestate every N seconds → `.mcrec` (drift recovery + seek)
- 256-byte per-frame telemetry → NATS or `.mctele` sidecar
- Detected events (when health crosses zero, when combo counter resets non-trivially, etc.) → NATS events topic

One write point, three downstream consumers. **This is exactly the right time to land it** — we're already in the recording-path code.

## AI training: the big asymmetric bet

Most fighting game AI work today either:
- Uses RL on a custom toy game (no real-world transfer)
- Uses screen-pixel input on a closed emulator (slow, fragile, expensive to scale)
- Uses MAME state-grabbing hacks (works for SF2-era, breaks on anything modern)

We have:
- Frame-perfect deterministic emulation (replay any state)
- Direct memory access to game state (no OCR, no pixel parsing)
- Decentralized node network (free training-data-collection capacity)
- Open ROM (MVC2) — no Capcom IP team filing takedowns

**That's a fighting-game-AI dataset moat** if we ship it. Specifically:

1. **Imitation learning** — train policy `(state) → action` on millions of human play frames. Should learn neutral, basic combos, situational defense.
2. **Offline RL** — use the replay buffer to train value functions / Q-networks. Better than imitation alone for late-game decisions.
3. **World model** — train a network `(state, action) → next_state` and use it as a fast simulator. Could enable Monte Carlo Tree Search rollouts for planning.
4. **Self-play augmentation** — once we have a baseline agent, run agent-vs-agent matches as additional training data, scale the dataset 1000× on commodity compute.

A trained MVC2 agent has commercial value:
- Tutorial bots ("here's an AI that plays like Yipes does")
- Online practice ("ranked-equivalent AI for unranked players")
- Tournament demonstrations
- Coaching ("our AI sees you doing X, the typical Yipes response is Y")

## Realistic phasing

| Phase | What | When |
|---|---|---|
| **Tele-0** | Define the per-frame telemetry blob (which addresses, which encoding) | Right after rollback Phase 1 ships |
| **Tele-1** | Hook the recording path to dump telemetry alongside `.mcrec` | Same |
| **Tele-2** | Stand up NATS on hub, publish telemetry live during matches | Once .mctele is stable |
| **Tele-3** | Leaderboard service — first NATS subscriber, validates the bus | Cheap product win, ~1 week |
| **Tele-4** | Stats dashboard / live overlay | Operator/spectator UX |
| **Tele-5** | Training-data pipeline — bulk export from JetStream to S3 | Once dataset ≥ 100 hours |
| **Tele-6** | First agent training run (imitation-learning baseline) | Whenever we have time + GPUs |

Phase 1 of rollback prediction (next on the queue) is independent of any of this. Land that first; come back here when its plumbing is stable.

## Open questions to answer before starting

1. Per-frame state read — does reading 256 bytes from SH4 RAM 60×/sec impose any meaningful overhead? (Probably no — these are direct memory reads, not emulated; nanoseconds. But measure.)
2. Do we want telemetry on EVERY match by default, or opt-in? Privacy-wise nothing here is sensitive (it's game state, not personal data) but operators might want a switch.
3. NATS hosting model — single hub-hosted broker, or federated? Single broker is simpler for V1; federation is a Phase 3+ concern.
4. Address-map versioning — MVC2 specifically is well-mapped, but we'd want to handle other Naomi/Dreamcast titles eventually. How does the telemetry schema handle "this game has different memory addresses"? Probably: per-game adapters that produce a normalized output blob.
5. AI-training data licensing — what license do we publish the dataset under? CC0? CC-BY? Operators contribute play hours; do they retain any rights?
