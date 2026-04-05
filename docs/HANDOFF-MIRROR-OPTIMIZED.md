# HANDOFF: Mirror Mode — Optimized, WebSocket Transport, Pipelined

**Date:** April 4, 2026
**Branch:** `ta-streaming`
**Status:** WORKING — server + client tested, smooth 60fps

---

## WHAT WAS DONE THIS SESSION

### 1. Stripped Dead Experiments (-4000 lines)
Deleted 10 dead experiment modules: nudge, rend_replay, rend_diff, scanner, gs_loopback, spritelearn, ta_capture, visual_cache, lookup_test, client (old recorder). Removed all hooks from Renderer_if.cpp, emulator.cpp, mainui.cpp, TexCache.cpp.

### 2. Client/Server Directory Split
Created `core/network/maplecast/client/` and `core/network/maplecast/server/` with separated code. Client has clean mirror renderer only. Server has full stack.

### 3. Server Optimizations
- **Double-buffered TA delta** — replaced `std::vector<uint8_t> prevTA` with two static 256KB buffers. No heap allocation per frame.
- **Removed 26MB brain snapshot** — was copying ALL Dreamcast memory every 30 frames (~5ms stall). Commented out.
- **Removed server-side checksum** — 140KB XOR scan per frame, TCP guarantees integrity. Sends zero placeholder.
- **Removed VRAM hash** — was computed every frame for shm client drift detection.
- **Lightweight WebSocket server** — split out of maplecast_stream.cpp into maplecast_ws_server.cpp. No CUDA, no NVENC, no JPEG. Pure WebSocket broadcast.

### 4. Client Optimizations
- **Zero-copy TA decode** — decode delta frames directly into flycast's TA context buffer (`clientCtx.tad.thd_root`). No intermediate `std::vector`, no 140KB memcpy.
- **Removed client checksum** — TCP integrity is sufficient.
- **Pipelined decode** — background thread receives + decodes TA + stages dirty pages. Render thread just applies pages + calls Process/Render. Decode is completely off the render thread.
- **Double-buffered TA contexts** — decode writes to buffer A while render reads buffer B. No race conditions.

### 5. WebSocket Transport
- **Raw POSIX TCP socket** — websocketpp's asio resolver fails inside flycast (`getaddrinfo` flags issue). Bypassed with direct `connect()` + manual RFC 6455 WebSocket handshake.
- **TCP_NODELAY** — Nagle's algorithm disabled for lowest latency.
- Two transports: SHM (default, same machine) or WebSocket (any machine, set `MAPLECAST_SERVER_HOST`).
- WebSocket is actually faster than SHM (~885µs vs ~1020µs avg) — no memory barriers, no ring buffer overhead.

---

## HOW TO RUN

### Server
```bash
# Mirror server with input (for NOBD)
MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 bash ~/projects/maplecast-flycast/start_maplecast.sh

# Or directly without the script
ROM="$HOME/roms/mvc2_us/Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi"
MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 ~/projects/maplecast-flycast/build/flycast "$ROM"
```

### Client (SHM — same machine)
```bash
MAPLECAST_MIRROR_CLIENT=1 ~/projects/maplecast-flycast/build/flycast "$ROM"
```

### Client (WebSocket — any machine)
```bash
MAPLECAST_MIRROR_CLIENT=1 MAPLECAST_SERVER_HOST=192.168.1.x ~/projects/maplecast-flycast/build/flycast "$ROM"
```

### VSync
Enable VSync on the server for smooth frame pacing: Settings → Video → VSync = On. Without it, frames arrive at uneven intervals.

---

## PERFORMANCE

### Server per-frame overhead
```
PVR snapshot:          ~0µs   (64 bytes)
TA copy to double buf: ~30µs  (140KB memcpy)
TA delta encode:       ~50-200µs (byte scan + run encoding)
VRAM page diff:        ~200-500µs (memcmp 2048 × 4KB — biggest cost)
WebSocket send:        ~10-50µs (async)
TOTAL:                 ~300-800µs per frame
```

### Client render thread
```
Apply dirty pages:     ~5-80µs (memcpy to VRAM/PVR)
palette_update:        ~5-10µs
renderer->Process:     ~200-500µs (flycast ta_parse + textures)
TOTAL:                 ~210-590µs per frame (decode is on background thread)
```

---

## ARCHITECTURE

```
SERVER (flycast + game)
  SH4 CPU → game logic → builds TA commands
  ↓
  Renderer_if.cpp hook: serverPublish(taContext)
  ↓
  Delta encode TA + diff VRAM pages → WebSocket broadcast (port 7200)

CLIENT (flycast renderer only, CPU stopped)
  Background thread: recv() → TA delta decode → stage dirty pages
  ↓ (atomic flag)
  Render thread: apply pages → palette → Process → Render → Present
```

---

## FILES CHANGED

| File | What |
|------|------|
| `core/network/maplecast_mirror.cpp` | Server publish + client receive, both transports |
| `core/network/maplecast_mirror.h` | Shared header |
| `core/network/maplecast_ws_server.cpp/h` | Lightweight WebSocket server (no CUDA) |
| `core/hw/pvr/Renderer_if.cpp` | Server publish hook, stream hook commented out |
| `core/hw/mem/mem_watch.cpp/h` | Added mirrorActive flag (for future memwatch VRAM) |
| `core/emulator.cpp` | Mirror init, stripped dead experiment code |
| `core/ui/mainui.cpp` | Clean mirror client render loop |
| `core/rend/TexCache.cpp` | Removed visual_cache hook |
| `core/audio/audiostream.cpp` | Kept audio pushSample (server still streams PCM) |
| `core/network/CMakeLists.txt` | Added ws_server, removed dead experiments |
| `start_maplecast.sh` | Skip MAPLECAST_STREAM in mirror mode |

---

## TODO / FUTURE

### Server
- **memwatch VRAM tracking** — replace 2048-page memcmp with OS page-fault tracking. Was implemented but crashed (interfered with save state load). Needs proper init ordering.
- **Strip NVENC from server build** — maplecast_stream.cpp still compiled but unused in mirror mode.

### Client
- See `core/network/maplecast/client/OPTIMIZE_TODO.md` for micro-optimizations.
- **WASM client** — flycast-wasm builds and loads in browser. EmulatorJS runs the game. Mirror JS hookup script exists but Module memory access needs fixing. The raw WebSocket client approach (POSIX sockets) doesn't apply to WASM — need the JS WebSocket API instead.

### Two-binary split
- Experimental `MAPLECAST_CLIENT` CMake option and `maplecast_mirror_client.cpp` / `maplecast_mirror_server.cpp` exist but aren't wired up yet. Current single binary with env var switching works.

---

## GIT LOG (this session)
```
2642d14 Fix port conflict: skip MAPLECAST_STREAM when mirror server active
7cc0567 Lightweight WS server, remove server-side bloat
5f24646 Client: zero-copy TA decode + fused checksum + timing
974fd7e WebSocket mirror client — raw POSIX socket, no resolver dependency
f86fb4d Server: double-buffered TA delta, no std::vector heap churn
eea512d Strip dead experiments, client/server split, optimize mirror pipeline
```
