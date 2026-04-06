// ============================================================================
// RELAY-BOOTSTRAP.MJS — P2P spectator relay initialization
// ============================================================================

import { state } from './state.mjs';
import { handleBinaryFrame } from './renderer-bridge.mjs';

// ICE servers for WebRTC P2P — STUN + TURN(S)
// TURN credentials are time-limited via REST API; for now using static ones
// fetched from /turn-cred endpoint (set up later). Anonymous TURN is OK for dev.
const ICE_SERVERS = [
  { urls: ['stun:stun.l.google.com:19302'] },
  { urls: ['stun:nobd.net:3478'] },
  // TURN — uncomment and add credentials when /turn-cred endpoint is live
  // { urls: ['turn:nobd.net:3478?transport=udp', 'turns:nobd.net:5349?transport=tcp'],
  //   username: '...', credential: '...' },
];

export function initRelay() {
  if (typeof MapleCastRelay === 'undefined') return;

  const check = setInterval(() => {
    if (state.ws?.readyState === 1 && !state.relay) {
      state.relay = new MapleCastRelay(
        state.ws,
        (data) => handleBinaryFrame(data),
        (data) => handleBinaryFrame(data),
      );
      // Inject ICE config if relay supports it
      if (state.relay.rtcConfig) state.relay.rtcConfig.iceServers = ICE_SERVERS;
      console.log('[relay] P2P relay initialized');
      clearInterval(check);
    }
  }, 1000);
}
