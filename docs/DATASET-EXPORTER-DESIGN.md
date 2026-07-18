# Dataset Exporter — server-integrated design (Tele-0 / Tele-1)

> **Status: DESIGN, for review before implementation.** Concrete plan to build the
> per-frame `(state, p1_input, p2_input, ids)` dataset tap into the authoritative
> MapleCast server, so every real match becomes imitation/RL training data for the
> `mvc2-ai` repo. Realizes the "Tele-0/Tele-1" phases of
> [`MATCH-DATA-PLATFORM.md`](MATCH-DATA-PLATFORM.md).
>
> Field list / consumed contract is OWNED by `mvc2-ai`
> (`docs/DATASET-FIELDS.md`, `docs/DATASET-SCHEMA.md`). This doc is the *server-side
> implementation* that produces it.

## Goal / non-goals

**Goal.** On the prod (or local) server, record per frame: the full game-state
projection + both players' inputs + player identity, one artifact per match,
deterministically joinable, with **zero measurable gameplay latency** and gated OFF
by default.

**Non-goals.** No ML on the server. No writing guest RAM (read-only). No change to
the proven `.mcrec` format or its HMAC. No self-play / RL loop. No per-player style
modeling without consent.

## Architecture — a passive tap, not a new path

Two halves, one already shipping:

| Half | Mechanism | Status |
|---|---|---|
| **Inputs** | `MAPLECAST_RECORD_MATCHES` → `.mcrec` (savestate + both-slot input log), fed by `maplecast_replay::append()` at [input_server.cpp:704](../core/network/maplecast_input_server.cpp#L704), init at [emulator.cpp:893](../core/emulator.cpp#L893) | **EXISTS, proven** (made the 38 recordings) |
| **State** | NEW: read the state projection each frame on the publish thread, write a `.mctele` sidecar | **to build** |

The **state read rides the thread that already reads state.** `serverPublish()` runs
on the render/publish thread, at a frame boundary where the SH4 is quiesced (the same
atomic-snapshot point as the PVR snapshot — regression-guard #5), and it *already*
does `addrspace::read8(0x8C289638)` etc. ([maplecast_mirror.cpp:867](../core/network/maplecast_mirror.cpp#L867)).
We add the char-struct + globals reads next to it. The authoritative input→sim step is
never touched; the sim never waits on the recorder.

### What we read (per frame)

Per the `mvc2-ai` field catalog (`docs/DATASET-FIELDS.md`). Read via the existing
`addrspace::read*` accessor, or bulk-`memcpy` from the guest RAM base `mem_b`
(guest `0x8C268340` → `mem_b[0x268340]`, 16 MB main RAM):

- **6 char structs** — `0x8C268340`, stride `0x5A4` → 6 × 0x5A4 = 8664 B (whole
  struct; projection happens offline so we never re-capture to add a field).
- **Global page** — `0x8C289600..0x8C289680` (~128 B): match state, timer, stage,
  both meters, both combos, round wins.
- **Input_DEC** — `0x8C2681DC`, both slots (the semantic input the game acted on).
- **frame_counter** — `0x8C3496B0` (u32) — the **join key**.

≈ 8.9 KB raw/frame; compresses hard (99% static frame-to-frame, per the state-wire
floor work). Budget: ~5–10 MB per 5-min match after zstd (matches the platform-doc
estimate).

### Output — a `.mctele` sidecar (not a change to `.mcrec`)

```
<match>.mcrec    inputs + savestate   (unchanged, HMAC intact)
<match>.mctele   NEW: per-frame state stream, frame-indexed
  header:  "MCTL" + schema_version + consented(u8) + p1/p2 handle
  frame:   frame_counter(u32) + zstd(state_blob)     × N
```

Sidecar, not embedded, so the proven `.mcrec` writer/format/HMAC is untouched and the
two are independently testable. New recorder call, mirroring `append()`:

```cpp
// maplecast_replay
void appendState(uint64_t frame, const uint8_t* blob, size_t len);  // ring → batched flush
```

Called from `serverPublish()` under the same `MAPLECAST_RECORD_MATCHES` gate (or a
dedicated `MAPLECAST_RECORD_STATE`). Same lifecycle as the input log: arm at match,
flush on stop, retention/rotation reuse the `.mcrec` machinery.

## Hot-path safety (the "would it add latency?" proof)

- **Off the critical path.** `serverPublish()` is the publish thread, not input→sim.
  The sim thread advances regardless.
- **Cheap.** ~8.9 KB `memcpy` + one ring push ≈ single-digit µs — under the existing
  ~80 µs publish-compress, and the measured server-side per-frame span is ~1.7 ms
  (E2E-latency baseline). No per-frame disk I/O (batched/flushed off-thread).
- **Read-only ⇒ determinism-safe.** We never write guest RAM (cardinal rule; the
  state-replica *injection* dead-end proved writes corrupt the sim). The tap cannot
  change the simulation. Gate ON, run the `MAPLECAST_DUMP_TA` determinism rig, confirm
  byte-identical TA vs gate OFF.

## Consent

- `.mctele`/dataset header carries `consented(u8)`. **Default 0** (anonymous) —
  usable for the anonymous rollback predictor, the default product.
- `consented=1` only when both seats opted in (client-signaled opt-in or server
  policy). Player handles already live in the `.mcrec` header. Per-player *style*
  modeling downstream refuses non-consented handles (`mvc2-ai` enforces it too).
- This is wired from day one, not retrofitted.

## Offline join (in `mvc2-ai`, not on the server)

A pure-Python tool: for each match, read `.mcrec` (inputs, both slots, by frame) +
`.mctele` (state, by frame), align on `frame_counter`, emit the training `.mc2f`.
Deterministic, re-runnable, and the projection (which fields → `STATE_DIM`) is chosen
here — so changing it never requires re-capture.

## Deploy plan (CLAUDE.md discipline — no shortcuts)

1. Branch off `master`; implement behind the gate (default OFF).
2. Local headless validation: record a match, join, inspect a `.mc2f`.
3. **Determinism rig** (`MAPLECAST_DUMP_TA`) ON vs OFF → byte-identical.
4. Measure CPU/disk delta on a 2-vCPU-equivalent (prod is ~12% CPU, no GPU).
5. Deploy via `deploy/scripts/deploy-headless.sh` (timestamped backup + md5), **gate
   still OFF**.
6. Enable via env on prod; monitor CPU/disk/latency; confirm `.mc2f` lands.

## Phasing

| Phase | What |
|---|---|
| **P0** | Freeze the state projection + `.mctele` byte layout (with `mvc2-sh4-re-expert`; resolve the `[I]` leads in DATASET-FIELDS.md §5) |
| **P1** | `appendState()` + `serverPublish` tap, gated, local headless |
| **P2** | `mvc2-ai` join tool `.mcrec`+`.mctele` → `.mc2f`; validate a real file |
| **P3** | Determinism rig + CPU/disk measurement |
| **P4** | Deploy gated OFF → enable → monitor |
| **P5** | Consent opt-in signal before any per-player style use |

## Open questions

1. **Where does 2-human traffic come from?** The flywheel only spins as fast as real
   matches are played on the box. Needs a play-volume plan (this is product/ops, not
   this doc).
2. `MAPLECAST_RECORD_STATE` as a **separate** gate from `MAPLECAST_RECORD_MATCHES`, so
   state capture can be toggled without the input recorder? (Leaning yes.)
3. `.mctele` cadence — every frame, or every Nth with dead-reckoning? (Start: every
   frame; it compresses away.)
4. Resolve the `[I]` field leads (P2 meter/combo offsets, DC→strength permutation,
   block-state) via one local capture before freezing P0.
