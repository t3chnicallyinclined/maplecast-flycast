# WORKSTREAM: King of Marvel — Full Architecture & Build Plan

**Date:** April 5, 2026
**Branch:** `ta-streaming`
**Mandate:** OVERKILL IS NECESSARY. EVERY MICROSECOND COUNTS.

---

## EXECUTIVE SUMMARY

Build "King of Marvel" — a browser-based MVC2 arcade cabinet where anyone can watch, queue up, and fight. The server runs the game. The browser renders native-quality 3D from raw GPU commands (not video). Players get sub-millisecond direct connections. Spectators fan out via P2P relay.

**Three deliverables:**
1. **Standalone WASM Renderer** — stripped flycast renderer (~35 files, ~3MB WASM), renders TA commands to a canvas via WebGL2. No EmulatorJS, no RetroArch.
2. **React App Shell** — king.html rebuilt as React + Vite + Zustand. Arcade cabinet UI, leaderboard, queue, chat, match HUD.
3. **LiveKit Spectator Fan-out** — players get direct WebRTC DataChannels from server. Spectators stream via LiveKit SFU. Zero change to player latency.

---

## ARCHITECTURE

```
                         ┌──────────────────────┐
                         │   FLYCAST SERVER      │
                         │   (runs MVC2 game)    │
                         │                       │
                         │ TA capture → delta    │
                         │ encode → broadcast    │
                         └─────┬──────┬──────────┘
                               │      │
              Direct WebRTC    │      │  LiveKit Data Publish
              DataChannel      │      │  (lossy binary)
              (unordered,      │      │
               unreliable)     │      ▼
                    │      ┌──────────────┐
              ┌─────┴───┐  │  LIVEKIT SFU  │
              │         │  │  (Go, Pion)   │
              ▼         ▼  └──────┬────────┘
          [Player 1] [Player 2]   │
          (<1ms hop)  (<1ms hop)  │ Fan-out to all spectators
                                  │
                    ┌─────────────┼─────────────┐
                    ▼             ▼              ▼
               [Spec 1]     [Spec 2]  ...  [Spec N]
               (+5-10ms)    (+5-10ms)      (+5-10ms)

Each browser client:
  ┌─────────────────────────────────────────────┐
  │  React App (Vite + Zustand)                 │
  │  ┌────────────────────────────────────────┐ │
  │  │ Arcade Cabinet UI                      │ │
  │  │  ┌──────────────────────────────┐      │ │
  │  │  │ <canvas> WebGL2              │      │ │
  │  │  │  ← flycast-mirror.wasm      │      │ │
  │  │  │  ← renders TA commands       │      │ │
  │  │  │  ← pixel-perfect 3D         │      │ │
  │  │  └──────────────────────────────┘      │ │
  │  │  Match HUD overlay (React)             │ │
  │  │  Health bars, timer, combo counter     │ │
  │  ├────────────────────────────────────────┤ │
  │  │ Sidebar: Hall of Fame / The Line       │ │
  │  │ Bottom: Chat / Reactions               │ │
  │  └────────────────────────────────────────┘ │
  │                                             │
  │  WebSocket (JSON) ←→ lobby/queue/stats      │
  │  WebRTC/LiveKit (binary) ←→ TA frames       │
  └─────────────────────────────────────────────┘
```

---

## TECH STACK

| Layer | Technology | Why |
|-------|-----------|-----|
| Game Server | Flycast (C++, existing) | Already works, runs MVC2 |
| WASM Renderer | Emscripten (C++ → WASM) | Existing flycast GLES renderer, proven in WASM. ~35 source files extracted. |
| UI Framework | React 19 + Vite | Fast HMR, standard ecosystem, clean component model |
| State Mgmt | Zustand (~3KB) | 12ms render latency, simplest API, perfect for real-time game state |
| Styling | CSS Modules or Tailwind | SF2 arcade aesthetic from king.html |
| Player Transport | WebRTC DataChannel (direct) | Already built in `maplecast_webrtc.cpp`. Unreliable/unordered = UDP semantics. Sub-1ms hop. |
| Spectator Transport | LiveKit SFU (self-hosted) | Open source Go server. `publishData()` lossy binary. Fans out to 500+ viewers on one box. |
| Signaling | WebSocket (existing port 7200) | Already handles lobby, queue, input, game state JSON |
| Font | Press Start 2P (Google Fonts) | Pixel arcade aesthetic |
| Build | pnpm workspace monorepo | 2 packages: `renderer` (C++ WASM) + `app` (React) |

---

## PHASE 1: STANDALONE WASM RENDERER (The Hard Part)

### Goal
Extract flycast's GLES renderer into a standalone WASM binary that renders TA commands to a raw `<canvas>`. No EmulatorJS. No RetroArch. No libretro. Direct WebGL2.

### Source Files Required (~35-40 files)

**Bridge (entry point) — 1 file:**
- `core/network/maplecast_wasm_bridge.cpp` (modify for standalone)

**PVR (Tile Accelerator + registers) — 10 files:**
- `core/hw/pvr/pvr_regs.cpp` + `.h`
- `core/hw/pvr/pvr_mem.cpp` + `.h`
- `core/hw/pvr/ta_vtx.cpp` (1700 lines — the TA command parser, THE critical file)
- `core/hw/pvr/ta_ctx.cpp` + `.h`
- `core/hw/pvr/ta_util.cpp`
- `core/hw/pvr/ta_structs.h`
- `core/hw/pvr/ta.h`
- `core/hw/pvr/Renderer_if.h`
- `core/hw/pvr/elan_struct.h`

**GLES Renderer — 10 files:**
- `core/rend/gles/gles.cpp` + `.h` (Process, Render, Present, shader compilation)
- `core/rend/gles/gldraw.cpp` (DrawStrips — the actual draw calls)
- `core/rend/gles/gltex.cpp` (GPU texture upload)
- `core/rend/gles/naomi2.cpp` + `.h` (compiled but unused for DC)
- `core/rend/gles/quad.cpp` + `.h`
- `core/rend/gles/postprocess.cpp` + `.h`
- `core/rend/gles/glcache.h`

**Texture/Rendering Support — 10 files:**
- `core/rend/texconv.cpp` + `.h` (palette_update, texture format converters, detwiddle tables)
- `core/rend/TexCache.cpp` + `.h` (texture cache, VramLockedWriteOffset)
- `core/rend/CustomTexture.cpp` + `.h` (stub to no-op)
- `core/rend/transform_matrix.h`
- `core/rend/tileclip.h`
- `core/rend/osd.cpp` + `.h`
- `core/rend/sorter.cpp` + `.h`
- `core/rend/shader_util.h`

**Core Infrastructure — 6 files:**
- `core/types.h`
- `core/stdclass.cpp` + `.h` (RamRegion)
- `core/emulator.h` (EventManager)
- `core/oslib/oslib.h`
- `core/cfg/option.h`

**WSI (GL context) — 4 files:**
- `core/wsi/context.h`
- `core/wsi/gl_context.cpp` + `.h`
- New: `wasm_gl_context.cpp` (emscripten WebGL2 context creation)

**Dependencies:**
- xxHash (`core/deps/xxHash/`)
- xBRZ (`core/deps/xbrz/xbrz.cpp`)
- GLM (header-only, `core/deps/glm/`)

**New file: `stubs.cpp`** (~25 no-op stubs):
- `rend_start_render()`, `CalculateSync()`, `rescheduleSPG()` — no-op
- `ta_vtx_ListInit()`, `ta_vtx_SoftReset()` — no-op
- `addrspace::protectVram/unprotectVram/getVramOffset` — no-op / return -1
- `bm_LockPage/bm_UnlockPage` — no-op
- `holly_intc` functions — no-op
- `settings` static instance: `vram_size=8MB, system=DC_PLATFORM_DREAMCAST`
- `config::*` options with hardcoded optimal defaults
- `mirror_present_frame()` — calls into JS to present the canvas

### Emscripten Build Flags
```
-s MODULARIZE=1
-s EXPORT_ES6=1
-s EXPORTED_FUNCTIONS=['_renderer_init','_renderer_frame','_renderer_sync','_renderer_resize','_malloc','_free']
-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8']
-s ALLOW_MEMORY_GROWTH=1
-s INITIAL_MEMORY=33554432        # 32MB (8MB VRAM + 32KB PVR + TA buffers + GL state)
-s MAXIMUM_MEMORY=67108864        # 64MB cap
-s USE_WEBGL2=1
-s FULL_ES3=1
-s MIN_WEBGL_VERSION=2
-s MAX_WEBGL_VERSION=2
-s NO_EXIT_RUNTIME=1
-s ENVIRONMENT='web'
-s FILESYSTEM=0                   # No virtual filesystem needed
-O3                               # Maximum optimization
-flto                             # Link-time optimization
```

### Exported API (C → JS)
```c
extern "C" {
    // Initialize renderer, create WebGL2 context on the provided canvas
    EMSCRIPTEN_KEEPALIVE int renderer_init(int width, int height);

    // Apply initial SYNC (8MB VRAM + 32KB PVR regs)
    EMSCRIPTEN_KEEPALIVE int renderer_sync(uint8_t* data, int size);

    // Decode delta frame and render immediately (push-driven, no rAF loop)
    EMSCRIPTEN_KEEPALIVE int renderer_frame(uint8_t* data, int size);

    // Handle canvas resize
    EMSCRIPTEN_KEEPALIVE void renderer_resize(int width, int height);
}
```

### Critical Bug Fix
`FillBGP()` (background polygon) is called on the server but NOT on the client.
The client has all required data (VRAM + PVR regs). Must add `FillBGP(&clientCtx)`
before `renderer->Process()` in the WASM bridge. Without this, background plane
renders incorrectly.

### Frame Rendering Path (Latency-Critical)
```
WebSocket/DataChannel binary message arrives
  → JS: new Uint8Array(event.data)                    // zero-copy view
  → JS: Module.HEAPU8.set(bytes, bufPtr)              // ONE memcpy into WASM heap
  → WASM: renderer_frame(bufPtr, size)                 // synchronous call
    → Decode TA delta (apply runs to previous buffer)  // ~0.1ms
    → Patch VRAM pages + VramLockedWriteOffset()       // ~0.05ms
    → Patch PVR register pages                         // ~0.01ms
    → palette_update()                                 // ~0.01ms
    → renderer->Process(&ctx)                          // ~0.3ms (TA parse + texture resolve)
    → renderer->Render()                               // ~0.5ms (WebGL2 draw calls)
    → renderer->Present()                              //
  ← JS: canvas shows the frame on next vsync
```

Total: **~1ms from WebSocket message to rendered frame.** Push-driven — no requestAnimationFrame polling loop adding 0-16ms of latency.

---

## PHASE 2: REACT APP SHELL

### Project Structure
```
king-of-marvel/
  pnpm-workspace.yaml
  package.json
  packages/
    renderer/                         # C++ WASM renderer
      CMakeLists.txt
      build.sh                        # emcc build script
      src/
        wasm_bridge.cpp               # Modified maplecast_wasm_bridge.cpp
        wasm_gl_context.cpp           # Emscripten WebGL2 context
        stubs.cpp                     # ~25 no-op stubs
        # Symlinks or copies of flycast source files
      dist/
        renderer.mjs                  # Built ES module
        renderer.wasm                 # Built WASM binary

    app/                              # React UI shell
      package.json
      vite.config.ts
      index.html
      src/
        main.tsx
        App.tsx
        components/
          Cabinet.tsx                 # Arcade cabinet frame + CRT effects
          GameCanvas.tsx              # <canvas> + WASM mount via useRef
          MatchHUD.tsx                # Health bars, timer, combo (overlaid on canvas)
          HallOfFame.tsx              # Leaderboard tabs (streak/combo/speed/perfect/masher/surgeon)
          TheLine.tsx                 # Queue: NOW PLAYING + I GOT NEXT + queue list
          NowPlaying.tsx              # P1 vs P2 display with records
          Chat.tsx                    # Trash talk + reaction buttons
          Ticker.tsx                  # Scrolling match highlights
          MatchResults.tsx            # Post-match overlay with stats
          NewChallenger.tsx           # "HERE COMES A NEW CHALLENGER" fullscreen overlay
          SignIn.tsx                  # Modal: fighter name + avatar picker
          KingBanner.tsx              # Top banner: current king + streak
          ViewerBar.tsx               # Watching / Playing / In Line counts
        hooks/
          useRenderer.ts              # WASM lifecycle: init, sync, frame, resize, destroy
          useWebSocket.ts             # WS connection: demux binary → WASM, JSON → Zustand
          useGameState.ts             # Zustand selector hook for match state
          useGamepad.ts               # Browser Gamepad API → binary input packets
          useQueue.ts                 # Queue join/leave/position
        stores/
          gameStore.ts                # Zustand store: lobby, match, queue, leaderboard, chat
        lib/
          wasmLoader.ts               # Emscripten module init + memory management
          frameRouter.ts              # Route binary frames to WASM, route JSON to store
          inputEncoder.ts             # Encode gamepad state to 4-byte binary packets
        styles/
          arcade.css                  # SF2 aesthetic: gold gradients, CRT scanlines, pixel font
          cabinet.css                 # Cabinet bezel, screen, control panel
          animations.css              # Combo shake, king pulse, challenger flash, blink-coin
      public/
        wasm/                         # Built WASM artifacts
          renderer.mjs
          renderer.wasm
```

### Vite Config
```ts
// vite.config.ts
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import wasm from 'vite-plugin-wasm';
import topLevelAwait from 'vite-plugin-top-level-await';

export default defineConfig({
  plugins: [react(), wasm(), topLevelAwait()],
  server: {
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
    proxy: {
      '/ws': {
        target: 'ws://localhost:7200',
        ws: true,
      },
    },
  },
});
```

### Core Hook: useRenderer.ts
```ts
import { useEffect, useRef } from 'react';
import createRenderer from '/wasm/renderer.mjs';

export function useRenderer(canvasRef: React.RefObject<HTMLCanvasElement>) {
  const moduleRef = useRef<any>(null);
  const bufPtr = useRef<number>(0);
  const MAX_FRAME = 512 * 1024; // 512KB max frame

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    createRenderer({ canvas }).then(mod => {
      moduleRef.current = mod;
      bufPtr.current = mod._malloc(MAX_FRAME);
      mod._renderer_init(canvas.width, canvas.height);
    });

    return () => {
      if (moduleRef.current && bufPtr.current) {
        moduleRef.current._free(bufPtr.current);
      }
    };
  }, []);

  // Push-driven: call these from WebSocket onmessage
  const sync = (data: Uint8Array) => {
    const mod = moduleRef.current;
    if (!mod) return;
    // Sync can be >512KB, realloc if needed
    const syncBuf = mod._malloc(data.length);
    mod.HEAPU8.set(data, syncBuf);
    mod._renderer_sync(syncBuf, data.length);
    mod._free(syncBuf);
  };

  const frame = (data: Uint8Array) => {
    const mod = moduleRef.current;
    if (!mod || !bufPtr.current) return;
    mod.HEAPU8.set(data, bufPtr.current); // ONE copy
    mod._renderer_frame(bufPtr.current, data.length);
  };

  return { sync, frame };
}
```

### Core Hook: useWebSocket.ts
```ts
// Demux: binary → WASM renderer, text → Zustand store
ws.binaryType = 'arraybuffer';
ws.onmessage = (e) => {
  if (e.data instanceof ArrayBuffer) {
    const view = new Uint8Array(e.data);
    // SYNC starts with 'S','Y','N','C' (0x53, 0x59, 0x4E, 0x43)
    if (view[0] === 0x53 && view[1] === 0x59 && view[2] === 0x4E && view[3] === 0x43) {
      renderer.sync(view);
    } else {
      renderer.frame(view);
    }
  } else {
    // JSON → Zustand
    const msg = JSON.parse(e.data);
    store.getState().handleMessage(msg);
  }
};
```

### Zustand Store
```ts
import { create } from 'zustand';

interface GameStore {
  // King
  kingName: string;
  kingStreak: number;

  // Match
  matchActive: boolean;
  p1: PlayerState;
  p2: PlayerState;
  timer: number;
  combo: { player: number; hits: number } | null;

  // Queue
  queue: QueueEntry[];
  myPosition: number; // -1 = not in queue

  // Leaderboard
  leaderboard: Record<string, LeaderboardEntry[]>;
  activeTab: string;

  // Chat
  messages: ChatMessage[];

  // Viewers
  watching: number;
  playing: number;
  inLine: number;

  // Actions
  handleMessage: (msg: any) => void;
}
```

---

## PHASE 3: LIVEKIT SPECTATOR FAN-OUT

### Architecture
- **Players (2):** Direct WebRTC DataChannel from flycast server. Existing code, zero change. Sub-1ms added latency.
- **Spectators (10-500+):** LiveKit SFU relay. Server publishes binary frames via LiveKit Go SDK. Spectators subscribe via `livekit-client` JS SDK. +5-10ms latency.

### Server Side (Go sidecar process)
```go
// Runs alongside flycast, receives frames via shared memory or local socket
room, _ := lksdk.ConnectToRoom(lkURL, lksdk.ConnectInfo{
    APIKey: apiKey, APISecret: apiSecret,
    RoomName: "king-of-marvel",
    ParticipantIdentity: "flycast-server",
})

// Publish each frame as lossy data packet (unreliable = low latency)
func publishFrame(frameData []byte) {
    room.LocalParticipant.PublishDataPacket(lksdk.DataPacket{
        Data: frameData,
        Kind: lksdk.DataPacket_LOSSY,
    })
}
```

### Browser Side (spectator)
```ts
import { Room, RoomEvent } from 'livekit-client';

const room = new Room();
await room.connect(lkURL, spectatorToken);

room.on(RoomEvent.DataReceived, (payload: Uint8Array) => {
  renderer.frame(payload); // Same WASM renderer, different transport
});
```

### Frame Chunking
LiveKit lossy mode recommends sub-1300B packets (MTU). A 60KB frame = ~46 chunks.
Header per chunk: `[2B seq][2B chunk_idx][2B total_chunks][1294B payload]`.
Reassemble on client before passing to WASM. At 60fps = 2760 packets/sec. LiveKit handles this.

### Signaling Flow
1. Browser connects to existing WebSocket (port 7200) for lobby/queue/chat
2. WebSocket server detects player vs spectator:
   - **Player (slot 0 or 1):** Server creates direct WebRTC PeerConnection (existing code)
   - **Spectator (slot -1):** Server returns a LiveKit room token in the `assigned` message
3. Browser uses the token to join the LiveKit room and subscribe to data
4. Same WASM renderer, different binary frame source

### Infrastructure
- Self-hosted LiveKit on the same server or a $20/month VPS
- 100 spectators × 2MB/s = 200MB/s = ~50TB/month
- Hetzner dedicated: ~$40/month with 20TB included
- Scale: one LiveKit server handles ~500 DataChannel-only subscribers

### Phase 3b: P2P Tree Relay (Future, 1000+ spectators)
When we outgrow SFU bandwidth:
- Promote stable spectators to "relay nodes"
- Each relay opens 2-3 WebRTC DataChannels to downstream peers (PeerJS)
- Coordinator tracks tree topology, rebalances on churn
- SFU latency + 1 P2P hop (~20-50ms) = still under 200ms for spectators

---

## PHASE 4: GAME STATE + STATS + LEADERBOARD

### RAM Reader (Already Built)
Restore `maplecast_gamestate.cpp` from git (commit 0fb28c476). Reads MVC2 RAM every frame:

| Address | Data | Size |
|---------|------|------|
| 0x8C268340 + stride×N + 0x420 | Health (6 chars) | 24 bytes |
| 0x8C268340 + stride×N + 0x424 | Red health (6 chars) | 24 bytes |
| 0x8C289670 / 0x8C289672 | Combo counter P1/P2 | 4 bytes |
| 0x8C28964A / 0x8C28964B | Meter level P1/P2 | 2 bytes |
| 0x8C289630 | Timer | 4 bytes |
| 0x8C289624 | In-match flag | 4 bytes |
| Base + 0x001 | Character ID (6 chars) | 6 bytes |

**Total: 253 bytes per frame. Already proven at 60fps with zero overhead.**

### Stats Tracked Per Match
- **Highest combo** — max of combo counter per player
- **Fastest win** — timer delta from match start to KO
- **Perfects** — opponent all 3 chars KO'd while your active char at max health
- **Comebacks** — won from below 20% health on active char
- **Input rate** — button state changes per second (from input packets, already counted)
- **Input accuracy** — ratio of intentional inputs to total inputs (low jitter = surgical)
- **Mash rate** — raw inputs per second (the chaos metric)

### Stats Tracked Per Session
- Win streak (current + best ever)
- Total wins/losses
- Character usage
- Time on cabinet (reign duration)

### Leaderboard Categories
| Tab | Stat | Player Archetype |
|-----|------|-----------------|
| STREAK | Most consecutive wins | The King |
| COMBO | Highest single combo | The Styler |
| SPEED | Fastest match win | The Executioner |
| PERFECT | Most perfect rounds | The Wall |
| MASHER | Highest inputs/sec with positive W/L | The Gremlin |
| SURGEON | Highest input accuracy | The Surgeon |

### Persistence
- Server-side: JSON file or SQLite for leaderboard persistence
- Client-side: localStorage for session identity (UUID + name + avatar)
- Future: PostgreSQL or Redis for multi-node

---

## BUILD ORDER (What Gets Done When)

### Sprint 1: Standalone WASM Renderer
1. Create `packages/renderer/` directory structure
2. Copy/symlink the ~35 flycast source files
3. Write `stubs.cpp` (~25 no-op functions)
4. Write `wasm_gl_context.cpp` (emscripten WebGL2 context creation)
5. Modify `wasm_bridge.cpp` for standalone (no libretro, add `FillBGP()` fix)
6. Write `CMakeLists.txt` for emscripten build
7. Build and test: load in a simple HTML page, connect to existing flycast server
8. **Success criteria:** MVC2 renders in a raw `<canvas>` at 60fps with no EmulatorJS

### Sprint 2: React App Shell
1. Scaffold Vite + React + TypeScript project in `packages/app/`
2. Port king.html CSS/layout into React components
3. Write Zustand store with game state types
4. Write `useRenderer` hook (WASM lifecycle)
5. Write `useWebSocket` hook (binary/JSON demux)
6. Wire up `GameCanvas.tsx` → WASM renderer → WebSocket binary frames
7. Wire up HUD components → Zustand store → WebSocket JSON status
8. **Success criteria:** Full arcade UI with live game stream, same as king.html but in React

### Sprint 3: Game State + Stats
1. Restore `maplecast_gamestate.cpp` from git
2. Wire game state into the WebSocket status JSON (already spec'd in handoff doc)
3. React components consume game state: animated health bars, combo counter, timer
4. Server-side match tracking: detect win/loss, accumulate stats
5. Input stat accumulator: mash rate, accuracy, efficiency
6. Leaderboard persistence (JSON file → SQLite)
7. **Success criteria:** Live match stats, post-match results overlay, working leaderboard

### Sprint 4: LiveKit Spectator Fan-out
1. Deploy self-hosted LiveKit server (Docker or binary)
2. Write Go sidecar: receives frames from flycast, publishes to LiveKit
3. Modify WebSocket signaling: players get direct WebRTC, spectators get LiveKit token
4. Browser: add LiveKit client path alongside direct WebRTC
5. Frame chunking/reassembly for lossy DataChannel
6. **Success criteria:** 50+ spectators streaming simultaneously, players unaffected

### Sprint 5: Queue System + Arcade Mode
1. Server-side queue management (already partially built)
2. Winner-stays-on logic: match end → loser kicked → next in queue promoted
3. Attract mode: when nobody's playing, server runs MVC2 attract screen
4. "I GOT NEXT" → queue up → auto-promote to P1/P2 when slot opens
5. Idle timeout: 60s no input → kicked to queue
6. **Success criteria:** Full arcade cabinet experience, anyone can walk up and play

---

## LATENCY BUDGET (Target: < 3ms server-to-pixel for players)

| Stage | Player | Spectator |
|-------|--------|-----------|
| Server TA capture | 0.1ms | 0.1ms |
| Delta encode | 0.2ms | 0.2ms |
| Network (WebRTC DC) | 0.1ms (LAN) | — |
| Network (LiveKit SFU) | — | 5-10ms |
| JS receive + memcpy to WASM | 0.05ms | 0.05ms |
| Delta decode | 0.1ms | 0.1ms |
| VRAM/PVR patch | 0.05ms | 0.05ms |
| palette_update() | 0.01ms | 0.01ms |
| renderer->Process() (TA parse) | 0.3ms | 0.3ms |
| renderer->Render() (WebGL2) | 0.5ms | 0.5ms |
| **TOTAL** | **~1.4ms** | **~11.3ms** |

Players: **sub-2ms on LAN.** Spectators: **~12ms.** Both pixel-perfect native 3D.

---

## NOT DOING (Explicitly Scoped Out)

- **Rust/wgpu port** — 3-6 months, save for v2. The C++ GLES renderer works in WASM today.
- **WebTransport** — server-only, no P2P. Doesn't solve spectator fan-out.
- **Pure P2P mesh/tree** — collapses under churn. SFU is battle-tested.
- **simple-peer, Bugout, Trystero** — dead/unmaintained/can't scale.
- **WebTorrent** — designed for file chunks, not 60fps real-time.
- **Custom video codec** — we're streaming GPU commands, not video. No transcoding ever.

---

## KEY RISKS & MITIGATIONS

| Risk | Impact | Mitigation |
|------|--------|------------|
| Standalone WASM build fails (missing deps) | Blocks everything | Dependency analysis complete — 35 files + 25 stubs identified. Build incrementally. |
| WebGL2 shader compilation issues in WASM | Black screen | Flycast GLES shaders already proven in nasomers/flycast-wasm. WebGL2 patches documented in handoff. |
| LiveKit frame chunking overhead | Spectator latency | Chunk to 1294B, reassemble client-side. Test at 60fps. Fallback: increase MTU or use reliable mode for keyframes. |
| VRAM dirty page tracking in WASM | Stale textures | memwatch stubs in WASM disable mprotect (not available). Server sends diffs, client applies with VramLockedWriteOffset(). Already working. |
| FillBGP() missing on client | Wrong background | Bug identified in research. Fix: add FillBGP() call before renderer->Process() in bridge. |

---

## WHAT MAKES THIS INSANE

1. **Not streaming video.** Streaming raw GPU commands. 253 bytes of game state + TA commands = pixel-perfect native 3D in a browser. No artifacts, no compression, no quality loss.

2. **The renderer runs on YOUR GPU.** Server sends the instructions, your browser's WebGL2 executes them. 4K resolution costs the server nothing extra.

3. **Sub-2ms player latency.** Direct WebRTC DataChannel, unreliable/unordered, push-rendered on arrival. No rAF polling. No buffering. Input → screen in under 2ms on LAN.

4. **Self-healing.** Keyframe every 60 frames (1 second). Miss a delta? Wait 1 second and you're fully synced. Checksums catch drift.

5. **1-5 MB/s bandwidth** vs 20-50 MB/s for video streaming at the same quality. 10-50x more efficient.

6. **The arcade cabinet IS the website.** You don't watch a stream — you stand at the cabinet. You see the game. You put your quarter up. Winner stays on.
