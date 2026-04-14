# MapleCast Architecture — Mental Model

## What Is MapleCast?

MapleCast turns a Flycast Dreamcast emulator into a game streaming server.
One instance of MVC2 runs on the server. Players connect with fight sticks
(NOBD) or browser gamepads. The server streams the game to all connected
clients in real-time via TA Mirror mode (raw GPU commands), zstd-compressed
and fanned out through a Rust relay on a public VPS.

**Where does the game actually run?** As of 2026-04-08, the entire pipeline
runs on a **single 2-vCPU VPS with no GPU**. Flycast is compiled in headless
mode (`-DMAPLECAST_HEADLESS=ON`, see "Mode 3: Headless" below) with zero
`libGL`/`libSDL`/`libX11`/`libvulkan`/`libcuda` linkage. The SH4 JIT, TA
capture, VRAM diff, zstd compression, and WebSocket broadcast are all pure
CPU. Total memory footprint on the VPS: ~322 MB (flycast ~301 MB + relay
~21 MB). CPU utilization: ~12% of 2 vCPU. Sustained 60 fps to public
`wss://nobd.net/ws`. The home box is no longer in the production path —
nobd.net spectators never touch it.

A separate GPU-backed flycast build still exists in the same source tree
for local sub-1ms play at a physical cab. Both builds coexist from one
checkout; see "Mode 3: Headless" below for the headless build invocation.
Operator-specific VPS configuration (systemd units, credentials, deploy
paths) lives in a separate private operator repo, not in this public
source tree.

## System Topology

```
nobd.net VPS                                         BROWSERS
═══════════════════════════════════════════                          ══════════

┌─────────────────────────────────────────┐                          ┌─────────┐
│  maplecast-headless.service              │                          │  king   │
│  (flycast, compile-out, no GPU libs)     │                          │  .html  │
│                                           │                          │         │
│  ┌───────────┐  ┌─────────────────┐      │                          │ renderer│
│  │ EMULATOR  │  │ INPUT SERVER    │      │                          │  .wasm  │
│  │ SH4 JIT + │◄─│ 7100/udp        │      │                          │  zstd   │
│  │ TA parse  │  │ NOBD UDP + WS   │      │                          │ decode  │
│  │ (norend)  │  │ input bridge    │      │                          │         │
│  └───────────┘  └─────────────────┘      │                          └─────────┘
│  ┌─────────────────────────────────┐     │                             ▲  ▲
│  │ STREAM SERVER                   │     │                             │  │
│  │  mirror.cpp + ws_server.cpp     │     │                             │  │
│  │  + compress.h                   │     │                             │  │
│  │  Listens: 127.0.0.1:7210        │     │                             │  │
│  │  (relay-only; no public expose) │     │                             │  │
│  └──────────────┬──────────────────┘     │                             │  │
└─────────────────┼────────────────────────┘                             │  │
                  │ ws://127.0.0.1:7210                                  │  │
                  │ (zstd-compressed TA frames, ~900 Kbps idle,          │  │
                  │  ~4 Mbps in-match)                                   │  │
                  ▼                                                      │  │
┌─────────────────────────────────────────┐                              │  │
│  maplecast-relay.service (Rust)          │                              │  │
│                                           │                              │  │
│  - WebSocket upstream client              │                              │  │
│  - zstd-aware fan-out                     │                              │  │
│  - SYNC cache (80× compressed, 0.1 MB)    │                              │  │
│  - signaling broadcast                    │                              │  │
│  - text/bin → upstream                    │                              │  │
│                                           │                              │  │
│  Listens: 0.0.0.0:7201                    │                              │  │
└──────────────┬──────────────────────────┘                               │  │
               │ nginx /ws → 127.0.0.1:7201                               │  │
               ▼                                                           │  │
┌─────────────────────────────────────────┐                               │  │
│  nginx (HTTPS, certbot, Let's Encrypt)   │                               │  │
│  /       → static (king.html, wasm)      │ ──── HTTPS on 443 ──────────┘  │
│  /ws     → relay  (wss://nobd.net/ws)    │                                │
│  /db     → SurrealDB                     │                                │
└─────────────────────────────────────────┘                                │
               │                                                            │
               ▼                                                            │
┌─────────────────────────────────────────┐                                │
│  SurrealDB (127.0.0.1:8000)              │                                │
│  player, match, ELO, badges, h2h, stats  │ ◄───── /db queries ────────────┘
└─────────────────────────────────────────┘
```

**Everything above lives on one VPS.** Two systemd services
(`maplecast-headless` and `maplecast-relay`) plus nginx and SurrealDB.
The flycast upstream listens only on localhost (`127.0.0.1:7210`) —
outside traffic never reaches flycast directly, it always goes through
the relay. This also means the input server on `:7100/udp` is
VPS-bound, so NOBD sticks at a physical cab trying to hit it would
need to know the VPS IP.

### Pillar 1: Emulator (Flycast, headless)
The Dreamcast emulator. Runs MVC2 at 60fps. The game thinks it's
talking to real controllers via the Maple Bus. It sends CMD9
(GetCondition) every frame to ask "what buttons are pressed?" The
answer comes from `kcode[]` globals. The server also reads 253 bytes
of MVC2 RAM each frame for live game state (health, combos, meter,
characters). **In production this is the headless build** — zero
GPU libraries, `norend` wired in, ~301 MB RSS, ~12% of 1 CPU. See
"Mode 3: Headless" later in this doc.

### Pillar 2: Input Server (`maplecast_input_server.cpp`)
Single source of truth for all player input. Receives from multiple
sources, writes to one place. Tracks who's connected, their latency,
their device type. Manages NOBD stick registration (rhythm-based
binding to browser users), player queue ("I Got Next"), and slot
assignment. Binds `0.0.0.0:7100/udp` on the VPS.

### Pillar 3: Stream Server (`maplecast_mirror.cpp` + `maplecast_ws_server.cpp` + `maplecast_compress.h`)
Captures raw TA command buffers + 14 PVR registers + VRAM page diffs
each frame, run-length-deltas the TA buffer vs the previous frame,
then **zstd-compresses** the assembled frame (level 1, ~80us per
frame) and broadcasts via WebSocket. SHM ring buffer for local mirror
clients stays uncompressed. Compressed envelope:
`[ZCST(4)][uncompSize(4)][zstd blob]`. Sustained ~4 Mbps for 60fps
MVC2 in-match, ~900 Kbps on a static title screen.

**Production listen address is `127.0.0.1:7210`** (loopback only —
only the relay on the same VPS can reach it). The GPU-backed home
build listens on `0.0.0.0:7200` instead, but that build isn't in
the nobd.net production path anymore.

### Pillar 4: Relay (`relay/` — Rust, on the same VPS)
Connects upstream as a WebSocket client to `ws://127.0.0.1:7210`, fans
frames out to up to 500 browser clients on port 7201. Maintains a
SYNC cache so late joiners get instant initial state (cached SYNC is
0.1 MB — the 8 MB full VRAM+PVR dump compresses 80× via zstd level 3).
ZCST-aware: decompresses for state inspection, forwards original
compressed bytes downstream (zero re-encode overhead). Also forwards
client-originated text/binary messages back to upstream flycast
(player input, queue commands, chat). nginx terminates TLS and
reverse-proxies `/ws` → `127.0.0.1:7201`.

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
Flycast Emulator (headless server, nobd.net VPS, listens on 127.0.0.1:7210)
  │ (no GPU — `norend` just runs ta_parse() on CPU, see Mode 3 below)
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
  │ Binds 127.0.0.1:7210 (loopback on the VPS).
  │ GPU/dev builds bind 0.0.0.0:7200 instead.
  │
  ▼
══════════════════════ VPS LOOPBACK ══════════════════════
  │ Zero network hop — flycast and relay are same-box neighbors.
  │ ~6-15KB per frame (60% bandwidth saved vs uncompressed).
  │
  ▼
MapleCast Relay (Rust, same VPS :7201)       [relay/src/fanout.rs]
  │ Connects upstream: ws://127.0.0.1:7210
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
| Black screen, "[renderer] SYNC applied" but no KEYFRAME log | Relay lost upstream — `ssh root@<your-vps> journalctl -u maplecast-relay` |
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
  → `scp web/renderer.{mjs,wasm} root@<your-vps>:/var/www/maplecast/`
  → **bump `?v=...` cache buster in `web/js/renderer-bridge.mjs`**
  → upload that too
- **emulator.html WASM core:** edit `core/network/maplecast_wasm_bridge.cpp`
  → also copy to `~/projects/flycast-wasm/upstream/source/core/network/`
  → `cd ~/projects/flycast-wasm/upstream/source/build-wasm && emmake make -j$(nproc)`
  → `cd ~/projects/flycast-wasm && bash upstream/link-ubuntu.sh`
  → 7z package → deploy → bump report timestamp
- **Rust relay:** edit `relay/src/*.rs` → `cd relay && bash deploy.sh <your-vps> 127.0.0.1`
  (upstream is now the VPS-local headless flycast on `127.0.0.1:7210`; the
  old home IP `<old-home-ip>` is no longer in the production path.)
- **Headless flycast server:** edit any source file → rebuild locally with
  `cmake --build build-headless -- -j$(nproc)` → run
  `./deploy/scripts/deploy-headless.sh root@<your-vps>` to ship the new
  binary + systemd restart. The deploy script runs an `ldd` sanity check
  before uploading; if the binary has any `libGL`/`libSDL`/`libX11` linkage
  it bails.

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

### Mode 3: Headless (No GPU) — PRODUCTION

**This is what's running on nobd.net as of 2026-04-08.** The headless
build runs flycast on a CPU-only box — no `libGL`, no `libSDL2`, no
`libX11`, no `libvulkan`, no window system at all. It produces
**byte-identical** TA mirror wire to the GPU-backed build, enforced
by the `MAPLECAST_DUMP_TA` determinism rig (460/460 TA buffers
matched, 0 differ in the final validation run).

The VPS instance runs as `maplecast-headless.service`, listens on
`127.0.0.1:7210` (loopback only), and feeds directly into the
relay on the same box — see "System Topology" above.

**Two ways to enable headless:**

1. **Runtime gate** on a GPU-capable build (useful for dev work on a
   machine that has a display):
   ```bash
   MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 MAPLECAST_HEADLESS=1 \
     ./build/flycast /path/to/mvc2.gdi
   ```
   Same binary that would also drive a physical cab if you wanted one.
   Takes a different branch at boot: skips `os_CreateWindow()`, wires
   `norend`, bypasses the `imguiDriver` null-check in `mainui_loop`,
   auto-loads the ROM, closes the GUI, enters the `emu.render()` path
   directly.

2. **Compile-out build** for deployment on CPU-only hosts:
   ```bash
   cmake -B build-headless -DMAPLECAST_HEADLESS=ON -DCMAKE_BUILD_TYPE=Release ..
   cmake --build build-headless
   # Result: 27 MB stripped binary with zero GPU/SDL/X11/audio linkage
   ```
   Forces `USE_OPENGL=OFF`, `USE_VULKAN=OFF`, `USE_DX*=OFF`, `USE_SDL=OFF`,
   audio backends off (pulseaudio transitively pulls X11), CUDA/NVENC
   detection skipped, libdatachannel submodule skipped. `MAPLECAST_HEADLESS_BUILD`
   compile-define is set; `isHeadless()` returns true at compile time, so
   the env var is optional.

**Why it works** (the invariant):

The mirror wire is generated from CPU-side state only. The render message
loop in [core/hw/pvr/Renderer_if.cpp:197-198](../core/hw/pvr/Renderer_if.cpp#L197)
calls `serverPublish()` **before** `renderer->Process()`:

```cpp
// Mirror server: capture TA commands BEFORE Process consumes them
if (maplecast_mirror::isServer() && taContext)
    maplecast_mirror::serverPublish(taContext);
try {
    renderer->Process(taContext);   // norend → ta_parse on CPU
} catch (...) { ... }
```

So the TA bytes, VRAM page diffs, PVR register snapshot, and dirty page
list that go out on the wire are the same whether the renderer is
GLES/Vulkan/DX or [core/rend/norend/norend.cpp](../core/rend/norend/norend.cpp)
(which just runs `ta_parse(ctx, true)` on CPU and no-ops everything else).
`Renderer::Present()` defaults to `return true;` in the base class, so
norend's frame cadence fires `presented = true` and `sh4->Stop()` just
like the GPU path — no render queue weirdness.

**What the headless build does NOT include:**

- **H.264/NVENC/CUDA** — GPU-only, compile-excluded via `NOT MAPLECAST_HEADLESS`
  gates on the `find_path(CUDA_INCLUDE)` and `add_subdirectory(libdatachannel)`
  blocks in `CMakeLists.txt`. `maplecast_stream.cpp` and `maplecast_webrtc.cpp`
  are dropped from `core/network/CMakeLists.txt` in headless.
- **Audio output** — `USE_ALSA`, `USE_LIBAO`, `USE_OSS`, `USE_PULSEAUDIO`
  forced OFF. The mirror wire has never shipped audio anyway; disabling
  them avoids `libpulse` transitively dragging in `libX11`.
- **SDL / X11 / DreamLink / libusb / libpico-port** — the entire SDL
  block is gated on `NOT MAPLECAST_HEADLESS`. Without SDL, the X11
  fallback in `find_package(X11 REQUIRED)` is also gated so headless
  builds don't fall through to an X11 dependency.
- **ImGui driver backends** (`OpenGLDriver`, `VulkanDriver`, DX drivers) —
  none compiled. `imguiDriver` global stays `nullptr`. The mainui_loop
  `imguiDriver == nullptr → forceReinit` trap is bypassed via
  `!headless` guards.
- **LUA scripting, Discord RPC, Breakpad** — forced OFF (not strictly
  needed but they pull in deps we don't need).

**What the headless build DOES include** (DT_NEEDED of the final binary):

```
libcurl.so.4    (http_client for HTTPS auth flows)
libz.so.1       (generic compression)
libxdp.so.1     (AF_XDP zero-copy input — optional)
libbpf.so.1     (companion to libxdp)
libgomp.so.1    (OpenMP used by the SH4 dynarec in places)
libudev.so.1    (transitive via libxdp)
libstdc++.so.6, libm.so.6, libgcc_s.so.1, libc.so.6
```

That's it. No graphics, no window system, no audio.

**Deployment:**

- **Native / systemd** (recommended): use `deploy/scripts/deploy-headless.sh`.
  Builds locally with `cmake -DMAPLECAST_HEADLESS=ON`, sanity-checks `ldd`
  for forbidden libs, scps binary + systemd unit + env file to the VPS,
  creates the `maplecast` user, installs to `/usr/local/bin/flycast`,
  enables + starts `maplecast-headless.service`. See `deploy/systemd/`.
- **Docker** (WIP): `Dockerfile.headless` builds a 117 MB Debian 12 slim
  image. Build works cleanly end-to-end. Container currently SIGBUSes
  at runtime before reaching the main loop — suspect seccomp / vmem
  namespace interaction with the SH4 dynarec's reserved-address space
  strategy in `addrspace::reserve()`. Use the native systemd path until
  this is debugged.

**Verified on the home box, then deployed to the nobd.net VPS** (CPU-only
runtime via the compile-out binary):

| Metric | Dev verification (home) | Production (nobd.net VPS) |
|---|---|---|
| Binary size | 27 MB stripped | 26 MB stripped |
| `ldd` forbidden libs | **zero** | **zero** |
| Frame rate | 60.1 fps sustained over 5s | 59.7 fps sustained via public `wss://nobd.net/ws` |
| Bandwidth | 3.7 Mbps (in-match) | ~900 Kbps (title screen idle), ~4 Mbps projected in-match |
| Wire magic | `ZCST` (correct envelope) | `ZCST` (same wire) |
| Determinism rig | **460/460 TA match, 0 differ** | — (not re-run on VPS; wire is the same) |
| Visual signoff | GPU mirror client from headless server | Browser on nobd.net rendering VPS-backed MVC2 |
| Memory (flycast) | ~300 MB RSS | **301 MB RSS** |
| Memory (flycast + relay) | — | **322 MB total** (flycast 301 + relay 21) |
| CPU (of one core) | ~24% | ~24% (~12% of 2 vCPU) |
| Relay flip latency (old home upstream → new `127.0.0.1:7210`) | — | **42 ms** (clients auto-reconnected) |

**2026-04-08 deploy timeline:**
- T+0:   SSH, install `libxdp1` + `libzip4t64` on Ubuntu 24.04 VPS
- T+30s: scp 26 MB flycast + 1.2 GB ROM + 6.7 MB savestate
- T+60s: install layout, create `maplecast` user, systemd unit with
         `MAPLECAST_SERVER_PORT=7210`, `MAPLECAST_ROM=/opt/maplecast/roms/mvc2.gdi`
- T+65s: `systemctl enable --now maplecast-headless` → alive, listening on `:7210`
- T+70s: `sed` relay `ExecStart` upstream from `ws://<old-home-ip>:7200`
         to `ws://127.0.0.1:7210`, `systemctl restart maplecast-relay`
- T+72s: Relay reconnected to local headless upstream (42 ms handshake),
         3 existing browser clients auto-reconnected, SYNC cached
- T+80s: Verified from home network via `wss://nobd.net/ws` — **59.7 fps**

The home box has been out of the nobd.net production path since
T+70s on 2026-04-08.

**Build pipeline** (add to the easy-to-forget steps list):

- **Headless server:**
  `cmake -B build-headless -DMAPLECAST_HEADLESS=ON -DCMAKE_BUILD_TYPE=Release ..`
  → `cmake --build build-headless -- -j$(nproc)`
  → Verify: `ldd build-headless/flycast | grep -iE 'libGL|libSDL|libX11|libvulkan'` returns empty.

**Side-by-side signoff recipe** (when touching anything headless-adjacent):

```bash
# Terminal 1 — headless server on sandbox ports
MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 \
MAPLECAST_PORT=7130 MAPLECAST_SERVER_PORT=7230 \
  ./build-headless/flycast path/to/mvc2.gdi

# Terminal 2 — GPU mirror client pointing at headless server
MAPLECAST=1 MAPLECAST_MIRROR_CLIENT=1 \
MAPLECAST_SERVER_HOST=127.0.0.1 MAPLECAST_SERVER_PORT=7230 \
MAPLECAST_PORT=7131 \
  ./build/flycast path/to/mvc2.gdi
```

GPU client window must show MVC2. If it stays black for more than
~2 seconds, the initial SYNC isn't arriving — check headless server
log for `[MIRROR] === SERVER MODE === streaming TA + memory diffs`.

The full Phase 1–5 implementation history for the headless server lives
in the `headless-server` branch commit history (and in a local archive
copy of the workstream doc — not committed to the public repo).

---

### Mode 4: WebGPU (Pure JS, Zero WASM)

Pure JavaScript + WebGPU renderer. No WASM, no compile step, no build
toolchain. Edit a `.mjs` file, refresh the browser, see the result. Lives
at `web/webgpu-test.html` and `web/webgpu/*.mjs`. See
[WEBGPU-RENDERER.md](WEBGPU-RENDERER.md) for full architecture details.

**Key differences from king.html WASM path:**
- Decode/parse/render all in JS (~5 KB modules + 60 KB vendored fzstd)
- WebGPU instead of WebGL2
- WebTransport (QUIC/UDP) with WebSocket fallback
- 1.88 ms per-frame process time (11% of 16.67 ms budget)
- Vsync-decoupled: network decode runs on arrival, RAF renders latest geometry

---

## WebTransport — QUIC/UDP Streaming

### Why WebTransport

The TA mirror stream is inherently loss-tolerant: delta frames are disposable
(the next keyframe, emitted every 60 frames, corrects any drift), while SYNC
packets must arrive intact. TCP (WebSocket) forces reliable delivery of every
byte, causing head-of-line blocking when a delta packet is lost — the entire
stream stalls while TCP retransmits a frame the renderer doesn't even need.

WebTransport over QUIC provides separate channels: **unreliable datagrams**
for disposable delta frames (no retransmit, no HOL blocking) and **reliable
unidirectional streams** for SYNC packets (guaranteed delivery).

### Adaptive transport

Implemented in `web/webgpu/transport.mjs`. The `AdaptiveTransport` class
tries WebTransport first with a 3-second timeout, then falls back to
WebSocket. Callers receive frames via a unified `onframe(Uint8Array)`
callback regardless of underlying transport.

### Relay dual-listener architecture

The Rust relay runs two listeners on the VPS:

| Listener | Port | Protocol | Purpose |
|----------|------|----------|---------|
| QUIC | UDP :443 | WebTransport | Browsers with WebTransport support |
| WebSocket | TCP :7201 | WS (via nginx /ws) | Legacy browsers, native clients |

Both listeners share the same upstream connection to `ws://127.0.0.1:7210`
(flycast headless on loopback). Compressed ZCST frames are forwarded
verbatim — zero re-encode on either path. The relay uses the `wtransport`
Rust crate for the QUIC listener, sharing the same Let's Encrypt TLS
certificate as nginx.

**Forwarding rule:** The relay MUST NOT forward `join`/`leave` control
messages to the upstream flycast server. The relay manages its own client
roster; forwarding these causes slot conflicts. Only gamepad input and
queue commands are relayed upstream.

### Measured performance (April 2026)

| Metric | WebTransport (QUIC/UDP) | WebSocket (TCP) |
|--------|------------------------|-----------------|
| RTT | **45.6 ms** | 72 ms |
| Process time | **1.88 ms** | ~2.06 ms |
| True E2E (RTT/2 + process) | **24.7 ms** | ~38 ms |
| Delta frame loss rate | ~0.1% | 0% |
| Recovery from loss | Next keyframe (< 1 second) | N/A |

The 37% RTT improvement comes from eliminating TCP head-of-line blocking.
The process time improvement comes from vsync decoupling enabled by
datagram arrival (no TCP ordering delay). For MVC2, 13 ms saved is nearly
one full frame of reduced input-to-pixel latency.

For the full WebGPU renderer architecture, texture decode pipeline, rendering
details, and known issues, see [WEBGPU-RENDERER.md](WEBGPU-RENDERER.md).

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
│    id: "nobd_192.0.2.100"                      │
│    name: "NOBD Stick"                            │
│    device: "NOBD 192.0.2.100:4977"             │
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
│    192.0.2.100:4977 → "a1b2c3d4" (browser ID)  │
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

The budget depends on **where the flycast instance lives** relative to
the player. There are two topologies:

1. **VPS-resident headless flycast (nobd.net production, 2026-04-08+).**
   Flycast and relay are on the same VPS box. Browsers on the internet
   hit `wss://nobd.net/ws`. Player input pays **internet RTT from
   player to VPS**. This is the live nobd.net stream for remote
   spectators.

2. **Home-resident GPU flycast (dev only).** Flycast runs on a local
   box with a GPU, browser connects over LAN. Player input pays LAN
   RTT (~0.2 ms). This is what you use when doing local development
   or sitting at a physical cab.

### Mode 1a — TA Mirror, VPS production (primary)

```
BUTTON PRESS → PIXEL ON SCREEN (browser on the internet)

Browser Gamepad:
  Button press                    0µs
  → Gamepad API state cache       0-16.67ms (vsync aligned, 60Hz monitor)
  → rAF-burst poll detects        ~1ms
  → WebSocket send                ~0.01ms
  → TLS wrap + NIC queue          ~0.1ms
  → Internet hop (browser→VPS)    varies by geo:
                                   - same city:           ~5-10ms RTT → ~2.5-5ms one-way
                                   - cross-country US:    ~40-70ms RTT → ~20-35ms one-way
                                   - transoceanic:        ~150-200ms RTT → ~75-100ms one-way
  → nginx TLS terminate           ~0.2ms
  → Relay → local flycast         <0.1ms (loopback on VPS)
  → Input server recvfrom         ~0.01ms
  → kcode[] atomic store          ~10ns
  ─── input latency (one-way) ─── ~3-100ms (dominated by geo)
  → Wait for next vblank          0-16.67ms (frame alignment)
  → SH4 processes input + renders included in frame
  → TA capture + VRAM diff        ~0.5ms
  → zstd compress (level 1)       ~80µs
  → WebSocket emit → relay        <0.1ms (VPS loopback)
  → Relay fanout                  <0.1ms (zero re-encode)
  → Internet hop (VPS→browser)    same as input path, one-way
  → TLS unwrap                    ~0.1ms
  → WASM decode + WebGL render    ~2ms
  ─── total E2E ────────────────── ~10-200ms depending on geo + monitor
```

**The internet RTT to the VPS is the single biggest variable.** A
player in New Jersey to a VPS in New Jersey measures sub-20ms total;
a player in Tokyo to a New Jersey VPS measures ~200ms total. The
emulator itself only contributes ~3-5ms.

### Mode 1b — TA Mirror, home LAN dev (legacy numbers)

These are the numbers when flycast runs on a LAN box next to the
player — useful if you're building a physical cab or doing local dev.

```
NOBD Stick (hardware, LAN):
  Button press                    0µs
  → GPIO → cmd9ReadyW3           1-2µs (firmware ISR)
  → W6100 UDP send               ~50µs
  → Network (LAN)                ~100µs
  → Input server recvfrom        ~1µs (SO_BUSY_POLL)
  → kcode[] atomic store         ~10ns
  ─── input latency ───          ~150µs
  → Wait for next vblank         0-16.67ms (frame alignment)
  → SH4 + TA capture             ~0.5ms
  → WebSocket emit               ~0.01ms
  → Network (LAN)                ~0.2ms
  → WASM decode + WebGL render   ~2ms
  ─── total E2E ───              ~3-4ms + frame alignment

Browser Gamepad (WebSocket, LAN):
  Button press                    0µs
  → Gamepad API state cache       0-16.67ms (vsync aligned)
  → rAF-burst poll detects change ~1ms
  → WebSocket send                ~0.01ms
  → UDP forward to 7100           ~0.01ms
  → Input server recvfrom         ~0.01ms
  → kcode[] atomic store          ~10ns
  ─── input latency ───           ~8ms avg, 17ms worst
  → (same render/publish path as NOBD)
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
MAPLECAST=1               # Enable MapleCast server mode
MAPLECAST_STREAM=1        # Enable H.264 streaming (legacy)
MAPLECAST_MIRROR=1        # Enable TA Mirror streaming (primary)
MAPLECAST_MIRROR_SERVER=1 # Mirror server mode (publishes TA wire on :7200)
MAPLECAST_MIRROR_CLIENT=1 # Mirror client mode (renders from wire, CPU stopped)
MAPLECAST_HEADLESS=1      # Runtime headless gate on GPU builds. No-op on
                          # compile-out builds (already headless at build time).
MAPLECAST_DUMP_TA=1       # Dump per-frame TA buffers to /tmp/ta-dumps/ for
                          # the determinism rig. Server side also dumps VRAM
                          # + PVR hashes. Client side dumps to /tmp/ta-dumps-client/.
MAPLECAST_SERVER_HOST=... # Mirror client: upstream host (default 127.0.0.1)
MAPLECAST_SERVER_PORT=... # Mirror server: WS listen port (default 7200).
                          # Mirror client: upstream port.
MAPLECAST_PORT=7100       # Input UDP port (default 7100)
MAPLECAST_STREAM_PORT=7200  # H.264 WebSocket port (default 7200, legacy only)
MAPLECAST_WEB_PORT=8000   # Web server port (default 8000)
```

---

## Ports

**Production (nobd.net) — everything runs on the VPS as of 2026-04-08:**

| Host | Port | Bind | Protocol | Purpose |
|------|------|------|----------|---------|
| VPS  | 7100 | 0.0.0.0 | UDP | NOBD stick input + WebSocket-forwarded browser input (input server) |
| VPS  | 7210 | 127.0.0.1 | TCP (WebSocket) | **Headless flycast mirror server** — loopback only, relay consumes from here |
| VPS  | 7201 | 0.0.0.0 | TCP (WebSocket) | maplecast-relay listens here. nginx `/ws` → 127.0.0.1:7201 |
| VPS  | 7202 | 127.0.0.1 | HTTP | relay HTTP endpoint: `/metrics`, `/health`, `/api/*`, `/turn-cred` |
| VPS  | 80   | 0.0.0.0 | HTTP  | nginx, redirects to HTTPS |
| VPS  | 443  | 0.0.0.0 | HTTPS | nginx (Let's Encrypt) → static files + `/ws` + `/db` |
| VPS  | 8000 | 127.0.0.1 | HTTP | SurrealDB (player auth, stats, ELO) |

**Dev / home (GPU build, local development only):**

| Host | Port | Protocol | Purpose |
|------|------|----------|---------|
| home | 7100 | UDP | NOBD stick input (dev only) |
| home | 7200 | TCP (WebSocket) | GPU flycast mirror server (dev only — NOT in nobd.net production path) |
| home | 7300 | UDP | Telemetry (server → telemetry.py) |
| home | 8000 | HTTP | Local dev web server (skipped when `RELAY_ONLY=1`) |

When doing local dev against a home flycast, the browser at
`http://localhost:8000/king.html` bypasses nginx and connects directly
to the home flycast's `:7200`. Nothing on this side is talking to
`nobd.net` or the VPS.

---

## Build Flags

| Flag | What | Set By |
|------|------|--------|
| `MAPLECAST_NVENC=1` | CUDA + NVENC encode (H.264 mode) | CMake (auto-detected) |
| `MAPLECAST_CUDA=1` | CUDA support (H.264 mode) | CMake (auto-detected) |
| `MAPLECAST_WEBRTC=1` | WebRTC DataChannel (H.264 mode) | CMake (libdatachannel found) |
| `MAPLECAST_XDP=1` | AF_XDP zero-copy input | CMake (libbpf/libxdp found) |
| `MAPLECAST_HEADLESS_BUILD` | CPU-only compile-out (no GL/SDL/X11/audio) | CMake `-DMAPLECAST_HEADLESS=ON` |

---

## Current Performance (April 2026)

### TA Mirror Mode — VPS production (2026-04-08+)

| Metric | Value |
|--------|-------|
| Host | VPS (2 vCPU, 2 GB RAM, no GPU) at nobd.net |
| Binary | 26 MB stripped compile-out (`-DMAPLECAST_HEADLESS=ON`) |
| Publish time (capture→send) | **~0.5ms** |
| Browser WASM decode + render | ~2ms |
| P1/P2 E2E | **dominated by internet RTT player↔VPS** (~10-200ms geo-dependent) |
| FPS | **59.7** (measured via public `wss://nobd.net/ws`) |
| Drops | 0 |
| Bandwidth | ~900 Kbps idle, ~4 Mbps in-match (zstd level 1) |
| SYNC bandwidth | **80× compression** (8 MB → 0.1 MB via zstd level 3) |
| Frame size | ~15-40 KB uncompressed, ~6-15 KB on wire |
| Resolution | Resolution-independent (client renders natively) |
| Codec | Raw TA commands + VRAM page diffs |
| Memory (flycast) | 301 MB RSS |
| Memory (flycast + relay) | 322 MB total |
| CPU | ~12% of 2 vCPU (~24% of one core) |

### TA Mirror Mode — Home GPU build (dev only)

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
| Resolution | Resolution-independent |
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

---

# Pillar 5: Distributed Input Server Network (the moat)

> **READ THIS BEFORE TOUCHING ANY HUB / NODE / NATIVE-CLIENT CODE.**
>
> This is the biggest architectural shift since the headless flycast migration.
> If you're an agent reading this for the first time, internalize the model
> before making changes.

## What changed and why

Until April 14, 2026, MapleCast ran on **one VPS in NJ**. Two players in LA
playing through it ate ~40ms one-way input latency from the geographic
distance alone. The flycast headless server is absurdly cheap (301MB RAM,
12% of 2 vCPU, no GPU) — anyone with a $5/month VPS can run one.

We turned that observation into the architecture: **anyone can run an "input
server" (which is just flycast headless + the relay)**. A central hub on
nobd.net lets browsers and native clients discover available servers,
ping-test them, and connect to whichever is closest. Same-region players
get ~2-5ms RTT instead of ~30-100ms. Twenty-fold latency improvement for
many users.

The hub is **discovery-only — never in the gameplay hot path.** Once a
match is assigned, browsers/clients connect directly to the chosen input
server. The hub is never on the data path.

Naming: in user-facing surface area we call them **"input servers"**
because that's what they ARE — the authoritative source of input for the
match (plus the SH4 game compute, plus the frame source). Internal
identifiers in code (`Node` struct, `node_id` field) keep the original
"node" naming for stability — both `/api/nodes/*` and
`/api/input-servers/*` URL routes work.

## Topology

```
                 ┌──────────────────────────────────┐
                 │ HUB (nobd.net, axum/Rust)         │
                 │ Discovery + Matchmaking ONLY      │
                 │ /hub/api/{input-servers,matchmake,│
                 │           dashboard/*}            │
                 │ Backed by SurrealDB               │
                 └──────────┬───────────┬───────────┘
                            │  10s heartbeat
              ┌─────────────┴──────┐ ┌──┴──────────────┐
              │ INPUT SERVER A     │ │ INPUT SERVER B  │  ← anyone runs these
              │ (nobd.net itself)  │ │ (Chicago, etc.) │
              │                    │ │                 │
              │  flycast headless  │ │  flycast headl. │
              │   + maplecast_relay│ │  + maplecast_relay
              │     (with hub      │ │   (with hub     │
              │      registration) │ │    registration)│
              │                    │ │                 │
              │ Ports:             │ │ Ports:          │
              │  7100/udp - input  │ │  7100/udp       │
              │  7201/tcp - relay  │ │  7201/tcp       │
              │  7210/tcp - control│ │  7210/tcp       │
              │  7213/tcp - audio  │ │  7213/tcp       │
              │  7220/tcp - hub    │ │  (hub on        │
              │   (only on the     │ │   nobd.net      │
              │    one running it) │ │   only)         │
              └────────┬─────┬─────┘ └────────┬─────────┘
                       ▲     │                ▲
              HOT PATH (no hub involvement after match assignment)
                       │     │                │
              ┌────────┴─────┴────────┐  ┌────┴──────────┐
              │ BROWSER (king.html)   │  │ NATIVE CLIENT │
              │ - WebTransport for    │  │ (flycast --   │
              │   TA frames           │  │  client mode) │
              │ - WS for control      │  │               │
              │ - SurrealDB on nobd   │  │ - WS for TA   │
              │ - Mixed-content note: │  │ - raw UDP for │
              │   community nodes     │  │   input (sub- │
              │   need TLS for HTTPS  │  │   1ms native) │
              │   page to reach them  │  │ - no TLS req'd│
              └───────────────────────┘  └───────────────┘
```

## Hub service

**Crate**: `hub/` (Rust + axum). Binds `127.0.0.1:7220`. Nginx proxies
`/hub/api/*` → `127.0.0.1:7220/hub/api/*`. Systemd unit
`/etc/systemd/system/maplecast-hub.service`. Storage in process memory
(SurrealDB for persistence in future); operator/token bootstrap from
`/etc/maplecast/hub.env`.

### Endpoints

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/hub/api/input-servers/register` | Node registers itself |
| POST | `/hub/api/input-servers/{id}/heartbeat` | Status + metrics every 10s |
| DELETE | `/hub/api/input-servers/{id}` | Graceful shutdown deregister |
| GET | `/hub/api/input-servers` | List all (public; for browsers/dashboard) |
| GET | `/hub/api/input-servers/nearby?limit=N` | GeoIP-filtered nearest N (for client probing) |
| POST | `/hub/api/matchmake` | Player submits ping results |
| POST | `/hub/api/matchmake/select` | Two players ready → matchmaker picks server |
| GET | `/hub/api/dashboard/stats` | Aggregate metrics for the map |
| GET | `/hub/api/dashboard/input-servers` | Full server list with geo for map render |

**Both `/api/input-servers/*` and legacy `/api/nodes/*` work** — same handlers
behind both prefixes for backward compatibility.

### GeoIP

`hub/src/geo.rs` calls `ip-api.com/json/{ip}` (free tier, no key required)
on each register. Stores `{lat, lng, city, country, region, isp}`. Used for
dashboard map pins and `/nearby` pre-filtering. Failures are non-fatal —
node still registers, just without geo data.

### Matchmaker — min-max fairness

`hub/src/matchmaker.rs:select_node()`. For each candidate server with both
players' ping results, compute `max(p1_rtt, p2_rtt)` (the WORSE player's
latency on that server). Pick the server that **minimizes that maximum**.
Optimizes for fairness: the disadvantaged player's experience is as good
as possible, rather than averaging (which can disadvantage one).

3 unit tests in `hub/src/matchmaker.rs::tests` — keep them green when
modifying the algorithm.

### Stale sweeper

Background tokio task every 10s in `hub/src/api.rs::stale_sweeper`:
- No heartbeat for 30s → status `stale` (excluded from matchmaking)
- No heartbeat for 60s → status `offline` (hidden from public list)
- Pending ping reports older than 5min get garbage collected

Important: the in-memory store loses all state on hub restart. Nodes
auto-recover via the **404-reregister** path in
`relay/src/hub_client.rs` — when a heartbeat returns HTTP 404, the relay
re-runs the registration call without restarting the process.

## Input server (node)

An "input server" is **flycast headless + the relay binary**. The
existing `maplecast-headless.service` runs flycast; the existing
`maplecast-relay.service` runs the relay. Adding distributed-network
participation is **just env vars** — set these on the relay's systemd
unit (or `/etc/systemd/system/maplecast-relay.service.d/hub.conf`):

```
MAPLECAST_HUB_URL=http://127.0.0.1:7220/hub/api      # or https://nobd.net/hub/api for community nodes
MAPLECAST_HUB_TOKEN=<operator-token>
MAPLECAST_NODE_NAME=<human-readable name>
MAPLECAST_NODE_REGION=<us-east|eu-west|ap-northeast|...>
MAPLECAST_PUBLIC_HOST=<hostname or IP>               # auto-detected via ifconfig.me if omitted
# Optional: override URLs for nodes behind nginx TLS termination
MAPLECAST_PUBLIC_RELAY_URL=wss://your.host/ws        # nginx → :7201 internally
MAPLECAST_PUBLIC_CONTROL_URL=wss://your.host/play    # nginx → :7210 internally
MAPLECAST_PUBLIC_AUDIO_URL=wss://your.host/audio     # nginx → :7213 internally
```

When `MAPLECAST_HUB_URL` is set, `relay/src/hub_client.rs` spawns:
1. **One-time registration** — POST to `/input-servers/register` with
   node_id (loaded from `~/.maplecast/node_id`, generated if absent),
   ports, capacity, GeoIP-resolved location (hub side), and any
   public_*_url overrides
2. **10-second heartbeat loop** — POST status + metrics
   (frames_received, avg_frame_interval_us, etc.) pulled from
   `RelayState::metrics()` (already exists for `/metrics` endpoint)
3. **Auto-reregister on 404** — if the hub was restarted and forgot
   us, transparently re-run the registration without process restart

### node_id persistence

Stored in `~/.maplecast/node_id` (typically `/opt/maplecast/.maplecast/`
on the VPS). UUIDv4. Stable across relay restarts. The directory must be
writable by the relay process — for the production VPS this means
chowned to `root:root` because the systemd unit uses
`CapabilityBoundingSet=CAP_NET_BIND_SERVICE` which strips
`CAP_DAC_OVERRIDE`, meaning even root behaves like a normal user for
file perms.

### Browser-vs-native: TLS requirement

- **Browsers loading https://nobd.net** are subject to the mixed-content
  rule. They CANNOT open `ws://` connections to a community node — only
  `wss://`. So: community nodes wanting browser players need TLS
  (Cloudflare Tunnel, Tailscale Funnel, or own LE cert).
- **Native clients have no such restriction.** They connect over plain
  WS + raw UDP. No TLS overhead, no certificate burden. **Community
  nodes that only want native-client tournament play don't need TLS at
  all.** Just open ports 7100/udp and 7201/tcp.

This is the cleanest separation: TLS is a browser problem, not a latency
problem. AEAD encryption costs <1µs per packet; the TLS handshake is a
one-time ~100ms cost. The real latency villain is TCP head-of-line
blocking, which we already mitigate via WebTransport (QUIC).

## Native client (`MAPLECAST_MIRROR_CLIENT=1`)

Lives in `core/network/maplecast_mirror.cpp::initClient()` and
`maplecast_input_sink.cpp`. Mode:
- No SH4 simulation (CPU stopped)
- TA frames received via WS, decoded + rendered locally
- Audio via separate WS on `relay_ws_port + 12` (not `+3` as old docs say —
  production now uses 7213 not 7203)
- Gamepad input via raw UDP to server `:7100` (sub-1ms)

### Hub-aware discovery (Phase 1, shipped 2026-04-14)

`core/network/hub_discovery.cpp/.h` — the entry point that makes the
native client useful in the distributed network:

```cpp
namespace maplecast_hub {
    std::vector<InputServer> discover(const std::string& hub_url, int limit = 5);
    void probeServer(InputServer& server, int probe_count = 5, int interval_ms = 200);
    std::vector<InputServer> probeServers(std::vector<InputServer> servers);
    InputServer discoverAndSelect(const std::string& hub_url);
}
```

`initClientWebSocket()` in `maplecast_mirror.cpp` calls
`discoverAndSelect(MAPLECAST_HUB_URL)` if the env var is set AND
`MAPLECAST_SERVER_HOST` is not (explicit override always wins). The
returned winner's `public_host:relay_ws_port` is used for the WS connection.

Fallback chain:
1. Explicit `MAPLECAST_SERVER_HOST/PORT` env vars (highest priority)
2. Hub discovery winner (if `MAPLECAST_HUB_URL` set)
3. Default `127.0.0.1:7200` (lowest priority — local dev)

### UDP ping probe protocol

This is what makes "best server" selection accurate. We measure on the
**actual input UDP path**, not WebSocket RTT (which TCP-stalls and
underestimates real-world latency).

**Wire format** (in `maplecast_input_server.cpp::udpThreadLoop`):

```
Client → Server  (probe):  [0xFF, seq:u8, 0, 0, 0, 0, 0]    (7 bytes)
Server → Client  (ACK):    [0xFE, seq:u8, ts:u48_LE]         (8 bytes)
                            ts = server CLOCK_MONOTONIC microseconds,
                                 truncated to 48 bits, little-endian
```

The probe is detected by `buf[0] == 0xFF` at the very top of the input
recv loop, before any of the existing input format detection. ~2µs to
process. Doesn't touch any input state — pure echo.

Client side (`hub_discovery.cpp::probeServer`):
- 5 probe packets, 200ms apart
- 50ms recv timeout per probe
- **Discard the first sample** (TCP/UDP socket cold start)
- Average remaining 4 → `avg_rtt_ms`
- Max of remaining 4 → `p95_rtt_ms` (rough approximation)
- Total wall time per server: ~1 second

`probeServers()` runs probes in parallel (one std::thread per server,
typically 5 servers max from the GeoIP pre-filter) — total wall time
stays ~1 second regardless of how many servers.

### What's coming (Phase 2-8)

Documented in detail in `docs/COMPETITIVE-CLIENT.md`. Summary:

| Phase | What | Status |
|-------|------|--------|
| 0 | Rename `node` → `input server` in user-facing UI | ✅ Shipped |
| 1 | Hub-aware discovery + UDP probing | ✅ Shipped |
| 2 | Multi-socket redundancy + failover + SCHED_FIFO | ✅ Shipped |
| 3 | Always-on diagnostic HUD overlay | ✅ Shipped |
| 4 | Deterministic replay recording + playback (.mcrec) | ✅ Shipped |
| 5 | Replay sharing + browser playback (server on-demand TA gen) | 📋 Planned |
| 6 | Spectator mode (single + multi-view grid at native res) | 📋 Planned |
| 7 | ED25519 match signing + ROM hash verification | 📋 Planned |
| 8 | DDR/Guitar Hero combo trainer (.mccombo files) | 📋 Planned |

### Phase 2 details — input redundancy + failover

**Wire format extension** (backward compatible — server detects length):
```
Old (7 bytes):  [P][C][slot][LT][RT][btn_hi][btn_lo]
New (11 bytes): [P][C][slot][seq:u32_LE][LT][RT][btn_hi][btn_lo]
```

Every gamepad change sends TWICE: T+0 (immediate from button event)
+ T+1ms (from a deferred-send queue thread). Server dedups by per-source
sequence number (`_lastSeenSeq` map in `maplecast_input_server.cpp`).
Bandwidth: 264 KB/s peak — negligible.

**Failover**: client opens UDP to TWO servers (primary + standby from
hub `discoverAndRank(2)`). Trigger poll thread does `recv MSG_DONTWAIT`
to detect heartbeat ACKs. If primary silent for >100ms (and we've been
sending), atomic flag flips → all subsequent sends go to standby.

**Probe-ACK heartbeat**: server replies `[0xFE, seq, ts:u48_LE]` (8 bytes)
to EVERY input packet — same wire as the hub-discovery probe-ACK. Client
treats this as the "primary alive" signal.

**SCHED_FIFO graceful degrade**: trigger thread tries `pthread_setschedparam(SCHED_FIFO, prio=50)`.
Falls back to SCHED_OTHER if no `CAP_SYS_NICE`. Doesn't crash on lack of
permission — just logs and continues.

### Phase 3 details — diagnostic HUD

`core/ui/gui_competitive_hud.{cpp,h}` — read-only ImGui windows in the
top-left corner during gameplay. Three sections:
- **NETWORK**: server name, RTT, range, send rate, network grade S/A/B/C/F,
  failover indicator
- **LATENCY**: E2E (button-to-pixel), EMA, sent + redundant counts
- **INPUT**: button/trigger change counts (placeholder for the future
  DDR-style scrolling input display)

Hooked into `gui_displayMirrorDebug()` (per-frame in mirror client mode).
F1/F2/F3 toggle individual sections, F12 toggles all.
`ImGuiWindowFlags_NoInputs` ensures the HUD never steals mouse from the
gear icon or game.

### Phase 4 details — deterministic replay

**KEY INSIGHT** (already documented above): SH4 emulation is fully
deterministic — same ROM + same flycast_version + same starting savestate
+ same inputs = byte-perfect identical TA frames. Replay = load
savestate, inject inputs at recorded frames, emulator regenerates.

**File format `.mcrec`** (271-byte header + savestate + input log + footer):
```
Offset  Size    Field
0       8       magic "MCREC\0\0\0"
8       4       version (u32 LE) = 1
12      4       flycast_ver (placeholder, 0 for now)
16      16      match_id (UUID)
32      16      server_id (UUID)
48      8       start_unix_us (u64 LE)
56      8       duration_us (patched in stop())
64      32      rom_hash (SHA-256)
96      64      p1_name (null-padded)
160     64      p2_name
224     3       p1_chars
227     3       p2_chars
230     1       winner (0=p1, 1=p2, 0xFF=unknown)
231     40      reserved
271     ─       SAVESTATE: [raw_size:u32][cmp_size:u32][zstd data]
+       N×16    INPUT LOG: TapeEntry[N] (frame:u64, seqAndSlot:u32, buttons:u16, lt:u8, rt:u8)
end-41  41      FOOTER: "MCEND" + entry_count:u32 + hmac:32B
```

**Writer** (`core/network/replay_writer.{cpp,h}`):
- `start(StartParams)` opens the file, writes header, captures starting
  savestate via `maplecast_mirror::buildFullSaveState()`, zstd compresses
  it (level 3 — ~3.7× reduction, 26.5 MB → 7.2 MB for MVC2)
- `append()` is hooked into `pushTapeEntryAtFrame()` in
  `maplecast_input_server.cpp` — captures EVERY input event the game
  sees. No-op when not recording (single atomic load returns early).
- `stop()` writes footer with HMAC slot (Phase 7 fills it), patches
  `duration_us` and `winner`. Auto-registered atexit handler so SIGTERM/
  Ctrl-C still finalizes the file.

**Reader** (`core/network/replay_reader.{cpp,h}`):
- `openReplay(path)` parses header + slurps savestate + input log into
  RAM. Tolerates missing footer (interrupted recording → uses file-end
  with 16-byte alignment trim + warning).
- `loadStartSavestate()` zstd decompresses + calls `dc_deserialize(buf)`
  via the existing `Deserializer(const void*, size_t)` ctor.
- `startPlayback(speed)` spawns a thread that walks input log entries,
  blocks until `maplecast_mirror::currentFrame()` catches up to each
  entry's recorded frame, then calls `maplecast_input::injectInput()`
  (the public API — same path live gameplay uses for atomic + accumulator
  observability).

**Env-var triggers**:
```
MAPLECAST_REPLAY_OUT=path.mcrec       — start recording on boot
MAPLECAST_REPLAY_P1_NAME=alice        — metadata
MAPLECAST_REPLAY_P2_NAME=bob
MAPLECAST_REPLAY_SERVER_ID=hex16
MAPLECAST_REPLAY_ROM_HASH=hex64
MAPLECAST_REPLAY_IN=path.mcrec        — load + auto-play
MAPLECAST_REPLAY_SPEED=2.0            — playback speed (1.0 = real time)
```

**Storage**: ~7-10 MB per match (savestate dominates, input log is tiny).
Compare to ~150 MB for TA-stream-based replay of a 5-min match.
**~15-20× reduction**, growing with match length (savestate is one-time,
input log scales linearly).

## The lossless spectating insight

**This is the unique-in-fighting-games property.** The TA mirror wire
format streams **GPU draw commands**, not pixels. Every native spectator
re-renders locally at their native resolution. Implications:

| Property | Twitch / YouTube | MapleCast Native Spectator |
|----------|-----------------|----------------------------|
| Resolution | Encoder limit (1080p free, 4K paid) | **Your monitor's native, up to 8K** |
| Compression artifacts | H.264/H.265, blocky in motion | **None — ever** |
| Bitrate per stream | 6 Mbps for 1080p60 | **~4 Mbps** for arbitrary resolution |
| Multi-view (4 matches) | 24 Mbps, 4 decoders | **16 Mbps**, 4 renderers, all 4K |
| Frame stepping | Keyframe-only | **Every frame is a keyframe** |
| Replay storage (per hour) | ~3 GB at 1080p60 | **~1.8 GB at any resolution** |

And then deterministic replay (Phase 4) makes that 1.8 GB number wrong:

## Deterministic replay — the 350× storage win

SH4 emulation is fully deterministic. Matches always start from the same
savestate (or one we record at match-start). For replays we don't need to
record the TA stream at all — **just the inputs + the start savestate**.
On playback, `dc_loadstate()` puts the emulator at the start, then we
inject the recorded inputs frame-by-frame. The emulator regenerates
byte-perfect identical frames.

Storage math:
- TA stream approach: ~30 MB/min ≈ 1.8 GB/hour
- **Inputs-only approach**: 16 bytes/frame × 60 fps ≈ **3.5 MB/hour**
- **350× reduction.** Tournament's worth of replays in tens of MB.

`.mcrec` file format (Phase 4):
```
[HEADER]      magic "MCREC", version, match_id, ROM hash,
              flycast_version (determinism boundary),
              players, characters, winner
[START SAVESTATE]   zstd(dc_serialize) — typically ~600 KB
[INPUT LOG]   [frame_num: u32, p1_input: u64, p2_input: u64] per frame
[SIGNATURE]   HMAC-SHA256 + (Phase 7) ED25519 sig from input server
```

Total file size for a 5-min match: **~1 MB** (vs. ~150 MB for TA-stream
approach).

### What we lose (acceptable)
- Browser playback can't run the SH4 emulator natively. Two options:
  - Include the WASM SH4 emulator (already exists as `/legacy` path)
  - **OR: server-side on-demand TA stream generation** (Phase 5) —
    a flycast process loads the .mcrec, plays it deterministically,
    pipes the resulting TA stream to the browser via WS. The
    existing WASM TA renderer (king.html) just plays it. Server cost:
    ~80µs/frame compress on a $5 VPS. Result cached for 5 min. Cheap.

### What we gain
- **350× smaller storage** + **future-proof rendering** (renderer
  improvements automatically apply to all old replays — they're
  re-rendered fresh) + **built-in determinism regression check**
  (a replay that doesn't reproduce identical state proves a determinism
  bug).

## Combo trainer (Phase 8) — the killer community feature

Deterministic replay enables this naturally. Every "sick combo" is a
small `.mccombo` file (input log + start savestate, a subset of `.mcrec`).
Players upload combos to a community library. Other players load them in
training mode:

1. Game executes the combo deterministically (same character, same chars,
   same screen state)
2. **DDR/Guitar Hero "note highway"** at the bottom of the screen:
   scrolling buttons fall toward a "hit zone" timed to each input
3. Player has to hit each button as it crosses the line
4. Score: PERFECT / GREAT / GOOD / MISS per input
5. Loop forever, slow-mo (50-110%), shadow-overlay mode

Why this is huge: **infinite community-generated learning content** that
has never existed for fighting games. Pro replays from EVO become
training material within hours. Community library grows forever. Network
effect.

## Dashboard — `web/network.html`

Public page at `https://nobd.net/network.html`. Leaflet.js world map with
node pins (color-coded by status: ready/in_match/stale/offline). Stats
bar shows aggregate metrics. Sortable table of all input servers. Auto-
probes the nearest 5 from your geographic location (browser-side WS ping)
and shows your latency to each.

Mixed-content gotcha: from `https://nobd.net/` the dashboard can only
ping nodes with TLS (`wss://`). Plain-`ws://` community nodes show as
"unreachable" — that's a browser security model thing, not a bug. To
test plain-ws nodes, serve the dashboard from `http://localhost` with
`?hub=https://nobd.net/hub/api`.

## Docker packaging — `Dockerfile.node`

Combined flycast + relay image, multi-stage build (debian:12-slim runtime,
~125MB). Published to `ghcr.io/t3chnicallyinclined/maplecast-node:latest`.

Critical: **`--shm-size=256m` is REQUIRED.** flycast allocates ~168 MB in
`/dev/shm` for the TA mirror RingHeader + BRAIN + RING buffers. The
default Docker `/dev/shm` is 64 MB → SIGBUS on the SH4 thread the moment
it touches the mapping.

Run command for community operators:
```bash
docker run -d --net=host --shm-size=256m \
  -v /path/to/mvc2.gdi:/data/mvc2.gdi:ro \
  -e MAPLECAST_HUB_URL=https://nobd.net/hub/api \
  -e MAPLECAST_HUB_TOKEN=<their-operator-token> \
  -e MAPLECAST_NODE_NAME="MyServer" \
  -e MAPLECAST_NODE_REGION="us-east" \
  ghcr.io/t3chnicallyinclined/maplecast-node:latest
```

`--net=host` is the recommended default. Without it, bridge mode adds
~1-5µs per UDP packet via iptables NAT — fine for spectator nodes,
suboptimal for tournament-grade. With `--net=host`, Docker is essentially
zero-overhead packaging.

## Files added/changed for this pillar

### Hub (new crate)
- `hub/Cargo.toml` — axum, tokio, serde, reqwest, chrono, uuid, tower-http
- `hub/src/main.rs` — server entry, route table, bootstrap operator
- `hub/src/api.rs` — handlers (register, heartbeat, list, nearby,
  matchmake, dashboard endpoints, stale_sweeper)
- `hub/src/types.rs` — `Node`, `Operator`, `PingResult`, `SharedStore`
- `hub/src/matchmaker.rs` — min-max fairness algorithm + 3 unit tests
- `hub/src/geo.rs` — ip-api.com lookup + Haversine distance
- `hub/src/schema.surql` — SurrealDB persistence schema (future)

### Relay
- `relay/src/hub_client.rs` — registration + heartbeat loop +
  404-reregister fallback
- `relay/src/main.rs` — new CLI args (`--hub-register`, `--hub-url`,
  `--hub-token`, `--node-name`, `--node-region`, `--public-host`,
  `--public-relay-url`, `--public-control-url`, `--public-audio-url`)
  + env var aliases (`MAPLECAST_INPUT_SERVER_NAME` → `_NODE_NAME`)

### Native client
- `core/network/hub_discovery.cpp/.h` — fetch, probe, select
- `core/network/maplecast_mirror.cpp::initClientWebSocket` — branches on
  `MAPLECAST_HUB_URL`
- `core/network/maplecast_input_server.cpp::udpThreadLoop` — probe-ACK
  responder ([0xFF, seq, 0×5] → [0xFE, seq, ts:u48_LE])

### Web dashboard
- `web/network.html` — Leaflet map, stats, sortable table, auto-probe
- `web/js/node-router.mjs` — browser-side discovery + WS ping probing
  for matchmaking
- `web/js/ws-connection.mjs` — `getControlWsUrl/getRendererWsUrl/`
  `getRendererAudioWsUrl` check `nodeState.assignedNode` first; if a
  match was assigned to a community node, route there instead of origin
- `web/js/queue.mjs` — `gotNext()` probes nodes in background;
  `handleMyPromotion()` reads `node_urls` from promotion row;
  `leaveGame()` clears the assignment

### Deployment
- `deploy/systemd/maplecast-hub.service` — systemd unit for the hub
- `deploy/scripts/test-network.sh` — local + VPS deploy harness
- `Dockerfile.node` — combined flycast+relay image (3-stage build)
- `deploy/node-entrypoint.sh` — starts flycast, waits for :7200, starts
  relay with hub args from env

### Docs
- `docs/COMPETITIVE-CLIENT.md` — full vision + feature catalog for the
  native tournament-grade client (Phases 0-8). Read before adding
  features to the native client.

## Current production state (2026-04-14)

```
Hub:           https://nobd.net/hub/api                  (axum, on the same VPS)
Dashboard:     https://nobd.net/network.html             (Leaflet map)
Input Server:  nobd-main (Piscataway, NJ)                ← VPS itself
Operator:      admin (token in /etc/maplecast/hub.env)
Backups:       /opt/maplecast-relay.backup-YYYYMMDD-HHMMSS (per deploy)
Image:         ghcr.io/t3chnicallyinclined/maplecast-node:latest
```

To add a community node from your laptop or another VPS:
```bash
docker run -d --net=host --shm-size=256m \
  -v /path/to/mvc2.gdi:/data/mvc2.gdi:ro \
  -e MAPLECAST_HUB_URL=https://nobd.net/hub/api \
  -e MAPLECAST_HUB_TOKEN=<admin-issued> \
  -e MAPLECAST_NODE_NAME="MyNode" \
  ghcr.io/t3chnicallyinclined/maplecast-node:latest
```

To verify hub-aware discovery from the native client:
```bash
MAPLECAST_MIRROR_CLIENT=1 \
MAPLECAST_HUB_URL=https://nobd.net/hub/api \
./build/flycast
```
Expected output:
```
[MIRROR] Hub discovery enabled — querying https://nobd.net/hub/api
[hub-discovery] Discovered N input server(s) from https://nobd.net/hub/api
[hub-discovery] probe nobd-main: X.Xms avg (4 samples)
[hub-discovery] ═══ Selected: nobd-main (nobd.net) — X.Xms RTT ═══
[MIRROR] Hub picked input server 'nobd-main' at nobd.net:7201 (X.Xms RTT)
```

## Hard-learned lessons (don't repeat these)

1. **Docker `/dev/shm` default is 64MB**, flycast wants ~168MB → SIGBUS.
   Always `--shm-size=256m`.
2. **`Text file busy` on cp /opt/maplecast-relay**. Stop the service
   first, then swap the binary, then start. Or use `install` which
   handles this.
3. **`CapabilityBoundingSet=CAP_NET_BIND_SERVICE`** on the relay systemd
   unit strips `CAP_DAC_OVERRIDE`, so root can't write to non-owned
   dirs. The `~/.maplecast/` dir for `node_id` persistence must be
   owned by the running user.
4. **WebTransport `dgram=0 stream=N` in WT client final stats is normal**
   — the relay falls back to uni-streams when datagrams aren't
   compatible. Not a bug. The disconnect-loop pattern ("N=80 in 2s,
   reconnect, N=80 in 2s, ...") IS a bug — usually a stale browser tab
   or a relay binary regression.
5. **Auto-deploys can race a manual deploy.** If you SCP a relay binary
   and 5 minutes later something else also deploys, your binary gets
   overwritten without your noticing. Always check
   `systemctl show maplecast-relay -p MainPID -p ExecMainStartTimestamp`
   if you suspect a stream regression.
6. **Hub stores public URLs as opaque strings.** If a node is behind
   nginx TLS termination, the operator MUST set `MAPLECAST_PUBLIC_RELAY_URL`
   etc. — otherwise the hub stores `ws://host:7201/ws` which gets
   mixed-content-blocked from `https://nobd.net/network.html`.
7. **Browsers cannot probe `ws://` from `https://`.** Don't try to
   work around this in JS. Either deploy TLS for the node or test
   from `http://localhost`.
8. **The probe-ACK responder MUST be early in `udpThreadLoop`** —
   before the input format detection. Otherwise `[0xFF, ...]` packets
   would fall through to the legitimate input handlers and either be
   ignored (fine) or treated as garbage input (less fine).
9. **Hub in-memory store is volatile.** Hub restart → all nodes vanish.
   The 404-reregister fallback in `relay/src/hub_client.rs` handles
   this transparently — don't remove it. Eventually we'll move state
   to SurrealDB but for now this is the recovery path.
10. **The hub is NOT in the gameplay hot path.** Don't add latency-
    sensitive operations to it. Don't proxy game data through it. It's
    discovery + matchmaking only. If you find yourself adding a
    websocket relay or input forwarder to the hub, stop and reconsider.
