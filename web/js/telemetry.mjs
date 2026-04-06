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
import { _telemetry } from './renderer-bridge.mjs';

const PUSH_INTERVAL_MS = 5000;
const PUSH_URL = '/api/telemetry';

let _disabled = false;
let _lastPushAt = 0;
let _lastFramesRendered = 0;
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

  const framesDelta = _telemetry.framesRendered - _lastFramesRendered;
  const bytesDelta  = _telemetry.bytesReceived - _lastBytesReceived;
  _lastFramesRendered = _telemetry.framesRendered;
  _lastBytesReceived  = _telemetry.bytesReceived;

  const fps  = (framesDelta * 1000 / elapsedMs);
  const mbps = (bytesDelta * 8 / 1000 / elapsedMs); // bytes/ms * 8 / 1000 = Mbps
  const avgIntervalUs = _telemetry.intervalCount > 0
    ? _telemetry.intervalSumUs / _telemetry.intervalCount
    : 0;
  const maxIntervalUs = _telemetry.intervalMaxUs;
  // Reset rolling jitter window
  _telemetry.intervalSumUs = 0;
  _telemetry.intervalCount = 0;
  _telemetry.intervalMaxUs = 0;

  const payload = {
    client_id: _clientId,
    ua: shortUa(),
    rtt_ms: state.diag.pingMs || 0,
    fps: Math.round(fps * 10) / 10,
    mbps: Math.round(mbps * 100) / 100,
    frame_jitter_avg_us: Math.round(avgIntervalUs),
    frame_jitter_max_us: Math.round(maxIntervalUs),
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
