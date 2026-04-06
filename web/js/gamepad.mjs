// ============================================================================
// GAMEPAD.MJS — Gamepad polling + stick registration
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

// === Stick Registration ===

export function registerStick() {
  if (!state.ws || state.ws.readyState !== WebSocket.OPEN || !state.signedIn) return;
  state.ws.send(JSON.stringify({ type: 'register_stick', id: state.myId }));
  document.getElementById('registerStickBtn').disabled = true;
  document.getElementById('registerStickBtn').textContent = 'Listening...';
  const regStatus = document.getElementById('registerStatus');
  regStatus.style.display = 'block';
  regStatus.textContent = 'Tap any button 5 times, pause, then 5 times again...';

  state.registerTimeout = setTimeout(() => {
    if (state.ws?.readyState === WebSocket.OPEN)
      state.ws.send(JSON.stringify({ type: 'cancel_register' }));
    document.getElementById('registerStickBtn').disabled = false;
    document.getElementById('registerStickBtn').innerHTML = '&#x1F3AE; REGISTER STICK';
    regStatus.textContent = 'Timed out — try again';
    setTimeout(() => { regStatus.style.display = 'none'; }, 3000);
  }, 15000);
}

export function unregisterStick() {
  if (state.ws?.readyState === WebSocket.OPEN)
    state.ws.send(JSON.stringify({ type: 'unregister_stick', id: state.myId }));
  localStorage.removeItem('maplecast_stick');
  document.getElementById('unregisterStickBtn').style.display = 'none';
  document.getElementById('registerStickBtn').style.display = 'block';
  document.getElementById('registerStickBtn').disabled = false;
  document.getElementById('registerStickBtn').innerHTML = '&#x1F3AE; REGISTER STICK';
}

export function updateStickButtons() {
  const section = document.getElementById('nobdSection');
  if (!state.signedIn) {
    if (section) section.style.display = 'none';
    return;
  }
  if (section) section.style.display = 'block';
  if (localStorage.getItem('maplecast_stick')) {
    document.getElementById('registerStickBtn').style.display = 'none';
    document.getElementById('unregisterStickBtn').style.display = 'block';
  } else {
    document.getElementById('registerStickBtn').style.display = 'block';
    document.getElementById('unregisterStickBtn').style.display = 'none';
  }
}
