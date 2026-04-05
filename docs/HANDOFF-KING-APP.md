# HANDOFF: King of Marvel — Standalone Web App

**Date:** April 5, 2026
**Branch:** `ta-streaming`
**Status:** EmulatorJS mirror streaming works but has resolution/scaling issues. Next step: standalone WASM renderer or Rust web app.

---

## WHAT WE HAVE

### Working Mirror Streaming (port 8000)
- Native flycast server runs MVC2, streams TA commands + VRAM diffs over WebSocket (port 7200)
- Browser client receives stream, renders via flycast WASM compiled as libretro core inside EmulatorJS
- 60fps, ~4MB/s bandwidth, ~0.5ms server publish time
- Auto-start: MVC2 CHD loads → game boots → pauses → mirror connects → streaming begins

### Working Features
- Lobby system: join, queue ("I Got Next"), disconnect, spectator count
- NOBD stick registration via rhythm detection (tap 5x, pause, 5x)
- Game state reading from MVC2 RAM (253 bytes: health, combos, meter, characters)
- Win detection (all 3 chars KO'd), auto-disconnect loser, queue promotion
- Leaderboard (localStorage), live match stats, player name overlay
- Diagnostics: ping, FPS, stream bandwidth, per-player input rate/E2E latency

### king.html — The Target UI
`web/king.html` is a stunning SF2-inspired arcade cabinet interface with:
- Arcade cabinet bezel with CRT scanlines and vignette effects
- `#game-screen` div (4:3 aspect ratio) — where the game stream goes
- Match HUD overlay: player names, health bars, timer, combo counter
- Hall of Fame leaderboard (left sidebar) with tabs: today/weekly/all-time/character/masher/surgeon
- Queue system (right sidebar): "THE LINE" with player queue, chat, register stick
- Ticker bar with match history
- Control panel with decorative joystick/buttons
- Match results overlay with stats

The `#game-screen` is currently empty (shows "INSERT COIN"). It needs the game stream rendered into it.

---

## THE RESOLUTION PROBLEM

### What Happens
EmulatorJS + libretro flycast has a complex resolution chain:
1. `reicast_internal_resolution` sets `config::RenderResolution` (height in pixels)
2. `getScaledFramebufferSize()` multiplies DC native viewport by `RenderResolution / 480.0f`
3. The result becomes the WebGL FBO size AND the dimensions reported to RetroArch via `video_cb()`
4. `setGameGeometry()` reports `base_width/base_height` and `max_width/max_height` to EmulatorJS
5. EmulatorJS sizes its canvas based on these dimensions

### Why It Breaks
- **Server's PVR registers** (TA_GLOB_TILE_CLIP, SCALER_CTL) encode the server's window size
- Mirror bridge applies these to the client, changing the viewport every frame
- Resizing the server window causes the client to resize in real-time
- `retro_get_system_av_info()` initially assumes 16:9 aspect (framebufferWidth = RenderResolution * 16 / 9 = 853), setting max_width to 853 even for 4:3 games
- `getPvrFramebufferSize()` reads global `FB_R_CTRL.vclk_div` and `SPG_CONTROL.interlace` — if these are stale/wrong, it clamps height to 240 instead of 480, causing 2x zoom
- EmulatorJS CSS fights with any container — it expands infinitely without an iframe boundary

### What Was Tried
- Hardcoding framebufferWidth/Height to 640x480 in WASM bridge — broke rendering
- Skipping TA_GLOB_TILE_CLIP from server — client had uninitialized values, nothing rendered
- Forcing tile clip to 20x15 tiles (640x480) — didn't fix the visual
- Setting FB_R_CTRL.vclk_div=1, SPG_CONTROL.interlace=1 — partial fix for 240p clamping
- CSS object-position, aspect-ratio, overflow:hidden — EmulatorJS overrides them
- Inline EmulatorJS (no iframe) — infinite page expansion
- Various iframe sizes — game renders but viewport doesn't match

### What Actually Worked (sort of)
- Iframe at ~605x600px with server window at 640x480 — game visible but slightly cropped/stretched
- The server's window size directly controls the client viewport via PVR registers
- Cable type matters: Composite (Cable=3) outputs 853x480, VGA (Cable=0) outputs 640x480

---

## THE PLAN: TWO OPTIONS

### Option A: Standalone WASM Renderer (no EmulatorJS)

Build a minimal WASM binary that contains ONLY the flycast renderer + mirror client. No RetroArch, no libretro API, no EmulatorJS.

**Architecture:**
```
king.html
  └─ #game-screen div (4:3, CRT effects)
     └─ <canvas> with WebGL2 context
        └─ flycast-mirror.wasm (standalone, ~3-5MB)
           ├─ mirror_init() — init OpenGL renderer
           ├─ mirror_connect(url) — WebSocket to server
           ├─ Receives SYNC + delta frames
           ├─ Calls renderer->Process() + Render() + Present()
           └─ Renders directly to the canvas
```

**What's needed:**
1. New cmake target: `flycast_mirror_wasm` — compile only:
   - `core/rend/gles/` (OpenGL ES renderer)
   - `core/hw/pvr/` (PVR registers, TA parsing)
   - `core/network/maplecast_wasm_bridge.cpp` (mirror protocol)
   - `core/rend/TexCache.cpp` (texture cache)
   - Minimal stubs for everything else
2. New `main()` that:
   - Creates WebGL2 context on a canvas element
   - Calls `mirror_init()`
   - Connects WebSocket
   - `requestAnimationFrame` loop calling `mirror_render_frame()`
3. Emscripten build with `-s FULL_ES3=1 -s MIN_WEBGL_VERSION=2`
4. No RetroArch, no libretro, no EmulatorJS, no ROM loading

**Pros:** Total control over canvas size, no CSS fighting, tiny binary, direct WebGL
**Cons:** Significant build effort, need to stub/extract renderer from flycast

### Option B: Rust Web App with WebGPU

Build the renderer client in Rust + wgpu, compile to WASM. Modern GPU API, no legacy OpenGL issues.

**Architecture:**
```
king.html (or Rust-served via Actix/Axum)
  └─ #game-screen div
     └─ <canvas> with WebGPU context
        └─ maplecast-client.wasm (Rust, ~2-4MB)
           ├─ WebSocket client (tokio-tungstenite)
           ├─ TA command decoder (port from C++)
           ├─ PVR register state machine
           ├─ VRAM texture manager
           ├─ WebGPU render pipeline
           │   ├─ Vertex shader: TA polygon transform
           │   ├─ Fragment shader: texture + palette lookup
           │   └─ Compute shader: TA parsing (optional)
           └─ Renders to canvas at any resolution
```

**What's needed:**
1. Port the TA command parser from C++ to Rust (core/hw/pvr/ta_vtx.cpp — ~2000 lines)
2. Port the texture decoder (core/rend/TexCache.cpp, texconv.cpp — palette lookup, twiddled textures)
3. WebGPU render pipeline that draws PVR polygon lists
4. WebSocket client for mirror protocol (SYNC + delta frames)
5. VRAM manager (8MB, page-level dirty tracking)

**Pros:** Modern GPU API, pixel-perfect control, blazing fast, future-proof (WebGPU is the future), full control over everything, can run as native desktop app too
**Cons:** Large porting effort, need deep understanding of PVR2 GPU architecture

---

## CRITICAL KNOWLEDGE FOR NEXT SESSION

### WebGL2 Patches (REQUIRED for flycast WASM)
These patches MUST be applied before any WebGL2 context is created:
- `getParameter(GL_VERSION)` → return `"OpenGL ES 3.0 WebGL 2.0"` (flycast checks this string)
- `getParameter(SHADING_LANGUAGE_VERSION)` → return `"OpenGL ES GLSL ES 3.00"`
- `getError()` → silently eat `GL_INVALID_ENUM` (0x500)
- `texParameteri/f()` → skip calls when no texture is bound

Without these, flycast crashes with `function signature mismatch` on the first render frame.

### BIOS Setup (REQUIRED for EmulatorJS)
The `startGame` patch must:
1. Fetch `dc_flash.bin` separately (EmulatorJS only loads `dc_boot.bin` via `EJS_biosUrl`)
2. Create `/dc/` in Emscripten virtual FS
3. Copy both BIOS files to `/dc/`
4. Write core options string to config file via `setupCoreSettingFile` callback
5. Set `system_directory = "/"` in `retroarch.cfg`

### EmulatorJS Cache Busting
EmulatorJS caches WASM cores in IndexedDB for 5 days. Use `EJS_cacheConfig = { enabled: false }` during development. Even incognito mode caches within the session.

### Mirror Protocol
Server (port 7200) sends:
1. **SYNC** on connect: `"SYNC"(4) + vramSize(4) + vram(8MB) + pvrSize(4) + pvr(32KB)`
2. **Delta frames** at 60fps:
   ```
   frameSize(4) + frameNum(4) + pvr_snapshot(64) +
   taOrigSize(4) + taDeltaSize(4) + deltaData + checksum(4) +
   dirtyPageCount(4) + [regionId(1) + pageIdx(4) + pageData(4096)] * N
   ```
3. **JSON** for lobby: status, join, leave, queue, ping/pong, register_stick, game state

### WASM Build Chain
```
flycast source (cmake) → libflycast_libretro_emscripten.a
  + emar d ZipArchive.cpp.o (remove libzip dependency)
  + RetroArch EmulatorJS objects (obj-emscripten/*.o)
  + link-ubuntu.sh → flycast_libretro_upstream.js + .wasm
  + 7z package → flycast-wasm.data
  + Deploy to demo/data/cores/flycast-wasm.data
```

Key: `chd_stream.o` must NOT be excluded from RetroArch objects (enables CHD loading).

### king.html Integration Points
The game stream needs to render inside `#game-screen`:
```html
<div id="game-screen">
  <!-- INSERT: canvas or iframe here -->
  <!-- CRT effects are CSS ::after and ::before pseudo-elements on #game-screen -->
  <!-- Match HUD is positioned absolute inside #game-screen -->
</div>
```

The `#game-screen` has:
- `width: 100%` (fills cabinet center column)
- `aspect-ratio: 4/3`
- CRT scanline overlay via `::after`
- Vignette via `::before`
- `image-rendering: pixelated`
- `overflow: hidden`

### Data Flow for king.html
All data comes from the WebSocket status JSON (broadcast every 1 second):
```json
{
  "type": "status",
  "p1": { "name": "...", "connected": true, "type": "hardware", "pps": 12000, "cps": 500 },
  "p2": { "name": "...", "connected": true, "type": "browser", "pps": 250, "cps": 80 },
  "spectators": 5,
  "queue": ["Player3", "Player4"],
  "frame": 123456,
  "fps": 60,
  "stream_kbps": 4000,
  "publish_us": 460,
  "game": {
    "in_match": true,
    "timer": 88,
    "p1_combo": 14,
    "p2_combo": 0,
    "p1_meter": 3,
    "p2_meter": 1,
    "p1_hp": [144, 100, 80],
    "p2_hp": [60, 0, 0],
    "p1_chars": [7, 43, 53],
    "p2_chars": [12, 39, 50]
  }
}
```

### Files Changed This Session
- `core/network/maplecast_ws_server.cpp/h` — lobby, queue, ping, stick registration, game state, telemetry
- `core/network/maplecast_input_server.cpp/h` — rhythm registration, no auto-assign NOBD
- `core/network/maplecast_mirror.cpp` — telemetry via updateTelemetry()
- `core/network/maplecast_wasm_bridge.cpp` — mirror_apply_sync, various resolution attempts
- `core/network/maplecast_gamestate.cpp/h` — setPlayerName (attempted RAM patching)
- `shell/libretro/libretro.cpp` — mirror_present_frame, geometry fixes
- `web/index.html` — full lobby UI, leaderboard, diagnostics, overlays
- `web/emulator.html` — self-contained EmulatorJS + mirror client

### Credit
- Player name format research: [Paxtez/MVC2EditPlayerNames](https://github.com/Paxtez/MVC2EditPlayerNames)
- Cheat Engine addresses: [lord-yoshi/MvC2-CE-Trainer-Script](https://github.com/lord-yoshi/MvC2-CE-Trainer-Script)

---

## RECOMMENDED NEXT STEPS

1. **Standalone WASM build** — extract flycast renderer into a minimal WASM module that renders to a raw canvas. No EmulatorJS, no RetroArch. This solves the resolution problem permanently.

2. **Integrate into king.html** — put the canvas inside `#game-screen`, wire up the WebSocket status data to the HUD elements (health bars, timer, combos, player names, queue).

3. **Rust WebGPU client** (longer term) — port the TA parser and renderer to Rust + wgpu for maximum performance and control. Can compile to both WASM (browser) and native (desktop).
