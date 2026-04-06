// ============================================================================
// RENDERER-BRIDGE.MJS — WASM renderer init + OVERKILL hot path
//
// FRAME PACING STRATEGY — ZERO LATENCY:
//   Render IMMEDIATELY on WS frame arrival (zero buffer, zero delay).
//   rAF loop acts as SAFETY NET only — if no new frame arrived since last
//   vsync, the browser holds the last rendered frame (WebGL double-buffer).
//   This gives us immediate rendering + smooth display without any added
//   latency. 2 frames of latency kills you in MVC2.
//
//   Network jitter is absorbed by the DISPLAY, not a buffer:
//   - Frame arrives early? Render it, browser presents at next vsync.
//   - Frame arrives late? Previous frame holds on screen (no blank).
//   - Two frames in one tick? Both render, browser shows the latest.
//
// OVERKILL IS NECESSARY.
// ============================================================================

import { state } from './state.mjs';
import createRenderer from '../renderer.mjs';
import { saveRenderSettings } from './settings.mjs';

const MAX_FRAME = 512 * 1024;
const SYNC_BUF_SIZE = 16 * 1024 * 1024;

// Module-local hot path cache
let _wasm = null;
let _buf = null;
let _gl = null;
let _syncBuf = null;
let _glStateInit = false;

// Telemetry counters — read by telemetry.mjs each tick. Module-local to keep
// the hot path branch-free (no state.diag lookups per frame).
export const _telemetry = {
  framesRendered: 0,        // total frames pushed to GPU since init
  bytesReceived: 0,         // total bytes of frame data
  syncCount: 0,             // total SYNCs applied
  lastFrameAt: 0,           // performance.now() of last frame render
  lastSyncAt: 0,
  // Inter-frame interval rolling stats (microseconds)
  intervalSumUs: 0,
  intervalCount: 0,
  intervalMaxUs: 0,
};

export async function initRenderer() {
  try {
    const Module = await createRenderer({
      canvas: document.getElementById('game-canvas'),
      // !!! BUMP THIS STRING after every renderer.wasm rebuild !!!
      // nginx serves the wasm with max-age + ETag. Without bumping the query
      // string, browsers will serve a cached wasm and your fix won't appear.
      // (See big comment block above handleBinaryFrame for the full story.)
      locateFile: (path) => `./${path}?v=parity1`,
    });

    if (!Module._renderer_init(640, 480)) throw new Error('renderer_init failed');

    state.frameBuf = Module._malloc(MAX_FRAME);
    _syncBuf = Module._malloc(SYNC_BUF_SIZE);

    state.wasmModule = Module;
    _wasm = Module;
    _buf = state.frameBuf;
    _gl = state.glCtx;

    const canvas = document.getElementById('game-canvas');
    canvas.style.width = '100%';
    canvas.style.height = '100%';

    state.connState = 'RENDERER OK';
    console.log('[renderer] WASM initialized — zero-latency immediate render');
  } catch (err) {
    console.error('[renderer] Failed to load:', err);
    state.connState = 'NO RENDERER';
  }
}

// ============================================================================
// HOT PATH — render IMMEDIATELY on WS frame arrival. Zero buffer.
// Every frame from the server hits the GPU the instant it arrives.
// Browser compositor double-buffers and presents at vsync automatically.
// ============================================================================
//
// !!! FRAGILE — READ BEFORE EDITING !!!
//
// This is the JS half of the king.html render path. The C++ half lives in
// packages/renderer/src/wasm_bridge.cpp (renderer_frame / renderer_sync).
// They MUST stay aligned with the wire format defined by the server in
// core/network/maplecast_mirror.cpp serverPublish().
//
// Common pitfalls:
//
//   1. CACHE BUSTING. After ANY rebuild of renderer.wasm, bump the `?v=...`
//      string in locateFile() above. Without that, browsers will cheerfully
//      serve a stale wasm forever (nginx caches with ETag, Chrome
//      respects max-age, and EmulatorJS keeps its own IndexedDB cache).
//      You will swear your fix didn't land. It did. The browser is lying.
//
//   2. SYNC vs DELTA discrimination is by magic + size. SYNC = "SYNC" or
//      "ZCST" with uncompressedSize > 1MB. DELTA = anything else with ZCST
//      or raw bytes. Don't add a third type without updating server +
//      desktop client + both WASM bridges + the Rust relay.
//
//   3. MAX_FRAME = 512KB. Compressed deltas are 6-15KB so this is fine.
//      If you ever see uncompressed deltas, this limit will silently drop
//      frames > 512KB. Compressed SYNC uses _syncBuf (16MB), separate.
//
//   4. The wasm bridge expects the buffer at offset _buf to contain raw wire
//      bytes — DO NOT pre-decompress or pre-parse them in JS. The WASM does
//      its own zstd decode. relay.js comments say so explicitly.
//
//   5. Black screen with one SYNC log and no KEYFRAME log usually means the
//      RELAY has lost its upstream connection to home flycast — not a JS or
//      WASM bug. Check `ssh root@66.55.128.93 journalctl -u maplecast-relay`.
//      Look for "Lost upstream connection" or "Connection refused".
//
// See docs/ARCHITECTURE.md "Mirror Wire Format — Rules of the Road" for the
// canonical list of rules all four parsers must obey.
// ============================================================================

export function handleBinaryFrame(buffer) {
  if (!_wasm || !_buf) return;

  const len = buffer.byteLength;
  if (len < 8) return;

  // SYNC detection — check for raw "SYNC" or ZCST-compressed SYNC
  const dv = new DataView(buffer);
  const magic = len >= 4 ? dv.getUint32(0, true) : 0;
  const isSYNC = magic === 0x434E5953; // "SYNC" little-endian
  const isZCST = magic === 0x5453435A; // "ZCST" little-endian
  // ZCST with uncompressedSize > 1MB = compressed SYNC
  const isCompressedSync = isZCST && len >= 8 && dv.getUint32(4, true) > 1024 * 1024;

  if (isSYNC || isCompressedSync) {
    const data = new Uint8Array(buffer);
    if (len <= SYNC_BUF_SIZE) {
      _wasm.HEAPU8.set(data, _syncBuf);
      _wasm._renderer_sync(_syncBuf, len);
    } else {
      const tmp = _wasm._malloc(len);
      _wasm.HEAPU8.set(data, tmp);
      _wasm._renderer_sync(tmp, len);
      _wasm._free(tmp);
    }

    state.connState = 'SYNC';
    document.getElementById('idleScreen').style.display = 'none';
    document.body.classList.add('streaming');
    state.rendererStreaming = true;
    _glStateInit = false;
    _telemetry.syncCount++;
    _telemetry.lastSyncAt = performance.now();
    _telemetry.bytesReceived += len;
    return;
  }

  // Delta/keyframe — immediate render
  if (len > MAX_FRAME) return;
  _wasm.HEAPU8.set(new Uint8Array(buffer), _buf);

  // GL state — set once after SYNC
  if (!_glStateInit && _gl) {
    _gl.enable(_gl.BLEND);
    _gl.enable(_gl.DEPTH_TEST);
    _gl.enable(_gl.STENCIL_TEST);
    _gl.enable(_gl.SCISSOR_TEST);
    _glStateInit = true;
  }

  _wasm._renderer_frame(_buf, len);
  state.connState = 'LIVE';

  // Telemetry — track render rate and inter-frame jitter
  const now = performance.now();
  if (_telemetry.lastFrameAt > 0) {
    const intervalUs = (now - _telemetry.lastFrameAt) * 1000;
    _telemetry.intervalSumUs += intervalUs;
    _telemetry.intervalCount++;
    if (intervalUs > _telemetry.intervalMaxUs) _telemetry.intervalMaxUs = intervalUs;
  }
  _telemetry.lastFrameAt = now;
  _telemetry.framesRendered++;
  _telemetry.bytesReceived += len;
}

export function setOpt(opt, val) {
  if (!_wasm) return;
  val = parseInt(val);
  _wasm._renderer_set_option(opt, val);
  if (opt === 0) {
    const h = val;
    const w = Math.round(h * 4 / 3);
    _wasm._renderer_resize(w, h);
    const canvas = document.getElementById('game-canvas');
    canvas.style.width = '100%';
    canvas.style.height = '100%';
  }
  _glStateInit = false;
  saveRenderSettings();
}
