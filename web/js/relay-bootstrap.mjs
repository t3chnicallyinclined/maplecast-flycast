// ============================================================================
// RELAY-BOOTSTRAP.MJS — P2P spectator relay initialization
//
// Fetches time-limited TURN credentials from /turn-cred on startup, falls back
// to public STUN if the endpoint is unavailable.
// ============================================================================

import { state } from './state.mjs';
import { handleBinaryFrame } from './renderer-bridge.mjs';

// Fallback ICE config if /turn-cred fails
const FALLBACK_ICE = [
  { urls: ['stun:stun.l.google.com:19302'] },
  { urls: ['stun:nobd.net:3478'] },
];

let _iceServers = FALLBACK_ICE;
let _iceFetchedAt = 0;

async function fetchTurnCreds() {
  try {
    const res = await fetch('/turn-cred', { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();
    if (data.iceServers && Array.isArray(data.iceServers)) {
      _iceServers = data.iceServers;
      _iceFetchedAt = Date.now();
      console.log('[relay] TURN credentials loaded, ttl:', data.ttl, 's');
      // Refresh halfway through TTL
      setTimeout(fetchTurnCreds, (data.ttl || 3600) * 500);
      return true;
    }
  } catch (e) {
    console.warn('[relay] /turn-cred fetch failed, using STUN-only fallback:', e.message);
  }
  return false;
}

export async function initRelay() {
  // Fetch TURN creds immediately, don't block init
  fetchTurnCreds();

  if (typeof MapleCastRelay === 'undefined') return;

  const check = setInterval(() => {
    if (state.ws?.readyState === 1 && !state.relay) {
      state.relay = new MapleCastRelay(
        state.ws,
        (data) => handleBinaryFrame(data),
        (data) => handleBinaryFrame(data),
      );
      // Inject ICE config (live TURN creds if available, fallback otherwise)
      if (state.relay.rtcConfig) state.relay.rtcConfig.iceServers = _iceServers;
      console.log('[relay] P2P relay initialized with', _iceServers.length, 'ICE servers');
      clearInterval(check);
    }
  }, 1000);
}

export function getIceServers() {
  return _iceServers;
}
