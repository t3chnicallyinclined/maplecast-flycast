# MapleCast Streaming Architecture Options

**Date:** April 4, 2026
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

---

## Option 1: H.264 Only (Current Production)

```
Server: SH4 compute → GPU render → CUDA copy → NVENC encode → WebSocket/WebRTC
Client: Browser receives → hardware decode → display

Bandwidth:  1.9 MB/s
Latency:    4.2ms E2E (NOBD stick), 4.9ms (browser gamepad)
Client:     Any browser
Resolution: 640x480 (server-locked)
Status:     PROVEN, SHIPPING
```

**Pros:** Dead simple, works everywhere, rock solid, lowest E2E proven
**Cons:** Resolution locked, decode latency, bandwidth scales with quality

---

## Option 2: Input Sync + 253-Byte State Correction

```
Server: receives inputs → runs SH4 → reads 253B state → broadcasts
Client: runs flycast + ROM → receives inputs → processes locally
        → receives 253B state → compares → corrects drift

Bandwidth:  ~16 KB/s (14B inputs + 253B state × 60fps)
Latency:    0ms (local render), corrections arrive 1 frame later
Client:     flycast + ROM
Resolution: ANY (local render)
Status:     COMPONENTS PROVEN, NOT INTEGRATED
```

**Pros:** Lowest bandwidth possible, instant local feedback, any resolution
**Cons:** Client needs flycast + ROM, possible micro-snaps on correction

### How Drift Correction Works
- Client runs same inputs through same ROM = 99.7% identical output
- 0.3% of frames drift (RAM autopsy proved this)
- Server sends authoritative 253B every frame
- Client compares and corrects ONLY drifted fields
- Sub-pixel position corrections = invisible to player
- Animation frame corrections = rare, 1-frame hitch at worst
- No rollback, no re-simulation, no lag

### The Snap Problem
Writing game state every frame causes CPU to fight writes.
Solutions:
a) Only correct when drift exceeds threshold (e.g. position > 1 pixel)
b) Correct AFTER CPU runs but BEFORE renderer reads (between compute + render)
c) Use soft correction (lerp toward server state instead of snap)
d) Only correct fields that matter visually (position, animation, health)

### Open Questions
- Does 253B correction every frame cause visual jitter?
- Should we correct every frame or only on drift?
- Can we correct between compute and render to avoid CPU fight?
- What's the minimum set of fields needed for visual sync?

---

## Option 3: Server-Authoritative Hybrid

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
```

**Pros:** Absolute minimum bandwidth, instant local play, self-healing
**Cons:** Complex sync logic, needs careful threshold tuning

---

## Option 4: TA Command Streaming (Mirror Mode)

```
Server: SH4 compute → captures TA commands + memory diffs → streams
Client: receives TA commands → ta_parse → render (no CPU)

Bandwidth:  370 KB/frame = 21 MB/s raw, ~7 MB/s compressed
Latency:    1 frame (network transit)
Client:     flycast renderer (no CPU) + save state
Resolution: ANY (local render)
Status:     PROVEN — 11 minutes stable, characters rendering
```

**Pros:** Pixel perfect, no ROM needed on client, no CPU, any resolution
**Cons:** High bandwidth, 1-frame latency, 11-min crash bug

---

## Option 5: Predictive Client

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
```

**Pros:** Best of both worlds — instant AND correct
**Cons:** Complex, dual render path, needs fast comparison

---

## Option 6: 253-Byte Lookup Renderer (No Emulator)

```
OFFLINE (one-time build):
  Play every character × every animation × every frame
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
```

**Pros:** Absolute minimum bandwidth, no ROM, no emulator, any resolution
**Cons:** Huge offline build step, may miss edge cases (effects, supers), doesn't exist yet

### What We Need to Build This
- Complete the visual_cache scan for all 59 characters
- Map every (char_id, anim_state, anim_timer) → TA display list
- Handle compositing (stage + 6 characters + effects)
- Handle super moves (unique visual states)

---

## Option 7: Split Pipeline (All Outputs Simultaneously)

```
One server compute cycle produces ALL of:
  → H.264 frame      for player       (4.2ms latency, 1.9 MB/s)
  → 253-byte state   for overlay      (0ms, 15 KB/s)
  → TA commands       for spectators   (1 frame, 8 MB/s compressed)
  → Memory page diffs for replay       (0ms, 168 KB/frame)

Each consumer subscribes to the streams they need:
  Player:     H.264 + 253B overlay
  Spectator:  TA commands (any resolution)
  Replay:     253B + page diffs (rewindable)
  Analytics:  253B (frame data, combos, damage)
```

**Pros:** Every use case served simultaneously, already built
**Cons:** Server does more work (but it's all parallel, GPU does the heavy lifting)

---

## RECOMMENDATION FOR COMPETITIVE PLAY

### Phase 1 (NOW): Option 1 + Option 2 Hybrid
```
Player sees: H.264 stream (4.2ms, proven)
Client also: receives 253B state for overlay (health bars, combo, frame data)
Background:  record 253B at 60fps for replay

Total bandwidth: 1.9 MB/s + 15 KB/s = basically same as current
New features: game state overlay, replay recording, frame data display
```

### Phase 2 (NEXT): Option 2 for Desktop App
```
Desktop client: runs flycast + ROM locally
Input: NOBD stick → local flycast (0ms) + server
Server: sends 253B corrections at 60fps
Result: 0ms input latency, 16 KB/s bandwidth, any resolution

This is the COMPETITIVE edge:
  - Fightcade: ~3-4 frames of rollback latency
  - MapleCast: 0 frames, server-authoritative, 16 KB/s
```

### Phase 3 (FUTURE): Option 6 for Browser Spectators
```
Browser client: WebGPU renderer + pre-cached TA lookup
Server sends: 253 bytes/frame via DataChannel
Browser renders: at 4K, 120fps, any resolution
Bandwidth: 15 KB/s for spectators

Thousands of spectators watching a tournament at 4K
using 15 KB/s each. On a phone. On 3G.
```

---

## LATENCY COMPARISON

| System | Input Latency | Bandwidth | Resolution |
|--------|--------------|-----------|------------|
| Fightcade (GGPO) | 3-4 frames (50-67ms) | ~50 KB/s | 640x480 |
| Parsec | 1-2 frames (16-33ms) | 5-15 MB/s | Any |
| GeForce NOW | 1-2 frames | 15-25 MB/s | Up to 4K |
| **MapleCast H.264** | **4.2ms (<1 frame)** | **1.9 MB/s** | 640x480 |
| **MapleCast Option 2** | **0ms (local)** | **16 KB/s** | Any |
| **MapleCast Option 6** | **0ms (lookup)** | **15 KB/s** | Any |

---

## WE ARE INSANE

253 bytes. 16 KB/s. 0ms input latency. Any resolution. Pixel perfect.
The Dreamcast has a 26 MB brain. Only 168 KB changes per frame.
We mapped every dirty page. We cracked the palette system.
We know more about how MVC2 renders than anyone outside Capcom.

**OVERKILL IS NECESSARY.**
