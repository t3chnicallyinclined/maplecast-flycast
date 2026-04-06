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

// WebSocket
let _ws = null;
let _wsUrl = null;
let _wsReconnectTimer = null;

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
    case 'shutdown':     return handleShutdown();
  }
};

async function handleInit(msg) {
  if (_initialized) return;

  const offscreen = msg.canvas;        // transferred OffscreenCanvas
  _wsUrl = msg.wsUrl;
  const initialW = msg.width || 640;
  const initialH = msg.height || 480;

  // Set the OffscreenCanvas dimensions BEFORE the WASM module loads.
  // The C side will call emscripten_set_canvas_element_size("#canvas", w, h)
  // which writes back to this same canvas — but doing it here ensures the
  // initial getContext('webgl2') sees a sensibly sized backbuffer.
  offscreen.width = initialW;
  offscreen.height = initialH;

  try {
    const Module = await createRenderer({
      // findCanvasEventTarget("#canvas") in libhtml5.js will resolve to
      // Module.canvas. The OffscreenCanvas supports getContext('webgl2'),
      // which is what GL.createContext() will call internally.
      canvas: offscreen,
      // Worker-relative path so emscripten finds the .wasm sibling
      locateFile: (path) => `../${path}?v=worker1`,
      print:    (s) => console.log('[render-worker]', s),
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

// ---------------------------------------------------------------------------
// Frame intake — SYNC renders immediately, deltas coalesce for rAF drain
// ---------------------------------------------------------------------------

function handleBinaryFrame(buffer) {
  if (!_initialized || !_wasm) return;
  const len = buffer.byteLength;
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

  // Delta/keyframe — store as pending. Newest wins; older pending = drop.
  if (len > MAX_FRAME) return;
  if (_pendingFrame !== null) _telemetry.framesDropped++;
  _pendingFrame = buffer;
  _pendingLen = len;
  _telemetry.bytesReceived += len;

  // Network jitter — measured at the moment WS hands us the frame.
  // This is independent of the rAF drain cadence (which is vsync-locked).
  // If THIS is uneven, the relay→browser hop has jitter. If this is even
  // but render interval is uneven, the rAF tick is being delayed.
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
