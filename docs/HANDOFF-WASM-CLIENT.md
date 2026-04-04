# HANDOFF: WASM Mirror Client — Build the RENDERER, Not the Emulator

**Date:** April 4, 2026
**Branch:** `ta-streaming`
**Status:** BLOCKED — previous agent built the full emulator to WASM instead of just the mirror client renderer. Need to build ONLY the renderer.

---

## CRITICAL MISTAKE TO AVOID

The previous agent built the FULL flycast emulator (SH4 CPU + renderer + everything) as WASM. This is WRONG. The whole point of the mirror client is that **the CPU is stopped and only the renderer runs**. Building the full emulator to WASM:
- Wastes 5.9 MB (renderer alone would be ~2 MB)
- Includes SH4 CPU code we never use
- Includes disc loader, sound chip, maple bus — all unnecessary
- Made the build complex with server-side code conflicts

**What we actually need:** A WASM binary that ONLY contains:
- flycast's OpenGL renderer (`gles.cpp`, `gldraw.cpp`, `gltex.cpp`)
- `ta_parse()` — converts TA commands into renderable geometry
- Texture cache (`TexCache.cpp`) — decodes textures from VRAM
- `palette_update()` — critical for character sprite rendering
- Memory to hold RAM/VRAM/ARAM/PVR regs
- A C function `mirror_render_frame(data, size)` that JS calls

That's it. No SH4. No disc. No sound. No maple bus. No network.

---

## WHAT WORKS RIGHT NOW

### Native Mirror Mode (PROVEN, STABLE)
```
Server flycast: plays MVC2, streams delta-encoded TA commands
Client flycast: CPU stopped, receives TA commands, renders via flycast's real renderer
Result: pixel-perfect MVC2 with characters, HUD, stage — stable 80+ seconds
```

### Delta Encoding (PROVEN)
- TA commands: ~140 KB/frame raw → 15-40 KB/frame delta (10% change rate)
- Keyframes every 60 frames for self-healing
- Checksum verification catches corruption
- Total: ~4.1 MB/s to browser via WebSocket

### Browser Transport (PROVEN)
- delta-test.html receives 60fps delta frames over WebSocket
- Frame types identified (delta vs keyframe)
- H.264 encode disabled when mirror active (saves 3.5ms GPU)

### The Working Native Mirror Client Does This:
```cpp
// For each frame received from server:
1. Apply memory diffs (VRAM pages, PVR regs)
2. pal_needs_update = true; palette_update();  // CRITICAL
3. renderer->updatePalette = true;
4. renderer->updateFogTable = true;
5. Copy TA commands into TA_context buffer
6. Set PVR register values on rend_context
7. renderer->Process(&ctx);  // calls ta_parse, resolves textures
8. renderer->Render();       // draws via OpenGL
9. renderer->Present();      // shows frame
```

---

## WHAT NEEDS TO BE BUILT

### Option A: Minimal WASM Renderer (RECOMMENDED)
Build a small WASM module containing ONLY the renderer code. Export `mirror_render_frame()` for JS to call.

Files needed:
```
core/rend/gles/gles.cpp        — OpenGL renderer
core/rend/gles/gldraw.cpp      — draw calls
core/rend/gles/gltex.cpp       — texture decode + cache
core/rend/gles/glcache.h       — GL state cache
core/rend/TexCache.cpp          — texture cache base
core/rend/texconv.cpp           — palette_update(), texture conversion
core/rend/transform_matrix.h    — coordinate transforms
core/rend/sorter.cpp            — triangle sorting
core/hw/pvr/ta_ctx.h/cpp        — TA_context, rend_context structs
core/hw/pvr/ta_vtx.cpp          — ta_parse() 
core/hw/pvr/ta_structs.h        — PolyParam, Vertex structs
core/hw/pvr/pvr_regs.h          — PVR register definitions
core/hw/pvr/Renderer_if.h       — Renderer base class
core/network/maplecast_wasm_bridge.cpp — the entry point
```

### Option B: Use Existing Full WASM Build
The 5.9 MB build at `~/projects/flycast-wasm/flycast_libretro_upstream.wasm` works. It's the full emulator. You could:
1. Load ROM + save state normally
2. Pause CPU via `_toggleMainLoop(0)`
3. Write TA data into WASM memory via JS
4. Trigger render

This avoids the build complexity but wastes resources running unused code.

### The Bridge File (maplecast_wasm_bridge.cpp)
Already written at `core/network/maplecast_wasm_bridge.cpp`. It has:
- `mirror_init()` — initialize renderer
- `mirror_render_frame(data, size)` — decode delta frame + render
- `mirror_get_frame_count()` — sync helper

It FAILED TO COMPILE because the include chain pulls in server-side headers (`maplecast_stream.h`, `cuda.h`) that don't exist in the WASM build. 

**Fix:** Either:
1. Guard the includes with `#ifndef __EMSCRIPTEN__`
2. Create a standalone renderer module that doesn't include the full flycast header chain
3. Build a separate CMakeLists for WASM that only compiles renderer files

---

## WASM BUILD SETUP

```bash
# Emscripten SDK
source ~/emsdk/emsdk_env.sh

# WASM source (upstream flycast with our files copied in)
~/projects/flycast-wasm/upstream/source.orig/

# Our files copied there:
core/network/maplecast_*.cpp/h  — all our modules
core/network/CMakeLists.txt     — modified to include our files

# Build:
cd ~/projects/flycast-wasm/upstream/source/build-wasm
emcmake cmake .. -DLIBRETRO=ON -DCMAKE_BUILD_TYPE=Release
emmake make -j$(nproc)

# Link:
cd ~/projects/flycast-wasm
bash upstream/link-ubuntu.sh

# Output:
flycast_libretro_upstream.wasm (5.9 MB)
flycast_libretro_upstream.js (290 KB)
```

### Link Script Exports (already added):
```
"_mirror_init","_mirror_render_frame","_mirror_get_frame_count"
```

---

## BROWSER CLIENT FLOW

```javascript
// 1. Load WASM
const Module = await loadWasm('flycast_libretro_upstream.wasm');

// 2. Initialize mirror renderer
Module._mirror_init();

// 3. Connect WebSocket to server
const ws = new WebSocket('ws://server:7200');
ws.binaryType = 'arraybuffer';

// 4. For each delta frame:
ws.onmessage = (event) => {
  if (event.data instanceof ArrayBuffer) {
    const data = new Uint8Array(event.data);
    const ptr = Module._malloc(data.length);
    Module.HEAPU8.set(data, ptr);
    Module._mirror_render_frame(ptr, data.length);
    Module._free(ptr);
  }
};
```

---

## DELTA FRAME FORMAT

Each binary frame sent over WebSocket:
```
[0-3]    frameSize (u32 LE) — total payload size minus 4
[4-7]    frameNum (u32 LE) — sequential frame number
[8-71]   pvr_snapshot (16 × u32) — PVR register values
[72-75]  taOrigSize (u32) — original TA buffer size
[76-79]  taDeltaPayloadSize (u32) — delta payload size
         if taDeltaPayloadSize == taOrigSize: full keyframe (raw TA data)
         if taDeltaPayloadSize < taOrigSize: delta runs
[80+]    delta data:
         if full: raw TA bytes (taOrigSize)
         if delta: [offset(u32) + len(u16) + data(len)] × N, terminated by 0xFFFFFFFF
[after delta] checksum (u32) — XOR of all TA 4-byte words
[after checksum] dirtyPages (u32) — number of memory page diffs
         [regionId(u8) + pageIdx(u32) + pageData(4096)] × dirtyPages
         regionId: 1=VRAM, 3=PVR regs
```

---

## CRITICAL TECHNICAL DETAILS

### palette_update() MUST be called
MVC2 uses paletted textures. Without `palette_update()` (texconv.cpp:81), character sprites are invisible. This converts PALETTE_RAM → palette32_ram.

### Renderer flags MUST be set
```cpp
renderer->updatePalette = true;
renderer->updateFogTable = true;
pal_needs_update = true;
```

### Keyframe every 60 frames
Server sends full TA (not delta) every 60 frames. Client must wait for first full frame before applying deltas.

### Server command to run:
```bash
MAPLECAST_MIRROR_SERVER=1 MAPLECAST_JPEG=95 bash ~/projects/maplecast-flycast/start_maplecast.sh
```

---

## FILES

| File | Purpose |
|------|---------|
| `core/network/maplecast_mirror.cpp` | Server: delta encode + shared memory + WebSocket broadcast |
| `core/network/maplecast_wasm_bridge.cpp` | WASM entry point (needs compile fix) |
| `core/network/maplecast_stream.cpp` | WebSocket server + broadcastBinary() |
| `web/delta-test.html` | Browser test: receives + logs delta frames |
| `web/mirror-client.html` | Browser framework for WASM renderer |
| `docs/HANDOFF-MIRROR.md` | Full mirror mode technical reference |
| `docs/MVC2-MEMORY-MAP.md` | Every dirty page cataloged |
| `docs/STREAMING-OPTIONS.md` | All 7 architecture options |

## GIT HISTORY (ta-streaming branch)
```
9fa9236 Session handoff
447acfb WASM bridge + WebSocket delta transport
a056457 Skip H.264 when mirror active — 4.1 MB/s delta only
b9ff1e7 Delta frames streaming to browser — TRANSPORT PROVEN  
3ce77e7 Delta encoding STABLE: keyframes + checksums
7656724 DELTA ENCODING WORKING: 3-16 KB/frame
4e10dab Stripped mirror: VRAM+PVR only, 182 KB/frame stable
853825a Characters rendering! Palette fix
56d427c Two-instance mirror nearly working
```
