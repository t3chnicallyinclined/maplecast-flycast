// ============================================================================
// RENDER-WORKER.MJS — Dedicated render thread for the WASM mirror renderer
//
// Owns:
//   1. The transferred OffscreenCanvas (received via postMessage from main)
//   2. The WASM renderer module (instantiated here, NOT on the main thread)
//   3. A WebSocket connection to the relay/upstream (binary frames only)
//   4. The latest pending frame (single-slot, newest wins)
//   5. Frame/jitter telemetry counters
//
// Drain model:
//   - Frames arrive over WS at ~60Hz with network jitter
//   - We DO NOT render on arrival. Just store as pending.
//   - The main thread sends a "tick" message every requestAnimationFrame.
//     On tick, we drain the latest pending frame and call _renderer_frame().
//   - This phase-aligns Present() with the compositor's vsync, identical to
//     the Fix 1 model — except now Process/Render/Present runs on the worker
//     thread, fully isolated from main-thread JSON / DOM / chat work.
//
// SYNC bypasses the rAF tick — it's a one-shot init payload that must apply
// before any subsequent delta. Renders immediately on receipt.
//
// Telemetry:
//   - Counters live in this worker. Main thread polls them via {type:'telemetry'}.
//   - Per-frame state is not duplicated to main on every frame (would waste
//     postMessage bandwidth). Main pulls every 1s for diag overlay + every 5s
//     for relay reporting.
// ============================================================================

// CRITICAL: install OffscreenCanvas WebGL2 patches BEFORE the renderer loads.
// The flycast GLES renderer calls ctx.enable(GL_FOG) and ctx.enable(GL_ALPHA_TEST)
// (DC fixed-function legacy that's #if-d out on real GL but still emitted in
// the rgl* code paths we link). WebGL2 rejects those enums with INVALID_ENUM.
// We install a Set-based filter on enable/disable + suppress INVALID_ENUM in
// getError so the renderer is none the wiser.
//
// On the main thread this is done by webgl-patches.mjs patching
// HTMLCanvasElement.prototype.getContext. The worker doesn't have
// HTMLCanvasElement — it has OffscreenCanvas instead. Same patch, different
// prototype. Must apply BEFORE createRenderer() because WASM module load
// triggers getContext() during emscripten_webgl_create_context.
// Stashed reference to the WebGL2 context, captured by the patched getContext
// below. Used after each SYNC to re-enable the GL caps the GLES renderer
// assumes are on (BLEND/DEPTH_TEST/STENCIL_TEST/SCISSOR_TEST). Same workaround
// as the pre-Fix-2 main-thread path in renderer-bridge.mjs.
let _gl = null;

(function patchOffscreenCanvas() {
  if (typeof OffscreenCanvas === 'undefined') return;
  const orig = OffscreenCanvas.prototype.getContext;
  OffscreenCanvas.prototype.getContext = function(type, attrs) {
    const ctx = orig.call(this, type, attrs);
    if (ctx && (type === 'webgl2' || type === 'webgl') && !ctx._patched) {
      ctx._patched = true;
      _gl = ctx;  // stash for post-SYNC cap re-enable in drainPending()
      const VALID = new Set([ctx.BLEND, ctx.CULL_FACE, ctx.DEPTH_TEST, ctx.DITHER,
        ctx.POLYGON_OFFSET_FILL, ctx.SAMPLE_ALPHA_TO_COVERAGE, ctx.SAMPLE_COVERAGE,
        ctx.SCISSOR_TEST, ctx.STENCIL_TEST, ctx.RASTERIZER_DISCARD]);
      const origEnable = ctx.enable.bind(ctx);
      ctx.enable = (cap) => { if (VALID.has(cap)) origEnable(cap); };
      const origDisable = ctx.disable.bind(ctx);
      ctx.disable = (cap) => { if (VALID.has(cap)) origDisable(cap); };
      const origGetError = ctx.getError.bind(ctx);
      ctx.getError = () => { const err = origGetError(); return err === ctx.INVALID_ENUM ? ctx.NO_ERROR : err; };
      const origTexParameteri = ctx.texParameteri.bind(ctx);
      ctx.texParameteri = (t, p, v) => { if (p !== 0x84FE) origTexParameteri(t, p, v); };
      const origTexParameterf = ctx.texParameterf.bind(ctx);
      ctx.texParameterf = (t, p, v) => { if (p !== 0x84FE) origTexParameterf(t, p, v); };
      console.log('[render-worker] OffscreenCanvas WebGL2 patches installed');
    }
    return ctx;
  };
})();

import createRenderer from '../renderer.mjs';

const MAX_FRAME = 512 * 1024;
const SYNC_BUF_SIZE = 16 * 1024 * 1024;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let _wasm = null;
let _frameBuf = 0;       // wasm heap pointer for delta/keyframe payloads
let _syncBuf = 0;        // wasm heap pointer for SYNC payloads (16MB)
let _initialized = false;
let _glStateInit = false;

// Coalescing slot — newest pending frame wins
let _pendingFrame = null;
let _pendingLen = 0;

// WebSocket — video/TA mirror pipe
let _ws = null;
let _wsUrl = null;
let _wsReconnectTimer = null;

// Second WebSocket — dedicated to PCM audio. Runs on its own TCP socket to
// its own server-side io_service thread so audio sends can NEVER head-of-line
// block video frames on the shared wire. See core/network/maplecast_audio_ws.h.
let _audioWs = null;
let _audioWsUrl = null;
let _audioWsReconnectTimer = null;

// Direct MessagePort to the pcm-worklet, handed in by the main thread after
// the AudioContext unlocks. When set, audio packets are shipped here with
// zero hops through the main thread — the main thread's rAF loop is never
// touched on the audio path. Until it's set, audio is dropped on the floor
// (we do NOT fall back to posting to main, because that's the whole bug
// we're fixing: main-thread postMessage for every audio chunk starves rAF).
let _audioPort = null;

// Telemetry counters — read by main via {type:'telemetry'}
//
// TWO INDEPENDENT TIMING METRICS:
//
//   1. RENDER interval (intervalSumUs/Count/Max) — measured in drainPending().
//      This is the time between successive _renderer_frame() calls. Because
//      we drain on rAF (vsync-locked), this is essentially always ~16.67ms.
//      It tells us "is the worker keeping up with vsync".
//
//   2. ARRIVAL interval (arrivalSumUs/Count/Max) — measured in handleBinaryFrame.
//      This is the wall-clock gap between WebSocket binary frames landing on
//      the worker. THIS is the real network jitter — bursty arrivals here mean
//      the upstream pacing or relay is uneven, even if the renderer hides it.
//
// They are NOT the same number. If they ever look identical in Grafana, the
// arrival path is being throttled by something (e.g. main-thread blocking).
const _telemetry = {
  framesRendered: 0,
  framesDropped: 0,
  bytesReceived: 0,
  syncCount: 0,
  // Render interval (drain-to-drain, vsync-locked)
  lastFrameAt: 0,
  intervalSumUs: 0,
  intervalCount: 0,
  intervalMaxUs: 0,
  // Arrival interval (WS frame-to-frame, network jitter)
  lastArrivalAt: 0,
  arrivalSumUs: 0,
  arrivalCount: 0,
  arrivalMaxUs: 0,
};

// ---------------------------------------------------------------------------
// Init: receive OffscreenCanvas + WS URL from main, instantiate WASM module
// ---------------------------------------------------------------------------

self.onmessage = async (e) => {
  const msg = e.data;
  switch (msg.type) {
    case 'init':         return handleInit(msg);
    case 'tick':         return drainPending();
    case 'set_opt':      return handleSetOpt(msg);
    case 'resize':       return handleResize(msg);
    case 'telemetry':    return handleTelemetryRequest(msg);
    case 'binary_frame': return handleBinaryFrame(msg.buffer);  // P2P forward
    case 'audio_port':   _audioPort = msg.port; return;         // direct-to-worklet port
    case 'shutdown':     return handleShutdown();
  }
};

async function handleInit(msg) {
  if (_initialized) return;

  const offscreen = msg.canvas;        // transferred OffscreenCanvas
  _wsUrl = msg.wsUrl;
  _audioWsUrl = msg.audioWsUrl || null;  // optional — main thread resolves it
  const initialW = msg.width || 640;
  const initialH = msg.height || 480;

  // Set the OffscreenCanvas dimensions BEFORE the WASM module loads.
  // The C side will call emscripten_set_canvas_element_size("#canvas", w, h)
  // which writes back to this same canvas — but doing it here ensures the
  // initial getContext('webgl2') sees a sensibly sized backbuffer.
  offscreen.width = initialW;
  offscreen.height = initialH;

  try {
    // Quiet the per-frame WASM printfs from console. wasm_bridge.cpp logs
    // KEYFRAME / DELTA / SYNC / palette / FB lines on every frame as a dev
    // aid; in production they drown out everything else (60 lines/sec).
    // Flip VERBOSE_RENDERER = true here when debugging the renderer itself.
    const VERBOSE_RENDERER = false;
    const NOISY_PATTERNS = /KEYFRAME |DELTA dropped|SYNC applied|PAL_RAM_CTRL|palette\d+\[|Computed FB:/;
    const filteredLog = (s) => {
      if (!VERBOSE_RENDERER && NOISY_PATTERNS.test(s)) return;
      console.log('[render-worker]', s);
    };

    const Module = await createRenderer({
      // findCanvasEventTarget("#canvas") in libhtml5.js will resolve to
      // Module.canvas. The OffscreenCanvas supports getContext('webgl2'),
      // which is what GL.createContext() will call internally.
      canvas: offscreen,
      // Worker-relative path so emscripten finds the .wasm sibling
      locateFile: (path) => `../${path}?v=worker15`,
      print:    filteredLog,
      printErr: (s) => console.warn('[render-worker]', s),
    });

    if (!Module._renderer_init(initialW, initialH)) {
      throw new Error('renderer_init returned 0');
    }

    _wasm = Module;
    _frameBuf = Module._malloc(MAX_FRAME);
    _syncBuf  = Module._malloc(SYNC_BUF_SIZE);
    _initialized = true;

    self.postMessage({ type: 'ready' });
    console.log('[render-worker] WASM initialized in worker');

    // Now open the WS — the renderer is ready to consume frames.
    connectWS();

    // Also open the dedicated audio WS. It runs completely independently
    // from the video pipe: its own TCP socket, its own reconnect loop, its
    // own server-side io_service thread. Audio frames go straight from
    // _audioWs.onmessage → _audioPort (the pcm-worklet) with zero detour
    // through WASM, rAF, or the main thread.
    if (_audioWsUrl) connectAudioWS();
  } catch (err) {
    console.error('[render-worker] init failed:', err && err.message);
    self.postMessage({ type: 'error', message: 'renderer init failed: ' + (err && err.message) });
  }
}

function handleSetOpt(msg) {
  if (!_wasm) return;
  const val = parseInt(msg.value, 10);
  _wasm._renderer_set_option(msg.opt, val);
  if (msg.opt === 0) {
    // Resolution change — recompute width/height (4:3) and resize.
    const h = val;
    const w = Math.round(h * 4 / 3);
    _wasm._renderer_resize(w, h);
  }
  _glStateInit = false;
}

function handleResize(msg) {
  if (!_wasm) return;
  _wasm._renderer_resize(msg.width, msg.height);
}

function handleTelemetryRequest(msg) {
  // Snapshot + reset semantics depend on caller. Main sends:
  //   {type:'telemetry', resetIntervals:true}  → telemetry.mjs 5s pull
  //   {type:'telemetry'}                       → diagnostics 1s pull
  // We send back the current counters; if resetIntervals is set we zero BOTH
  // the render interval AND the arrival interval rolling windows so the next
  // pull reflects only the new 5s window.
  self.postMessage({
    type: 'telemetry',
    framesRendered: _telemetry.framesRendered,
    framesDropped:  _telemetry.framesDropped,
    bytesReceived:  _telemetry.bytesReceived,
    syncCount:      _telemetry.syncCount,
    // Render interval (drain cadence)
    intervalSumUs:  _telemetry.intervalSumUs,
    intervalCount:  _telemetry.intervalCount,
    intervalMaxUs:  _telemetry.intervalMaxUs,
    // Arrival interval (network jitter)
    arrivalSumUs:   _telemetry.arrivalSumUs,
    arrivalCount:   _telemetry.arrivalCount,
    arrivalMaxUs:   _telemetry.arrivalMaxUs,
  });
  if (msg.resetIntervals) {
    _telemetry.intervalSumUs = 0;
    _telemetry.intervalCount = 0;
    _telemetry.intervalMaxUs = 0;
    _telemetry.arrivalSumUs = 0;
    _telemetry.arrivalCount = 0;
    _telemetry.arrivalMaxUs = 0;
  }
}

function handleShutdown() {
  if (_ws) try { _ws.close(); } catch {}
  _ws = null;
  if (_wsReconnectTimer) clearTimeout(_wsReconnectTimer);
  if (_audioWs) try { _audioWs.close(); } catch {}
  _audioWs = null;
  if (_audioWsReconnectTimer) clearTimeout(_audioWsReconnectTimer);
  _pendingFrame = null;
  _pendingLen = 0;
}

// ---------------------------------------------------------------------------
// WebSocket — owned by the worker, never crosses to main thread
// ---------------------------------------------------------------------------

function connectWS() {
  if (!_wsUrl) return;
  if (_ws) try { _ws.close(); } catch {}

  const ws = new WebSocket(_wsUrl);
  ws.binaryType = 'arraybuffer';
  _ws = ws;

  ws.onopen = () => {
    self.postMessage({ type: 'ws_open' });
  };

  ws.onclose = () => {
    self.postMessage({ type: 'ws_close' });
    _wsReconnectTimer = setTimeout(connectWS, 2000);
  };

  ws.onerror = () => {
    // onclose will fire and trigger reconnect
  };

  ws.onmessage = (e) => {
    if (typeof e.data === 'string') return;  // ignore JSON on the binary pipe
    handleBinaryFrame(e.data);
  };
}

// Dedicated audio WebSocket. Binary-only, one-directional (server → client).
// Every incoming message is a PCM chunk with the 4-byte [0xAD][0x10][seq]
// header followed by 1024 × int16 stereo samples. We ship it straight to
// the pcm-worklet via _audioPort. If _audioPort isn't set yet (the user
// hasn't clicked anything to unlock AudioContext), we drop silently — the
// browser wouldn't play audio without a gesture anyway.
//
// This WS is 100% isolated from the video pipe:
//   - different TCP socket to server
//   - different server-side io_service thread
//   - different asio event loop
//   - different reconnect state machine here in the worker
// The only shared state is _audioPort (a MessagePort held by this worker).
function connectAudioWS() {
  if (!_audioWsUrl) return;
  if (_audioWs) try { _audioWs.close(); } catch {}

  const ws = new WebSocket(_audioWsUrl);
  ws.binaryType = 'arraybuffer';
  _audioWs = ws;

  ws.onopen = () => {
    console.log('[render-worker] audio WS connected');
  };

  ws.onclose = () => {
    console.log('[render-worker] audio WS closed, reconnecting in 2s');
    _audioWsReconnectTimer = setTimeout(connectAudioWS, 2000);
  };

  ws.onerror = () => {
    // onclose fires and handles reconnect
  };

  ws.onmessage = (e) => {
    if (typeof e.data === 'string') return;
    const buffer = e.data;
    if (buffer.byteLength < 6) return;
    // Confirm magic before forwarding to the worklet port. Anything that
    // isn't 0xAD 0x10 is unexpected noise on this channel and we drop it.
    const u8 = new Uint8Array(buffer, 0, 2);
    if (u8[0] !== 0xAD || u8[1] !== 0x10) return;
    if (_audioPort) {
      try {
        const pcm = new Int16Array(buffer, 4);
        _audioPort.postMessage(pcm);
      } catch {}
    }
  };
}

// ---------------------------------------------------------------------------
// Frame intake — SYNC renders immediately, deltas coalesce for rAF drain
// ---------------------------------------------------------------------------

function handleBinaryFrame(buffer) {
  const len = buffer.byteLength;
  if (len < 4) return;

  // Audio now rides a dedicated WS — see connectAudioWS(). If something
  // ever sends a 0xAD 0x10 packet on the video pipe anyway, drop it fast
  // so we don't confuse the frame parser below.
  const hdr0 = new Uint8Array(buffer, 0, 2);
  if (hdr0[0] === 0xAD && hdr0[1] === 0x10) return;

  // Everything below is video — needs the WASM renderer initialized.
  if (!_initialized || !_wasm) return;
  if (len < 8) return;

  // SYNC detection — magic + size as in renderer-bridge.mjs
  const dv = new DataView(buffer);
  const magic = dv.getUint32(0, true);
  const isSYNC = magic === 0x434E5953;  // "SYNC"
  const isZCST = magic === 0x5453435A;  // "ZCST"
  const isCompressedSync = isZCST && len >= 8 && dv.getUint32(4, true) > 1024 * 1024;

  if (isSYNC || isCompressedSync) {
    if (len <= SYNC_BUF_SIZE) {
      _wasm.HEAPU8.set(new Uint8Array(buffer), _syncBuf);
      _wasm._renderer_sync(_syncBuf, len);
    } else {
      const tmp = _wasm._malloc(len);
      _wasm.HEAPU8.set(new Uint8Array(buffer), tmp);
      _wasm._renderer_sync(tmp, len);
      _wasm._free(tmp);
    }
    _telemetry.syncCount++;
    _telemetry.bytesReceived += len;
    _glStateInit = false;
    self.postMessage({ type: 'sync_applied' });
    return;
  }

  // Delta/keyframe — DIAGNOSTIC: process every frame inline as it arrives,
  // no pending coalescing. The previous "newest wins" model dropped any
  // older pending frame when a new one arrived before the next rAF tick,
  // which silently lost dirty pages on scene transitions when frames
  // piled up. Lost dirty pages are unrecoverable (the server only re-ships
  // pages on memcmp delta), causing wasm-only scene-revisit garble.
  _telemetry.bytesReceived += len;

  // GL state init — same one-shot setup that drainPending() used to do.
  if (!_glStateInit && _gl) {
    _gl.enable(_gl.BLEND);
    _gl.enable(_gl.DEPTH_TEST);
    _gl.enable(_gl.STENCIL_TEST);
    _gl.enable(_gl.SCISSOR_TEST);
    _glStateInit = true;
  }

  // Oversized-frame fallback — mirrors the SYNC path above. The 512KB
  // _frameBuf fits the in-match steady state (taSize ~230KB + 21 dirty
  // pages = ~80KB header), but the post-scene-change keyframe that the
  // server emits right after a SYNC carries 300+ dirty pages plus a fresh
  // TA buffer. That envelope routinely exceeds 512KB compressed. Silently
  // dropping it (the previous behaviour) was the wasm scene-change garble
  // root cause: the wasm received the SYNC, cleared _prevTA, then NEVER
  // saw the keyframe and got stuck dropping deltas in the empty-prevTA
  // branch until the next periodic safety SYNC. Use a temp malloc for
  // the rare giant frame so we never lose one.
  if (len > MAX_FRAME) {
    const tmp = _wasm._malloc(len);
    _wasm.HEAPU8.set(new Uint8Array(buffer), tmp);
    _wasm._renderer_frame(tmp, len);
    _wasm._free(tmp);
  } else {
    _wasm.HEAPU8.set(new Uint8Array(buffer), _frameBuf);
    _wasm._renderer_frame(_frameBuf, len);
  }
  _telemetry.framesRendered++;

  // Arrival jitter telemetry (kept for diagnostics)
  const arrivedAt = performance.now();
  if (_telemetry.lastArrivalAt > 0) {
    const arrivalUs = (arrivedAt - _telemetry.lastArrivalAt) * 1000;
    _telemetry.arrivalSumUs += arrivalUs;
    _telemetry.arrivalCount++;
    if (arrivalUs > _telemetry.arrivalMaxUs) _telemetry.arrivalMaxUs = arrivalUs;
  }
  _telemetry.lastArrivalAt = arrivedAt;
}

// ---------------------------------------------------------------------------
// Drain — called when main posts a 'tick' (one per main-thread rAF)
// ---------------------------------------------------------------------------

function drainPending() {
  if (!_initialized || !_wasm) return;
  const buffer = _pendingFrame;
  const len = _pendingLen;
  if (!buffer) return;
  _pendingFrame = null;
  _pendingLen = 0;

  // GL state — set once after each SYNC. The flycast GLES renderer assumes
  // BLEND/DEPTH_TEST/STENCIL_TEST/SCISSOR_TEST start enabled because OpenGL
  // ES leaves them at their default-on state in some contexts. WebGL2 starts
  // them at the GL spec defaults (off). flycast's glcache caches the assumed
  // state, so the first time it tries to "disable BLEND" it skips the call
  // because it thinks BLEND was already off. Pre-empt that by enabling them
  // outside the cache, then glcache catches up on its first toggle.
  // Same workaround as pre-Fix-2 renderer-bridge.mjs handleBinaryFrame().
  if (!_glStateInit && _gl) {
    _gl.enable(_gl.BLEND);
    _gl.enable(_gl.DEPTH_TEST);
    _gl.enable(_gl.STENCIL_TEST);
    _gl.enable(_gl.SCISSOR_TEST);
    _glStateInit = true;
  }

  _wasm.HEAPU8.set(new Uint8Array(buffer), _frameBuf);
  _wasm._renderer_frame(_frameBuf, len);

  // Telemetry — inter-render-tick interval
  // (using performance.now() — workers have it)
  const now = performance.now();
  if (_telemetry.lastFrameAt > 0) {
    const intervalUs = (now - _telemetry.lastFrameAt) * 1000;
    _telemetry.intervalSumUs += intervalUs;
    _telemetry.intervalCount++;
    if (intervalUs > _telemetry.intervalMaxUs) _telemetry.intervalMaxUs = intervalUs;
  }
  _telemetry.lastFrameAt = now;
  _telemetry.framesRendered++;
}
