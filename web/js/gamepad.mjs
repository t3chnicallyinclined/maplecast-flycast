// ============================================================================
// GAMEPAD.MJS — Gamepad polling + stick registration
//
// PERF: rAF-synced polling (not setInterval). Pre-allocated buffer.
//       Delta compression. bufferedAmount backpressure.
// ============================================================================

import { state } from './state.mjs';

// Pre-allocated send buffer — zero GC pressure
const _inputBuf = new Uint8Array(4);
let _lastBtn = 0, _lastLT = 0, _lastRT = 0;
let _gamepadRAF = null;
const MAX_BUFFERED = 8192; // backpressure limit (2sec of sends)

export function startGamepadPolling() {
  stopGamepadPolling();
  _lastBtn = 0; _lastLT = 0; _lastRT = 0;

  function poll() {
    if (!state.ws || state.ws.readyState !== WebSocket.OPEN || state.mySlot < 0) {
      _gamepadRAF = requestAnimationFrame(poll);
      return;
    }

    // Backpressure — skip if WS send buffer is full
    if (state.ws.bufferedAmount > MAX_BUFFERED) {
      _gamepadRAF = requestAnimationFrame(poll);
      return;
    }

    const gp = navigator.getGamepads()[0];
    if (!gp) {
      state.diag.gamepadActive = false;
      _gamepadRAF = requestAnimationFrame(poll);
      return;
    }
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

    // Delta compression — skip if no change
    if (btn === _lastBtn && lt === _lastLT && rt === _lastRT) {
      _gamepadRAF = requestAnimationFrame(poll);
      return;
    }
    _lastBtn = btn; _lastLT = lt; _lastRT = rt;

    _inputBuf[0] = lt;
    _inputBuf[1] = rt;
    _inputBuf[2] = (btn >> 8) & 0xFF;
    _inputBuf[3] = btn & 0xFF;
    state.ws.send(_inputBuf);
    state.diag.inputSendCount++;

    _gamepadRAF = requestAnimationFrame(poll);
  }

  _gamepadRAF = requestAnimationFrame(poll);
}

export function stopGamepadPolling() {
  if (_gamepadRAF) {
    cancelAnimationFrame(_gamepadRAF);
    _gamepadRAF = null;
  }
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
