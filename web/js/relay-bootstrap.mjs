// ============================================================================
// RELAY-BOOTSTRAP.MJS — P2P spectator relay initialization
//
// ⚠️  DEPRECATED ARCHITECTURE — slated for deletion ⚠️
//
// The WebRTC P2P spectator mesh exists to amortize home upstream bandwidth
// when flycast lives on the home box and the VPS relay would otherwise need
// to push every TA frame to every viewer over the home→VPS link.
//
// As of 2026-04-08 (see docs/VPS-SETUP.md and the project_headless_in_production
// memory), flycast runs on the SAME VPS as the relay. Frames travel zero
// internet hops between flycast and the relay, and the relay is hosted on a
// 1Gbit-symmetric VPS pipe — there is no upstream bandwidth to save. The
// P2P mesh is doing work that the trivial single-source fanout would do for
// less code, less latency, and zero ICE/TURN overhead.
//
// Why it's still here: the code works, ripping it out is a non-trivial sweep
// across relay-bootstrap.mjs + relay.js + renderer-bridge.mjs + the relay_*
// signaling cases in fanout.rs. Slated for Phase 6c proper. For now, the
// MapleCastRelay constructor is a no-op if the global isn't loaded, and the
// /turn-cred fetch is harmless if it falls through to STUN.
//
// If you're touching this file, ask: "do we still need P2P fanout?" The
// answer is almost certainly no, and the right move is to delete the whole
// path rather than maintain it.
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
