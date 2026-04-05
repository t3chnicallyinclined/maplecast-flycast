# HANDOFF: King of Marvel — Full Web App Integration

**Date:** April 5, 2026
**Branch:** `ta-streaming`
**Goal:** Merge the standalone WASM renderer + arcade cabinet UI + P2P relay + stats into one cohesive King of Marvel web app

---

## WHAT EXISTS NOW

### 1. Standalone WASM Renderer (PIXEL PERFECT, 60fps)
**Path:** `packages/renderer/`
**Output:** `web/renderer.mjs` (97KB) + `web/renderer.wasm` (790KB)

A 750KB standalone Dreamcast PVR2 GPU renderer compiled from 28 flycast source files + 40 stubs. Renders MVC2 pixel-perfect at 60fps from raw TA commands streamed over WebSocket. No ROM, no BIOS, no emulator.

**Exported API:**
```js
Module._renderer_init(width, height)     // Create WebGL2 context + renderer
Module._renderer_sync(dataPtr, size)     // Apply 8MB VRAM + 32KB PVR regs
Module._renderer_frame(dataPtr, size)    // Decode delta frame + render
Module._renderer_resize(width, height)   // Handle canvas resize
Module._renderer_set_option(opt, value)  // Change quality settings at runtime
Module._renderer_get_option(opt)         // Read current quality setting
Module._renderer_destroy()              // Cleanup
```

**Quality options (via renderer_set_option):**
| Option ID | Name | Values |
|-----------|------|--------|
| 0 | RenderResolution | 480 (native) to 1920 (4x) |
| 1 | TextureUpscale | 1/2/4 (xBRZ — disabled in WASM, freezes) |
| 2 | Fog | 0/1 |
| 3 | ModifierVolumes | 0/1 |
| 4 | PerStripSorting | 0/1 |
| 5 | AnisotropicFiltering | 1/2/4/8/16 |
| 6 | TextureFiltering | 0 (default) / 1 (nearest) / 2 (linear) |
| 7 | PerPixelLayers | 4/8/16/32 |
| 8 | EmulateFramebuffer | 0/1 (MUST stay 0) |

**CRITICAL: The JS GL State Hack**
Emscripten's C-level `glEnable`/`glDisable` inside the GLSM do NOT propagate to the WebGL2 context. Before EVERY `_renderer_frame()` call, you MUST:
```js
const gl = canvas.getContext('webgl2');
gl.enable(gl.BLEND);
gl.enable(gl.DEPTH_TEST);
gl.enable(gl.STENCIL_TEST);
gl.enable(gl.SCISSOR_TEST);
Module._renderer_frame(bufPtr, size);
```
Without this, all translucent polygons (health bars, sprites, HUD) render as opaque black boxes.

**Canvas element:** Must have `id="game-canvas"`. The WASM module targets `#game-canvas` for WebGL2 context creation.

**WebGL2 patches needed (before WASM loads):**
```js
// Wrap enable/disable to silently eat invalid enums (GL_FOG, GL_ALPHA_TEST)
ctx.enable = function(cap) { try { origEnable(cap); } catch(e) {} };
ctx.disable = function(cap) { try { origDisable(cap); } catch(e) {} };
ctx.getError = function() { const e = orig(); return e === 0x500 ? 0 : e; };
ctx.texParameteri = function(t,p,v) { try { orig(t,p,v); } catch(e) {} };
```

**Build:** `cd packages/renderer && ./build.sh` (requires emsdk)

---

### 2. King of Marvel UI (`web/king.html`)
**48KB** SF2-inspired arcade cabinet interface. Static HTML/CSS/JS mockup with placeholder data.

**Components:**
- **Top banner:** "KING OF MARVEL" title + current king name + streak
- **Viewer bar:** Watching / Playing / In Line counts
- **Left sidebar — Hall of Fame:** Tabbed leaderboard (Streak/Combo/Speed/Perfect/Masher/Surgeon)
- **Center — Arcade cabinet:** `#game-screen` div (4:3 aspect ratio) where the renderer goes
- **Right sidebar — Live Arcade:**
  - NOW PLAYING (P1 vs P2 with records)
  - "I GOT NEXT" button (queue up)
  - THE LINE (queue list with positions)
- **Bottom — Chat:** "TRASH TALK" with reaction buttons (HYPE/BODIED/GGs/SALTY/FRAUD)
- **Ticker bar:** Scrolling match highlights
- **Match HUD:** Health bars, timer, combo counter (overlaid on game screen)
- **Match Results:** Post-match overlay with stats
- **"HERE COMES A NEW CHALLENGER"** fullscreen overlay
- **Sign-in modal:** Fighter name + avatar picker

**All interactive — tabs switch, queue join/leave, chat works with local state.**

---

### 3. Current Lobby Page (`web/index.html`)
**32KB** working lobby/spectator page on port 8000. Has:
- EmulatorJS WASM mirror client (renders game via iframe)
- WebSocket connection to port 7200
- Join/leave/queue via JSON messages
- Gamepad input polling at 250Hz → binary packets to server
- WebRTC P2P DataChannel setup for video/input/audio
- Player slot display (P1/P2/spectator)
- Full diagnostics overlay (FPS, bandwidth, latency, input rate, E2E timing)

**This page is the reference for WebSocket protocol and lobby interactions.**

---

### 4. P2P Spectator Relay (`web/relay.js`)
**15KB** WebRTC tree-topology relay for spectator fan-out.

**Architecture:**
- Server feeds 2-3 seed spectators directly
- Seeds relay to up to 3 children each via WebRTC DataChannels
- Two channels per connection:
  - `ta-mirror` (unordered, unreliable) — delta frames at 60fps
  - `ta-sync` (ordered, reliable) — initial 8MB SYNC, chunked to 64KB
- Server manages tree via WebSocket signaling messages

**Usage:**
```js
const relay = new MapleCastRelay(ws, rtcConfig);
relay.onFrame = (data) => { /* render delta frame */ };
relay.onSync = (data) => { /* apply VRAM+PVR sync */ };
```

---

### 5. SurrealDB Stats System (`web/schema.surql`)
**14.5KB** database schema for competitive stats.

**Tables:**
- **player** — ELO rating (K=32), rank tiers (Rookie 0-499 → Legend 3000+), W/L, streaks
- **stick** — Hardware MAC/IP registration
- **char_stats** — Per-player per-character: games, wins, best combo, kills/deaths
- **team_stats** — Per-player per-team-composition stats
- **h2h** — Head-to-head records between players
- **match** — Full match detail: players, characters, HP, combos, damage, meter, inputs, finish type
- **game_event** — Significant moments (combo breaks, perfects, comebacks)
- **badge** — Achievements (combo monster, streak king, veteran, perfect, OCV, comeback, clutch)

**Rank Tiers:** Rookie → Fighter → Warrior → Champion → Master → Grand Master → Legend

---

### 6. Rust Stats Collector (`web/collector/`)
Rust service that bridges the flycast WebSocket (port 7200) to SurrealDB.

**Reads from WS:** Input snapshots, game state, stream telemetry, match events
**Writes to SurrealDB:** Player records, character/team stats, ELO changes, badges
**Has:** Full MVC2 character ID lookup table (56 characters)

---

### 7. Server Components

**Flycast server** runs MVC2 and broadcasts:
- **Port 7200 (WebSocket):**
  - Binary: TA mirror frames (SYNC on connect, then deltas at 60fps, keyframe every 60 frames)
  - JSON: Lobby status (broadcast every 1s), join/leave/queue, match state, telemetry
- **Port 7100 (UDP):** Gamepad input (4-byte packets: LT, RT, btnHi, btnLo)
- **Port 8000 (HTTP):** Web server with COEP/COOP headers (serve.py)

**Mirror protocol (binary frames):**
```
SYNC (on connect): "SYNC"(4) + vramSize(4) + vram(8MB) + pvrSize(4) + pvr(32KB)

Delta frame: frameSize(4) + frameNum(4) + pvr_snapshot(64) +
  taSize(4) + deltaPayloadSize(4) + deltaData(var) + checksum(4) +
  dirtyPageCount(4) + [regionId(1) + pageIdx(4) + data(4096)] * N

Keyframe: deltaPayloadSize == taSize (full TA buffer, every 60 frames)
```

**Lobby JSON status (broadcast every 1s):**
```json
{
  "type": "status",
  "p1": { "name": "...", "connected": true, "type": "hardware|browser", "pps": 12000 },
  "p2": { "name": "...", "connected": true },
  "spectators": 5,
  "queue": ["Player3", "Player4"],
  "game": {
    "in_match": true, "timer": 88,
    "p1_combo": 14, "p2_combo": 0,
    "p1_hp": [144, 100, 80], "p2_hp": [60, 0, 0],
    "p1_chars": [7, 43, 53], "p2_chars": [12, 39, 50]
  }
}
```

---

## WHAT TO BUILD NEXT

### Integration Plan

Merge everything into a single cohesive web app at `king.html`:

1. **Game Canvas** — Embed the standalone WASM renderer
   - Load `renderer.mjs` + `renderer.wasm`
   - Canvas inside the arcade cabinet `#game-screen` div
   - Apply the GL state hack before each frame
   - CSS post-processing presets (CRT scanlines, bloom, etc.)

2. **WebSocket Demux** — One connection, two data types
   - Binary frames → WASM renderer (`_renderer_sync` / `_renderer_frame`)
   - JSON messages → UI state (lobby, queue, match state, chat)

3. **P2P Relay Integration** — Use `relay.js` for spectators
   - Players get direct WebSocket from server
   - Spectators use relay tree for frame delivery
   - Same `_renderer_frame` call regardless of source

4. **Game State Overlay** — React to server JSON status
   - Health bars animated from `game.p1_hp` / `game.p2_hp`
   - Timer from `game.timer`
   - Combo counter with animation from `game.p1_combo` / `game.p2_combo`
   - Character portraits from `game.p1_chars` / `game.p2_chars`

5. **Queue System** — Wire up to server
   - "I GOT NEXT" sends `{"type":"join", "id":"...", "name":"...", "device":"Browser"}`
   - Queue position from `status.queue[]`
   - Auto-promote on match end
   - Winner stays on

6. **Leaderboard** — Wire to SurrealDB via collector
   - 6 categories: Streak/Combo/Speed/Perfect/Masher/Surgeon
   - Real-time updates from match results

7. **Chat** — WebSocket JSON messages
   - Send: `{"type":"chat", "name":"...", "text":"..."}`
   - Receive: included in status broadcasts or separate chat messages
   - Reaction buttons: HYPE/BODIED/GGs/SALTY/FRAUD

### Tech Stack Options

**Option A: Keep as static HTML/JS** (fastest, what king.html already is)
- Add `<script type="module">` for WASM loader
- Vanilla JS for state management
- CSS for all styling (already done in king.html)

**Option B: React + Vite + Zustand** (from the workstream doc)
- Proper component architecture
- Zustand for real-time game state
- Vite for HMR during development
- Better long-term maintainability

**Option C: Rust web app (Actix/Axum + HTMX or Leptos)**
- Server-side rendering
- Type-safe WebSocket handling
- Integrated with the Rust collector

---

## FILES REFERENCE

| File | Purpose |
|------|---------|
| `packages/renderer/src/wasm_bridge.cpp` | Standalone WASM renderer API |
| `packages/renderer/src/wasm_gl_context.cpp` | WebGL2 context + GLSM init |
| `packages/renderer/src/glsm_patched.c` | GLSM patched for WebGL2 |
| `packages/renderer/src/stubs.cpp` | ~40 no-op stubs |
| `packages/renderer/CMakeLists.txt` | Emscripten build config |
| `web/king.html` | Arcade cabinet UI (target for integration) |
| `web/test-renderer.html` | Working renderer test + control panel |
| `web/index.html` | Current lobby page (WebSocket protocol reference) |
| `web/relay.js` | P2P spectator relay |
| `web/schema.surql` | SurrealDB stats schema |
| `web/collector/src/main.rs` | Rust stats collector |
| `web/serve.py` | HTTP server with COEP/COOP |
| `core/network/maplecast_ws_server.cpp` | Server WebSocket (port 7200) |
| `core/network/maplecast_gamestate.cpp` | Game state reader (253 bytes) |
| `core/network/maplecast_mirror.cpp` | TA capture + delta encoding |
| `docs/WORKSTREAM-KING-OF-MARVEL.md` | Full architecture plan |

---

## CRITICAL WARNINGS

1. **GL_BLEND hack is MANDATORY** — Without the JS-level `glCtx.enable(gl.BLEND)` before each frame, all transparency breaks. This is an emscripten bug, not a flycast bug.

2. **EmulateFramebuffer MUST be false** — Setting it to true causes `writeFramebufferToVRAM()` which corrupts VRAM texture data.

3. **postProcessor.render(0) is called INSIDE renderFrame()** — Do NOT call it again from JS/bridge code. Double-call causes visual artifacts.

4. **Canvas ID must be `game-canvas`** — The emscripten WebGL context is created on this element.

5. **Framebuffer cap** — `getScaledFramebufferSize()` scales by `config::RenderResolution / 480.0`. Values above 1920 can cause OOM. Cap at 1920 max.

6. **SYNC is 8MB** — First binary message after WebSocket connect. Must `_malloc` a temp buffer, copy data, call `_renderer_sync`, then `_free`.

7. **Keyframe detection** — `deltaPayloadSize == taSize` means keyframe (full TA buffer). Client must wait for first keyframe before rendering (returns -10 until then).
