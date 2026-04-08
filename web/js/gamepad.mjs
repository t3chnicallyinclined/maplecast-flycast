// ============================================================================
// GAMEPAD.MJS — Gamepad polling + connection detection
//
// PERF: hybrid rAF + MessageChannel polling.
//
//       Each requestAnimationFrame tick (60-144Hz) we burst-poll several
//       times via MessageChannel to detect input changes within ~1ms of
//       them happening, instead of waiting up to 16.67ms for the next vsync.
//
//       MessageChannel postMessage schedules a callback in the next event
//       loop tick (~0.1ms latency, no setTimeout 4ms clamp). Used by
//       React Scheduler etc. for the same reason.
//
//       Tab backgrounding: rAF stops firing → bursts stop → CPU is free.
//       Battery and laptop friendly.
//
//       Delta compression — only sends on change. Pre-allocated buffer.
//       bufferedAmount backpressure to drop when WS send queue is full.
//
// UX GATE: state.gamepadConnected is the single source of truth for whether
//       the player can step up to the cabinet. Updated reactively from the
//       gamepadconnected / gamepaddisconnected browser events.
// ============================================================================

import { state } from './state.mjs';

const _inputBuf = new Uint8Array(4);
let _lastBtn = 0, _lastLT = 0, _lastRT = 0;
let _gamepadRAF = null;
let _burstRemaining = 0;
const MAX_BUFFERED = 8192;
// How many MessageChannel polls per rAF tick. At 60Hz rAF, 16 bursts gives
// ~1ms polling resolution. Each poll is essentially free (just reads the
// gamepad state cache).
const BURSTS_PER_RAF = 16;

const _mc = new MessageChannel();

// ---- Connection detection ----
// Chrome/Edge require a button press before getGamepads() returns a non-null
// entry for a plugged-in pad (security/privacy). The gamepadconnected event
// fires on that first press, so we listen for both the initial sweep and
// the event to catch either path.

function sweepGamepads() {
  const pads = navigator.getGamepads();
  for (let i = 0; i < pads.length; i++) {
    if (pads[i]) {
      setGamepadConnected(true, pads[i].id);
      return;
    }
  }
  setGamepadConnected(false, '');
}

function setGamepadConnected(connected, id) {
  const changed = state.gamepadConnected !== connected;
  state.gamepadConnected = connected;
  state.gamepadId = (id || '').substring(0, 30);
  if (changed) {
    // Re-evaluate the cabinet button — disabled when no pad, enabled when present
    import('./queue.mjs').then(m => m.updateCabinetControls?.());
  }
}

window.addEventListener('gamepadconnected', (e) => {
  setGamepadConnected(true, e.gamepad.id);
});

window.addEventListener('gamepaddisconnected', () => {
  // Re-sweep in case a second pad is still plugged in
  sweepGamepads();
});

// Initial sweep once DOM is ready (Firefox returns pads immediately, Chrome
// needs a button press but we still want to try).
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', sweepGamepads);
} else {
  sweepGamepads();
}

// Safety-net poll every 2s — Chrome's gamepadconnected event is sometimes
// flaky when the page reloads with a pad already plugged in. Cheap enough
// to run forever.
setInterval(sweepGamepads, 2000);

// ---- Input polling (only runs while in a slot) ----

function pollOnce() {
  if (!state.ws || state.ws.readyState !== WebSocket.OPEN || state.mySlot < 0) return;
  if (state.ws.bufferedAmount > MAX_BUFFERED) return;

  const gp = navigator.getGamepads()[0];
  if (!gp) { state.diag.gamepadActive = false; return; }
  state.diag.gamepadActive = true;

  let btn = 0xFFFF;
  if (gp.buttons[0]?.pressed) btn &= ~0x0004;
  if (gp.buttons[1]?.pressed) btn &= ~0x0002;
  if (gp.buttons[2]?.pressed) btn &= ~0x0400;
  if (gp.buttons[3]?.pressed) btn &= ~0x0200;
  if (gp.buttons[9]?.pressed) btn &= ~0x0008;
  if (gp.buttons[12]?.pressed) btn &= ~0x0010;
  if (gp.buttons[13]?.pressed) btn &= ~0x0020;
  if (gp.buttons[14]?.pressed) btn &= ~0x0040;
  if (gp.buttons[15]?.pressed) btn &= ~0x0080;
  const lt = Math.floor((gp.buttons[6]?.value || 0) * 255);
  const rt = Math.floor((gp.buttons[7]?.value || 0) * 255);

  if (btn === _lastBtn && lt === _lastLT && rt === _lastRT) return;
  _lastBtn = btn; _lastLT = lt; _lastRT = rt;

  _inputBuf[0] = lt;
  _inputBuf[1] = rt;
  _inputBuf[2] = (btn >> 8) & 0xFF;
  _inputBuf[3] = btn & 0xFF;
  state.ws.send(_inputBuf);
  state.diag.inputSendCount++;
}

// MessageChannel handler — fired ~immediately after postMessage.
_mc.port1.onmessage = () => {
  pollOnce();
  if (_burstRemaining > 0) {
    _burstRemaining--;
    _mc.port2.postMessage(null);
  }
};

function rafTick() {
  // Schedule a burst of MessageChannel polls before the next rAF.
  _burstRemaining = BURSTS_PER_RAF - 1;
  _mc.port2.postMessage(null);
  _gamepadRAF = requestAnimationFrame(rafTick);
}

export function startGamepadPolling() {
  stopGamepadPolling();
  _lastBtn = 0; _lastLT = 0; _lastRT = 0;
  _gamepadRAF = requestAnimationFrame(rafTick);
}

export function stopGamepadPolling() {
  if (_gamepadRAF) {
    cancelAnimationFrame(_gamepadRAF);
    _gamepadRAF = null;
  }
  _burstRemaining = 0;
}

// ---- NOBD stick registration (DISABLED) ----
// The NOBD hardware stick path is parked until the protocol stabilises.
// These stubs exist so king.html's inline onclick handlers still bind —
// wired to deleted UI they're never called, but the export contract stays.
export function registerStick() { /* disabled */ }
export function unregisterStick() { /* disabled */ }
export function updateStickButtons() { /* disabled */ }
