# MapleCast Latency Workstream — Every Millisecond is Gold

## Current State (TA Mirror Mode): ~7ms button-to-pixel (browser), ~3-4ms (NOBD)
## Previous State (H.264 only): ~35ms button-to-pixel (2.1 frames)
## Improvement: 28ms / 80% reduction over original H.264 pipeline

---

## Measured Latencies (TA Mirror Mode, April 2026)

These are real numbers from telemetry, not estimates.

| Metric | Measured | Notes |
|--------|----------|-------|
| Server TA publish | 0.46ms/frame | Capture + serialize + WebSocket send |
| Client WebSocket ping | 0.2ms | LAN, WebSocket ping/pong |
| Stream bandwidth | ~4 Mbps | TA commands + memory diffs |
| Frame rate | 59-60fps | Sustained, stable |
| Full E2E (NOBD stick) | ~3-4ms | XDP input to pixel on screen |
| Full E2E (browser) | ~7ms | Browser gamepad to pixel on screen |

### How the 7ms breaks down (browser path)

```
Browser gamepad poll:     ~1ms (requestAnimationFrame aligned)
WebSocket send to server: ~0.2ms (LAN)
Server processes input:   ~0.5ms
Server publishes TA:      ~0.46ms
WebSocket to client:      ~0.2ms (LAN)
WASM TA parse + render:   ~4ms (WebGL2 draw calls)
Display compositor:       ~1ms
                          ─────
Total:                    ~7ms
```

### How the 3-4ms breaks down (NOBD path)

```
NOBD XDP input:           ~0.1ms (AF_XDP zero-copy, NIC DMA)
Server processes input:   ~0.5ms
Server publishes TA:      ~0.46ms
WebSocket to client:      ~0.2ms (LAN)
WASM TA parse + render:   ~2ms (less overhead than browser gamepad path)
                          ─────
Total:                    ~3-4ms
```

---

## What Was Completed

### TA Mirror Mode (replaced Phases 1-3 for browser clients)

The TA mirror approach eliminated the entire H.264 encode/decode pipeline for browser clients. Instead of capturing the framebuffer, encoding to H.264, sending compressed video, and decoding on the client — we stream the GPU commands themselves and let the client render natively.

**What this eliminated:**
- GPU framebuffer capture (was 3-12ms)
- sws_scale color conversion (was 0.5-1ms)
- NVENC H.264 encode (was 1-3ms)
- Browser H.264 decode (was 1-3ms)
- Resolution lock (was 640x480, now any)

**What this added:**
- TA command capture: 0.46ms (server side)
- TA parse + WebGL2 render: ~2-4ms (client side, resolution dependent)

Net improvement: ~12-22ms of pipeline eliminated.

### WebSocket Server in Flycast (Phase 3 done)

The Python proxy is gone. WebSocket server runs directly in Flycast using websocketpp (already in `core/deps/websocketpp/`). Browser connects directly — no middleman, no extra process, no event loop overhead.

### Input Threading (Phase 2 done)

Dedicated input thread drains UDP continuously. `getInput()` reads atomics — zero syscalls, zero blocking, zero vblank alignment delay. Input is always fresh.

---

## Phase 1: True GPU-Only Encode (H.264 path only)

This phase applies only to the H.264 streaming path, which is secondary to TA mirror but still used for NOBD stick players.

### Problem
The "zero-copy" path is a lie. Frame data goes GPU->CPU->GPU:
```
GL texture → CUDA map → cuMemcpy2D to HOST (GPU→CPU) → sws_scale on CPU → NVENC re-uploads (CPU→GPU)
```

### Solution
NVENC supports `NV_ENC_BUFFER_FORMAT_ABGR` input directly. The GL texture IS ABGR on the GPU. No conversion needed.

### Steps

**1.1** Download NVIDIA Video Codec SDK headers (nvEncodeAPI.h)
- Source: https://developer.nvidia.com/video-codec-sdk
- Or extract from FFmpeg's `nv-codec-headers` package
- Only need the headers — the runtime is in the NVIDIA driver

**1.2** Replace FFmpeg avcodec with direct NVENC SDK calls
- `NvEncOpenEncodeSessionEx()` — create encoder session on the CUDA device
- `NvEncInitializeEncoder()` — configure: 640x480, baseline, P1 preset, ULL tune, GOP=1, CBR 15Mbps
- `NvEncRegisterResource()` — register the CUDA-mapped GL texture as input
  - `resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR`
  - `bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR` (matches GL RGBA texture)
- Remove `sws_scale` entirely — NVENC handles the color space internally
- Remove `avcodec_send_frame/avcodec_receive_packet` — replace with `NvEncEncodePicture()`

**1.3** The new frame path (zero copy, real this time):
```
GL texture (GPU VRAM)
  → cuGraphicsGLRegisterImage (once)
  → cuGraphicsMapResources (per frame, ~0.01ms)
  → cuGraphicsSubResourceGetMappedArray → get CUdeviceptr
  → NvEncMapInputResource (maps CUDA ptr as NVENC input)
  → NvEncEncodePicture (encode on GPU, output is H.264 bitstream)
  → NvEncLockBitstream (get pointer to encoded bytes — this IS on CPU, but it's tiny ~30KB)
  → TCP send the bitstream bytes
  → NvEncUnlockBitstream
  → cuGraphicsUnmapResources
```

**1.4** What gets deleted:
- FFmpeg dependency (avcodec, avutil, swscale) — entirely
- `sws_scale()` — gone
- `std::vector<u8> rgbaData(w * h * 4)` — gone (1.2MB allocation per frame)
- `_swFrame` (AVFrame) — gone
- All FFmpeg init/cleanup code

**1.5** What gets added:
- `nvEncodeAPI.h` header (from Video Codec SDK)
- Direct NVENC API calls (~150 lines replacing ~100 lines of FFmpeg code)
- CUDA-GL interop (already have this)

### Files Changed
- `core/network/maplecast_stream.cpp` — rewrite encode path
- `CMakeLists.txt` — remove FFmpeg libs, add NVENC header path
- `external/` — add nv-codec-headers

### Verification
- Telemetry `capture` time should drop from 3-12ms to <0.5ms
- Telemetry `encode` time should drop from 1-3ms to <1ms
- Telemetry `scale` time should be 0ms (eliminated)
- Total `onFrameRendered()` should be <1.5ms

---

## Phase 4: Browser Optimizations (saves 1-2ms, applies to H.264 path)

### Problem
Browser decode and render path has unnecessary overhead:
- Missing hardware acceleration hint on VideoDecoder
- Using 2D canvas `drawImage()` which composites through CPU
- No frame scheduling alignment

### Steps

**4.1** Add hardware acceleration to VideoDecoder config:
```javascript
decoder.configure({
    codec: 'avc1.42001e',
    codedWidth: 640,
    codedHeight: 480,
    optimizeForLatency: true,
    hardwareAcceleration: "prefer-hardware",
});
```

**4.2** Replace 2D canvas with `<video>` element + MediaStreamTrackGenerator:
```javascript
const generator = new MediaStreamTrackGenerator({ kind: 'video' });
const writer = generator.writable.getWriter();
const stream = new MediaStream([generator]);
videoElement.srcObject = stream;

// In decoder output callback:
output: (frame) => {
    writer.write(frame);
    // Don't close frame — the video element manages it
}
```
This uses the browser's native video pipeline — GPU composited, vsync aligned, zero CPU copies.

**4.3** Add `requestVideoFrameCallback` for precise frame timing telemetry:
```javascript
videoElement.requestVideoFrameCallback((now, metadata) => {
    const presentTime = metadata.presentationTime;
    const expectedDisplay = metadata.expectedDisplayTime;
    // Log timing for diagnostics
});
```

### Files Changed
- `web/index.html` — rewrite decode/render path

---

## Phase 5: Pre-allocate Everything (saves 0.3ms)

### Problem
Per-frame heap allocations in the H.264 path:
- `std::vector<u8> rgbaData(w * h * 4)` — 1.2MB every frame (Phase 1 eliminates this)
- `std::vector<u8> msg(4 + totalPayload)` — ~30KB every frame in broadcastPacket
- `std::vector<u8> rgbData` in GetLastFrame fallback path

### Steps

**5.1** Pre-allocate send buffer in `init()`:
```cpp
static std::vector<u8> _sendBuf;
// In init():
_sendBuf.reserve(256 * 1024);  // 256KB — enough for any frame
```

**5.2** In `broadcastPacket()`, reuse `_sendBuf`:
```cpp
_sendBuf.resize(4 + totalPayload);
memcpy(_sendBuf.data(), &totalPayload, 4);
// etc
```

**5.3** Replace `_clientMutex` lock for `hasClients` check with atomic:
```cpp
static std::atomic<int> _clientCount{0};
// In accept: _clientCount++;
// In disconnect: _clientCount--;
// In onFrameRendered: if (_clientCount.load(std::memory_order_relaxed) == 0) return;
```

**5.4** Use scatter-gather send to avoid copying header + payload into one buffer.

### Files Changed
- `core/network/maplecast_stream.cpp` — pre-allocate, scatter-gather send

---

## Phase 6: Advanced Optimizations (future)

**6.1** WebTransport (UDP-based browser streaming)
- Eliminates TCP head-of-line blocking
- Each frame as independent datagram
- Lost frames skipped, not retransmitted

**6.2** Adaptive bitrate based on network conditions
- Monitor round-trip time from telemetry
- Lower TA command detail when network is congested

**6.3** Input prediction on the server
- If a player's input hasn't arrived when the frame starts, predict "same as last frame"
- Correct on next frame if wrong
- Saves 0-16ms of input wait in exchange for 1 frame of wrong opponent state (rare)

---

## Success Metrics

| Metric | Original (H.264) | Current (TA Mirror) | Target |
|--------|-------------------|---------------------|--------|
| Server publish | 3-12ms (capture+encode) | 0.46ms | <0.5ms |
| Network transit | 0.5-2ms (via proxy) | 0.2ms (direct WS) | <0.5ms |
| Client render | 1-3ms (H.264 decode) | 2-4ms (WebGL2) | <3ms |
| Input latency (NOBD) | 0-16ms (vblank aligned) | <0.5ms (threaded) | <0.5ms |
| Input latency (browser) | ~8ms avg | ~1ms | <1ms |
| **Full E2E (NOBD)** | **~35ms (2.1 frames)** | **~3-4ms (<0.25 frame)** | **<3ms** |
| **Full E2E (browser)** | **~35ms (2.1 frames)** | **~7ms (<0.5 frame)** | **<5ms** |
| Stream bandwidth | 1.9 MB/s (H.264) | ~4 Mbps (TA mirror) | <4 Mbps |
| Frame rate | 60fps | 59-60fps | 60fps |
| Resolution | 640x480 (locked) | Any (client renders) | Any |
