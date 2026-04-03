# MapleCast Architecture — Mental Model

## What Is MapleCast?

MapleCast turns a Flycast Dreamcast emulator into a game streaming server. One instance of MVC2 runs on the server. Players connect with fight sticks (NOBD) or browser gamepads. The server streams H.264 video to all connected clients in real-time. Sub-5ms end-to-end latency.

## The Three Pillars

```
┌─────────────────────────────────────────────────────────┐
│                    FLYCAST (one binary)                  │
│                                                         │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────┐  │
│  │   EMULATOR   │  │ INPUT SERVER │  │  STREAM SERVER│  │
│  │             │  │              │  │               │  │
│  │  Dreamcast  │  │ UDP thread   │  │ CUDA→NVENC    │  │
│  │  SH4 CPU    │←─│ kcode[] ←────│  │ H.264 encode  │  │
│  │  PVR GPU    │  │              │  │               │  │
│  │  Maple Bus  │  │ Player       │  │ WebSocket     │  │
│  │  AICA Sound │  │ registry     │  │ (signaling)   │  │
│  │             │  │              │  │               │  │
│  │  CMD9 reads │  │ Latency      │  │ WebRTC DC     │  │
│  │  kcode[] ───│──│→ tracking    │  │ (P2P video)   │  │
│  └─────────────┘  └──────────────┘  └───────────────┘  │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### Pillar 1: Emulator (Flycast)
The Dreamcast emulator. Runs MVC2 at 60fps. The game thinks it's talking to real controllers via the Maple Bus. It sends CMD9 (GetCondition) every frame to ask "what buttons are pressed?" The answer comes from `kcode[]` globals.

### Pillar 2: Input Server (`maplecast_input_server.cpp`)
Single source of truth for all player input. Receives from multiple sources, writes to one place. Tracks who's connected, their latency, their device type.

### Pillar 3: Stream Server (`maplecast_stream.cpp` + `maplecast_webrtc.cpp`)
Captures each rendered frame, encodes to H.264 via NVIDIA GPU, and delivers to all connected clients. Uses WebRTC DataChannels for P2P delivery with NAT hole-punching.

---

## Input Flow — How Button Presses Reach The Game

```
NOBD Stick (hardware fight stick)
  │ W6100 Ethernet, 12,000 packets/sec
  │ 4 bytes: [LT][RT][buttons_hi][buttons_lo]
  ▼
UDP:7100 ──→ Input Server UDP Thread
               │ recvfrom() + SO_BUSY_POLL
               │ Auto-assign by source IP (first=P1, second=P2)
               ▼
            updateSlot(slot, lt, rt, buttons)
               │
               ▼
            kcode[0] = buttons    ← P1 atomic write
            lt[0]    = trigger
            rt[0]    = trigger


Browser Gamepad (remote player)
  │ Gamepad API, 250Hz polling
  │ 4 bytes: [LT][RT][buttons_hi][buttons_lo]
  ▼
WebRTC DataChannel "input" ──→ maplecast_webrtc.cpp
  │  (or WebSocket fallback)      │ onMessage callback
  │                               │ Direct call, no UDP hop
  ▼                               ▼
                               injectInput(slot, lt, rt, buttons)
                                  │
                                  ▼
                               kcode[1] = buttons    ← P2 atomic write
                               lt[1]    = trigger
                               rt[1]    = trigger


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

**Key insight:** The game reads buttons once per frame at vblank via CMD9. The input server keeps `kcode[]` always up-to-date in the background. There's never a socket read in the hot path. Like NOBD firmware's pre-computed `cmd9ReadyW3` — the answer is always waiting.

---

## Video Flow — How Frames Reach The Browser

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
       (for peers without DC)


Browser
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
2. WebSocket connects to ws://server:7200
   │ Receives JSON: {"type":"status", "p1":{...}, "p2":{...}}
   │ Shows lobby: who's connected, which slots are open
   │
   ▼
3. Player clicks "Join" → sends JSON:
   │ {"type":"join", "id":"uuid", "name":"tris", "device":"PS4 Controller"}
   │
   ▼
4. Input Server: registerPlayer()
   │ Checks which slots NOBD sticks already took
   │ Assigns next free slot
   │ Returns slot number (0=P1, 1=P2, -1=spectator)
   │
   ▼
5. Server responds: {"type":"assigned", "slot":1}
   │
   ▼
6. Browser starts WebRTC negotiation:
   │
   ├─ Creates RTCPeerConnection (STUN: stun.l.google.com:19302)
   ├─ Creates DataChannel "video" {ordered:false, maxRetransmits:0}
   ├─ Creates DataChannel "input" {ordered:false, maxRetransmits:0}
   ├─ Creates SDP offer → sends via WebSocket
   │
   ▼
7. Server: handleOffer()
   │ Creates rtc::PeerConnection
   │ Sets remote description (browser's offer)
   │ Auto-generates SDP answer → sends via WebSocket
   │
   ▼
8. ICE candidate exchange (trickle):
   │ Browser ←→ Server exchange candidates via WebSocket
   │ STUN discovers public IP:port
   │ ICE hole-punches through NAT
   │
   ▼
9. DataChannels open → [P2P] appears in status
   │
   │ Video flows: Server → DataChannel → Browser (P2P)
   │ Input flows: Browser → DataChannel → Server (P2P)
   │ WebSocket: signaling only (lobby updates, status)
   │
   │ If DataChannel fails (5s timeout):
   │ Video flows via WebSocket (TCP fallback)
   │ Input flows via WebSocket → UDP:7100 (legacy path)
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
│  → Both visible in lobby                          │
│  → Both update kcode[] atomics                    │
│  → CMD9 reads same globals regardless of source   │
└──────────────────────────────────────────────────┘
```

---

## File Map

```
core/network/
├── maplecast_input_server.cpp   ← THE input authority
│   ├── UDP thread (NOBD sticks, SO_BUSY_POLL)
│   ├── Player registry (slots, stats, latency)
│   ├── updateSlot() → kcode[]/lt[]/rt[] writes
│   └── injectInput() API for WebRTC/WebSocket
│
├── maplecast_input_server.h     ← Public API: init, registerPlayer, injectInput, getPlayer
│
├── maplecast_stream.cpp         ← Video encode + WebSocket server
│   ├── CUDA GL interop (texture capture)
│   ├── NVENC H.264 encode (0.67ms)
│   ├── WebSocket server (port 7200, signaling)
│   ├── broadcastBinary() → DC first, WS fallback
│   ├── onWsMessage() → join, input forward, SDP/ICE signaling
│   └── onFrameRendered() → called after Present()
│
├── maplecast_webrtc.cpp         ← WebRTC DataChannel transport
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
├── Renderer_if.cpp              ← Hook: calls onFrameRendered() after Present()
└── spg.cpp                      ← Scanline scheduler, triggers vblank → maple_DoDma()

web/
├── index.html                   ← Browser client
│   ├── WebSocket connect (signaling)
│   ├── setupWebRTC() → DataChannel P2P
│   ├── handleVideoFrame() → VideoDecoder → canvas
│   ├── Gamepad polling → DC or WS send
│   └── Lobby display
│
├── proxy.py                     ← DEAD (was Python WebSocket proxy, killed in Phase 3)
└── telemetry.py                 ← Telemetry display server

start_maplecast.sh               ← Starts flycast + telemetry + web server
                                    Auto-kills stale processes
                                    Graceful shutdown on Ctrl+C
```

---

## Latency Budget

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
  → CUDA copy                    0.03ms
  → NVENC encode                 0.67ms
  → DataChannel send             ~0.01ms
  → Network (LAN)                ~0.1ms
  → Browser decode               ~2.5ms
  ─── total E2E ───              ~3.6ms + frame alignment

Browser Gamepad (WebRTC P2P):
  Button press                    0µs
  → Gamepad API poll              ~4ms (250Hz)
  → DataChannel send              ~0.01ms
  → Input server injectInput()   ~0.01ms (direct call, no UDP)
  → kcode[] atomic store          ~10ns
  ─── input latency ───           ~4ms
  → (same render/encode path)
  ─── total E2E ───               ~4.3ms + frame alignment
```

---

## Environment Variables

```bash
MAPLECAST=1              # Enable MapleCast server mode
MAPLECAST_STREAM=1       # Enable H.264 streaming
MAPLECAST_PORT=7100      # Input UDP port (default 7100)
MAPLECAST_STREAM_PORT=7200  # WebSocket/WebRTC port (default 7200)
MAPLECAST_WEB_PORT=8000  # Web server port (default 8000)
```

---

## Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 7100 | UDP | NOBD stick input + WebSocket-forwarded browser input |
| 7200 | TCP+UDP | WebSocket (signaling) + WebRTC DataChannel (video/input) |
| 7300 | UDP | Telemetry (server → telemetry.py) |
| 8000 | HTTP | Web client (index.html) |

---

## Build Flags

| Flag | What | Set By |
|------|------|--------|
| `MAPLECAST_NVENC=1` | CUDA + NVENC encode | CMake (auto-detected) |
| `MAPLECAST_CUDA=1` | CUDA support | CMake (auto-detected) |
| `MAPLECAST_WEBRTC=1` | WebRTC DataChannel | CMake (libdatachannel found) |
| `MAPLECAST_XDP=1` | AF_XDP zero-copy input | CMake (libbpf/libxdp found) |

---

## Current Performance (April 2026)

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
| Resolution | 640×480 |
| Codec | H.264 Baseline, all-IDR, CABAC |
