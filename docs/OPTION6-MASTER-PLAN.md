# Option 6 — Master Plan (post-Phase-2 pivot)

> **Status:** Phase 2 hit a 0% hit rate / 5.7 GB cache from 10 min play. This document explains why, surveys the alternatives surfaced from internal docs + external prior art, and recommends a pivot.

## TL;DR

The naive "hash game state → look up TA buffer" approach was the wrong abstraction. The renderer doesn't need *game state*; it needs *draw commands*, which we already produce. The right pivot is to **dedupe the existing TA stream itself** rather than build a parallel state-keyed cache. Three architectures are viable; **Pivot A (TA-buffer dedup)** is the 80/20 — ships in a day, wins 3-4× bandwidth, doesn't preclude the other two.

## Why Phase 2 Failed — Root Cause

The 12-byte `VisualKey(char_id, knockdown_state, anim_animID, anim_groupID, key_frame, attack_number, facing, palette_id, sprite_id, airborne)` was over-discriminated. Several fields tick every animation frame:

- `key_frame` (anim_timer lo byte) — increments every tick during a move
- `attack_number` — changes during attacks
- `anim_animID` / `anim_groupID` — sub-frame state

A 60-frame Wolverine clothesline → 60 unique keys → 60 cache entries. Combined with **per-entry size of ~200 KB** (full parsed `rend_context`: vertices, indices, all 3 poly arrays, render passes, sorted triangles), **5.7 GB / 27,400 entries / 0% hits in 10 min**.

**Three deeper structural problems exposed the over-discrimination:**

1. **VRAM rot was never the only blocker** — the 2026-04 prototype's "garbled sprites during replay" symptom was a *consequence* of caching state-keyed TA buffers without invalidating on VRAM page changes. The fix isn't bundling textures into entries (Phase 1) — the fix is keying on *what changed*, not *what state we're in*.
2. **The renderer doesn't want game state, it wants draw commands.** Per `web/webgpu/texture-manager.mjs`, the existing dirty-page-aware texture cache is keyed by `(TCW, TSP)` — TA-stream parameters, not character state. Steady-state misses: 0. The right key shape was already in the production code.
3. **Headless mode skips `TexCache::Update()`** — the prototype's `captureTexture()` hook never fires on the VPS, so the texture pool was always empty. Phase 1's `TexRefs` list referenced files that didn't exist. The fix isn't "force decode in headless"; it's **don't try to bundle decoded textures at all** — clients already get textures via the live VRAM SYNC.

## What Prior Art Says

External research turned up three relevant precedents (full citations in the agent reports):

- **LiveRender (HKUST, ACM MM '14)** — closest academic analogue. Hash D3D draw-call *parameters* (vertex buffer ptr + transform), keep an LRU on the client, ship hashes on cache hit. 52–73% bandwidth reduction. Cache stayed in the thousands. Their key insight: hash command parameters, **never** surrounding state.
- **RDP/RemoteFX bitmap cache** — 64-bit content hashes for tiles. Server-driven invalidation. Caches max out at a few thousand entries per session for desktop workloads. Conceptually identical to "hash the TA buffer, dedup on the wire."
- **bombzj/arcade-sprite-viewer** — sprite-ID-keyed rendering for CPS1/NeoGeo. Confirms the technique works for older Capcom systems. **Nobody has shipped this for Naomi.** No published `sprite_id → animation_frame` table for MVC2 exists.
- **Rollback netcode (SnapNet, infil.net)** — visual state should be *excluded* from rollback snapshots. Particles, hit-sparks, screen shake → ephemera. Per-frame visual key should be minimal gameplay-relevant state only.

External community work + RAM-autopsy numbers triangulate the **realistic full-roster cache size at 5,000–15,000 unique entries per match**, ~50,000–80,000 for the entire game. Our 27,400 entries from one 10-minute match meant the key was leaking 4-5× too much state.

## What MapleCast Already Has That Helps

Internal survey turned up infrastructure we hadn't been using:

| Existing capability | What it gives Option 6 |
|---|---|
| `state_sync` (full savestate every 60 frames, ~1 MB compressed) | Natural keyframe boundary; could be the indexable cache unit |
| `replica` client (catchup loop with rendering disabled) | Already does scriptable frame-stamped replay |
| `.mcrec` replay format | 8.3 KB/frame, indexable, HMAC-signed, hub-distributed |
| `MirrorCompressor` (zstd L1/L3, 3-8×) | Drop-in delta compression between cache entries |
| Hub `nodes` endpoint | Distributed cache with no new infrastructure |
| `texture-manager.mjs` (dirty-page-aware, `(TCW,TSP)`-keyed) | The right key shape, already in production |
| `xxhash` (already linked, used in TexCache) | Fast TA-buffer fingerprinting |

The gap isn't capabilities; it's that we didn't compose them.

## Three Viable Pivots

### Pivot A — TA-buffer dedup ring (server adds REUSE packets)

**~1 day. Most pragmatic.** Adds zero new architecture; extends the existing TA mirror.

```
Server (maplecast_mirror.cpp::serverPublish):
  taHash = XXH64(ta_buffer, ta_size)
  if (recentHashes.contains(taHash)):
    emit REUSE(taHash) packet (10 bytes)
  else:
    recentHashes.add(taHash, frame_num)
    emit normal compressed TA frame (existing path)

Client (web/webgpu/frame-decoder.mjs):
  ringBuffer[hash] = ta_buffer  // last N frames' TA
  on REUSE: re-render from ringBuffer[hash]
  on TA: parse + cache by hash
```

- **Wire savings:** 3-4× on fighting-game frames (heavy sprite reuse during stationary animations, supers, etc.)
- **Storage:** zero on disk; ring is in-memory (~10 MB cap)
- **Hit rate:** measurable per-frame; expected 30-60% in match, near-zero on motion
- **Bug surface:** small. The only cache invalidation is "same hash" — which means *byte-identical TA buffer*, which means *same frame*. Zero ambiguity.
- **Doesn't preclude B or C.** Layer on top.

### Pivot B — LiveRender-style per-draw-call cache (closer to original Option 6 promise)

**~3-5 days.** The original "15 KB/s, 0ms latency, no emulator" pitch.

```
Server splits TA buffer into individual draw-calls (parameter blocks).
For each draw-call:
  hash = XXH64(params)
  if client_already_has(hash): emit hash (8 bytes) + transform (16 bytes)
  else: emit hash + full params (~200 bytes) + cache_id

Client:
  IndexedDB { hash → draw-call params }
  Renders by replaying cached params + new transforms
```

- **Wire savings:** 5-8× (LiveRender's measured range)
- **Storage:** ~20 MB IndexedDB per session (LiveRender) → ~50-200 MB long-term
- **Hit rate:** 80-95% within a match; 60-80% across matches (sprite reuse)
- **Cost:** new wire protocol, WASM bridge change, relay protocol bump
- **Doesn't preclude C.**

### Pivot C — Offline-first keyframe replay (the "Netflix of MVC2")

**~1-2 weeks.** Different use case — *recorded match playback*, not live streaming.

```
Per-match .mcrec extended with keyframes every 30 frames:
  [savestate_at_frame_0]
  [TA_delta frames 0..29]
  [savestate_at_frame_30]
  [TA_delta frames 30..59]
  ...

Replay client:
  fetch .mcrec from hub
  scrub via keyframe seek (loadstate + replay 0-29 deltas)
  no live server needed
```

- **Wire savings:** N/A (offline)
- **Storage:** ~50 MB per match on disk, ~5 GB for a tournament's worth
- **Replay seekability:** instant
- **Cost:** new format, hub storage, replay UI
- **Already 70% done** — `.mcrec` exists, `replica` client exists.

## What to Keep, What to Drop

**Keep:**
- Sandbox infrastructure (`deploy/scripts/deploy-lookup.sh`, systemd unit, env file, isolated paths/ports) — works, production-safe
- Master-HEAD regression fixes (gui_displayMirrorDebug headless, SOCKET typedef, SHM_NAME env override, MAPLECAST_INPUT_TARGET_PORT) — production needs these regardless of Option 6 path
- `maplecast_gamestate` reads — useful for the live overlay separately from any cache work
- The branch `feat/option6-lookup-renderer` as the staging area

**Drop / mothball:**
- Phase 1's `TexRefs` cache file format — was solving the wrong problem
- Phase 2's 12-byte `VisualKey` and the over-discriminated hash — wrong abstraction layer
- The 5.7 GB / 29,440 cache files on the VPS — wipe
- The salvaged `maplecast_lookup_test.cpp` (record-then-replay rig) — superseded by Pivot A's hash ring
- The salvaged `maplecast_scanner.cpp` (brute-force RAM injection) — Pivot A doesn't need pre-population

**Convert:**
- `maplecast_visual_cache.cpp` becomes a thin RAM-side stats logger (frames seen, hash distribution) — useful for measuring Pivot A's hit rate empirically.

## Recommendation

**Ship Pivot A first.** Concrete day-1 wins, doesn't burn the bridges to B or C. After Pivot A is in production for a few real matches, the per-frame hit-rate telemetry tells us empirically whether the harder pivots (B or C) are worth the cost.

Sequence:
1. Wipe 5.7 GB sandbox cache. Mothball Phase 1+2 code under `#ifdef MAPLECAST_LEGACY_CACHE` (don't delete; it documents what didn't work).
2. Implement Pivot A in `maplecast_mirror.cpp::serverPublish` + `frame-decoder.mjs`. Measure hit rate on a recorded match.
3. If Pivot A hit rate > 30% in normal play and bandwidth drops > 2×, ship it to production behind a flag. Sandbox stays the test bed.
4. Decide on Pivot B / C from data, not from doc-driven planning.

## Files Touched by Pivot A

- `core/network/maplecast_mirror.cpp` — add `_taBufferRing` (size 1024 hashes), emit `REUSE` opcode in `serverPublish`
- `core/network/maplecast_mirror.h` — `REUSE` opcode constant
- `web/webgpu/frame-decoder.mjs` — receive REUSE, look up ring, dispatch cached TA
- `relay/src/protocol.rs` — pass-through (REUSE is just bytes; relay doesn't need to parse)
- `core/network/maplecast_compress.h` — REUSE bypasses zstd (already small)

## Acceptance Criteria for Pivot A

- Determinism rig (`MAPLECAST_DUMP_TA=1`) passes byte-for-byte
- Sandbox + production both decode REUSE correctly (relay is opaque to it)
- 30%+ hit rate during a normal match (Sentinel vs Cable, in-match)
- 2×+ bandwidth reduction in steady state
- Zero added latency (REUSE is 10 bytes, decode time ~µs)
- No regression in scene transitions (Test surface from `wasm_bridge.cpp` comments A-E: char select, stage select, rotating-globe scene)
