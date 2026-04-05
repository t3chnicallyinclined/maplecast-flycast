# MapleCast Streaming Architecture Options

**Date:** April 5, 2026
**Goal:** Extreme low latency, pixel perfect, competitive play
**Constraint:** LOW BANDWIDTH, LOW LATENCY

---

## What We Have (Arsenal)

| Component | Built? | Location |
|-----------|--------|----------|
| 253-byte game state read/write | YES | maplecast_gamestate.cpp |
| TA command capture | YES | maplecast_mirror.cpp |
| Memory page diffs (42 pages/frame) | YES | maplecast_mirror.cpp |
| Palette update fix | YES | mirror clientReceive |
| NOBD XDP input (sub-1ms) | YES | maplecast_xdp_input.cpp |
| H.264 NVENC encode (3.5ms) | YES | maplecast_stream.cpp |
| WebRTC DataChannels (P2P) | YES | maplecast_webrtc.cpp |
| Save state sync | YES | maplecast_mirror.cpp |
| Visual cache (5000+ textures) | YES | maplecast_visual_cache.cpp |
| RAM autopsy (42 dirty pages mapped) | YES | docs/MVC2-MEMORY-MAP.md |
| WebGL replay player | YES | web/replay.html |
| Input injection (maple bus) | YES | maplecast_input_server.cpp |
| **TA Mirror WASM client** | **YES** | **maplecast_mirror_client.cpp + EmulatorJS** |

---

## Option 4: TA Command Streaming — Mirror Mode [IMPLEMENTED, PRIMARY]

This is the primary streaming mode. Server captures TA display list commands and memory diffs, streams them over WebSocket. The WASM client receives them and renders via WebGL2 — no SH4 CPU emulation, no ROM needed on the client.

```
Server: SH4 compute → captures TA commands + memory diffs → WebSocket publish
Client: WASM receives TA commands → ta_parse → WebGL2 render (no CPU)

Bandwidth:  ~4 MB/s (measured)
Latency:    0.46ms server publish, ~7ms full E2E (browser)
FPS:        59-60 (measured, stable)
Client:     Flycast WASM via EmulatorJS (no ROM, no BIOS needed)
Resolution: ANY — client renders locally via WebGL2, resolution independent
Status:     IMPLEMENTED — MVC2 running 60fps in browser
```

**Measured performance:**
- Server publish time: 0.46ms/frame (telemetry)
- Client WebSocket ping: 0.2ms (LAN)
- Stream bandwidth: ~4 Mbps
- Frame rate: 59-60fps sustained

**Pros:** Pixel perfect, no ROM needed on client, no CPU emulation, any resolution, works in any browser with WebGL2
**Cons:** Higher bandwidth than pure game state (4MB/s vs 16KB/s), requires WebSocket connection

---

## Option 1: H.264 Video Streaming [IMPLEMENTED, SECONDARY]

Still works, still available. Preferred for native desktop clients or when TA mirror is not suitable.

```
Server: SH4 compute → GPU render → CUDA copy → NVENC encode → WebSocket/WebRTC
Client: Browser receives → hardware decode → display

Bandwidth:  1.9 MB/s
Latency:    4.2ms E2E (NOBD stick), 4.9ms (browser gamepad)
Client:     Any browser with WebCodecs
Resolution: 640x480 (server-locked)
Status:     IMPLEMENTED — proven, stable
```

**Pros:** Dead simple, works everywhere, rock solid, lowest E2E proven for NOBD
**Cons:** Resolution locked to server, decode latency, bandwidth scales with quality

**When to use H.264 over TA mirror:** When the client cannot run WASM (old devices), when bandwidth is not a concern and simplicity matters, or for recording/archival.

---

## Option 2: Input Sync + 253-Byte State Correction [NOT IMPLEMENTED]

```
Server: receives inputs → runs SH4 → reads 253B state → broadcasts
Client: runs flycast + ROM → receives inputs → processes locally
        → receives 253B state → compares → corrects drift

Bandwidth:  ~16 KB/s (14B inputs + 253B state x 60fps)
Latency:    0ms (local render), corrections arrive 1 frame later
Client:     flycast + ROM
Resolution: ANY (local render)
Status:     NOT IMPLEMENTED — components proven individually, not integrated
```

**Pros:** Lowest bandwidth possible, instant local feedback, any resolution
**Cons:** Client needs flycast + ROM, possible micro-snaps on correction, complex drift correction logic

---

## Option 3: Server-Authoritative Hybrid [NOT IMPLEMENTED]

```
Server: runs THE game → sends inputs (14B) + state checksum (32B)
Client: runs flycast locally → checks state hash vs server
        if match: continue (0ms latency)
        if minor drift: server sends 253B correction (253B)
        if major desync: server sends 3 dirty pages (12 KB)
        if total desync: server sends save state (26 MB)

Bandwidth:  ~3 KB/s normal, ~16 KB/s on correction, ~720 KB/s on page sync
Latency:    0ms local
Client:     flycast + ROM
Resolution: ANY
Status:     NOT IMPLEMENTED — theoretical, complex sync logic
```

---

## Option 5: Predictive Client [NOT IMPLEMENTED]

```
Server: compute → sends inputs + 253B + TA commands
Client: receives inputs FIRST (smallest packet, arrives first)
        → runs local flycast prediction (instant render)
        → TA commands arrive next (~1-2ms later)
        → compares local TA vs server TA
        → if match (99.7%): keep local frame (0ms latency!)
        → if mismatch (0.3%): re-render with server TA commands

Bandwidth:  ~140 KB/frame = 8 MB/s
Latency:    0ms for 99.7% of frames, 1 frame for 0.3%
Client:     flycast + ROM
Resolution: ANY
Status:     NOT IMPLEMENTED — requires dual render path
```

---

## Option 6: 253-Byte Lookup Renderer (No Emulator) [FUTURE]

```
OFFLINE (one-time build):
  Play every character x every animation x every frame
  Record: (char_id, anim_state, anim_timer, facing, palette) → TA commands
  Store as lookup table (~50-200 MB)
  The visual_cache already has 5000+ entries!

RUNTIME:
  Server sends 253 bytes per frame
  Client: for each of 6 characters:
    look up (char_id, anim_state, anim_timer, facing) → pre-cached TA template
    position at (screen_x, screen_y) from the 253 bytes
    render via WebGPU/OpenGL

Bandwidth:  253 bytes/frame = 15 KB/s
Latency:    0ms (lookup is instant)
Client:     Custom renderer + lookup table (cached, no ROM)
Resolution: ANY
Status:     FUTURE — visual_cache has entries but not complete coverage
```

---

## Option 7: Split Pipeline (All Outputs Simultaneously)

```
One server compute cycle produces ALL of:
  → TA commands       for browser      (0.46ms publish, 4 MB/s)  ← PRIMARY
  → H.264 frame      for player       (4.2ms latency, 1.9 MB/s)
  → 253-byte state   for overlay      (0ms, 15 KB/s)
  → Memory page diffs for replay       (0ms, 168 KB/frame)

Each consumer subscribes to the streams they need:
  Browser:    TA mirror (any resolution, pixel perfect)
  Player:     H.264 + 253B overlay (lowest latency for NOBD)
  Spectator:  TA mirror (any resolution)
  Replay:     253B + page diffs (rewindable)
  Analytics:  253B (frame data, combos, damage)
```

---

## CURRENT PRODUCTION SETUP

### For Browser Clients: TA Mirror (Option 4)
```
Server publishes TA commands at 60fps (0.46ms/frame)
Browser runs Flycast WASM via EmulatorJS
WASM client receives TA commands over WebSocket
Renders via WebGL2 at any resolution
No ROM, no BIOS, no save state needed on client
Bandwidth: ~4 MB/s
E2E latency: ~7ms (browser)
```

### For NOBD Stick Players: H.264 (Option 1)
```
Server encodes H.264 via NVENC (sub-frame)
Client decodes via hardware WebCodecs
E2E latency: ~3-4ms (NOBD XDP input)
Bandwidth: ~1.9 MB/s
```

---

## LATENCY COMPARISON

| System | Input Latency | Bandwidth | Resolution |
|--------|--------------|-----------|------------|
| Fightcade (GGPO) | 3-4 frames (50-67ms) | ~50 KB/s | 640x480 |
| Parsec | 1-2 frames (16-33ms) | 5-15 MB/s | Any |
| GeForce NOW | 1-2 frames | 15-25 MB/s | Up to 4K |
| **MapleCast TA Mirror** | **~7ms (< 0.5 frame)** | **4 MB/s** | **Any** |
| **MapleCast H.264** | **3-4ms (<0.25 frame)** | **1.9 MB/s** | 640x480 |
| **MapleCast Option 6** | **0ms (lookup)** | **15 KB/s** | Any |

---

## OVERKILL IS NECESSARY.

TA mirror mode streams GPU commands, not video. The browser renders them natively via WebGL2. Pixel perfect. Resolution independent. 60fps in a browser tab. No ROM on the client. 4 MB/s. Sub-frame latency.

We cracked the palette system. We mapped every dirty page. We know more about how MVC2 renders than anyone outside Capcom.
