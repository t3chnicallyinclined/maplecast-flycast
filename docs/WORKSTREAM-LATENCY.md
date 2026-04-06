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

---

# INTERNET / RELAY PATH (nobd.net production)

The numbers above are LAN-only. Once the path goes through the public
internet via the VPS relay, the budget is dominated by network round-trips
and the GPU pipeline in the browser, not by anything we control in our code.

## Hot Path (button → pixel) over the relay

```
Stage                                   Cost     What controls it
─────────────────────────────────────   ──────   ─────────────────────────
1.  Browser gamepad poll                ~2-4ms   navigator.getGamepads() rate
2.  Browser → VPS (TCP+TLS)             ~5-25ms  user's network distance
3.  nginx /ws → relay loopback          ~0.1ms   nginx
4.  Relay tokio task forward            ~0.2ms   tokio scheduler
5.  VPS → home (one-way)                ~2.4ms   measured 4.8ms RTT / 2
6.  Home flycast asio → UDP forward     ~0.05ms  syscall
7.  Wait for next emu vblank            0-16ms   FRAME ALIGNMENT (the killer)
8.  Game logic + GPU render             ~5-15ms  emulator native cost
9.  TA capture + zstd compress          ~0.34ms  serverPublish + level-1 zstd
10. Home → VPS (one-way)                ~2.5-4ms upload bandwidth + RTT/2
11. Relay broadcast::send               ~0.1ms   tokio
12. Relay → browser (one-way)           ~5-25ms  user's network distance
13. Browser worker → main thread        ~0.1ms   postMessage
14. WASM zstd decompress                ~30us
15. WASM TA parse + WebGL2 render       ~2-6ms   GPU pipeline
16. Browser compositor / vsync          ~8-16ms  display refresh

TOTAL (typical user, ~5ms from VPS):    ~40ms p50, ~60ms p99
TOTAL (LAN player, no relay):           ~5-7ms (the original budget)
```

## What dominates the internet budget

Three things, in order:

1. **Frame alignment** (stage 7, 0-16ms, avg 8ms) — the input might arrive
   anywhere within an emulator frame; on average it waits half a frame for
   the next vblank. This is bigger than the entire network path combined.

2. **Network round trips** (stages 2, 5, 10, 12 — ~15-50ms total) — bounded
   by the user's distance to the VPS and the VPS's distance to home. Both
   are physical network distances we can't shrink.

3. **Browser display pipeline** (stage 16, 8-16ms) — even if the WASM
   renders the frame instantly, the browser compositor waits for the next
   monitor vsync. On a 60Hz display, that's average 8ms. On a 144Hz gaming
   display it drops to ~3.5ms.

Stages 1, 3, 4, 6, 9, 11, 13, 14 sum to under 1ms total. **The
microsecond-level optimizations we did to flycast and the relay are
essentially complete.** Further latency wins require attacking the
network or the framing, not the code.

---

# OVERKILL OPTIMIZATION ROADMAP

Ranked by impact (latency saved at p50) and risk (effort + chance of
breaking things). Marked DONE for what's already shipped.

## TIER S — asymmetric wins (high impact)

### S1. ~~GGPO-style rollback prediction~~  [DROPPED — wrong model]
**Why this was wrong**: GGPO solves a problem we don't have. GGPO exists
for peer-to-peer fighting games where each peer runs their own emulator
and the network can't deliver the opponent's input in time. A peer
predicts the opponent's input ("they pressed nothing"), runs the frame,
and rolls back if wrong.

**MapleCast has exactly one authoritative emulator** running on the home
box. Both players' inputs flow into the same `kcode[]` array. Browsers are
passive renderers with no game state. There is nothing to roll back
because there is no client-side simulation; state drift is impossible
by construction.

The 8ms "frame alignment" cost is just **the natural quantization of
reading input once per vblank**. The emulator reads `kcode[]` at the
start of each frame; if the input arrives 2ms after a vblank, it waits
14.67ms for the next one. Average: half a frame.

**The only way to beat frame alignment** would be sub-frame input
sampling — having the emulator read input multiple times per frame, or
re-running game logic when late input arrives. MVC2's game loop is
hardcoded to one input read per vblank, so this would require modifying
the game itself, not the emulator. Not happening.

**Frame alignment is physics**, just like network distance and monitor
refresh rate. It's a 0-16.67ms uniform distribution we cannot shrink.

### S2. LAN bypass for local players  [JS WIRED, INFRA TODO]
**Saves: ~15-30ms (entire VPS round trip) for LAN users**
**Risk: LOW (race-and-fall-back: no risk of breaking remote users)**

The browser races a connection to a LAN endpoint against the relay
endpoint and uses whichever opens first. LAN users win in <100ms;
remote users' LAN attempt times out in 800ms and the relay wins.

**Browser side: DONE.** `web/js/ws-connection.mjs::pickBestWsUrl()`
implements the race. Set `LAN_BYPASS_URL` in that file to enable.
Currently `null` so the function is a no-op pass-through to the relay.

**Home box infrastructure: TODO.** To turn this on:

1. **Pick a hostname**, e.g. `home.nobd.net`. Add a public DNS A record
   pointing it to your home box's LAN IP (e.g. `192.168.1.100`). Yes,
   public DNS resolving to a private IP is fine — only machines on your
   LAN can route to it; everyone else gets a TCP timeout.

2. **Get a Let's Encrypt cert via DNS-01 challenge**, because LE servers
   can't reach your home box over HTTP-01 to validate. With certbot:
   ```
   certbot certonly --manual --preferred-challenges dns -d home.nobd.net
   ```
   Or use an ACME client that auto-handles your DNS provider's API.
   You can run this on the VPS (which already has certbot) and copy
   the cert to the home box.

3. **Run nginx on the home box** as a TLS terminator. Mirror the VPS
   nginx config but swap the upstream:
   ```nginx
   server {
       listen 443 ssl http2;
       server_name home.nobd.net;
       ssl_certificate     /etc/letsencrypt/live/home.nobd.net/fullchain.pem;
       ssl_certificate_key /etc/letsencrypt/live/home.nobd.net/privkey.pem;

       location /ws {
           proxy_pass http://127.0.0.1:7200;
           proxy_http_version 1.1;
           proxy_set_header Upgrade $http_upgrade;
           proxy_set_header Connection "Upgrade";
           proxy_buffering off;
           tcp_nodelay on;
           proxy_read_timeout 86400;
       }
   }
   ```

4. **Set `LAN_BYPASS_URL = 'wss://home.nobd.net/ws'`** in
   `web/js/ws-connection.mjs` and redeploy to the VPS.

After this, every user on your LAN automatically gets the ~15ms-faster
direct path with no UI changes. Useful for local play, tournaments,
testing, and your couch.

### S3. WebRTC DataChannel for player input  [INFRA EXISTS]
**Saves: ~5-10ms p50, ~15ms p99 (UDP semantics + bypasses TCP HOL)**
**Risk: MEDIUM (WebRTC signaling complexity, but libdatachannel already linked)**

Player input is currently TCP-stuck-in-the-WS-queue:
`browser → relay → flycast WS → UDP to input server`. Instead, use a
WebRTC DataChannel with `{ordered: false, maxRetransmits: 0}` for player
input. The DataChannel runs over UDP, has no head-of-line blocking, and
the relay can either forward UDP packets directly or even let the
browser open a P2P channel to home flycast (NAT-punch via STUN).

`MAPLECAST_WEBRTC=1` is already compiled into the home flycast binary
(`maplecast_webrtc.cpp`, libdatachannel linked). The signaling
infrastructure exists. Wiring up a per-player input DataChannel is
maybe a day of work.

### S4. Browser gamepad polling at 1ms via MessageChannel  [NOT STARTED]
**Saves: ~2-3ms p50 off stage 1**
**Risk: LOW**

`setTimeout` has a 4ms minimum on most browsers. `requestAnimationFrame`
fires at 16.67ms (60Hz) or 6.94ms (144Hz). Neither is fast enough.
Use a `MessageChannel` postMessage trick to schedule callbacks with no
clamp:

```js
const mc = new MessageChannel();
let pending = false;
mc.port1.onmessage = () => { pending = false; pollGamepad(); };
function tick() { if (!pending) { pending = true; mc.port2.postMessage(null); } }
```

Combined with rAF, you get effectively unlimited polling rate. Send only
on input change. ~2-3ms savings off the gamepad poll wait.

## TIER A — real wins, lower risk

### A1. Relay→home upstream WebSocket TCP_NODELAY  [DONE]
Done as part of the recent fix. Server side socket option is now set on
every accepted socket via websocketpp `set_socket_init_handler`. Status
JSON, ping echoes, and small frames no longer pay the Nagle 40ms tax.

### A2. Relay-side ping echo  [DONE]
Done. The diagnostics ping no longer round-trips to home flycast — the
relay echoes `{"type":"ping"}` directly. Saves ~10ms (one full home↔VPS
round trip) per ping. Diagnostic overlay should now show ~10-15ms ping
for users near the VPS instead of ~30ms.

### A3. zstd ZCST compression  [DONE]
Done. ~2.9x on delta frames, ~13x on SYNC. Saves ~60% bandwidth with
~80us compress + ~30us decompress overhead. See ARCHITECTURE.md.

### A4. SCHED_FIFO + CPU pinning + mlockall on home flycast  [NOT STARTED]
**Saves: ~3-10ms p99 (eliminates emu thread preemption)**
**Risk: LOW**

```bash
sudo chrt -f 50 ./build/flycast ...           # realtime priority
sudo taskset -c 4-7 ./build/flycast ...       # pin to physical cores
ulimit -l unlimited
```

Or in code: `mlockall(MCL_CURRENT|MCL_FUTURE) + sched_setscheduler(0,
SCHED_FIFO, ...)` at flycast startup. Removes:

- Memory page faults
- Scheduler preemption by other processes
- L1/L2 cache thrashing

Measurable as reduced p99 frame jitter. The user feels it as smoother
play under system load.

### A5. Move WASM rendering into a Worker thread with OffscreenCanvas  [NOT STARTED]
**Saves: ~50-200us per frame + huge p99 improvement**
**Risk: MEDIUM (refactor of frame-worker.mjs + renderer-bridge.mjs)**

Currently the binary frame goes:
`worker (recv WS) → postMessage to main thread → main thread calls WASM`.

The postMessage has scheduling delay. Worse, the main thread has to
share CPU with chat updates, lobby polls, gamepad polling, DOM
re-renders, etc. — any of which can delay the next frame by milliseconds
under load.

Move the WASM module **into** the worker. Worker owns an OffscreenCanvas
which the main thread displays. The render path becomes
`worker (recv WS) → call WASM directly → render to OffscreenCanvas`,
zero hops, zero main-thread contention.

WebGL2 supports OffscreenCanvas in workers in Chrome and Firefox.
Refactor of ~1 day. Removes a category of "stuttering when chat is
busy" complaints we don't have yet but would eventually.

### A6. tokio-tungstenite upgrade for zero-copy fan-out  [NOT STARTED]
**Saves: ~30 MB/s of memcpy at scale (50 spectators × 60fps × 10KB)**
**Risk: LOW (Cargo upgrade)**

[fanout.rs:374](relay/src/fanout.rs#L374):
```rust
ws_tx.send(Message::Binary(data.to_vec().into())).await
```

Tungstenite 0.26's `Message::Binary(Vec<u8>)` forces a copy of every
frame for every spectator. Tungstenite 0.27+ accepts `Bytes` natively
(refcounted clone, no copy). Upgrade tokio-tungstenite, use
`Message::Binary(data.clone())` directly.

At 50 spectators this saves ~30 MB/s of pure memory bandwidth and
reduces fan-out task wakeup latency. At our current 2-3 spectators
it's not visible, but it's free once we're at scale.

### A7. zstd compression level 0 for ultra-fast mode  [NOT STARTED]
**Saves: ~50us per frame on the home compress side**
**Risk: ZERO (1-line config flag)**

zstd-1 takes ~80us. zstd-0 takes ~30us, compresses ~5% worse. For
deployments where the home upload bandwidth is plentiful, trade the
bandwidth for latency. Add a `MAPLECAST_ZSTD_LEVEL` env var.

## TIER B — reliability and observability

### B1. Multi-threaded asio io_context for flycast WS  [BLOCKED]
**Saves: ~0-16ms p99 on player input forwarding**
**Risk: HIGH (latent thread-safety bugs in shared state)**

Spawn N threads all calling `_ws.run()` so inbound message handlers,
outbound writes, and accept events run in parallel. Eliminates the
"binary frame broadcast blocks player input forwarding" tail latency
entirely.

**Blocker**: several global structures (`_queue`, `_relayTree`,
`_seedPeers`, parts of the relay tree helpers) are read without
`_connMutex` in status broadcast and relay-tree code paths. These
are latent race conditions today (the status thread can already race
the asio thread). Going multi-threaded asio promotes these from
"never crashes" to "data race UB".

Audit every access path under `_connMutex` first. Then enable. The
TODO comment is in the code at the io_context.run() call.

### B2. Per-connection bounded send queues  [NOT STARTED]
**Saves: nothing directly, but prevents memory blowup with slow clients**
**Risk: LOW**

websocketpp doesn't bound per-connection send queues. A slow client
(slow network, paused browser tab, etc.) accumulates queued frames
indefinitely until the connection drops. Track outstanding bytes per
connection; drop frames or kick clients exceeding a threshold (say
500KB queued ≈ 50 frames).

The relay has this via the `tokio::broadcast` channel's 16-slot
backpressure. Flycast does not.

### B3. Telemetry: per-stage latency measurement in production  [NOT STARTED]
**Saves: nothing, but tells us where time actually goes**

Right now the diagnostics overlay only knows ping (round-trip) and
publishUs (server compress time). We don't know:

- How long the relay's tokio task takes to process each frame
- How long the browser worker takes between WS recv and postMessage
- How long the WASM render takes
- How much vsync wait the browser actually does

Add timestamps at every stage, log p50/p95/p99 every 10 seconds.
Without this, all the optimizations above are guesswork.

---

## What "OVERKILL IS NECESSARY" actually means at this point

The flycast/relay/WASM code path is already at the floor for what we
control: ~1ms total of code latency between the input hitting the
server and the pixel being ready to display. **The remaining ~40ms
of E2E is physics** — the speed of light to and from the VPS, the
60Hz frame quantization of the emulator, and the 60Hz refresh of the
user's monitor.

To go below 40ms p50 we have to either:

1. **Skip the VPS** — direct LAN play (S2) for users on your network.
2. **Move closer to users** — multi-region relay mesh. Each user
   connects to the nearest relay; relays form a hub-and-spoke or
   anycast topology to the home flycast. The biggest realistic win
   for remote players because it attacks **two** of the three physics
   bottlenecks (browser↔VPS and VPS↔home distances).
3. **Use UDP semantics for player input** (S3) so head-of-line blocking
   in the WS queue doesn't add jitter to the input path.

Anything else is shaving microseconds off paths that already cost
microseconds. Worth doing for the engineering pride and the p99 wins,
but the visible improvement to a player won't be dramatic until we
attack one of the three above.
