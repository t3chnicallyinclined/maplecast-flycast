# MapleCast Developer Guide

## What is this?

MapleCast turns the Flycast Dreamcast emulator into a **game streaming server**. One MVC2 instance runs on a VPS with NO GPU. Browsers connect and watch/play at 60fps.

This is a **single repository** that builds multiple components:

```
┌─────────────────────────────────────────────────────────────┐
│                    maplecast-flycast repo                     │
│                                                               │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │ Flycast       │  │ Relay         │  │ Web               │   │
│  │ (C++)         │  │ (Rust)        │  │ (JS/HTML)         │   │
│  │               │  │               │  │                    │   │
│  │ • Server      │  │ • Fan-out     │  │ • king.html (WASM)│   │
│  │ • Input       │  │ • WebTransport│  │ • webgpu-test     │   │
│  │ • Audio       │  │ • WebSocket   │  │ • Gamepad          │   │
│  │ • Mirror      │  │               │  │ • Overlord         │   │
│  │ • Game State  │  │               │  │ • Effects          │   │
│  └──────────────┘  └──────────────┘  └──────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## The 5 Components

### 1. Flycast Headless Server (C++)

**What:** Dreamcast emulator running headlessly — no GPU, no window. Executes MVC2 game logic, captures the TA (Tile Accelerator) command stream, and streams it to viewers.

**Where:** `core/` directory (the entire flycast emulator), with MapleCast additions in `core/network/maplecast_*.cpp`

**Build:**
```bash
cmake -B build-headless -DMAPLECAST_HEADLESS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-headless -j$(nproc)
```

**Key files:**
| File | Purpose |
|------|---------|
| `core/network/maplecast_mirror.cpp` | TA stream capture, delta encoding, zstd compression, dirty page scanning |
| `core/network/maplecast_ws_server.cpp` | WebSocket server (:7210) — stream + lobby + player control |
| `core/network/maplecast_input_server.cpp` | UDP input server (:7100) — receives gamepad data from sticks + browsers |
| `core/network/maplecast_audio.cpp` | Audio capture from AICA, SPSC ring buffer to sender thread |
| `core/network/maplecast_gamestate.cpp` | 253-byte game state extraction from DC RAM |
| `core/network/maplecast_palette.cpp` | PVR palette override system (custom skins) |
| `core/network/maplecast_control_ws.cpp` | Control WS (:7211) — admin commands, config, savestates |
| `core/network/maplecast_compress.h` | ZCST zstd compression wrapper |
| `core/network/maplecast_xdp_input.cpp` | AF_XDP zero-copy input (Linux kernel bypass) |
| `core/hw/aica/aica.cpp` | ARM7 audio processor scheduling (batched, Phase C) |

**Ports:**
| Port | Protocol | Purpose |
|------|----------|---------|
| 7100 | UDP | Input server (NOBD sticks, browser gamepads) |
| 7101 | UDP | Tape publisher (input recording) |
| 7102 | TCP | State sync (savestates for native clients) |
| 7210 | TCP/WS | Mirror stream + lobby (relay connects here) |
| 7211 | TCP/WS | Control (loopback only — admin dashboard) |
| 7213 | TCP/WS | Audio-only stream |

### 2. Input Server (built into Flycast)

**What:** Receives gamepad input from multiple sources and writes to the emulator's `kcode[]` array atomically.

**NOT a separate binary.** It's part of the flycast headless server, running on dedicated threads.

**Input sources:**
- NOBD arcade sticks → UDP :7100 (12KHz, zero-copy XDP)
- Browser gamepads → WebSocket :7210 (via relay) or :7210 (direct /play)
- Native mirror clients → UDP :7100

**Key architecture:** Dual-policy input latch (LatencyFirst / ConsistencyFirst). See `docs/INPUT-LATCH.md`.

### 3. Relay (Rust)

**What:** Fan-out server. Connects to flycast :7210 as a WebSocket client, broadcasts the TA stream to all connected browsers. Handles WebTransport (QUIC/UDP) for lower latency.

**Where:** `relay/` directory

**Build:**
```bash
cd relay && cargo build --release
```

**Key files:**
| File | Purpose |
|------|---------|
| `relay/src/main.rs` | Entry point, CLI args, spawns listeners |
| `relay/src/fanout.rs` | Core relay engine — upstream connector, client listener, broadcast |
| `relay/src/webtransport.rs` | QUIC/HTTP3 listener via wtransport crate |
| `relay/src/protocol.rs` | Wire format detection (ZCST, SYNC, audio) |
| `relay/src/turn.rs` | HTTP API (/metrics, /health, /turn-cred) |

**Ports:**
| Port | Protocol | Purpose |
|------|----------|---------|
| 7201 | TCP/WS | WebSocket client listener |
| 443 | UDP/QUIC | WebTransport listener (needs dedicated CPU) |

**CRITICAL RULE:** The relay must NOT forward `join`/`leave` messages to upstream flycast. These go via the browser's direct `/play` WebSocket connection. See `relay/src/fanout.rs` line ~553.

### 4. WASM Renderer (king.html)

**What:** The original browser renderer. Compiles flycast's OpenGL rendering code to WebAssembly. Receives the TA stream, decodes it, and renders via WebGL2.

**Where:** `packages/renderer/` (WASM build) + `web/king.html` + `web/js/` (browser modules)

**Build:**
```bash
cd packages/renderer && ./build.sh
```

**Key files:**
| File | Purpose |
|------|---------|
| `packages/renderer/src/wasm_bridge.cpp` | C++ WASM bridge — frame decode, texture cache, GL setup |
| `web/king.html` | Main spectator page — full arcade UI, queue, skins, chat |
| `web/js/renderer-bridge.mjs` | JS↔WASM bridge, audio worklet, WebSocket management |
| `web/js/render-worker.mjs` | Worker thread for decode + render |
| `web/js/gamepad.mjs` | Gamepad polling with MessageChannel burst (proven, battle-tested) |
| `web/js/state.mjs` | Shared state container (player info, connection, UI state) |
| `web/js/ws-connection.mjs` | WebSocket connection management (stream + control) |

### 5. WebGPU Renderer (webgpu-test.html) — NEW

**What:** Pure JavaScript + WebGPU renderer. Zero WASM, zero compile step. Includes post-processing effects, custom 3D backgrounds, AI-generated stages, and gamepad controls.

**Where:** `web/webgpu/` + `web/webgpu-test.html`

**Key files:**
| File | Purpose |
|------|---------|
| `web/webgpu/pvr2-renderer.mjs` | GPU render pipeline (gold standard config at top) |
| `web/webgpu/ta-parser.mjs` | TA command parser (vertex types 0-8, sprites) |
| `web/webgpu/texture-manager.mjs` | Dirty-page texture cache, VQ/mipmap/palette decode |
| `web/webgpu/shaders.mjs` | WGSL shaders + 20 visual effects |
| `web/webgpu/post-process.mjs` | Resolution scaling + 15 post-process effects |
| `web/webgpu/transport.mjs` | Adaptive WebTransport/WebSocket |
| `web/webgpu/frame-decoder.mjs` | ZCST/zstd decompression, delta frame application |
| `web/webgpu/frame-predictor.mjs` | Vertex extrapolation for predictive rendering |
| `web/webgpu/fzstd.mjs` | Local fzstd 0.1.1 bundle (no CDN dependency) |

**Full documentation:** `docs/WEBGPU-RENDERER.md`

---

## How They Connect

```
                    INTERNET
                       │
    ┌──────────────────┼──────────────────┐
    │                  │                  │
    ▼                  ▼                  ▼
 Browser A          Browser B          NOBD Stick
 (king.html         (webgpu-test)      (UDP 12KHz)
  WASM+WebGL)       (JS+WebGPU)           │
    │                  │                   │
    │ wss://../ws      │ WebTransport      │ UDP :7100
    ▼                  ▼                   ▼
 ┌──────────────────────────────────────────────┐
 │              VPS (nobd.net)                   │
 │                                               │
 │  nginx (:443 TCP)                             │
 │    ├─ /ws  → relay :7201 (WS)                │
 │    ├─ /play → flycast :7210 (WS, direct)     │
 │    └─ /audio → flycast :7213 (WS)            │
 │                                               │
 │  relay (:7201 WS, :443 UDP/QUIC)             │
 │    └─ upstream → flycast :7210               │
 │                                               │
 │  flycast headless (:7210, :7100, :7211)       │
 │    ├─ SH4 CPU emulation (game logic)          │
 │    ├─ TA stream capture + compression          │
 │    ├─ Input server (UDP + WS)                  │
 │    └─ Audio capture + streaming                │
 └──────────────────────────────────────────────┘
```

## Wire Format

All components speak the same wire format:

```
Compressed: ZCST(4) + uncompressedSize(u32 LE) + zstd_blob(N)
Delta frame: frameSize(4) + frameNum(4) + pvr_snapshot(64) + TA delta + dirty pages
SYNC frame: "SYNC"(4) + vramSize(4) + vram(8MB) + pvrSize(4) + pvr(32KB)
Audio: 0xAD 0x10 + seq(2) + 512×int16 stereo PCM = 2052 bytes
```

**ALL FOUR PARSERS must stay in sync:**
1. `core/network/maplecast_mirror.cpp` (C++ server)
2. `core/network/maplecast_wasm_bridge.cpp` (WASM renderer)
3. `web/webgpu/frame-decoder.mjs` (WebGPU renderer)
4. `relay/src/protocol.rs` (Rust relay)

## Build Targets

| Target | Command | Output |
|--------|---------|--------|
| Headless server | `cmake -B build-headless -DMAPLECAST_HEADLESS=ON && cmake --build build-headless` | `build-headless/flycast` |
| Desktop client | `cmake -B build && cmake --build build` | `build/flycast` |
| Mirror client | `MAPLECAST_MIRROR_CLIENT=1 ./build/flycast` | (runtime flag) |
| WASM renderer | `cd packages/renderer && ./build.sh` | `packages/renderer/build/` |
| Relay | `cd relay && cargo build --release` | `relay/target/release/maplecast-relay` |
| WebGPU renderer | No build — pure JS, edit and deploy | `web/webgpu/*.mjs` |

## Deployment

**ALWAYS use deploy scripts. NEVER raw scp. See CLAUDE.md.**

```bash
# Headless server
./deploy/scripts/deploy-headless.sh root@66.55.128.93

# Web files
./deploy/scripts/deploy-web.sh root@66.55.128.93

# Relay (manual)
cd relay && cargo build --release
scp target/release/maplecast-relay root@66.55.128.93:/opt/maplecast-relay
ssh root@66.55.128.93 "systemctl restart maplecast-relay"
```

## Key Branches

| Branch | Purpose |
|--------|---------|
| `master` | Stable base |
| `lockstep-player-client` | WebGPU renderer + all features (gold-webgpu-v1 tag) |
| `threaded-publish` | Latest — 3D backgrounds, post-effects, Phase C ARM7 |

## Documentation Index

| Doc | What |
|-----|------|
| `CLAUDE.md` | AI assistant rules, deployment safety, wire format, memory map |
| `docs/ARCHITECTURE.md` | Full system architecture, wire format, bugs, WebTransport |
| `docs/WEBGPU-RENDERER.md` | WebGPU renderer deep dive (9 sections) |
| `docs/DEVELOPER-GUIDE.md` | This file |
| `docs/INPUT-LATCH.md` | Dual-policy input latch architecture |
| `docs/WASM-BUILD-GUIDE.md` | WASM renderer build instructions |
| `docs/SKIN-SYSTEM.md` | PVR palette override system |
| `docs/MVC2-MEMORY-MAP.md` | Game state addresses |
| `docs/DEPLOYMENT.md` | VPS deployment guide |
