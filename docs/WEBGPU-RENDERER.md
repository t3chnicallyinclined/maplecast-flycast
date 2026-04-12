# WebGPU Renderer — Architecture & Reference

Pure JavaScript + WebGPU renderer for the MapleCast TA mirror stream.
Zero WASM, zero compile step, zero build toolchain. Edit a `.mjs` file,
refresh the browser, see the change. Live at `nobd.net/webgpu-test.html`.

---

## 1. Architecture Overview

### Design principles
- **Zero WASM** — all decode, parse, and render logic is plain JS ES modules.
  No Emscripten, no C++, no compile step. The WASM renderer (`renderer.wasm`,
  831 KB) still exists in `king.html` as the production path; this renderer is
  the next-generation replacement.
- **Vsync-decoupled pipeline** — network receive and frame decode/parse are
  decoupled from the GPU render loop. `WS.onmessage` (or WebTransport datagram
  arrival) triggers decode + parse immediately. `requestAnimationFrame` picks up
  the latest parsed geometry and renders it. This means decode never blocks
  render and render never blocks decode.
- **Dirty-page-aware** — textures are only re-decoded when the VRAM pages they
  overlap actually changed in the delta frame. Steady-state re-decodes: **0**.

### Module map

```
web/webgpu-test.html          Entry point, debug panel, gamepad viz
web/webgpu/
  transport.mjs               Adaptive transport (WebTransport → WS fallback)
  frame-decoder.mjs           ZCST decompress, SYNC/FSYN/delta frame apply
  ta-parser.mjs               PVR2 TA command stream → vertex/index/poly lists
  texture-manager.mjs          Texture decode + GPUTexture cache (dirty-page aware)
  pvr2-renderer.mjs           WebGPU render pipeline, state batching, draw dispatch
  shaders.mjs                 WGSL vertex + fragment shaders
  fzstd.mjs                   zstd decompressor (JS port of fzstd)
  decode-worker.mjs           Worker thread: decode + parse off main thread
  render-worker.mjs           Worker thread: owns OffscreenCanvas + full pipeline
```

### Data flow

```
Network (WebTransport datagrams or WebSocket binary)
  │
  ▼
transport.mjs onframe(Uint8Array)
  │
  ▼
frame-decoder.mjs
  ├─ _decompress(): ZCST magic → fzstd.mjs zstd decompress
  ├─ applySync(): SYNC/FSYN → memcpy VRAM (8MB) + PVR regs (32KB)
  └─ applyFrame(): delta decode TA vs prevTA, apply dirty VRAM/PVR pages
  │
  ▼
ta-parser.mjs parse(taBuffer, taSize)
  ├─ Walk PCW/ISP/TSP/TCW parameter words
  ├─ Emit 28-byte vertices (pos, color, specular, UV)
  ├─ Build poly lists: opaque[], punchthrough[], translucent[]
  ├─ fillBGP(): ISP_BACKGND_T register → background polygon
  └─ Return { vertexData, indexData, opaque, punch, trans, renderPasses }
  │
  ▼
texture-manager.mjs
  ├─ getOrCreate(TCW, TSP, vram, pvrRegs, dirtyPages)
  ├─ Decode only if texture's VRAM pages overlap dirty set
  └─ Return GPUTexture bind group
  │
  ▼
pvr2-renderer.mjs render(geometry, texMgr, frameDecoder)
  ├─ Upload vertex + index buffers (double-buffered)
  ├─ Fill dynamic uniform buffer (256B-aligned slots per poly)
  ├─ Per-poly: get/create pipeline, set bind groups, drawIndexed
  └─ State batching: skip redundant setPipeline/setBindGroup
  │
  ▼
Canvas (640x480, WebGPU)
```

---

## 2. WebTransport (QUIC/UDP)

### Adaptive transport

The renderer tries WebTransport first, falls back to WebSocket automatically.
This is implemented in `transport.mjs` via the `AdaptiveTransport` class.

```
Browser
  │
  ├─ 1. Try WebTransport (QUIC/UDP)
  │     new WebTransport('https://nobd.net/webtransport')
  │     3-second timeout
  │     If .ready resolves → use QUIC
  │
  ├─ 2. Fallback: WebSocket (TCP)
  │     new WebSocket('wss://nobd.net/ws')
  │     Standard TCP path (same as king.html)
  │
  ▼
  Unified onframe(Uint8Array) callback
  (caller doesn't know or care which transport)
```

### Frame routing by transport type

| Frame type | WebTransport | WebSocket |
|-----------|-------------|-----------|
| Delta frames (~6-15 KB) | Unreliable datagrams | Binary message |
| SYNC packets (~0.6 MB) | Reliable unidirectional stream | Binary message |
| Control/join/leave | Reliable bidirectional | Text message |

Unreliable datagrams for deltas means: if a delta is lost, the next keyframe
(every 60 frames, ~1 second) corrects it. No head-of-line blocking, no TCP
retransmit stalls. This matches the TA stream's design — deltas are disposable,
keyframes are self-contained.

### Relay dual-listener architecture

The Rust relay (`relay/`) runs two listeners on the VPS:

```
Relay (Rust, wtransport crate)
  ├─ UDP :443 (QUIC) ← WebTransport connections
  │     TLS via same Let's Encrypt cert as nginx
  │     Unreliable datagrams for delta fan-out
  │     Reliable streams for SYNC delivery
  │
  └─ TCP :7201 (WebSocket) ← legacy WS connections
       nginx /ws reverse proxy (wss://nobd.net/ws)
       Same frame data, TCP-wrapped
```

Both listeners connect upstream to `ws://127.0.0.1:7210` (flycast headless,
loopback only) and fan out the same compressed ZCST frames. The relay does
NOT re-encode — original compressed bytes are forwarded verbatim.

### Relay forwarding rule

The relay MUST NOT forward `join`/`leave` control messages to the upstream
flycast server. The relay manages its own client roster; forwarding these
causes slot conflicts where the relay and flycast disagree on who is
connected. Only gamepad input and queue commands are forwarded upstream.

### Measured performance

| Metric | WebTransport (QUIC) | WebSocket (TCP) | Improvement |
|--------|--------------------|-----------------| ------------|
| RTT | **45.6 ms** | 72 ms | 37% faster |
| True E2E (RTT/2 + process) | **24.7 ms** | ~38 ms | 35% faster |
| Lost deltas | ~0.1% | 0% | Acceptable |
| Recovery | Next keyframe (1s max) | N/A | — |

The RTT improvement comes from eliminating TCP's head-of-line blocking and
retransmission delays. For a fighting game where every frame matters, 26ms
saved is 1.5 fewer frames of lag.

---

## 3. Performance

### Per-frame budget

Total budget at 60fps: **16.67 ms**. The WebGPU renderer uses **1.88 ms**
(11% of budget).

```
Decode:  0.29 ms   (ZCST decompress + delta apply + dirty pages)
Parse:   0.19 ms   (TA command walk + vertex emit + index build)
Render:  1.39 ms   (GPU pipeline setup + draw calls + present)
─────────────────
Total:   1.88 ms   (11% of 16.67ms budget)
```

### Optimization history

```
5.00 ms → 2.57 ms   dirty-page texture cache        -49%
2.57 ms → 2.29 ms   state batching (skip redundant)  -11%
2.29 ms → 2.06 ms   pre-allocated buffers            -10%
2.06 ms → 1.88 ms   WebTransport + vsync decouple    -9%
                                          Total:      -62%
```

### Key optimizations

- **Dirty-page texture cache**: Textures are keyed by (TCW, TSP). On each
  frame, compute which VRAM pages are dirty from the delta. Only re-decode
  textures whose page range overlaps the dirty set. Steady-state cache misses
  across 24,000 frames: **0**.

- **State batching**: Track current pipeline, bind group 0, bind group 1.
  Skip redundant `setPipeline()` and `setBindGroup()` calls. MVC2 has many
  consecutive polys sharing the same blend/depth/texture state.

- **Double-buffered vertex/index uploads**: Alternate between two GPU buffers
  each frame. Avoids write-after-read stalls where the GPU is still reading
  last frame's data while the CPU wants to write new data.

- **Pre-allocated staging buffer**: Fragment uniforms use a pre-allocated
  `ArrayBuffer` (256B * 8192 slots = 2MB) with a `DataView` for writes.
  No per-frame allocation. The staging buffer is uploaded to the GPU
  dynamic uniform buffer in one `writeBuffer` call.

- **Vsync decoupling**: Network frames arrive asynchronously (especially
  over WebTransport datagrams). The decode+parse runs immediately on arrival.
  `requestAnimationFrame` always renders the latest available parsed geometry.
  This eliminates the "wait for vsync to process network data" stall.

---

## 4. Texture Decode

### Supported formats

| Format | Bits | Expansion | Notes |
|--------|------|-----------|-------|
| ARGB1555 | 16bpp | 5-bit → 8-bit via `(v<<3)\|(v>>2)` | 1-bit alpha: 0→0, 1→255 |
| RGB565 | 16bpp | 5/6/5 → 8/8/8, alpha=255 | No alpha channel |
| ARGB4444 | 16bpp | 4-bit → 8-bit via `(v<<4)\|v` | Full 4-bit alpha |

Bit-replication expansion matches flycast's C++ decode exactly. The expanded
values are identical between the WebGPU JS path and the native OpenGL path.
This was verified by comparing vertex color output byte-for-byte.

### VQ compressed textures

VQ (Vector Quantization) is the Dreamcast's compressed texture format:

- **Codebook**: 256 entries, each entry = 4 texels (2x2 block). 2048 bytes
  at the start of the texture data in VRAM.
- **Indices**: One byte per 2x2 block, twiddled (Morton order) addressing.
- **Decode**: For each 2x2 block position, read the twiddled index byte,
  look up 4 texels from the codebook, place in correct (x,y) within block.

VQ pixel order fix (bug found during this renderer's development): within
the 2x2 codebook entry, pixel indices map to positions as:
```
  p[0] = (0,0) top-left
  p[1] = (1,0) top-right      ← was bottom-left, caused garbled VQ textures
  p[2] = (0,1) bottom-left
  p[3] = (1,1) bottom-right
```
The correct layout: `prel(x,y) = data[y*w + x]`, reading `p[1]` as bottom-left
is wrong.

### Mipmapped textures

Mipmap data is stored before the main texture in VRAM. Offset tables:

- **VQ mipmapped**: `VQMipPoint[]` — codebook + mip chain offsets
- **Other mipmapped**: `OtherMipPoint[]` — raw pixel mip chain offsets

The renderer reads the base mip level (full resolution). Mip level selection
for rendering is not yet implemented (always uses level 0).

### Dirty-page cache

Each texture's VRAM address range is computed from `(TCW.addr, width, height,
format, VQ, mip)`. This range is converted to a set of 4KB page indices.
On each delta frame, the decoder reports which pages were updated. A texture
is only re-decoded if `dirtyPages.has(pageIdx)` for any page in its range.

Palette textures (4bpp/8bpp) are additionally re-decoded when PVR register
pages are dirty (region ID 3), since palette data lives in PVR register space.

---

## 5. Rendering Pipeline

### TA command parsing

The `TAParser` walks the TA command buffer byte-by-byte, interpreting
PowerVR2 Tile Accelerator commands:

- **PCW** (Parameter Control Word): list type (opaque/punch/translucent),
  group/end-of-list flags, vertex type (0-8), object type
- **ISP** (Image Synthesis Processor): depth mode, cull mode, depth write
- **TSP** (Texture/Shading Processor): blend src/dst factors, texture enable,
  shading mode (flat/Gouraud), ignore alpha, color clamp
- **TCW** (Texture Control Word): texture address, pixel format, VQ flag,
  mipmap flag, U/V size

Vertex types 0-7 handle different combinations of packed/floating-point
color, intensity, UV formats. Type 4 (sprite) uses a plane equation for
the 4th vertex position — this was a source of bugs (garbled character
sprites) until the plane equation math was fixed.

### Triangle strip to triangle list conversion

PVR2 outputs triangle strips. The renderer converts to triangle lists with
an index buffer for WebGPU consumption. For each strip of N vertices, emit
(N-2) triangles with alternating winding:
```
  tri 0: [0, 1, 2]
  tri 1: [2, 1, 3]   ← reversed winding
  tri 2: [2, 3, 4]
  tri 3: [4, 3, 5]   ← reversed winding
  ...
```

### Per-poly fragment uniforms

Each polygon gets a 256-byte-aligned slot in the dynamic uniform buffer
containing:

| Field | Type | What |
|-------|------|------|
| `atv` | f32 | Animation time value (for shader effects) |
| `si` | u32 | Shading instruction (texture/color/ignore-alpha flags) |
| `ht` | u32 | Reserved |
| `ua` | u32 | Use alpha flag |
| `ita` | u32 | Ignore texture alpha flag |
| `ho` | u32 | Reserved |
| `at` | u32 | Alpha test threshold |
| `packed` | u32 | Packed effect bits (bits 12-31) |

The dynamic offset is passed per-draw-call via `setBindGroup(0, bg, [offset])`.
This avoids creating a separate bind group per polygon.

### Pipeline cache

Render pipelines are cached by a composite key:
```
key = srcBlend_dstBlend_depthMode_depthWrite_cullMode_topology
```

MVC2 typically uses ~15-25 unique pipeline configurations per frame.
Creating pipelines is expensive (involves shader compilation), so caching
is critical. The cache persists across frames.

### State batching

Before each draw call, the renderer checks if the pipeline and bind groups
have changed since the last call. If they haven't, the `setPipeline` and
`setBindGroup` calls are skipped. This saves significant command encoder
overhead — MVC2 scenes often have 50-100 consecutive polys with identical
blend/depth/texture state.

### FillBGP (background polygon)

The `ISP_BACKGND_T` PVR register specifies a background polygon that fills
the screen behind all other geometry. The parser reads this register, finds
the polygon parameters and vertices in the TA buffer at the specified tag
offset, and emits a full-screen colored quad as the first draw call.

Known issue: during supers (special moves) in MVC2, the background color
changes for 1-2 frames causing a green flash. This is the game legitimately
changing the background polygon color — not a rendering bug.

### User Tile Clip (scissor rect)

Some polygons specify a tile clip region via bits in the PCW. The parser
extracts the clip rectangle and passes it to the renderer, which sets
`setScissorRect()` on the render pass encoder. This is used by MVC2 for
HUD elements and portrait frames.

---

## 6. Known Issues & Bugs Found

### Fixed during development

| Bug | Symptom | Fix |
|-----|---------|-----|
| VQ pixel order | Garbled VQ textures | `p[1]` = top-right, not bottom-left |
| Sprite plane equation | Character sprites garbled | Correct 4th vertex from plane eq |
| Sprite vertex order | Wrong triangle winding for sprites | Match flycast's strip order |
| Bit-replication colors | Washed-out colors | Use `(v<<3)\|(v>>2)` not `v*8` |
| Perspective-multiply on colors | Colors too dark/light | Don't multiply vertex colors by Z |
| Cull flip | Backface cull reversed | Match flycast's front-face convention |

### Open issues

1. **Green flash during supers** — FillBGP background color changes for 1-2
   frames. This is the game's actual behavior, not a rendering bug. Could be
   masked by holding the previous BG color when the change duration is < 3
   frames, but that risks hiding legitimate BG changes.

2. **Multi-pass rendering** — The current EndOfList pass detection doesn't
   fully match flycast's TA context model. Flycast tracks render passes
   internally via the TA state machine; the JS parser approximates this by
   detecting repeated list-type starts. This causes minor ordering issues
   in rare scenes.

3. **Z-sort disabled** — Translucent polygons should be Z-sorted
   back-to-front, but enabling Z-sort with depth write causes flicker on
   overlapping translucent geometry. Currently using submission order (the
   order the game emits polygons), which works for MVC2 because the game
   already emits translucent geometry in roughly back-to-front order.

4. **Relay join/leave forwarding** — The relay MUST NOT forward join/leave
   messages to the upstream flycast server. If it does, flycast and the
   relay disagree on connected clients, causing slot conflicts. Only
   gamepad input and queue commands go upstream.

### Vertex color verification

Vertex colors were compared byte-by-byte between the native OpenGL renderer
and the WebGPU JS renderer. The packed ARGB bytes in the vertex buffer are
identical. This confirms the TA parser's color extraction (from packed u32,
float, and intensity formats) matches flycast's C++ implementation.

---

## 7. Gamepad Input

### Architecture

The WebGPU renderer reuses king.html's proven gamepad modules rather than
reimplementing input handling:

- `web/js/state.mjs` — application state (player name, slot assignment)
- `web/js/ws-connection.mjs` — WebSocket connection management
- `web/js/gamepad.mjs` — Gamepad API polling, button mapping, input dispatch

### Control path

Gamepad input goes over a **separate WebSocket** directly to flycast, NOT
through the relay:

```
Browser Gamepad API
  │ requestAnimationFrame poll
  ▼
gamepad.mjs
  │ Map buttons to Dreamcast layout
  │ Pack: [LT][RT][buttons_hi][buttons_lo]
  ▼
WebSocket → nginx /play → flycast :7210 (direct)
  │ NOT through the relay on :7201
  │ The relay handles stream fan-out only
  ▼
flycast input server → kcode[slot]
```

This is important: the relay on `:7201` handles TA stream fan-out. Gamepad
input bypasses it entirely via nginx `/play` routing to flycast's direct
WebSocket on `:7210`. This avoids adding relay hop latency to input.

### MVC2 button layout

```
LP  HP  A1
LK  HK  A2
```

Mapped from standard gamepad:
- X = LP, Y = HP, A = LK, B = HK
- RB = LT (Assist 1), LB = RT (Assist 2)
- Note: RB/LB are **swapped** relative to what you'd expect — RB maps to
  Dreamcast LT (A1), LB maps to Dreamcast RT (A2). This matches the
  physical MVC2 cab layout where A1 is right of HP and A2 is right of HK.
- Start = Start

---

## 8. File Inventory

| File | Lines | Purpose |
|------|-------|---------|
| `web/webgpu-test.html` | ~800 | Entry point, UI, debug panel, gamepad viz |
| `web/webgpu/transport.mjs` | ~200 | Adaptive WebTransport/WebSocket |
| `web/webgpu/frame-decoder.mjs` | ~180 | ZCST + SYNC + delta decode |
| `web/webgpu/ta-parser.mjs` | ~600 | TA command stream → geometry |
| `web/webgpu/texture-manager.mjs` | ~500 | Texture decode + GPU cache |
| `web/webgpu/pvr2-renderer.mjs` | ~400 | WebGPU render dispatch |
| `web/webgpu/shaders.mjs` | ~120 | WGSL shaders (vertex + fragment) |
| `web/webgpu/fzstd.mjs` | ~2000 | zstd decompressor (vendored) |
| `web/webgpu/decode-worker.mjs` | ~100 | Worker: decode + parse |
| `web/webgpu/render-worker.mjs` | ~200 | Worker: full pipeline on OffscreenCanvas |

### Relation to king.html WASM path

The WebGPU renderer and the king.html WASM renderer are **independent
implementations** of the same TA mirror stream consumer:

| Aspect | king.html (WASM) | webgpu-test.html (JS) |
|--------|------------------|-----------------------|
| Decode | C++ via Emscripten | JS (frame-decoder.mjs) |
| Parse | C++ `ta_parse()` | JS (ta-parser.mjs) |
| Render | WebGL2 (flycast's GLES renderer) | WebGPU (custom pipeline) |
| Texture | C++ texture cache | JS (texture-manager.mjs) |
| Size | 831 KB .wasm + loader | ~5 KB JS modules + 60 KB fzstd |
| Build | `emmake make` (minutes) | None (edit + refresh) |
| Transport | WebSocket only | WebTransport + WS fallback |

Both consume the same wire format (ZCST-compressed delta/SYNC frames).
Wire format changes still require updating both paths (plus the C++ desktop
client and the Rust relay — see ARCHITECTURE.md "Mirror Wire Format — Rules
of the Road").

---

## 9. Future Work

1. **Storage buffer uniforms** — Replace dynamic uniform buffer with
   `var<storage,read>` array indexed by `instance_index`. Removes the
   256-byte alignment waste. First attempt hit pipeline validation errors;
   needs WGSL debugging.

2. **Bundle fzstd locally as Worker** — Move zstd decompress to a dedicated
   Worker thread, enabling true parallel decode+parse off the main thread.

3. **Delta-aware TA parse** — Hash TA sections, skip parsing unchanged
   regions. Most frames only change a small portion of the TA buffer.

4. **IndexedDB SYNC cache** — Cache the 8MB VRAM snapshot in IndexedDB for
   instant reconnect without waiting for a fresh SYNC from the server.

5. **Predictive rendering** — Use the 253-byte MVC2 game state to
   pre-render anticipated next frames during network gaps.
