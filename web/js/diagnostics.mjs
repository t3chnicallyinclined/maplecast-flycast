// ============================================================================
// DIAGNOSTICS.MJS — Performance diagnostics overlay + ping timer
// ============================================================================

import { state } from './state.mjs';
import { avg } from './ui-common.mjs';

export function toggleDiag() {
  document.getElementById('diagOverlay').classList.toggle('open');
}

export function updateDiag() {
  const el = document.getElementById('diagOverlay');
  if (!el.classList.contains('open')) return;

  const d = state.diag;
  const p1 = d.p1 || {};
  const p2 = d.p2 || {};
  const publishMs = ((d.publishUs || 0) / 1000).toFixed(2);
  const streamMbps = ((d.streamKbps || 0) / 1000).toFixed(1);
  const pingMs = d.pingMs.toFixed(1);
  const clientRenderMs = 2.0;

  const p1InputMs = (p1.pps || 0) > 1000 ? 0.3 : 1.0;
  const p2InputMs = (p2.pps || 0) > 1000 ? 0.3 : 1.0;
  const networkMs = 1.0;
  const p1E2E = (p1InputMs + parseFloat(publishMs) + networkMs + clientRenderMs).toFixed(1);
  const p2E2E = (p2InputMs + parseFloat(publishMs) + networkMs + clientRenderMs).toFixed(1);

  el.textContent =
`=== MAPLECAST ===
Status: ${state.connState || '...'}
FPS: ${d.fps}  Frame: ${d.serverFrame}

-- Connection --
Ping: ${pingMs}ms
Stream: ${streamMbps} Mbps
Publish: ${publishMs}ms/frame
Dirty Pages: ${d.dirtyPages}

-- P1 ${(p1.pps||0) > 1000 ? '(NOBD)' : '(Web)'} --
Name: ${p1.name || '-'}
Input: ${p1.pps||0}/s  Chg: ${p1.cps||0}/s
Btn>Server: ${p1InputMs}ms
Btn>Screen: ${p1E2E}ms E2E

-- P2 ${(p2.pps||0) > 1000 ? '(NOBD)' : '(Web)'} --
Name: ${p2.name || '-'}
Input: ${p2.pps||0}/s  Chg: ${p2.cps||0}/s
Btn>Server: ${p2InputMs}ms
Btn>Screen: ${p2E2E}ms E2E

-- Me --
Slot: ${state.mySlot >= 0 ? 'P'+(state.mySlot+1) : (state.wsInQueue ? 'QUEUED' : 'SPECTATOR')}
Pad: ${d.gamepadActive ? 'YES' : 'no'}  Sent: ${d.inputSendCount}
RTT>Screen: ${(parseFloat(pingMs) + clientRenderMs).toFixed(1)}ms`;
}

export function startDiagInterval() {
  setInterval(() => {
    updateDiag();
    if (state.ws?.readyState === 1) {
      state.ws.send(JSON.stringify({ type: 'ping', t: performance.now() }));
    }
  }, 1000);
}
