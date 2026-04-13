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
let _lastSendMs = 0;
let _gamepadRAF = null;
let _burstRemaining = 0;
const MAX_BUFFERED = 8192;
// Heartbeat interval: how often to re-send the current input state even
// when nothing changed. flycast's input server kicks players idle after
// 30s of no `lastChangeUs` updates, and the C++ side bumps that clock
// only on state CHANGE. Without a heartbeat, a player who isn't pressing
// buttons (e.g. character select countdown, watching the round timer)
// gets kicked even though they're still at the cabinet. 5s is well
// below the 30s threshold and adds 0.8 bytes/sec of bandwidth.
const HEARTBEAT_MS = 5000;
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

// First-gesture wake-up. Chrome/Edge gate navigator.getGamepads() behind user
// activation: until the user has interacted with the page once, getGamepads()
// returns [null, null, null, null] even when a pad is plugged in, AND the
// gamepadconnected event does not fire on page reload (only on actual NEW
// connections). The combination silently breaks reload-recovery: the server
// still owns the user's slot, but the browser thinks there's no pad and the
// "I GOT NEXT" button stays disabled.
//
// Listening for the first pointerdown/keydown/touchstart triggers an immediate
// sweep — by the time the listener fires, user activation is in effect and
// getGamepads() returns the live cache. We self-remove after the first event
// to keep the listener set tight; the 2s setInterval continues as a backstop
// in case the wake-up sweep raced the cache populating.
function firstGestureWake() {
  sweepGamepads();
  window.removeEventListener('pointerdown', firstGestureWake, true);
  window.removeEventListener('keydown', firstGestureWake, true);
  window.removeEventListener('touchstart', firstGestureWake, true);
}
window.addEventListener('pointerdown', firstGestureWake, true);
window.addEventListener('keydown', firstGestureWake, true);
window.addEventListener('touchstart', firstGestureWake, true);

// ---- Input polling (only runs while in a slot) ----

function pollOnce() {
  // Inputs go to the DIRECT flycast control WS (home.nobd.net), NOT the
  // relay WS (state.ws → nobd.net). The relay carries spectator broadcast
  // and queue chatter; the control WS is where the actual emulator listens
  // for player frames. Sending to state.ws here is what caused the
  // "kicked: idle" bug — flycast never saw any input and dropped the slot.
  const cws = state.controlWs;
  if (!cws || cws.readyState !== WebSocket.OPEN || state.mySlot < 0) return;
  if (cws.bufferedAmount > MAX_BUFFERED) return;

  // Walk all 4 pad slots and pick the most recently active one. Chrome
  // assigns pads to slots by plug-in order and never renumbers, so [0]
  // goes stale when the first pad is unplugged. Using the pad with the
  // newest `timestamp` also lets users hot-swap mid-session: touch a
  // button on pad B and the next poll picks it up automatically.
  const pads = navigator.getGamepads();
  let gp = null;
  let newest = -1;
  for (let i = 0; i < pads.length; i++) {
    const p = pads[i];
    if (!p) continue;
    if (p.timestamp > newest) { newest = p.timestamp; gp = p; }
  }
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
  // Triggers: analog LT/RT + digital LB/RB mapped as triggers (swapped: RB=A1/LT, LB=A2/RT)
  let lt = Math.floor((gp.buttons[6]?.value || 0) * 255);
  let rt = Math.floor((gp.buttons[7]?.value || 0) * 255);
  if (gp.buttons[5]?.pressed) lt = 255; // RB → LT (Assist 1)
  if (gp.buttons[4]?.pressed) rt = 255; // LB → RT (Assist 2)

  // Send when input changed OR when the heartbeat window has elapsed OR
  // when the phase-aligned scheduler has fired (Phase B). The phase
  // scheduler sets _forceSendNext just before the predicted server latch,
  // so a packet lands at the server with maximum freshness even when the
  // player isn't pressing anything.
  const now = performance.now();
  const changed = btn !== _lastBtn || lt !== _lastLT || rt !== _lastRT;
  const heartbeatDue = now - _lastSendMs >= HEARTBEAT_MS;
  const forcedByPhase = shouldForceSend();
  if (!changed && !heartbeatDue && !forcedByPhase) return;

  _lastBtn = btn; _lastLT = lt; _lastRT = rt;
  _lastSendMs = now;

  _inputBuf[0] = lt;
  _inputBuf[1] = rt;
  _inputBuf[2] = (btn >> 8) & 0xFF;
  _inputBuf[3] = btn & 0xFF;

  // Try WebTransport datagram first (QUIC/UDP = lowest latency)
  // Falls back to controlWs (TCP) if WT not available
  let sentViaQUIC = false;
  if (window._transport && window._transport.sendInput) {
    sentViaQUIC = window._transport.sendInput(
      state.mySlot, lt, rt, (btn >> 8) & 0xFF, btn & 0xFF);
  }
  if (!sentViaQUIC) {
    cws.send(_inputBuf);  // TCP fallback
  }
  state.diag.inputSendCount++;
  state.diag.inputViaQUIC = sentViaQUIC;
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

// ============================================================================
// PHASE B — server-frame-phase-aligned sends
// ============================================================================
//
// In addition to the rAF burst-poll path above (which catches state changes
// within ~1ms of them happening), we ALSO schedule one targeted send per
// server vblank — timed to land 3-4ms before the server's predicted next
// latch. This gives the server's CMD9 latch a packet whose lastPacketUs is
// always within ~3ms of the latch time, instead of the random 0-16ms
// staleness inherent to rAF-only sends.
//
// Two pieces of state:
//   _serverClockOffsetMs — running EMA of (server-CLOCK_MONOTONIC ms) -
//                          (local performance.now() at receive). Updated on
//                          every status frame. Smoothed over ~16 samples.
//   _phaseTimer          — setTimeout handle for the next scheduled send.
//
// On every status frame from the server (frame_phase block), we recompute
// the offset, predict when the next vblank will fire on the server in our
// local clock, and schedule a one-shot timer to fire ~4ms before that point.
// The timer fires pollOnce() with the heartbeat override so a packet goes
// out even if button state hasn't changed.
//
// If the server doesn't publish frame_phase yet (older builds), this whole
// system is dormant — _serverClockOffsetMs stays 0 and _phaseTimer is never
// set. Existing rAF burst path is unchanged.

let _serverClockOffsetMs = 0;     // (server_us / 1000) - performance.now()
let _haveServerOffset = false;
let _phaseTimer = null;

// Tunables. The server publishes guard_us in frame_phase; we aim to land
// (guard + safety) before the next latch so even with clock jitter we're
// outside the guard window.
const PHASE_SAFETY_MS = 3.0;    // additional margin beyond the guard window
const MIN_LEAD_MS = 1.0;        // never schedule less than 1ms ahead

// Force a send on the next rAF even if state hasn't changed. The phase
// timer sets this when it fires; pollOnce() consumes it.
let _forceSendNext = false;

// Patch pollOnce()'s "no change → return" gate by exposing a forced path.
// We do this by intercepting the heartbeat check, since that's already a
// "send even when nothing changed" mechanism.
const _origHeartbeatMs = HEARTBEAT_MS;
function shouldForceSend() {
  if (_forceSendNext) {
    _forceSendNext = false;
    return true;
  }
  return false;
}
// Note: pollOnce() is local to this module so we can't monkey-patch it.
// Instead, we directly call pollOnce() from the phase timer AFTER setting
// _forceSendNext, and pollOnce checks the flag. See the change to pollOnce
// above. (We added the _forceSendNext check in pollOnce too.)

// Status frame receiver hook. Called from ws-connection.mjs handleStatus()
// after the diag fields are populated. Updates the clock offset EMA and
// re-arms the phase timer for the next vblank.
//
// framePhase: { frame, t_last_latch_us, t_next_latch_us, frame_period_us, guard_us }
// rttMs: optional one-way RTT estimate (we use rttMs/2)
export function onServerFramePhase(framePhase, rttMs) {
  if (!framePhase || typeof framePhase.t_last_latch_us !== 'number') return;
  if (!state.controlWs || state.controlWs.readyState !== WebSocket.OPEN) return;
  if (state.mySlot < 0) return;

  const localNow = performance.now();
  const serverLastLatchMs = framePhase.t_last_latch_us / 1000;
  const sample = serverLastLatchMs - localNow;

  // Initial sample: anchor. Subsequent samples: 1/16 EMA.
  if (!_haveServerOffset) {
    _serverClockOffsetMs = sample;
    _haveServerOffset = true;
  } else {
    _serverClockOffsetMs += (sample - _serverClockOffsetMs) / 16;
  }

  // Convert: server's predicted next-latch time → our local clock
  const serverNextLatchMs = framePhase.t_next_latch_us / 1000;
  const localNextLatchMs = serverNextLatchMs - _serverClockOffsetMs;

  // We want our packet to ARRIVE at the server right before the latch.
  // One-way delay: half the WS RTT (or 5 ms if unknown).
  const oneWayMs = rttMs > 0 ? rttMs / 2 : 5.0;
  const guardMs = (framePhase.guard_us || 500) / 1000;

  // Fire `oneWayMs + guardMs + PHASE_SAFETY_MS` before the latch. That puts
  // the packet arrival just outside the guard window, which means under
  // ConsistencyFirst it lands in THIS frame's accumulator (not deferred).
  const sendOffsetMs = oneWayMs + guardMs + PHASE_SAFETY_MS;
  let leadMs = (localNextLatchMs - localNow) - sendOffsetMs;

  if (leadMs < MIN_LEAD_MS) {
    // Too close — skip this vblank, the next status broadcast will re-arm us
    return;
  }

  // Re-arm
  if (_phaseTimer) clearTimeout(_phaseTimer);
  _phaseTimer = setTimeout(() => {
    _phaseTimer = null;
    _forceSendNext = true;   // pollOnce will treat this like a heartbeat send
    pollOnce();
  }, leadMs);
}

export function startGamepadPolling() {
  stopGamepadPolling();
  _lastBtn = 0; _lastLT = 0; _lastRT = 0;
  _lastSendMs = 0;
  _haveServerOffset = false;
  _gamepadRAF = requestAnimationFrame(rafTick);
}

export function stopGamepadPolling() {
  if (_gamepadRAF) {
    cancelAnimationFrame(_gamepadRAF);
    _gamepadRAF = null;
  }
  if (_phaseTimer) {
    clearTimeout(_phaseTimer);
    _phaseTimer = null;
  }
  _burstRemaining = 0;
  _forceSendNext = false;
  _haveServerOffset = false;
}

// NOBD stick registration removed — USB-only client.
// All hardware stick paths live server-side now and the browser doesn't
// participate in the binding lifecycle.
