// ============================================================================
// RENDERER-BRIDGE.MJS — Main-thread proxy for the render worker
//
// FIX 2 ARCHITECTURE — RENDER WORKER + OFFSCREEN CANVAS:
//   This file no longer instantiates the WASM module on the main thread.
//   It spawns render-worker.mjs, transfers an OffscreenCanvas to it, and
//   acts as a thin postMessage bridge for the rest of the app.
//
//   The worker owns:
//     - The WASM module + WebGL2 context (rendering to OffscreenCanvas)
//     - The WebSocket binary frame pipe
//     - The latest pending frame slot + drain logic
//     - Telemetry counters
//
//   Main thread is responsible for:
//     - Spawning the worker, transferring the canvas
//     - Driving requestAnimationFrame: each rAF tick → postMessage('tick')
//       to the worker, which drains its latest pending frame and presents.
//     - Forwarding user-driven config changes (setOpt, resize) to the worker
//     - Polling the worker for telemetry every ~250ms so diagnostics.mjs and
//       telemetry.mjs can keep their existing sync read interface.
//
// Why rAF stays on main: workers don't have requestAnimationFrame. The main
// thread is the only place that can hand us a vsync-aligned tick. The actual
// render work runs in the worker, but the *decision to render* still rides
// on the main-thread rAF. This is the documented Emscripten pattern for
// non-pthread OffscreenCanvas rendering.
//
// Why this is still a big win over Fix 1:
//   - Process/Render/Present (~700-1090us) runs in the worker, not main.
//     Main-thread rAF callback is now just a postMessage (~10us).
//   - The WebSocket binary pipe lives in the worker too — frame intake never
//     hits the main event loop, so chat/JSON/DOM bursts can't delay frame
//     receipt or coalescing.
//   - Telemetry is pulled, not pushed-per-frame, so we don't postMessage on
//     every frame.
// ============================================================================

import { state } from './state.mjs';
import { saveRenderSettings } from './settings.mjs';

// MAX_FRAME / SYNC_BUF_SIZE constants are kept in render-worker.mjs.
// This file owns nothing wasm-related directly anymore.

// ---- Worker handle + ready state ----
let _worker = null;
let _workerReady = false;        // _renderer_init returned ok in worker
let _wsConnected = false;        // worker's WebSocket reported open

// ---- rAF tick driver ----
let _rafScheduled = false;

// ---- Telemetry cache, refreshed by polling the worker ----
//
// The interface intentionally matches the pre-Fix-2 _telemetry shape so that
// diagnostics.mjs and telemetry.mjs can keep their synchronous reads. Values
// are refreshed every ~250ms by _pollTelemetry() — fresh enough for a 1s
// diag tick and a 5s relay-report tick.
//
// Two interval metrics are surfaced (see render-worker.mjs for the full
// rationale):
//   - intervalSumUs/Count/Max  : render drain interval (vsync-locked, ~16.67ms)
//   - arrivalSumUs/Count/Max   : WS frame arrival interval (real network jitter)
export const _telemetry = {
  framesRendered: 0,
  framesDropped: 0,
  bytesReceived: 0,
  syncCount: 0,
  lastFrameAt: 0,        // unused on main now (worker tracks it) — kept for compat
  lastSyncAt: 0,
  // Render drain interval (vsync cadence)
  intervalSumUs: 0,
  intervalCount: 0,
  intervalMaxUs: 0,
  // WS arrival interval (network jitter)
  arrivalSumUs: 0,
  arrivalCount: 0,
  arrivalMaxUs: 0,
};

// Whether to ask the worker to reset its rolling jitter window on the next
// pull. telemetry.mjs flips this on its 5s tick — we forward to the worker,
// which is the actual owner of the counter.
let _resetIntervalsOnNextPoll = false;

// ---- Audio pipeline (PCM-over-WS, AudioWorklet) ----
//
// The flycast server taps the AICA output in WriteSample() and broadcasts
// 512-sample stereo chunks (2052-byte packets with [0xAD][0x10][seqHi][seqLo]
// header) over the same WebSocket the TA mirror video frames ride on. The
// render worker detects the magic and postMessages the raw bytes back here;
// we strip the 4-byte header, wrap the int16 PCM in a Uint8Array view, and
// send it to a pcm-worklet AudioWorkletNode running on a dedicated audio
// thread. The worklet converts int16 → float32 and feeds AudioContext.
//
// Audio plays by default. Users mute their own browser tab to silence it.
//
// Browser autoplay policy: AudioContext starts suspended until the user
// interacts with the page (click, keydown, touch). We queue incoming chunks
// in a bounded ring until the context is running, then flush them all. The
// worklet itself has a ~50ms startup buffer so a brief queue dump is fine.
let _audioCtx = null;
let _audioWorkletPort = null;
let _audioPendingChunks = [];           // buffer between worker arrival and worklet ready
let _audioCtxUnlockArmed = false;       // have we armed the user-gesture listeners?
const AUDIO_PENDING_MAX = 64;           // cap to ~730ms so a stalled unlock doesn't leak memory

async function _ensureAudio() {
  if (_audioCtx) return _audioCtx;
  try {
    // Dreamcast AICA outputs at 44100 Hz — match it so the browser doesn't
    // resample (which would introduce pitch drift). If the platform refuses
    // 44100 (some mobile browsers force 48000), fall back gracefully.
    try {
      _audioCtx = new AudioContext({ sampleRate: 44100 });
    } catch {
      _audioCtx = new AudioContext();
      console.warn('[audio] 44.1 kHz unavailable — using', _audioCtx.sampleRate, 'Hz (pitch may drift)');
    }
    await _audioCtx.audioWorklet.addModule('/pcm-worklet.js');
    const workletNode = new AudioWorkletNode(_audioCtx, 'pcm-processor', {
      outputChannelCount: [2],
    });
    workletNode.connect(_audioCtx.destination);
    _audioWorkletPort = workletNode.port;
    console.log('[audio] AudioWorklet ready — sampleRate:', _audioCtx.sampleRate,
      'state:', _audioCtx.state, 'baseLatency:', _audioCtx.baseLatency?.toFixed(4));

    // Flush any chunks we received before the worklet was ready.
    if (_audioPendingChunks.length) {
      for (const pcm of _audioPendingChunks) _audioWorkletPort.postMessage(pcm);
      console.log('[audio] flushed', _audioPendingChunks.length, 'pending chunks');
      _audioPendingChunks.length = 0;
    }
  } catch (err) {
    console.error('[audio] init failed:', err);
    _audioCtx = null;
  }
  return _audioCtx;
}

function _armAudioUnlock() {
  if (_audioCtxUnlockArmed) return;
  _audioCtxUnlockArmed = true;
  const unlock = async () => {
    // Browser autoplay policy: AudioContext starts suspended until a gesture.
    // We attempt creation+resume here so the worklet is alive and the next
    // audio chunk flows directly.
    await _ensureAudio();
    if (_audioCtx && _audioCtx.state === 'suspended') {
      try { await _audioCtx.resume(); } catch {}
    }
    if (_audioCtx && _audioCtx.state === 'running') {
      console.log('[audio] AudioContext running');
      document.removeEventListener('click', unlock);
      document.removeEventListener('keydown', unlock);
      document.removeEventListener('touchstart', unlock);
    }
  };
  document.addEventListener('click', unlock);
  document.addEventListener('keydown', unlock);
  document.addEventListener('touchstart', unlock);
}

function _handleAudioChunk(buffer) {
  // buffer is the full 2052-byte packet including the 4-byte header.
  // Skip the header and view the remaining bytes as Int16 samples.
  if (buffer.byteLength < 6) return;  // need at least 4-byte header + 1 sample
  const pcm = new Int16Array(buffer, 4);

  if (_audioWorkletPort && _audioCtx && _audioCtx.state === 'running') {
    // Hot path: worklet ready, ship it.
    _audioWorkletPort.postMessage(pcm);
    return;
  }
  // Cold path: worklet not ready yet. Queue and arm the unlock listeners.
  _armAudioUnlock();
  if (_audioPendingChunks.length >= AUDIO_PENDING_MAX) {
    _audioPendingChunks.shift();  // drop oldest to bound memory
  }
  _audioPendingChunks.push(pcm);
}

// ============================================================================
// Init — create OffscreenCanvas, spawn worker, transfer canvas, kick WS URL
// ============================================================================

export async function initRenderer() {
  try {
    const canvas = document.getElementById('game-canvas');
    if (!canvas) throw new Error('game-canvas element not found');

    // Layout: hand the canvas the same CSS we used to set on init in the
    // pre-Fix-2 code path. The DOM element keeps its CSS-driven width/height;
    // only the BACKBUFFER is transferred to the worker.
    canvas.style.width = '100%';
    canvas.style.height = '100%';

    const offscreen = canvas.transferControlToOffscreen();

    // Pick the same WebSocket URL the JSON control connection uses, so the
    // render worker hits the same relay/upstream as ws-connection.mjs. We
    // import this lazily to avoid a circular import (ws-connection imports
    // from us via state).
    const { getRendererWsUrl } = await import('./ws-connection.mjs');
    const wsUrl = await getRendererWsUrl();

    _worker = new Worker(new URL('./render-worker.mjs', import.meta.url), { type: 'module' });

    _worker.onmessage = _handleWorkerMessage;
    _worker.onerror = (e) => {
      console.error('[renderer] worker error:', e.message || e);
      state.connState = 'WORKER ERROR';
    };

    // Send the canvas + WS URL. Canvas is transferable; WS URL is a string.
    _worker.postMessage(
      { type: 'init', canvas: offscreen, wsUrl, width: 640, height: 480 },
      [offscreen]
    );

    // Start polling the worker for telemetry — fresh enough for the 1s diag
    // tick and 5s relay tick. Doesn't run until _workerReady; the handler
    // ignores telemetry messages before then.
    setInterval(_pollTelemetry, 250);

    state.connState = 'INIT WORKER...';
  } catch (err) {
    console.error('[renderer] failed to spawn render worker:', err);
    state.connState = 'NO RENDERER';
  }
}

// ============================================================================
// Worker message handler — protocol with render-worker.mjs
// ============================================================================

function _handleWorkerMessage(e) {
  const msg = e.data;
  switch (msg.type) {
    case 'ready':
      _workerReady = true;
      state.connState = 'RENDERER OK';
      console.log('[renderer] worker ready, starting rAF tick driver');
      _scheduleTick();
      break;

    case 'ws_open':
      _wsConnected = true;
      console.log('[renderer] worker ws connected');
      break;

    case 'ws_close':
      _wsConnected = false;
      console.log('[renderer] worker ws disconnected');
      state.rendererStreaming = false;
      break;

    case 'sync_applied': {
      state.connState = 'SYNC';
      const idle = document.getElementById('idleScreen');
      if (idle) idle.style.display = 'none';
      document.body.classList.add('streaming');
      state.rendererStreaming = true;
      _telemetry.lastSyncAt = performance.now();
      console.log('[renderer] SYNC applied at frame', _telemetry.framesRendered);
      break;
    }

    case 'telemetry':
      _telemetry.framesRendered = msg.framesRendered;
      _telemetry.framesDropped  = msg.framesDropped;
      _telemetry.bytesReceived  = msg.bytesReceived;
      _telemetry.syncCount      = msg.syncCount;
      _telemetry.intervalSumUs  = msg.intervalSumUs;
      _telemetry.intervalCount  = msg.intervalCount;
      _telemetry.intervalMaxUs  = msg.intervalMaxUs;
      _telemetry.arrivalSumUs   = msg.arrivalSumUs   || 0;
      _telemetry.arrivalCount   = msg.arrivalCount   || 0;
      _telemetry.arrivalMaxUs   = msg.arrivalMaxUs   || 0;
      // If LIVE state isn't set yet but frames are flowing, flip it
      if (msg.framesRendered > 0 && state.connState !== 'LIVE' && state.rendererStreaming) {
        state.connState = 'LIVE';
      }
      break;

    case 'audio':
      // PCM audio chunk from the worker's binary WS pipe. buffer is the
      // full packet including the 4-byte header; _handleAudioChunk strips
      // it and pipes int16 samples to the pcm-worklet (or queues them
      // until the user interacts and AudioContext unlocks).
      _handleAudioChunk(msg.buffer);
      break;

    case 'error':
      console.error('[renderer] worker reported error:', msg.message);
      state.connState = 'NO RENDERER';
      break;
  }
}

// ============================================================================
// rAF tick driver — main thread requestAnimationFrame → worker postMessage
// ============================================================================
//
// We schedule a continuous rAF loop while the worker is ready. Each tick
// posts a 'tick' message; the worker drains its latest pending frame on
// receipt. Postmessage is fast (sub-100us) and the worker's drain runs
// concurrently with the main thread.
//
// We also use _rafScheduled defensively to coalesce — if for some reason
// _scheduleTick is called twice in one rAF window, we don't double-post.

function _scheduleTick() {
  if (_rafScheduled) return;
  _rafScheduled = true;
  requestAnimationFrame(_tick);
}

function _tick() {
  _rafScheduled = false;
  if (_workerReady && _worker) {
    _worker.postMessage({ type: 'tick' });
  }
  // Continuous loop while the worker is alive
  if (_worker) _scheduleTick();
}

// ============================================================================
// Telemetry pull — forwards to worker, response cached in _telemetry above
// ============================================================================

function _pollTelemetry() {
  if (!_workerReady || !_worker) return;
  _worker.postMessage({
    type: 'telemetry',
    resetIntervals: _resetIntervalsOnNextPoll,
  });
  _resetIntervalsOnNextPoll = false;
}

// telemetry.mjs calls this on its 5s tick to reset the rolling jitter window.
// We can't reset locally — the counters live in the worker. We set a flag and
// the next _pollTelemetry tells the worker to reset alongside the read.
export function resetTelemetryIntervals() {
  _resetIntervalsOnNextPoll = true;
}

// ============================================================================
// Public API — setOpt / resize, called from settings.mjs and elsewhere
// ============================================================================

export function setOpt(opt, val) {
  if (!_worker) return;
  val = parseInt(val);
  _worker.postMessage({ type: 'set_opt', opt, value: val });
  if (opt === 0) {
    // Resolution change — main thread can't read the OffscreenCanvas size,
    // but the CSS layout already covers visual sizing via 100%/100%.
    const canvas = document.getElementById('game-canvas');
    if (canvas) {
      canvas.style.width = '100%';
      canvas.style.height = '100%';
    }
  }
  saveRenderSettings();
}

// ============================================================================
// External binary frame forwarding — relay-bootstrap.mjs (P2P fan-out)
// ============================================================================
//
// The render worker has its own WebSocket and handles the primary binary
// pipe. But the P2P spectator relay (relay.js + relay-bootstrap.mjs) also
// produces binary frames — when this client receives a frame from a peer
// over WebRTC, it needs to be rendered AND optionally re-broadcast to the
// client's own peer downstream.
//
// We forward those frames into the worker via postMessage. ArrayBuffers are
// transferable, so this is zero-copy on the JS side. The worker treats the
// forwarded frame identically to one that arrived on its own WebSocket
// (SYNC vs delta detection, coalescing, drain on next tick).
//
// Pre-Fix-2 this function called the WASM module directly on the main
// thread. The signature is preserved so callers don't need to change.

export function handleBinaryFrame(buffer) {
  if (!_worker || !_workerReady) return;
  // Transfer if the caller gave us an ArrayBuffer they don't need anymore.
  // P2P relay wraps things as Uint8Array sometimes — accept either.
  if (buffer instanceof ArrayBuffer) {
    _worker.postMessage({ type: 'binary_frame', buffer }, [buffer]);
  } else if (ArrayBuffer.isView(buffer)) {
    // View into a larger buffer — copy out the view's range so we can transfer
    // a clean ArrayBuffer without breaking the original.
    const ab = buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength);
    _worker.postMessage({ type: 'binary_frame', buffer: ab }, [ab]);
  }
}
