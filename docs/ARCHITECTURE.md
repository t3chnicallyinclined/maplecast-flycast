# MapleCast Architecture — Mental Model

## What Is MapleCast?

MapleCast turns a Flycast Dreamcast emulator into a game streaming server. One instance of MVC2 runs on the server. Players connect with fight sticks (NOBD) or browser gamepads. The server streams the game to all connected clients in real-time via TA Mirror mode (raw GPU commands), zstd-compressed and fanned out through a Rust relay on a public VPS. Sub-5ms end-to-end latency on LAN; ~4 Mbps sustained over the internet.

## System Topology

```
HOME (74.101.20.197)                  VPS (66.55.128.93 — nobd.net)        BROWSERS
═════════════════════                 ═══════════════════════════════      ══════════

┌──────────────────────┐              ┌──────────────────────────┐         ┌─────────┐
│      FLYCAST          │              │   maplecast-relay (Rust) │         │  king   │
│  (one binary)         │   wss://     │                          │  wss:// │  .html  │
│                       │  ─────────►  │  - WebSocket upstream    │ ──────► │         │
│ ┌────────┐ ┌────────┐│   compressed │  - zstd-aware fan-out    │ relay   │ renderer│
│ │EMULATOR│ │ INPUT  ││  TA frames   │  - SYNC cache for late   │ frames  │  .wasm  │
│ │ SH4 +  │◄│ SERVER ││              │    joiners               │         │  zstd   │
│ │ PVR    │ │ 7100   ││              │  - signaling broadcast    │         │ decode  │
│ └────────┘ └────────┘│              │  - text/bin → upstream    │         │         │
│ ┌─────────────────┐  │              │                          │         └─────────┘
│ │ STREAM SERVER   │  │              │  Listens 7201 (nginx ↦) │            │ ▲
│ │ ws_server.cpp   │  │              │  Connects to flycast 7200│            │ │
│ │ + mirror.cpp    │  │              └──────────────────────────┘            │ │
│ │ + compress.h    │  │                          ▲                            │ │
│ │  port 7200      │  │                          │                            │ │
│ └─────────────────┘  │              ┌──────────────────────────┐            │ │
└──────────────────────┘              │  nginx (HTTPS, certbot)  │  HTTPS    │ │
                                       │  /  → static (king,wasm) │ ◄──────────┘ │
                                       │  /ws → relay             │              │
                                       │  /db → SurrealDB         │              │
                                       └──────────────────────────┘              │
                                                  │                              │
                                       ┌──────────────────────────┐              │
                                       │  SurrealDB (8000)        │              │
                                       │  player, match, ELO      │ ◄────────────┘
                                       │  badges, h2h, stats      │   /db queries
                                       └──────────────────────────┘
```

### Pillar 1: Emulator (Flycast)
The Dreamcast emulator. Runs MVC2 at 60fps. The game thinks it's talking to real controllers via the Maple Bus. It sends CMD9 (GetCondition) every frame to ask "what buttons are pressed?" The answer comes from `kcode[]` globals. The server also reads 253 bytes of MVC2 RAM each frame for live game state (health, combos, meter, characters).

### Pillar 2: Input Server (`maplecast_input_server.cpp`)
Single source of truth for all player input. Receives from multiple sources, writes to one place. Tracks who's connected, their latency, their device type. Manages NOBD stick registration (rhythm-based binding to browser users), player queue ("I Got Next"), and slot assignment.

### Pillar 3: Stream Server (`maplecast_mirror.cpp` + `maplecast_ws_server.cpp` + `maplecast_compress.h`)
Captures raw TA command buffers + 14 PVR registers + VRAM page diffs each frame, run-length-deltas the TA buffer vs the previous frame, then **zstd-compresses** the assembled frame (level 1, ~80us per frame) and broadcasts via WebSocket on port 7200. SHM ring buffer for local mirror clients stays uncompressed. Compressed envelope: `[ZCST(4)][uncompSize(4)][zstd blob]`. Sustained ~4 Mbps for 60fps MVC2.

### Pillar 4: Relay (`relay/` — Rust, on VPS)
**This is a separate process running on a Vultr VPS at nobd.net.** Connects upstream as a WebSocket client to flycast:7200, fans frames out to up to 500 browser clients on port 7201. Maintains a SYNC cache so late joiners get instant initial state. ZCST-aware: decompresses for state inspection, forwards original compressed bytes downstream (zero re-encode overhead). Also forwards client-originated text/binary messages back to upstream flycast (player input, queue commands, chat).

---

## Input Flow — How Button Presses Reach The Game

```
NOBD Stick (hardware fight stick)
  │ W6100 Ethernet, 12,000 packets/sec
  │ 4 bytes: [LT][RT][buttons_hi][buttons_lo]
  ▼
UDP:7100 ──→ Input Server UDP Thread
               │ recvfrom() + SO_BUSY_POLL
               │
               ├─ Is stick registered? (rhythm binding to browser user)
               │  NO → silently ignored (no auto-assign)
               │  YES → check if bound browser user has active slot
               │         NO → silently ignored
               │         YES → route to that slot
               ▼
            updateSlot(slot, lt, rt, buttons)
               │
               ▼
            kcode[slot] = buttons    ← atomic write
            lt[slot]    = trigger
            rt[slot]    = trigger


Browser Gamepad (remote player)
  │ Gamepad API, rAF-driven burst poll via MessageChannel
  │ (16 polls per vsync ≈ 1ms input-change resolution)
  │ 4 bytes: [LT][RT][buttons_hi][buttons_lo]
  ▼
WebSocket (port 7200) ──→ maplecast_ws_server.cpp
  │  Binary 4-byte frame          │ onMessage callback
  │                               │ Looks up connection → slot mapping
  │                               │ Sends tagged 5-byte UDP to 7100
  │                               ▼
  │                            UDP:7100 (loopback)
  │                               │
  │                               ▼
  │                            updateSlot(slot, lt, rt, buttons)
  │                               │
  │                               ▼
  │                            kcode[slot] = buttons    ← atomic write
  │
  │  (WebRTC DataChannel also
  │   supported for H.264 mode,
  │   bypasses UDP hop)


                    ┌─────────────────────────┐
                    │  Emulated Dreamcast      │
                    │                          │
                    │  Maple Bus DMA (vblank)  │
                    │  ├─ ggpo::getLocalInput()│
                    │  │  reads kcode[]/lt[]   │ ← Always fresh,
                    │  │  (just memory loads)  │   zero syscalls
                    │  ▼                       │
                    │  CMD9 GetCondition       │
                    │  ├─ MapleConfigMap::      │
                    │  │  GetInput(&pjs)       │
                    │  ▼                       │
                    │  Game processes buttons  │
                    └─────────────────────────┘
```

**Key insight:** The game reads buttons once per frame at vblank via CMD9. The input server keeps `kcode[]` always up-to-date in the background. There's never a socket read in the hot path. NOBD sticks no longer auto-assign to P1/P2 — they must be registered via a rhythm pattern (tap 5x, pause, 5x) that binds the physical stick to a browser user ID. Input only routes when that user has an active slot.

---

## Video Flow — How Frames Reach The Browser

### Mode 1: TA Mirror (Primary)

```
Flycast Emulator (server, home box at 74.101.20.197)
  │ PVR GPU renders frame via TA command list
  ▼
maplecast_mirror::serverPublish()            [maplecast_mirror.cpp]
  │
  ├─ Capture TA command buffer               Raw GPU command list
  │    (varies per frame, ~2-30KB)
  │    Run-length delta vs previous frame
  │    Keyframe every 60 frames
  │
  ├─ Capture PVR registers                   14 critical regs as snapshot (64B)
  │
  ├─ Diff VRAM pages (4KB granularity)       Texture/palette changes
  │    Shadow copy comparison via memcmp
  │    Only changed pages included
  │
  ├─ Assemble uncompressed delta frame:
  │    [frameSize(4)] [frameNum(4)] [pvr_snapshot(64)]
  │    [taOrigSize(4)] [deltaPayloadSize(4)] [TA delta data]
  │    [checksum(4)] [dirtyCount(4)] [dirty pages...]
  │    Total: ~15-40KB/frame
  │
  ├─ Write to SHM ring buffer                Local mirror client (uncompressed)
  │
  ├─ MirrorCompressor.compress(level 1)      [maplecast_compress.h]
  │    │
  │    └─ ZSTD_compressCCtx                  ~80us per frame
  │       Output: [ZCST(4)] [uncompSize(4)] [zstd blob]
  │       Compression: ~2.5x (15-40KB → 6-15KB)
  │
  ▼
maplecast_ws::broadcastBinary()              [maplecast_ws_server.cpp]
  │ Port 7200 WebSocket — sends compressed bytes
  │
  ▼
══════════════════════ INTERNET ══════════════════════
  │ ~6-15KB per frame instead of 15-40KB (60% bandwidth saved)
  │
  ▼
MapleCast Relay (Rust, VPS at 66.55.128.93:7201)   [relay/src/fanout.rs]
  │ Connects upstream as a WebSocket client
  │
  ├─ on_upstream_frame()
  │  │
  │  ├─ Detect ZCST magic                    [relay/src/protocol.rs]
  │  ├─ zstd::decode_all() for inspection    Only for SYNC detection + cache update
  │  ├─ apply_dirty_pages() to cached state  Maintains live VRAM/PVR copy
  │  └─ Forward ORIGINAL compressed bytes    No re-encode, zero added latency
  │
  ├─ tokio broadcast channel (16 slots)      Backpressure: lagging clients drop
  │
  ▼
nginx (HTTPS termination, /ws → 127.0.0.1:7201)
  │ wss://nobd.net/ws
  │
  ▼
Browser (king.html on nobd.net)              [web/king.html, web/js/]
  │
  ├─ frame-worker.mjs                        Dedicated Worker thread
  │  │ Owns one WebSocket connection
  │  │ ZERO event-loop contention
  │  │ Forwards via postMessage Transferable (zero copy)
  │  ▼
  │
  ├─ ws-connection.mjs onmessage             Main thread
  │  │ Routes to handleBinaryFrame()
  │  ▼
  │
  ├─ renderer-bridge.mjs handleBinaryFrame() [web/js/renderer-bridge.mjs]
  │  │
  │  ├─ Read first 4 bytes as u32 LE
  │  ├─ "SYNC" (0x434E5953) → uncompressed sync (legacy path)
  │  ├─ "ZCST" (0x5453435A) → compressed
  │  │   ├─ uncompressedSize > 1MB → compressed SYNC
  │  │   └─ uncompressedSize ≤ 1MB → compressed delta frame
  │  │
  │  ├─ SYNC path: _renderer_sync(buf, len)
  │  └─ Delta path: _renderer_frame(buf, len)
  │
  ▼
WASM (renderer.wasm, 831KB)                  [packages/renderer/src/wasm_bridge.cpp]
  │ Has zstd decompress sources linked
  │
  ├─ MirrorDecompressor.decompress()         [core/network/maplecast_compress.h]
  │  ├─ Check for ZCST magic
  │  ├─ ZSTD_decompressDCtx                  ~30us in browser
  │  └─ Return pointer to decompressed data
  │
  ├─ Parse uncompressed frame (same format as before)
  ├─ Apply VRAM dirty pages
  ├─ Apply PVR register snapshot
  ├─ Delta-decode TA commands vs prev buffer
  ├─ FillBGP() → background polygon
  ├─ palette_update()
  │
  ▼
renderer->Process(&_ctx) → Render() → Present()
  │ flycast's real GLES renderer through WebGL2
  ▼
Pixel-perfect MVC2 at 60fps
```

### Compression Layer

zstd compression (level 1 for delta frames, level 3 for SYNC) is applied at the
flycast server before WebSocket broadcast. The compressed envelope uses a "ZCST"
magic header so receivers can transparently detect compressed vs uncompressed:

```
Compressed envelope:
  ┌──────┬──────────────┬──────────────────┐
  │ ZCST │uncompressedSz│ zstd blob        │
  │ 4B   │ 4B           │ N bytes          │
  └──────┴──────────────┴──────────────────┘
```

Detection rules at every receiver (relay, browser, native client):
1. Read magic at offset 0
2. If `0x53 0x59 0x4E 0x43` ("SYNC") → uncompressed sync
3. If `0x5A 0x43 0x53 0x54` ("ZCST"):
   - Read `uncompressedSize` at offset 4
   - If > 1MB → compressed SYNC (decompresses to "SYNC..." payload)
   - Else → compressed delta frame
4. Otherwise → uncompressed delta frame

**Measured performance (Apr 2026, MVC2 keyframe-heavy stream):**
| Metric | Uncompressed | zstd | Ratio |
|--------|--------------|------|-------|
| Avg frame size | ~25KB | ~8.6KB | 2.9x |
| SYNC packet (level 3) | 8.0MB | 0.6MB | **13.3x** |
| Server compress time | 0us | ~80us | — |
| Browser decompress time | 0us | ~30us | — |
| Sustained bandwidth @ 60fps | ~12 Mbps | ~4.1 Mbps | 2.9x |

The relay decompresses ONLY for state inspection (SYNC detection + dirty page
cache update). Compressed bytes are forwarded verbatim downstream — zero re-encode
overhead, zero added latency.

**CRITICAL — magic constant byte order:** The wire bytes for the ZCST magic are
`[0x5A, 0x43, 0x53, 0x54]` ("ZCST" ASCII). When stored as a `uint32_t` via
`memcpy` on a little-endian machine, the value MUST be `0x5453435A`, NOT
`0x5A435354`. The latter serializes to bytes "TSCZ" — wire-incompatible with
the JS reader (`magic === 0x5453435A`) and the Rust reader (`&data[0..4] == b"ZCST"`).
All three sides (C++, JS, Rust) verify against the same wire bytes; the
constant in `core/network/maplecast_compress.h` is the canonical source.

---

## ⚠️ THE WIRE IS DETERMINISTIC AND BYTE-PERFECT (commit 466d72d54)

**READ THIS BEFORE TOUCHING ANYTHING IN THE MIRROR PIPELINE.**

As of `466d72d54` ("Mirror wire format is now PROVABLY DETERMINISTIC end-to-end"),
the mirror wire stream is **byte-perfect end to end**. Every TA buffer the server
captures is reproduced byte-identically on the receiver. Same for VRAM and PVR.
Verified by a per-frame byte/hash diff harness running server + client side by
side: **TA 5395/5395 + VRAM 6203/6203 + PVR 6203/6203 = 100/100/100 across
thousands of frames including scene transitions.**

This was not always true. For months the wire was racy and we masked it with
workarounds (DMA bitmap, periodic SYNCs, scene-change broadcasts, FSYN-on-connect,
texture cache resets). Six race conditions in 466d72d54 fixed the underlying
problem; the workarounds were then removed. **Do not re-add them. Run the
determinism test rig instead — that's the regression check.**

### The six fixes (don't reintroduce these bugs)

1. **`DecodedFrame::pages` is a `std::vector`, NOT a fixed array.** The previous
   `pages[128]` silently truncated dirty page records past 128. In-match never
   tripped this (0–7 pages), scene transitions ship 100–200+ pages, the bulk
   was dropped. Sanity-bound at 4096 entries.

2. **TA delta encoder runLen MUST be clamped to 65535 BEFORE the gap-merge step.**
   The gap-merge can push `(i - runStart)` past 65535, and the cast to `uint16_t`
   wraps. Server then writes `runLen=7` to the wire but copies 65543 bytes of
   data; client mis-aligns the entire rest of the wire stream. Manifested as
   scene-change garble on every buffer-grow transition. See `serverPublish()`
   line ~860, the `if (fullLen > 65535) i = runStart + 65535;` clamp.

3. **The diff loop snapshots live → shadow ONCE per dirty page**, then reads
   the wire copy from the shadow. Never re-read `reg.ptr` between memcmp and
   wire memcpy — the SH4 thread can race in there and write new bytes, leaving
   the shadow holding state the wire never carried, which becomes a permanent
   divergence (next frame's memcmp sees `shadow == ptr` and never re-ships).

4. **`_decoded` overwrite race in the WS background thread:** the producer used
   to unconditionally `std::move()` a new frame into `_decoded`, silently losing
   the previous frame's dirty pages if the consumer hadn't drained yet. The
   server's next memcmp said "unchanged" and the page was never re-sent.
   Permanent divergence per dropped frame. Now the producer **merges** the
   previous frame's unconsumed pages into the new frame's pages.

5. **PVR atomic snapshot at top of `serverPublish`.** The Sync Pulse Generator
   (`spg.cpp`) writes `SPG_STATUS.scanline`, `SPG_STATUS.fieldnum`,
   `SPG_STATUS.vsync` from the SH4 scheduler thread, multiple times per frame.
   The diff loop, the inline `pvr_snapshot[16]`, and the hash log all used to
   read live `pvr_regs[]` — racing the scheduler. Now the entire 32 KB
   `pvr_regs` block is snapshotted into a thread-local `_pvrAtomicSnap` once at
   the top of `serverPublish`, and the PVR region's `_regions[].ptr` is
   temporarily swapped to point at the snapshot. Restored at function exit.

6. **`_decodedMtx` mutex on producer/consumer.** Even after #4, the producer's
   `std::move(df)` could destroy the previous `_decoded.pages` vector while the
   consumer was iterating it. Use-after-free / corrupted iteration. The mutex
   serializes the merge/move and the snapshot. Consumer takes a local
   `df_local = std::move(_decoded)` under the lock, then drops the lock and
   iterates `df_local.pages` outside the lock.

### The determinism test rig — your regression check

Set `MAPLECAST_DUMP_TA=1` on both server and a flycast client (mirror_client
mode). The server dumps each frame's TA buffer to `/tmp/ta-dumps/frame_NNNNNN.bin`
and writes per-frame VRAM+PVR hashes to `/tmp/ta-dumps/hash.log`. The client
does the same to `/tmp/ta-dumps-client/`. Run both, then byte-cmp the dumps
and line-diff the hash logs. **If either test shows non-zero divergence, you
have a regression.**

Quick test recipe:

```bash
# Terminal 1 — server
MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 MAPLECAST_DUMP_TA=1 \
  ./build/flycast "$ROM"

# Terminal 2 — flycast client (renderer-only, CPU stopped)
MAPLECAST=1 MAPLECAST_MIRROR_CLIENT=1 MAPLECAST_SERVER_HOST=127.0.0.1 \
MAPLECAST_DUMP_TA=1 ./build/flycast "$ROM"

# Run for a minute, navigate scenes, then in another terminal:
cd /tmp
match=0; differ=0
for f in ta-dumps-client/frame_*.bin; do
  base=$(basename "$f")
  if cmp -s "$f" "ta-dumps/$base"; then match=$((match+1));
  else differ=$((differ+1)); fi
done
echo "TA byte: $match match, $differ differ"  # MUST be 0 differ
```

### Workarounds that were REMOVED — do not bring them back

Each of these existed to mask a wire race that no longer exists. They are
correlated with classes of bug — if you find yourself reaching for one of these,
you almost certainly have a regression in the determinism test instead.

- **DMA dirty bitmap** (`markVramDirty` + 11 hooks across `sh4_mem.cpp`,
  `pvr_mem.cpp`, `elan.cpp`, `ta.cpp`). The function still exists as a no-op so
  the call sites compile, but it does nothing. The memcmp diff loop catches
  every VRAM change correctly with shadow snapshot ordering fixed.

- **Scene-change `broadcastFreshSync()` heuristic** (used to fire when
  `totalDirty >= 128`). Removed; per-frame deltas converge correctly across
  scene transitions.

- **Periodic 10-second safety SYNC.** Was a band-aid for wasm drift between
  scene-change broadcasts. No longer needed.

- **`onOpen` FSYN broadcast.** Removed; the initial SYNC alone is sufficient
  for new clients to bootstrap.

- **`renderer->resetTextureCache = true` on every dirty VRAM page** (still
  present but no longer load-bearing for correctness — texture cache cleanup
  on its own would be enough; the forced reset is now just a safety belt).

What's KEPT (these are NOT workarounds, they are real correctness):
- Initial SYNC on connect (clients need a baseline)
- Forced SYNC on emulator reset (state is genuinely invalidated)
- `_decoded.pages` merge in the producer (prevents page loss on consumer overrun)
- 60-frame keyframe interval (lets new clients bootstrap mid-stream)

---

## Mirror Wire Format — Rules of the Road

The mirror stream's wire format is decoded by **FOUR independent parsers** that
must stay byte-for-byte aligned. Editing one without the others is the #1 way
to break this app:

| Role | File | Language |
|---|---|---|
| Producer (server) | `core/network/maplecast_mirror.cpp` `serverPublish()` | C++ |
| Desktop client | `core/network/maplecast_mirror.cpp` `clientReceive()` | C++ |
| king.html browser | `packages/renderer/src/wasm_bridge.cpp` `renderer_frame()` | C++ → WASM |
| emulator.html browser | `core/network/maplecast_wasm_bridge.cpp` `mirror_render_frame()` | C++ → WASM |
| VPS relay | `relay/src/protocol.rs` + `fanout.rs` | Rust |

Wire envelope is defined in `core/network/maplecast_compress.h` (ZCST magic
header for zstd-compressed frames). The desktop client (`clientReceive()`) is
the gold standard — when fixing a render bug in any browser, the answer is
almost always "make it look like clientReceive()."

### Frame Structure (uncompressed, after ZCST decode)

```
Delta frame:  frameSize(4) + frameNum(4) + pvr_snapshot(64) +
              taSize(4) + deltaPayloadSize(4) + deltaData(var) +
              checksum(4) + dirtyPageCount(4) +
              [regionId(1) + pageIdx(4) + data(4096)] * N

Keyframe:     deltaPayloadSize == taSize (full TA buffer, emitted every 60 frames)

SYNC:         "SYNC"(4) + vramSize(4) + vram(8MB) + pvrSize(4) + pvr(32KB)
              (compressed SYNC envelope wraps this in ZCST at level 3, ~13x ratio)
```

### Eight bugs we already paid for (don't reintroduce them)

1. **Decompressor sized too small.** Use a single 16MB shared decompressor for
   both SYNC and per-frame paths. Browser bridges that init at 512KB on the
   per-frame path will blow up on the next 8MB SYNC.

2. **Dropping dirty pages while waiting for first keyframe.** When a delta
   arrives before any keyframe (the first 1-59 frames after connect), you must
   STILL walk the dirty-pages section and apply VRAM/PVR updates. Skip only
   the actual `Process()`/`Render()` call. If you skip dirty pages too, VRAM
   drifts behind the server until the next page rewrite (sometimes seconds
   later).

3. **`VramLockedWriteOffset(pageOff)` MUST be called BEFORE `memcpy` into VRAM.**
   On desktop the texture cache mprotects pages, and writing first triggers
   SIGSEGV. Harmless on WASM but keep aligned across parsers.

4. **`_prevTA` must NEVER shrink.** Only grow. Truncating on a smaller frame
   loses tail bytes the next delta might patch into.

5. **`renderer->resetTextureCache = true` whenever ANY VRAM page is dirty.**
   This is THE bug that hid character select / loading screens for weeks.
   In-match VRAM is stable enough that the WebGL texture cache works without
   this flag. Scene transitions, where VRAM turns over heavily, silently
   render with stale textures from the previous scene unless this is set.

6. **Per-frame buffer in `web/js/render-worker.mjs` MUST handle oversized
   frames via temp malloc — NEVER silently drop them.** The persistent
   `_frameBuf` is 512KB, sized for the in-match steady state (~80KB header +
   ~230KB TA + 21 dirty pages). The post-scene-change keyframe that the server
   emits right after a fresh SYNC carries 300-540 dirty pages plus a fresh TA
   buffer — that envelope routinely exceeds 512KB compressed. The previous
   `if (len > MAX_FRAME) return;` silently dropped it. The wasm received the
   SYNC, cleared `_prevTA`, then NEVER saw the keyframe and got stuck dropping
   deltas in the empty-prevTA branch until the next periodic safety SYNC.
   The fix mirrors the SYNC path: malloc a temp buffer for oversized frames,
   call `_renderer_frame`, free. NEVER reintroduce a silent drop. If you must
   gate frame size, log it loudly and treat it as a bug.

7. **After `broadcastFreshSync()`, the server MUST reset `_taHasPrev = false`.**
   The wasm's `renderer_sync()` clears its `_prevTA` on SYNC receipt. If the
   very next frame the server ships is a delta (because `_taHasPrev` is still
   true), the wasm hits the `_prevTA.empty()` branch in `renderer_frame()`
   and silently drops the delta payload. Measured impact before fix: ~23
   frames of dropped renders per scene transition. Forcing the next frame to
   be a full keyframe (`deltaPayloadSize == taSize`) makes the wasm hit the
   keyframe branch which re-populates `_prevTA` from scratch. **Anywhere you
   call `broadcastFreshSync()`, set `_taHasPrev = false` immediately after.**

8. **After `broadcastFreshSync()`, the server MUST also reset
   `_regions[].shadow` from live `vram[]/pvr_regs`.** `broadcastFreshSync()`
   ships the live VRAM/PVR snapshot DIRECTLY — it never touches the per-region
   shadows that the per-frame diff loop reads. Without resetting the shadows,
   the next frame's memcmp diff is computed against the pre-SYNC shadow,
   shipping wrong-base deltas grafted on top of the SYNC bytes. The wasm
   receives the SYNC bytes correctly, then receives those wrong-base deltas
   on top — permanent VRAM divergence until something forces a full re-sync.
   Match the existing `client_request_sync` handler: after every
   `broadcastFreshSync()`, do `for (i...) memcpy(_regions[i].shadow,
   _regions[i].ptr, _regions[i].size);`.

### How a fragile-flow bug looks

| Symptom | Likely cause |
|---|---|
| In-match perfect, character select garbled / missing | Texture cache reset (#5) or dirty page skip (#2) |
| Wasm garbles for ~10s after every scene transition then self-heals | MAX_FRAME drop (#6) — post-SYNC keyframe is being silently discarded |
| Wasm vram diverges from server starting at scene-change SYNC, never recovers | Shadow reset missing after `broadcastFreshSync()` (#8) |
| Wasm renders stuck for ~23 frames after every SYNC then re-converges | `_taHasPrev` not reset after `broadcastFreshSync()` (#7) |
| Black screen, "[renderer] SYNC applied" but no KEYFRAME log | Relay lost upstream — `ssh root@66.55.128.93 journalctl -u maplecast-relay` |
| Black screen after a wasm rebuild | Browser cache — bump `?v=...` in `web/js/renderer-bridge.mjs` |
| SIGSEGV in TexCache on desktop client | `VramLockedWriteOffset` order wrong (#3) |
| First keyframe takes >1s on connect | Server keyframe interval changed (default 60 frames) |
| Server change "works" but browsers black | You forgot to update one of the four parsers above |

### Other hard-learned lessons

- **`palette_update()` must be called on every client.** MVC2 uses paletted
  textures. `PALETTE_RAM` contains raw entries; `palette_update()` converts
  them to `palette32_ram[]` using `PAL_RAM_CTRL & 3`. Without it, texture
  decode produces RGBA(0,0,0,0) and characters render invisible. The server
  gets this for free via `rend_start_render()`; clients skip that and must
  call `pal_needs_update = true; palette_update(); renderer->updatePalette = true;`
  manually.
- **NEVER use `emu.loadstate()` for live resync.** Corrupts scheduler/DMA/interrupt
  state → SIGSEGV after ~1000 frames. Use direct `memcpy` of RAM/VRAM/ARAM instead.
- **`memwatch::unprotect()` after any state sync.** `loadstate()` and normal
  boot call `memwatch::protect()`, which mprotects VRAM pages read-only. Our
  `memcpy` patches are silently dropped until unprotect.
- **PVR registers (32KB) must be diffed as their own region.** They contain
  palette RAM, FOG_TABLE, and ISP_FEED_CFG (translucent sort mode). Treat as
  a 4th memory region alongside RAM/VRAM/ARAM.
- **MVC2 characters are NOT render-to-texture.** All frames have `isRTT=0`.
  Characters are regular translucent textured polygons.
- **Save states store raw TA commands, not `rend_context`.** After a load,
  SH4 must run to trigger STARTRENDER → `ta_parse` to rebuild `rend_context`.

### Build pipeline (the easy-to-forget steps)

- **Desktop client:** edit `maplecast_mirror.cpp` → `cmake --build build` (live)
- **king.html WASM renderer:** edit `packages/renderer/src/wasm_bridge.cpp`
  → `cd packages/renderer/build && emmake make -j$(nproc)`
  → `cp build/renderer.{mjs,wasm} ../../web/`
  → `scp web/renderer.{mjs,wasm} root@66.55.128.93:/var/www/maplecast/`
  → **bump `?v=...` cache buster in `web/js/renderer-bridge.mjs`**
  → upload that too
- **emulator.html WASM core:** edit `core/network/maplecast_wasm_bridge.cpp`
  → also copy to `~/projects/flycast-wasm/upstream/source/core/network/`
  → `cd ~/projects/flycast-wasm/upstream/source/build-wasm && emmake make -j$(nproc)`
  → `cd ~/projects/flycast-wasm && bash upstream/link-ubuntu.sh`
  → 7z package → deploy → bump report timestamp
- **Rust relay:** edit `relay/src/*.rs` → `cd relay && bash deploy.sh 66.55.128.93 74.101.20.197`

### Dead code landmines

`core/network/maplecast/{client,server}/` and `core/network/maplecast_mirror_{client,server}.cpp`
exist but are NOT in the build (see `core/network/maplecast/README_DEAD_CODE.md`).
The experimental `MAPLECAST_CLIENT` CMake option and two-binary split live
there, unwired. Don't waste time editing these — your changes won't take effect.
Single binary with env var switching (`MAPLECAST_MIRROR_SERVER=1` vs
`MAPLECAST_MIRROR_CLIENT=1`) is the working path.

### Server per-frame breakdown (measured)

```
PVR snapshot:          ~0µs   (64 bytes)
TA copy to double buf: ~30µs  (140KB memcpy, double-buffered, no heap churn)
TA delta encode:       ~50-200µs (byte scan + run encoding vs _prevTA)
VRAM page diff:        ~200-500µs (memcmp 2048 × 4KB — biggest cost)
WebSocket send:        ~10-50µs (async)
zstd compress (lvl 1): ~80µs
TOTAL:                 ~370-880µs per frame
```

### Client render thread breakdown (measured)

```
Apply dirty pages:     ~5-80µs (memcpy to VRAM/PVR)
palette_update:        ~5-10µs
renderer->Process:     ~200-500µs (flycast ta_parse + texture resolve)
renderer->Render:      ~500µs (WebGL2 draw calls)
TOTAL:                 ~710-1090µs per frame
```

Decode runs on a background thread via double-buffered TA contexts, so it
does not add to the render-thread budget above.

### Mode 2: H.264 (Legacy, still works)

```
Flycast Emulator
  │ OpenGL renders frame at 640x480
  ▼
renderer->Present()
  │ Frame is on GPU as GL texture
  ▼
onFrameRendered()                          [maplecast_stream.cpp]
  │
  ├─ cuGraphicsMapResources()              GL texture → CUDA array
  │    0.03ms (GPU→GPU, zero CPU)
  │
  ├─ cuMemcpy2D()                          CUDA array → linear buffer
  │    (stays on GPU, never touches CPU)
  │
  ├─ nvEncEncodePicture()                  NVENC H.264 encode
  │    0.67ms (dedicated ASIC on RTX 3090)
  │    CABAC entropy, deblock filter, 30Mbps CBR
  │    Every frame is IDR (independently decodable)
  │
  ├─ nvEncLockBitstream()                  Get encoded bytes (~52KB)
  │
  ├─ Assemble packet:
  │    [header 32 bytes] + [H.264 NAL units ~52KB]
  │
  │    Header format:
  │    ┌────────┬────────┬────────┬────────┐
  │    │pipeline│ copy   │ encode │ frame  │ 4 bytes each, uint32 µs
  │    │  Us    │  Us    │  Us    │  Num   │
  │    ├────────┴────────┴────────┴────────┤
  │    │ P1: pps(2) cps(2) btn(2) lt rt   │ 8 bytes
  │    │ P2: pps(2) cps(2) btn(2) lt rt   │ 8 bytes
  │    ├───────────────────────────────────┤
  │    │ H.264 bitstream (Annex B)        │ ~52KB
  │    └───────────────────────────────────┘
  │
  ▼
broadcastBinary()
  │
  ├─→ WebRTC DataChannel "video"           P2P, UDP semantics
  │     (for peers with active DC)         No TCP head-of-line blocking
  │     {ordered: false, maxRetransmits: 0}
  │
  └─→ WebSocket (TCP)                      Fallback for non-P2P peers


Browser (H.264 mode)
  │ Receives binary frame (DataChannel or WebSocket)
  ▼
handleVideoFrame(data)                     [index.html]
  │
  ├─ Parse 32-byte header (diag stats)
  │
  ├─ Extract H.264 NAL units
  │
  ├─ VideoDecoder.decode()                 Hardware-accelerated
  │    codec: avc1.42001e (Baseline)
  │    optimizeForLatency: true
  │    0.9-2.6ms decode
  │
  ▼
ctx.drawImage(frame, 0, 0)                Canvas render
```

---

## Connection Flow — How Players Connect

```
1. Browser opens http://server:8000
   │
   ▼
2. index.html loads with:
   │ ├─ iframe src="emulator.html" (EmulatorJS + flycast WASM)
   │ ├─ Lobby UI (slots, queue, diagnostics, leaderboard)
   │ └─ WebSocket client for lobby protocol
   │
   ▼
3. iframe (emulator.html):
   │ ├─ Applies WebGL2 compatibility patches
   │ │   (GL_VERSION override, INVALID_ENUM suppression, texParameteri guard)
   │ ├─ Loads EmulatorJS with flycast core
   │ ├─ Boots MVC2 CHD from web/roms/mvc2.chd
   │ ├─ On game start: pauses CPU emulation, calls _mirror_init()
   │ ├─ Opens WebSocket to ws://server:7200 (binary mirror data)
   │ └─ Receives SYNC (full VRAM+PVR), then delta frames at 60fps
   │
   ▼
4. Parent page (index.html):
   │ ├─ Opens WebSocket to ws://server:7200 (JSON lobby + binary input)
   │ ├─ Receives status JSON every 1 second:
   │ │   {type:"status", p1:{...}, p2:{...}, spectators:N,
   │ │    queue:[...], frame:N, stream_kbps:N, publish_us:N,
   │ │    fps:N, dirty:N, registering:bool, sticks:N,
   │ │    game:{in_match, timer, p1_hp:[...], p2_hp:[...],
   │ │          p1_chars:[...], p2_chars:[...], p1_combo, p2_combo,
   │ │          p1_meter, p2_meter, stage}}
   │ ├─ Shows lobby: player slots, spectator count, queue list
   │ └─ Shows diagnostics: server FPS, bandwidth, publish time, dirty pages
   │
   ▼
5. Player sets name → clicks "I Got Next":
   │ Sends: {"type":"queue_join", "name":"tris"}
   │ Server adds to ordered queue, broadcasts updated status
   │
   ▼
6. When slot opens → queue auto-assigns next player:
   │ Player sends: {"type":"join", "id":"uuid", "name":"tris", "device":"..."}
   │ Server: registerPlayer() → assigns slot
   │ Responds: {"type":"assigned", "slot":0}
   │
   ▼
7. Browser gamepad input flows:
   │ Gamepad API → 4-byte binary via WebSocket → server forwards UDP:7100
   │
   ▼
8. NOBD stick registration (if needed):
   │ Player clicks "Register My Stick"
   │ Sends: {"type":"register_stick", "id":"browser-uuid"}
   │ Server enters registration mode
   │ Player taps any button 5 times, pauses, taps 5 times again
   │ Server detects rhythm → binds stick IP:port to browser user ID
   │ Stick input now routes to that user's slot (when they have one)
   │
   ▼
9. Spectators:
   │ Mirror data flows to ALL WebSocket clients (no slot required)
   │ Everyone sees the game, only assigned players can send input
   │ Spectator count + queue broadcast in status JSON
```

---

## Player Registry — Who's Who

```
┌──────────────────────────────────────────────────┐
│            Input Server Registry                  │
│         (maplecast_input_server.cpp)              │
│                                                   │
│  Slot 0 (P1):                                    │
│    connected: true                                │
│    type: NobdUDP                                  │
│    id: "nobd_192.168.1.100"                      │
│    name: "NOBD Stick"                            │
│    device: "NOBD 192.168.1.100:4977"             │
│    pps: 12200/s                                   │
│    buttons: 0xFFFF (idle)                        │
│    bound_to: "a1b2c3d4" (browser user ID)        │
│                                                   │
│  Slot 1 (P2):                                    │
│    connected: true                                │
│    type: BrowserWS                                │
│    id: "a1b2c3d4"                                │
│    name: "tris"                                   │
│    device: "PS4 Controller"                       │
│    pps: 250/s                                     │
│    buttons: 0xFFFF (idle)                        │
│                                                   │
│  Stick Bindings:                                  │
│    192.168.1.100:4977 → "a1b2c3d4" (browser ID)  │
│    (registered via rhythm: 5 taps, pause, 5 taps) │
│    Unregistered sticks are IGNORED, not routed    │
│                                                   │
│  Queue: ["player3", "player4"]                    │
│    Ordered list, next player auto-joins on open   │
│                                                   │
│  → Both visible in lobby                          │
│  → Both update kcode[] atomics                    │
│  → CMD9 reads same globals regardless of source   │
└──────────────────────────────────────────────────┘
```

---

## Game State & Leaderboard

```
maplecast_gamestate.cpp reads 253 bytes from MVC2 RAM every status tick:
  ├─ in_match: bool
  ├─ game_timer: uint8
  ├─ stage_id: uint8
  ├─ Per player (x2):
  │   ├─ 3 character IDs
  │   ├─ 3 character health values
  │   ├─ combo counter
  │   └─ super meter level
  └─ All frame-deterministic, verified via RAM autopsy

Server includes game state in status JSON → browser shows live stats.
Client tracks wins/losses in localStorage for leaderboard.
```

---

## File Map

```
core/network/
├── maplecast_input_server.cpp   ← THE input authority
│   ├── UDP thread (NOBD sticks, SO_BUSY_POLL)
│   ├── Player registry (slots, stats, latency)
│   ├── Stick registration (rhythm detection: 5 taps, pause, 5 taps)
│   ├── Stick bindings (IP:port → browser user ID)
│   ├── updateSlot() → kcode[]/lt[]/rt[] writes
│   └── injectInput() API for WebRTC/WebSocket
│
├── maplecast_input_server.h     ← Public API: init, registerPlayer, injectInput,
│                                   getPlayer, startStickRegistration, isRegistering,
│                                   registerStick, unregisterStick, registeredStickCount
│
├── maplecast_mirror.cpp         ← TA Mirror streaming (PRIMARY mode)
│   ├── Shadow copies for memcmp-based VRAM/PVR page diffs (4KB granularity)
│   ├── TA command buffer capture + run-length delta vs prev frame
│   ├── 14 PVR register snapshot
│   ├── serverPublish() → assemble frame → zstd compress → broadcast
│   ├── _compressor (MirrorCompressor) — pre-allocated ZSTD_CCtx
│   ├── SHM ring buffer for local client (uncompressed path)
│   ├── wsClientRun() — native client decode + decompression
│   └── Telemetry via updateTelemetry()
│
├── maplecast_mirror.h           ← Public API: initServer, initClient, publishFrame
│
├── maplecast_compress.h         ← zstd wire envelope (header-only)
│   ├── MCST_MAGIC_COMPRESSED = 0x5453435A (wire bytes "ZCST")
│   ├── MirrorCompressor — pre-allocated ZSTD_CCtx, level 1 frames / 3 SYNC
│   ├── MirrorDecompressor — pre-allocated ZSTD_DCtx, auto-grow output buf
│   ├── ZCST envelope: [magic(4)][uncompressedSize(4)][zstd blob]
│   └── Define MAPLECAST_COMPRESS_ONLY_DECOMPRESS for client-only builds
│
├── maplecast_ws_server.cpp      ← Unified WebSocket server (port 7200)
│   ├── Binary broadcast: mirror delta frames (compressed) to all clients
│   ├── Initial SYNC on connect: zstd-level-3 compressed (~8MB → ~600KB)
│   ├── JSON lobby: join, leave, queue_join, register_stick
│   ├── Status broadcast: every 1s with players/queue/game/telemetry/compression
│   ├── Browser input: binary 4-byte → UDP forward to 7100
│   ├── Game state inclusion (health, combos, meter, characters)
│   └── Spectator/viewer counting
│
├── maplecast_ws_server.h        ← Public API: init, broadcastBinary, updateTelemetry, active
│                                   Telemetry struct includes compressedSize + compressUs
│
├── maplecast_gamestate.cpp      ← Reads MVC2 RAM (253-byte format)
│   └── readGameState() → health, combo, meter, characters, timer, stage
│
├── maplecast_gamestate.h        ← GameState struct, readGameState()
│
├── maplecast_wasm_bridge.cpp    ← WASM exports for libretro/EmulatorJS browser client
│   ├── mirror_init() → initialize renderer for mirror mode
│   ├── mirror_apply_sync(ptr, size) → ZCST decompress → load VRAM + PVR
│   ├── mirror_render_frame(ptr, size) → ZCST decompress → apply diffs → render
│   └── mirror_present_frame() → present rendered frame to WebGL
│
├── maplecast_stream.cpp         ← H.264 encode (LEGACY mode, still works)
│   ├── CUDA GL interop (texture capture)
│   ├── NVENC H.264 encode (0.67ms)
│   └── onFrameRendered() → called after Present()
│
├── maplecast_webrtc.cpp         ← WebRTC DataChannel transport (H.264 mode)
│   ├── PeerConnection per client
│   ├── Video DC: server→client H.264
│   ├── Input DC: client→server W3 gamepad → injectInput()
│   ├── ICE/STUN NAT traversal
│   └── Signaling via callback to WebSocket
│
├── maplecast_webrtc.h           ← Public API: init, handleOffer, broadcastFrame
│
├── maplecast_xdp_input.cpp      ← AF_XDP zero-copy (future, needs Intel NIC)
├── maplecast_xdp_input.h
├── xdp_input_kern.c             ← BPF filter program
│
├── maplecast.cpp                ← Legacy (getPlayerStats reads kcode[] directly)
├── maplecast.h
├── maplecast_telemetry.cpp      ← UDP telemetry to localhost:7300
└── maplecast_telemetry.h

core/hw/maple/
├── maple_if.cpp                 ← Maple Bus DMA handler
│   └── maple_DoDma() → ggpo::getInput() → reads kcode[]
│       (clean — no maplecast code in this hot path)
│
└── maple_devs.cpp               ← CMD9 GetCondition handler
    └── config->GetInput(&pjs) → reads mapleInputState[]

core/hw/pvr/
├── Renderer_if.cpp              ← Hook: calls publishFrame() / onFrameRendered()
└── spg.cpp                      ← Scanline scheduler, triggers vblank → maple_DoDma()

shell/libretro/
└── libretro.cpp                 ← Added mirror_present_frame() for WASM builds

web/                             ← Static assets served by nginx on nobd.net
├── king.html                    ← PRIMARY browser client (modular ES6)
│   └── Imports from js/*.mjs (renderer-bridge, ws-connection, etc.)
│
├── js/
│   ├── renderer-bridge.mjs      ← WASM init + handleBinaryFrame()
│   │   ├── ZCST detection (magic === 0x5453435A)
│   │   ├── If isCompressedSync → _renderer_sync()
│   │   └── Else → _renderer_frame()
│   ├── ws-connection.mjs        ← Dual WebSocket: Worker (binary) + main (JSON)
│   ├── frame-worker.mjs         ← Inline Worker — zero-copy ArrayBuffer transfer
│   ├── relay-bootstrap.mjs      ← Initializes WebRTC P2P fan-out (relay.js)
│   ├── webgl-patches.mjs        ← GL_VERSION override, cap filtering
│   ├── lobby.mjs, queue.mjs, gamepad.mjs, chat.mjs, leaderboard.mjs
│   └── auth.mjs, profile.mjs, surreal.mjs, diagnostics.mjs, settings.mjs
│
├── relay.js                     ← MapleCastRelay class (WebRTC P2P fan-out)
│                                   ZCST-aware: skips parsing for compressed frames
│
├── renderer.mjs                 ← Emscripten loader (96KB)
├── renderer.wasm                ← Standalone WASM renderer (831KB, includes zstd)
│
├── emulator.html, play.html, mirror-wasm.html, test-renderer.html
│                                ← Legacy clients, all ZCST-aware
│
├── ejs-data/                    ← EmulatorJS runtime
├── bios/                        ← dc_boot.bin, dc_flash.bin
└── roms/                        ← mvc2.chd

packages/renderer/               ← Standalone WASM mirror renderer
├── src/wasm_bridge.cpp          ← renderer_init/sync/frame/resize/destroy
│   ├── ZCST decompression at top of renderer_sync + renderer_frame
│   └── Static MirrorDecompressor (16MB output buf, ZSTD_DCtx reused)
├── src/wasm_gl_context.cpp      ← WebGL2 context creation
├── src/glsm_patched.c           ← Libretro GL state machine (WebGL2 patched)
├── CMakeLists.txt               ← Emscripten build, links zstd decompress sources
└── build.sh                     ← emcmake + emmake wrapper
                                   Output: dist/renderer.{mjs,wasm}

relay/                            ← Rust zero-copy fan-out relay (runs on VPS)
├── src/main.rs                  ← CLI args, tokio runtime, mode select
├── src/fanout.rs                ← Core relay logic
│   ├── on_upstream_frame() — ZCST-aware: decompress for inspection only
│   ├── SyncCache — keeps last SYNC bytes for late joiners
│   ├── tokio broadcast channel (16-slot, lagging clients drop)
│   └── handle_ws_client() — sends cached SYNC then subscribes to fanout
├── src/protocol.rs              ← Wire format helpers
│   ├── is_sync, is_compressed (b"ZCST" check), decompress
│   ├── parse_sync, build_sync, apply_dirty_pages, frame_num
│   └── Detects ZCST envelope and handles both compressed + raw SYNCs
├── src/signaling.rs             ← Relay signaling messages (WebRTC P2P)
├── src/splice.rs                ← Future: kernel splice() zero-copy path
├── Cargo.toml                   ← deps: tokio, tokio-tungstenite, bytes, zstd
└── deploy.sh                    ← Build + scp + systemd install on VPS

start_maplecast.sh               ← Starts flycast + telemetry + (optional) web server
                                    Set RELAY_ONLY=1 to skip local web serve
                                    Set MAPLECAST_MIRROR_SERVER=1 for TA mirror mode
                                    Graceful shutdown on Ctrl+C
```

---

## Latency Budget

### TA Mirror Mode (Primary)

```
BUTTON PRESS → PIXEL ON SCREEN

NOBD Stick (hardware, LAN):
  Button press                    0µs
  → GPIO → cmd9ReadyW3           1-2µs (firmware ISR)
  → W6100 UDP send               ~50µs
  → Network (LAN)                ~100µs
  → Input server recvfrom        ~1µs (SO_BUSY_POLL)
  → kcode[] atomic store         ~10ns
  ─── input latency ───          ~150µs
  → Wait for next vblank         0-16.67ms (frame alignment)
  → CMD9 reads kcode[]           ~1ns
  → Game processes input          included in frame
  → GPU renders frame             included in frame
  → TA capture + VRAM diff       ~0.5ms (publish)
  → WebSocket send               ~0.01ms
  → Network (LAN)                ~0.2ms
  → WASM decode + WebGL render   ~2ms
  ─── total E2E ───              ~3-4ms + frame alignment

Browser Gamepad (WebSocket):
  Button press                    0µs
  → Gamepad API state cache       0-16.67ms (cache refreshes at vsync,
                                              60Hz monitor; ~7ms on 144Hz)
  → rAF-burst poll detects change ~1ms (16 MessageChannel polls per vsync)
  → WebSocket send                ~0.01ms
  → UDP forward to 7100           ~0.01ms
  → Input server recvfrom         ~0.01ms
  → kcode[] atomic store          ~10ns
  ─── input latency ───           ~8ms avg, 17ms worst (60Hz monitor)
                                  ~4ms avg, 8ms worst (144Hz monitor)
  → (same render/publish path)
  → TA capture + VRAM diff        ~0.5ms
  → WebSocket send                ~0.01ms
  → Network (LAN)                 ~0.2ms
  → WASM decode + WebGL render    ~2ms
  ─── total E2E ───               ~7ms + frame alignment
```

### H.264 Mode (Legacy)

```
NOBD Stick (hardware, LAN):
  (same input path as above)
  ─── input latency ───          ~150µs
  → CUDA copy                    0.03ms
  → NVENC encode                 0.67ms
  → DataChannel send             ~0.01ms
  → Network (LAN)                ~0.1ms
  → Browser decode               ~2.5ms
  ─── total E2E ───              ~3.6ms + frame alignment

Browser Gamepad (WebRTC P2P):
  (same input path, but via DataChannel — no UDP hop)
  ─── input latency ───          ~4ms
  → (same render/encode path)
  ─── total E2E ───              ~4.3ms + frame alignment
```

---

## Diagnostics & Telemetry

```
Server → Client (via status JSON, every 1 second):
  ├─ frame: current frame number
  ├─ fps: server render FPS
  ├─ stream_kbps: mirror bandwidth in Kbps
  ├─ publish_us: time to publish one frame (µs)
  ├─ dirty: number of dirty VRAM pages this frame
  ├─ registering: stick registration in progress
  ├─ sticks: number of registered sticks

Client-side measurements:
  ├─ WebSocket ping/pong latency
  ├─ Mirror FPS (frames rendered / elapsed time)
  └─ Displayed in diagnostics overlay (top-right corner)
```

---

## Environment Variables

```bash
MAPLECAST=1              # Enable MapleCast server mode
MAPLECAST_STREAM=1       # Enable H.264 streaming (legacy)
MAPLECAST_MIRROR=1       # Enable TA Mirror streaming (primary)
MAPLECAST_PORT=7100      # Input UDP port (default 7100)
MAPLECAST_STREAM_PORT=7200  # WebSocket port (default 7200)
MAPLECAST_WEB_PORT=8000  # Web server port (default 8000)
```

---

## Ports

| Host | Port | Protocol | Purpose |
|------|------|----------|---------|
| home | 7100 | UDP | NOBD stick input + WebSocket-forwarded browser input |
| home | 7200 | TCP (WebSocket) | Mirror binary broadcast (ZCST compressed) + JSON lobby + WebRTC signaling. Relay connects here as upstream client |
| home | 7300 | UDP | Telemetry (server → telemetry.py) |
| home | 8000 | HTTP | Local dev web server (skipped when RELAY_ONLY=1) |
| VPS  | 7201 | TCP (WebSocket) | maplecast-relay listens here. nginx /ws → 127.0.0.1:7201 |
| VPS  | 80   | HTTP | nginx, redirects to HTTPS |
| VPS  | 443  | HTTPS | nginx (Let's Encrypt) → static files + /ws + /db |
| VPS  | 8000 | HTTP | SurrealDB (player auth, stats, ELO) |

---

## Build Flags

| Flag | What | Set By |
|------|------|--------|
| `MAPLECAST_NVENC=1` | CUDA + NVENC encode (H.264 mode) | CMake (auto-detected) |
| `MAPLECAST_CUDA=1` | CUDA support (H.264 mode) | CMake (auto-detected) |
| `MAPLECAST_WEBRTC=1` | WebRTC DataChannel (H.264 mode) | CMake (libdatachannel found) |
| `MAPLECAST_XDP=1` | AF_XDP zero-copy input | CMake (libbpf/libxdp found) |

---

## Current Performance (April 2026)

### TA Mirror Mode (Primary)

| Metric | Value |
|--------|-------|
| Publish time (capture→send) | **~0.5ms** |
| Browser WASM decode + render | ~2ms |
| P1 E2E (NOBD HW, LAN) | **~3-4ms** |
| P2 E2E (browser gamepad, LAN) | **~7ms** |
| FPS | 60.0 |
| Drops | 0 |
| Bandwidth | ~4 MB/s (~32 Mbps) |
| Frame size | ~15-40KB |
| Resolution | Resolution-independent (client renders natively) |
| Codec | Raw TA commands + VRAM page diffs |

### H.264 Mode (Legacy)

| Metric | Value |
|--------|-------|
| Pipeline (capture→send) | **0.70ms** |
| CUDA copy | 0.03ms |
| NVENC encode | 0.67ms |
| Browser decode | 2.5ms |
| P1 E2E (NOBD HW) | **3.6ms** |
| P2 E2E (browser P2P) | **4.3ms** |
| FPS | 60.0 |
| Drops | 0 |
| Bandwidth | 25 Mbps |
| Frame size | ~52KB |
| Resolution | 640x480 |
| Codec | H.264 Baseline, all-IDR, CABAC |
