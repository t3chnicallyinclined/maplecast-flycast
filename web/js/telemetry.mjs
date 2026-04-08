// ============================================================================
// TELEMETRY.MJS — Browser-side latency measurement, reported to relay
//
// Each browser pushes a small JSON snapshot to /api/telemetry every 5 seconds.
// The relay aggregates across all clients into Prometheus metrics that show
// real end-to-end latency from each viewer's perspective.
//
// What we report:
//   - rtt_ms        WebSocket round-trip (browser → relay → browser)
//   - fps           rendered frames in the last window
//   - frame_jitter  max inter-frame interval (worst-case stutter)
//   - mbps          inbound bandwidth this client is consuming
//   - sync_count    number of SYNCs since page load
//   - ua            short user agent string (for "is firefox slow?" debug)
//
// Send is fire-and-forget — never blocks rendering or chat. If /api/telemetry
// 503s or 404s, we silently stop pushing for the rest of the page lifetime
// so a broken relay doesn't fill the console with errors.
// ============================================================================

import { state } from './state.mjs';
import { _telemetry, resetTelemetryIntervals } from './renderer-bridge.mjs';

const PUSH_INTERVAL_MS = 5000;
const PUSH_URL = '/api/telemetry';

let _disabled = false;
let _lastPushAt = 0;
let _lastFramesRendered = 0;
let _lastFramesDropped = 0;
let _lastBytesReceived = 0;
let _clientId = null;

function makeClientId() {
  // Stable per page load — random 6-char id
  return Math.random().toString(36).slice(2, 8);
}

function shortUa() {
  const ua = navigator.userAgent || '';
  if (ua.includes('Firefox/')) return 'firefox';
  if (ua.includes('Chrome/'))  return 'chrome';
  if (ua.includes('Safari/'))  return 'safari';
  return 'other';
}

async function pushTelemetry() {
  if (_disabled) return;

  const now = performance.now();
  const elapsedMs = _lastPushAt > 0 ? (now - _lastPushAt) : PUSH_INTERVAL_MS;
  _lastPushAt = now;

  const framesDelta  = _telemetry.framesRendered - _lastFramesRendered;
  const droppedDelta = _telemetry.framesDropped  - _lastFramesDropped;
  const bytesDelta   = _telemetry.bytesReceived  - _lastBytesReceived;
  _lastFramesRendered = _telemetry.framesRendered;
  _lastFramesDropped  = _telemetry.framesDropped;
  _lastBytesReceived  = _telemetry.bytesReceived;

  const fps  = (framesDelta * 1000 / elapsedMs);
  const mbps = (bytesDelta * 8 / 1000 / elapsedMs); // bytes/ms * 8 / 1000 = Mbps

  // Render interval (drain cadence — vsync-locked, ~16.67ms always)
  const renderAvgUs = _telemetry.intervalCount > 0
    ? _telemetry.intervalSumUs / _telemetry.intervalCount
    : 0;
  const renderMaxUs = _telemetry.intervalMaxUs;

  // Arrival interval (WS frame-to-frame — actual network jitter)
  const arrivalAvgUs = _telemetry.arrivalCount > 0
    ? _telemetry.arrivalSumUs / _telemetry.arrivalCount
    : 0;
  const arrivalMaxUs = _telemetry.arrivalMaxUs;

  // Reset the rolling jitter windows in the render worker. Counters live in
  // the worker now (Fix 2); the next renderer-bridge telemetry poll will
  // forward the reset request alongside the read, so the worker zeros its
  // accumulators atomically with the snapshot. Local _telemetry fields will
  // pick up the zeroed values on the next poll cycle (~250ms).
  resetTelemetryIntervals();

  const payload = {
    client_id: _clientId,
    ua: shortUa(),
    rtt_ms: state.diag.pingMs || 0,
    fps: Math.round(fps * 10) / 10,
    mbps: Math.round(mbps * 100) / 100,
    frames_dropped: droppedDelta,
    // Network jitter — gap between WS frame arrivals (the real jitter)
    frame_jitter_avg_us: Math.round(arrivalAvgUs),
    frame_jitter_max_us: Math.round(arrivalMaxUs),
    // Render cadence — gap between drain calls (vsync-locked, sanity check)
    render_interval_avg_us: Math.round(renderAvgUs),
    render_interval_max_us: Math.round(renderMaxUs),
    sync_count: _telemetry.syncCount,
    streaming: state.rendererStreaming ? 1 : 0,
  };

  try {
    const res = await fetch(PUSH_URL, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
      // keepalive=true so the request survives a tab close
      keepalive: true,
    });
    if (res.status === 404 || res.status === 405) {
      console.warn('[telemetry] endpoint missing, disabling');
      _disabled = true;
    }
  } catch (e) {
    // Network error — silent, will retry next tick
  }
}

export function startTelemetry() {
  if (_clientId) return; // already started
  _clientId = makeClientId();
  _lastPushAt = performance.now();
  setInterval(pushTelemetry, PUSH_INTERVAL_MS);
  console.log('[telemetry] started, client_id =', _clientId);
}
