# HANDOFF: MVC2 IN A BROWSER — IT WORKS

**Date:** April 5, 2026  
**Branch:** `ta-streaming`  
**Status:** MVC2 streaming from native server to browser at 60fps over WebSocket

---

## WHAT HAPPENED

Marvel vs Capcom 2 running in a browser tab. No ROM on the client. Server streams delta-encoded TA commands + VRAM diffs over WebSocket. Browser renders them through flycast's OpenGL renderer compiled to WASM/WebGL2.

### The Stack
```
SERVER (native Linux, RTX 3090):
  flycast runs MVC2 → delta encodes TA commands → WebSocket broadcast on :7200

BROWSER CLIENT:
  EmulatorJS loads flycast WASM core (3.4MB download)
  → boots Dreamcast BIOS
  → JS connects WebSocket to server
  → receives 8MB VRAM SYNC on connect
  → pumps 60fps delta frames into _mirror_render_frame()
  → flycast renderer draws to WebGL2 canvas
```

### Numbers
- WASM core: 3.4MB (7z compressed)
- VRAM sync on connect: 8MB (one-time)
- Delta frames: ~15-40KB each at 60fps (~4MB/s)
- Browser rendering: 60fps via WebGL2

---

## HOW TO RUN

### Server
```bash
MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 bash ~/projects/maplecast-flycast/start_maplecast.sh
```

### Native Client
```bash
# With ROM (plays locally, mirrors server)
~/projects/maplecast-flycast/build/flycast --server 127.0.0.1

# Without ROM (romless, server sends everything)
MAPLECAST_MIRROR_CLIENT=1 MAPLECAST_SERVER_HOST=127.0.0.1 ~/projects/maplecast-flycast/build/flycast
```

### Browser Client
1. Start demo server: `cd ~/projects/flycast-wasm/demo && node server.js 3030`
2. Open http://localhost:3030
3. Load mvc2.chd (or mirror.bin for BIOS-only)
4. Once game is running, open console and type: `_startMirror()`

### Browser Client (auto-start)
The `_startMirror()` function is defined in server.js. Can be triggered via:
- `EJS_onGameStart` callback (auto on game start)
- Manual console call
- URL parameter (TODO)

---

## KEY FILES

### Native (maplecast-flycast repo)
| File | Purpose |
|------|---------|
| `core/network/maplecast_mirror.cpp` | Server publish + client receive, shm + WebSocket |
| `core/network/maplecast_ws_server.cpp/h` | Lightweight WebSocket server (no CUDA) |
| `core/network/maplecast_mirror.h` | Shared header |
| `core/hw/pvr/Renderer_if.cpp` | Server publish hook |
| `core/emulator.cpp` | Mirror init, romless client, client defaults |
| `core/linux-dist/main.cpp` | --server flag, romless auto-load |
| `core/ui/mainui.cpp` | Mirror client render loop |
| `start_maplecast.sh` | Server launch script |

### WASM (flycast-wasm repo)
| File | Purpose |
|------|---------|
| `core/network/maplecast_wasm_bridge.cpp` | WASM exports: mirror_init, mirror_render_frame, mirror_apply_sync |
| `core/network/maplecast_mirror.cpp/h` | Client-only mirror code (from maplecast/client/) |
| `shell/libretro/libretro.cpp` | mirror_present_frame (video_cb wrapper) |
| `upstream/link-ubuntu.sh` | Link script with all exports + libzip |
| `demo/server.js` | EmulatorJS page + mirror JS + core options |

---

## KNOWN ISSUES

1. **Browser canvas presentation** — `_mirror_render_frame` renders to flycast's FBO but RetroArch's `video_cb` doesn't always present it. Current workaround: let MVC2 boot first (initializes renderer), then start mirror. Proper fix: bypass RetroArch video pipeline or hook into `retro_run`.

2. **Dual render** — when mirror is active alongside the running game, both render to the same FBO causing occasional flicker. Need to stop the game's render loop while keeping the main loop alive for video_cb.

3. **EmulatorJS cache** — core files cached in IndexedDB. Clear site data when deploying new WASM builds.

4. **Core options** — all options set in server.js defaultOptions. `reicast_enable_rttb: enabled` is critical for MVC2 sprites. `reicast_hle_bios: enabled` skips BIOS boot screen.

---

## WHAT'S NEXT

- [ ] Auto-start mirror without manual `_startMirror()` call
- [ ] Stop game CPU while keeping RetroArch main loop for video_cb
- [ ] Apply SYNC data properly (mirror_apply_sync works but needs link fix)
- [ ] Browser cache for SYNC data (IndexedDB)
- [ ] Embed in iframe for web integration
- [ ] Input forwarding: browser gamepad → WebSocket → server
- [ ] Multiple spectators on one server
- [ ] Remote server (not localhost) — test over internet
