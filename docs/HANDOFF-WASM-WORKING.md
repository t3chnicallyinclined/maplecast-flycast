# HANDOFF: MVC2 IN A BROWSER — IT WORKS

**Date:** April 5, 2026  
**Branch:** `ta-streaming`  
**Status:** MVC2 streaming from native server to browser at 60fps over WebSocket. Auto-start mirror fully working.

---

## WHAT HAPPENED

Marvel vs Capcom 2 running in a browser tab. No ROM on the client. Server streams delta-encoded TA commands + VRAM diffs over WebSocket. Browser renders them through flycast's OpenGL renderer compiled to WASM/WebGL2.

EmulatorJS on port 8000 now WORKS with auto-start mirror. The main page (`index.html`) embeds `emulator.html` in an iframe. `emulator.html` is the self-contained EmulatorJS page that handles core loading, BIOS setup, game boot, and mirror connection.

### The Stack
```
SERVER (native Linux, RTX 3090):
  flycast runs MVC2 → delta encodes TA commands → WebSocket broadcast on :7200

BROWSER CLIENT (EmulatorJS + flycast WASM):
  emulator.html loads flycast WASM core via EmulatorJS
  → BIOS setup via startGame patch (fetch dc_flash.bin, create /dc/, write core options, set system_directory)
  → MVC2 CHD loads → game boots
  → EJS_onGameStart fires → 1s delay → _startMirror() auto-called
  → receives 8MB VRAM SYNC on connect (mirror_apply_sync)
  → pumps 60fps delta frames via mirror_render_frame
  → flycast renderer draws to WebGL2 canvas at 640x480
  → mirror_present_frame in libretro.cpp handles canvas presentation
```

### Numbers
- WASM core: 3.4MB (7z compressed)
- VRAM sync on connect: 8MB (one-time)
- Delta frames: ~15-40KB each at 60fps (~4MB/s)
- Browser rendering: 60fps via WebGL2
- Internal resolution: 640x480 on client (server sends resolution-independent TA commands)

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
1. Start the server (see above)
2. Open http://localhost:8000
3. `index.html` loads with `emulator.html` embedded in an iframe
4. MVC2 CHD loads automatically, game boots, mirror starts on its own

The flow: MVC2 CHD loads → game boots → `EJS_onGameStart` → 1s delay → `_startMirror()` auto-called. No manual console commands needed.

---

## CRITICAL PATCHES — WebGL2 COMPATIBILITY

These were the main blocker. EmulatorJS + flycast WASM targets WebGL2, but several GL calls fail silently in browsers. All three patches are **REQUIRED**:

1. **GL_VERSION override** — flycast queries `GL_VERSION` and expects a desktop-style string. Must intercept and return a WebGL2-compatible version string.

2. **INVALID_ENUM suppression** — certain GL enum values valid on desktop OpenGL are invalid in WebGL2. These generate `INVALID_ENUM` errors that halt rendering. Must suppress/ignore them.

3. **texParameteri guard** — some `glTexParameteri` calls use parameters not supported in WebGL2 (e.g., `GL_TEXTURE_MAX_LEVEL` edge cases). Must guard or skip these calls.

Without all three, the renderer initializes but produces a black screen or crashes on the first frame.

---

## BIOS SETUP (startGame patch)

EmulatorJS does not natively set up Dreamcast BIOS. The `startGame` function is patched to:

1. Fetch `dc_flash.bin` from the server
2. Create `/dc/` directory in the WASM filesystem
3. Write core options file with correct settings
4. Set `system_directory` so flycast finds the BIOS

This runs before the core starts. Without it, flycast boots to a BIOS error screen.

---

## DEVELOPMENT CONFIG

```javascript
EJS_cacheConfig = { enabled: false };
```

Required during development. EmulatorJS aggressively caches the WASM core and assets in IndexedDB. With caching enabled, deploying a new WASM build has no effect until site data is manually cleared. Disable caching to always fetch fresh builds.

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

### Web Integration
| File | Purpose |
|------|---------|
| `index.html` | Main page, embeds emulator.html in iframe |
| `emulator.html` | Self-contained EmulatorJS page: core loading, BIOS setup, mirror JS |

---

## KNOWN ISSUES — RESOLVED

1. **Canvas presentation** — `_mirror_render_frame` rendered to flycast's FBO but RetroArch's `video_cb` didn't present it. **Fixed:** `mirror_present_frame` added in `libretro.cpp` to explicitly call `video_cb` after mirror renders.

2. **Dual render** — when mirror was active alongside the running game, both rendered to the same FBO causing flicker. **Fixed:** `emu.pause()` stops the game's render loop while keeping the main loop alive for video_cb.

3. **IndexedDB caching** — core files cached in IndexedDB meant new WASM builds were ignored. **Fixed:** `EJS_cacheConfig = { enabled: false }` during development.

4. **Core options** — all options set in defaultOptions. `reicast_enable_rttb: enabled` is critical for MVC2 sprites. `reicast_hle_bios: enabled` skips BIOS boot screen.

---

## WHAT'S NEXT

- [x] Auto-start mirror without manual `_startMirror()` call
- [x] Stop game CPU while keeping RetroArch main loop for video_cb
- [x] Apply SYNC data properly (mirror_apply_sync)
- [x] Embed in iframe for web integration
- [ ] Input forwarding: browser gamepad → WebSocket → server
- [ ] Multiple spectators on one server
- [ ] Remote server (not localhost) — test over internet
- [ ] Browser cache for SYNC data (IndexedDB)
