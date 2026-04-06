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
  │ Gamepad API, 250Hz polling
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
  → Gamepad API poll              ~4ms (250Hz)
  → WebSocket send                ~0.01ms
  → UDP forward to 7100           ~0.01ms
  → Input server recvfrom         ~0.01ms
  → kcode[] atomic store          ~10ns
  ─── input latency ───           ~4ms
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
