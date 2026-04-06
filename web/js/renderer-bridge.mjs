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

export async function initRenderer() {
  try {
    const Module = await createRenderer({
      canvas: document.getElementById('game-canvas'),
      locateFile: (path) => `./${path}?v=zstd1`,
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
