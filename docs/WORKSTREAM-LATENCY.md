# MapleCast Latency Workstream — Every Millisecond is Gold

## Current State: ~35ms button-to-pixel (2.1 frames)
## Target: ~19ms button-to-pixel (1.14 frames)
## Savings: 16ms / 45% reduction

---

## Phase 1: True GPU-Only Encode (saves 2-4ms)

### Problem
The "zero-copy" path is a lie. Frame data goes GPU→CPU→GPU:
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

## Phase 2: Fix Input Timing (saves avg 8ms)

### Problem
Input arrives over UDP at any time. But `maplecast::getInput()` only runs inside `maple_DoDma()` which fires once per frame at vblank. An input that arrives 1ms after vblank waits 15.67ms for the next one.

```
vblank → maple_DoDma reads input → 16.67ms gap → next vblank reads input
              ↑                                           ↑
         input arrives here...                    ...doesn't get read until here
         wasted: 15.67ms
```

### Solution
Dedicated input thread that continuously drains UDP and updates `_w3[]` atomically. `getInput()` just reads the latest value — zero syscalls, zero blocking.

### Steps

**2.1** Create input receiver thread in `maplecast.cpp`
```cpp
static std::thread _inputThread;
static std::atomic<uint32_t> _w3Atomic[2];  // packed W3 as uint32

void inputThreadLoop() {
    while (_active) {
        // recvfrom with short timeout (1ms)
        // update _w3Atomic[player] with atomic store
    }
}
```

**2.2** `getInput()` becomes lock-free:
```cpp
void getInput(MapleInputState inputState[4]) {
    uint32_t w3p1 = _w3Atomic[0].load(std::memory_order_relaxed);
    uint32_t w3p2 = _w3Atomic[1].load(std::memory_order_relaxed);
    w3ToInput(w3p1, inputState[0]);
    w3ToInput(w3p2, inputState[1]);
}
```
Zero syscalls. Zero locks. Just atomic reads.

**2.3** Input is always fresh — no vblank alignment delay

### Files Changed
- `core/network/maplecast.cpp` — add input thread, atomic state

### Verification
- Input-to-frame latency should be <1ms instead of 0-16ms
- Telemetry should show consistent input freshness

---

## Phase 3: Kill the Python Proxy (saves 1-2ms)

### Problem
Every frame goes through Python asyncio:
```
Flycast TCP:7200 → proxy.py (Python) → WebSocket:8080 → Browser
```
Python adds event loop overhead, GIL contention, memory copies, syscall overhead.

### Solution
Build the WebSocket proxy in Rust. Or better: add WebSocket directly to Flycast's C++ code using websocketpp (already in Flycast's deps).

### Steps

**3.1 Option A: Rust proxy (simpler, still fast)**
- New binary in `maplecast-server/` repo
- `tokio` + `tokio-tungstenite` for async WebSocket
- Raw TCP read from Flycast → WebSocket forward
- Single binary, ~50 lines of Rust
- Sub-0.1ms per frame forwarding

**3.2 Option B: WebSocket in Flycast C++ (zero proxy)**
- Use websocketpp (already in `core/deps/websocketpp/`)
- Replace the raw TCP listener in `maplecast_stream.cpp` with a WebSocket server
- Browser connects directly to Flycast — no middleman
- Eliminates an entire network hop and process

**3.3 Option C: WebTransport (future, best)**
- WebTransport uses QUIC/UDP — no head-of-line blocking
- Browser API: `new WebTransport("https://host:port")`
- Requires TLS certificate (self-signed OK for local)
- Each frame as an independent datagram — lost frames just skipped
- Since GOP=1, every frame is independently decodable

### Recommended: Option B first (websocketpp is there), Option C later

### Files Changed
- `core/network/maplecast_stream.cpp` — replace TCP with WebSocket server
- `web/proxy.py` — deleted
- `start_maplecast.bat` — remove proxy startup

### Verification
- Telemetry should show frame delivery time (server send → client receive) < 0.5ms on LAN
- No more proxy process needed

---

## Phase 4: Browser Optimizations (saves 1-2ms)

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

**4.4** Reduce gamepad polling to 4ms or use `requestAnimationFrame`:
```javascript
function pollGamepad() {
    // read + send
    requestAnimationFrame(pollGamepad);
}
```

### Files Changed
- `web/index.html` — rewrite decode/render path

### Verification
- Decode latency in diagnostics should drop from ~1-3ms to <1ms
- No canvas CPU compositing overhead
- Frame timing aligned to display vsync

---

## Phase 5: Pre-allocate Everything (saves 0.3ms)

### Problem
Per-frame heap allocations:
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

**5.4** Use scatter-gather send (`sendmsg` / `WSASend` with multiple buffers) to avoid copying header + payload into one buffer:
```cpp
WSABUF bufs[3] = {
    { 4, (char*)&totalPayload },
    { 12, (char*)&header },
    { (ULONG)pkt->size, (char*)pkt->data }
};
WSASend(client, bufs, 3, &sent, 0, nullptr, nullptr);
```
Zero copy in the send path.

### Files Changed
- `core/network/maplecast_stream.cpp` — pre-allocate, scatter-gather send

### Verification
- Zero heap allocations per frame (verify with profiler)
- Consistent frame times with no GC/allocation jitter

---

## Phase 6: Advanced Optimizations (future)

**6.1** WebTransport (UDP-based browser streaming)
- Eliminates TCP head-of-line blocking
- Each frame as independent datagram
- Lost frames skipped, not retransmitted

**6.2** NVENC B-frame lookahead for better quality at same bitrate
- Only if we ever relax the all-IDR requirement
- Adds 1-2 frames of encode latency — NOT for competitive play

**6.3** Adaptive bitrate based on network conditions
- Monitor round-trip time from telemetry
- Lower bitrate when network is congested
- Higher bitrate when network is clear

**6.4** Input prediction on the server
- If a player's input hasn't arrived when the frame starts, predict "same as last frame"
- Correct on next frame if wrong
- Saves 0-16ms of input wait in exchange for 1 frame of wrong opponent state (rare)

**6.5** Direct GPU capture without GL interop
- Use NVIDIA's NvFBC (Frame Buffer Capture) API
- Captures the final composited framebuffer directly
- Bypasses GL entirely — works with any renderer (GL, Vulkan, DX11)
- Requires NVIDIA Capture SDK

---

## Execution Order

```
Phase 1: True GPU-Only Encode        ← DO FIRST (biggest encode win)
Phase 2: Fix Input Timing            ← DO SECOND (biggest overall win)
Phase 3: Kill Python Proxy           ← DO THIRD (removes a whole component)
Phase 4: Browser Optimizations       ← DO FOURTH (client-side wins)
Phase 5: Pre-allocate Everything     ← DO FIFTH (polish)
Phase 6: Advanced                    ← FUTURE
```

## Success Metrics

| Metric | Current | Phase 1 | Phase 2 | Phase 3 | Phase 4 | Phase 5 | Target |
|--------|---------|---------|---------|---------|---------|---------|--------|
| Capture | 3-12ms | <0.5ms | <0.5ms | <0.5ms | <0.5ms | <0.5ms | <0.5ms |
| Scale | 0.5-1ms | 0ms | 0ms | 0ms | 0ms | 0ms | 0ms |
| Encode | 1-3ms | <1ms | <1ms | <1ms | <1ms | <1ms | <1ms |
| Input wait | 0-16ms | 0-16ms | <1ms | <1ms | <1ms | <1ms | <1ms |
| Proxy | 0.5-2ms | 0.5-2ms | 0.5-2ms | 0ms | 0ms | 0ms | 0ms |
| Browser decode | 1-3ms | 1-3ms | 1-3ms | 1-3ms | <1ms | <1ms | <1ms |
| Alloc jitter | 0.3ms | 0ms | 0ms | 0ms | 0ms | 0ms | 0ms |
| **Total overhead** | **~19ms** | **~14ms** | **~6ms** | **~4ms** | **~3ms** | **~2.5ms** | **<3ms** |
| **Button-to-pixel** | **~35ms** | **~30ms** | **~23ms** | **~21ms** | **~20ms** | **~19ms** | **~19ms** |
| **Frames of lag** | **2.1** | **1.8** | **1.4** | **1.3** | **1.2** | **1.14** | **1.14** |
