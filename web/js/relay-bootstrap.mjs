// ============================================================================
// RELAY-BOOTSTRAP.MJS — P2P spectator relay initialization
// ============================================================================

import { state } from './state.mjs';
import { handleBinaryFrame } from './renderer-bridge.mjs';

export function initRelay() {
  if (typeof MapleCastRelay === 'undefined') return;

  const check = setInterval(() => {
    if (state.ws?.readyState === 1 && !state.relay) {
      state.relay = new MapleCastRelay(
        state.ws,
        (data) => handleBinaryFrame(data),
        (data) => handleBinaryFrame(data),
      );
      console.log('[relay] P2P relay initialized');
      clearInterval(check);
    }
  }, 1000);
}
