# MapleCast Architecture — Mental Model

## What Is MapleCast?

MapleCast turns a Flycast Dreamcast emulator into a game streaming server. One instance of MVC2 runs on the server. Players connect with fight sticks (NOBD) or browser gamepads. The server streams the game to all connected clients in real-time. Two streaming modes exist: TA Mirror (primary, streams raw GPU commands) and H.264 (legacy, streams encoded video). Sub-5ms end-to-end latency on LAN.

## The Three Pillars

```
┌──────────────────────────────────────────────────────────────────┐
│                      FLYCAST (one binary)                        │
│                                                                  │
│  ┌──────────────┐  ┌───────────────┐  ┌───────────────────────┐  │
│  │   EMULATOR    │  │ INPUT SERVER  │  │   STREAM SERVER       │  │
│  │              │  │               │  │                       │  │
│  │  Dreamcast   │  │ UDP thread    │  │ TA Mirror (primary):  │  │
│  │  SH4 CPU     │←─│ kcode[] ←─────│  │  VRAM diffs + TA cmds│  │
│  │  PVR GPU     │  │               │  │  ~15-40KB/frame       │  │
│  │  Maple Bus   │  │ Player        │  │                       │  │
│  │  AICA Sound  │  │ registry      │  │ H.264 (legacy):       │  │
│  │              │  │               │  │  CUDA→NVENC encode    │  │
│  │  CMD9 reads  │  │ Stick         │  │  ~52KB/frame          │  │
│  │  kcode[] ────│──│→ registration │  │                       │  │
│  │              │  │               │  │ WebSocket server      │  │
│  │  Game state  │  │ Queue         │  │  (mirror + lobby)     │  │
│  │  253B reads ─│──│→ leaderboard  │  │  port 7200            │  │
│  └──────────────┘  └───────────────┘  └───────────────────────┘  │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### Pillar 1: Emulator (Flycast)
The Dreamcast emulator. Runs MVC2 at 60fps. The game thinks it's talking to real controllers via the Maple Bus. It sends CMD9 (GetCondition) every frame to ask "what buttons are pressed?" The answer comes from `kcode[]` globals. The server also reads 253 bytes of MVC2 RAM each frame for live game state (health, combos, meter, characters).

### Pillar 2: Input Server (`maplecast_input_server.cpp`)
Single source of truth for all player input. Receives from multiple sources, writes to one place. Tracks who's connected, their latency, their device type. Manages NOBD stick registration (rhythm-based binding to browser users), player queue ("I Got Next"), and slot assignment.

### Pillar 3: Stream Server (`maplecast_mirror.cpp` + `maplecast_ws_server.cpp`)
Two streaming modes, one unified WebSocket server on port 7200 that handles both binary mirror/video broadcast AND JSON lobby protocol (join/leave/queue/ping/register_stick).

**TA Mirror mode (primary):** Captures raw TA command buffers + PVR registers + VRAM page diffs each frame. Clients run flycast's own renderer (WASM/WebGL2) to reconstruct the frame pixel-perfect. Resolution-independent. ~15-40KB/frame, ~4MB/s at 60fps.

**H.264 mode (legacy):** Captures each rendered frame via CUDA GL interop, encodes to H.264 via NVENC, delivers as NAL units. ~52KB/frame, ~25Mbps. Requires NVIDIA GPU. Uses WebRTC DataChannels for P2P delivery.

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
Flycast Emulator (server)
  │ PVR GPU renders frame via TA command list
  ▼
maplecast_mirror::publishFrame()             [maplecast_mirror.cpp]
  │
  ├─ Capture TA command buffer               Raw GPU command list
  │    (varies per frame, ~2-30KB)
  │
  ├─ Capture PVR registers                   Palette, fog, ISP config
  │    (~32KB region, only dirty pages sent)
  │
  ├─ Diff VRAM pages (4KB granularity)       Texture/palette changes
  │    Shadow copy comparison
  │    Only changed pages included
  │
  ├─ Assemble delta frame:
  │    ┌────────┬────────┬──────────┬────────────┐
  │    │frameLen│frameNum│TA cmd buf│dirty pages  │
  │    │ 4B     │ 4B     │ var      │ var         │
  │    └────────┴────────┴──────────┴────────────┘
  │    Total: ~15-40KB/frame at 60fps
  │
  ├─ Write to shared memory ring buffer      For local mirror client
  │
  ▼
maplecast_ws::broadcastBinary()              [maplecast_ws_server.cpp]
  │ Port 7200 WebSocket
  │ Binary frames to all connected clients
  │
  ▼
Browser (EmulatorJS iframe)
  │ WebSocket receives binary delta frame
  ▼
emulator.html onmessage handler
  │
  ├─ First message: SYNC packet              Full VRAM + PVR regs (~8MB)
  │    → Module._mirror_apply_sync()         Writes directly into WASM memory
  │    → Resets texture cache
  │
  ├─ Subsequent: delta frames
  │    → Module._mirror_render_frame()       [maplecast_wasm_bridge.cpp]
  │       │
  │       ├─ Apply VRAM dirty pages          Update textures in place
  │       ├─ Apply PVR register changes      Palette, fog tables
  │       ├─ Feed TA commands to ta_parse()  Builds rend_context
  │       ├─ pal_needs_update = true         Force palette reload
  │       ▼
  │       renderer->RenderFrameFromTA()      flycast's real GL renderer
  │       │
  │       ▼
  │       WebGL2 canvas output               Pixel-perfect Dreamcast graphics
  │
  │ WebGL2 patches required:
  │   GL_VERSION → "OpenGL ES 3.0 WebGL 2.0"
  │   INVALID_ENUM errors suppressed
  │   texParameteri guarded (no-op if no texture bound)
  │
  ▼
60fps in browser, resolution-independent
```

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
│   ├── Shared memory ring buffer (server→local client)
│   ├── VRAM + PVR register page diffs (4KB granularity)
│   ├── TA command buffer capture
│   ├── publishFrame() → SHM + maplecast_ws::broadcastBinary()
│   ├── Telemetry via updateTelemetry()
│   └── Shadow copies for diff computation
│
├── maplecast_mirror.h           ← Public API: initServer, initClient, publishFrame
│
├── maplecast_ws_server.cpp      ← Unified WebSocket server (port 7200)
│   ├── Binary broadcast: mirror delta frames to all clients
│   ├── Initial SYNC: full VRAM + PVR regs on connect (~8MB)
│   ├── JSON lobby: join, leave, queue_join, register_stick
│   ├── Status broadcast: every 1s with players/queue/game/telemetry
│   ├── Browser input: binary 4-byte → UDP forward to 7100
│   ├── Game state inclusion (health, combos, meter, characters)
│   └── Spectator/viewer counting
│
├── maplecast_ws_server.h        ← Public API: init, broadcastBinary, updateTelemetry, active
│
├── maplecast_gamestate.cpp      ← Reads MVC2 RAM (253-byte format)
│   └── readGameState() → health, combo, meter, characters, timer, stage
│
├── maplecast_gamestate.h        ← GameState struct, readGameState()
│
├── maplecast_wasm_bridge.cpp    ← WASM exports for browser mirror client
│   ├── mirror_init() → initialize renderer for mirror mode
│   ├── mirror_apply_sync(ptr, size) → load full VRAM + PVR regs
│   ├── mirror_render_frame(ptr, size) → apply diffs, run ta_parse(), render
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

web/
├── index.html                   ← Main browser client
│   ├── Embeds emulator.html in iframe
│   ├── WebSocket connect to 7200 (lobby + input)
│   ├── Lobby UI: slots, queue, spectator count
│   ├── "I Got Next" queue system
│   ├── "Register My Stick" / "Unregister Stick"
│   ├── Gamepad polling → 4-byte binary via WebSocket
│   ├── Diagnostics overlay (server FPS, bandwidth, ping)
│   ├── Game state display (health bars, combos, meter)
│   └── Leaderboard (localStorage wins/losses)
│
├── emulator.html                ← EmulatorJS + flycast WASM mirror client (iframe)
│   ├── WebGL2 compatibility patches:
│   │   GL_VERSION override, INVALID_ENUM suppression,
│   │   texParameteri guard (no-op if no texture bound)
│   ├── EmulatorJS loads flycast core + MVC2 CHD
│   ├── On game start: pauses CPU, calls _mirror_init()
│   ├── WebSocket to 7200 for binary mirror data
│   ├── Receives SYNC → mirror_apply_sync()
│   └── Receives deltas → mirror_render_frame() at 60fps
│
├── serve.py                     ← HTTP server for web/ directory (port 8000)
│                                   Sets COEP/COOP headers for SharedArrayBuffer
├── telemetry.py                 ← Telemetry display server
├── ejs-data/                    ← EmulatorJS runtime (loader.js, cores, etc.)
├── bios/                        ← dc_boot.bin, dc_flash.bin
└── roms/                        ← mvc2.chd

start_maplecast.sh               ← Starts flycast + telemetry + web server
                                    Auto-kills stale processes
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

| Port | Protocol | Purpose |
|------|----------|---------|
| 7100 | UDP | NOBD stick input + WebSocket-forwarded browser input |
| 7200 | TCP (WebSocket) | Mirror binary broadcast + JSON lobby (join/leave/queue/register_stick/status) + WebRTC signaling (H.264 mode) |
| 7300 | UDP | Telemetry (server → telemetry.py) |
| 8000 | HTTP | Web client (index.html → iframe emulator.html). Requires COEP/COOP headers for SharedArrayBuffer |

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
