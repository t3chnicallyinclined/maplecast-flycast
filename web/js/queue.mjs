// ============================================================================
// QUEUE.MJS — Queue management, gotNext, leave
// ============================================================================

import { state } from './state.mjs';
import { renderChat } from './chat.mjs';
import { stopGamepadPolling } from './gamepad.mjs';
import { ANON_NAMES } from './ui-common.mjs';
import { getCachedRecord, formatRecord } from './player-cards.mjs';

export function renderQueue() {
  const list = document.getElementById('queueList');
  if (state.queueData.length === 0 && !state.inQueue) {
    list.innerHTML = '<div class="queue-empty">NO ONE IN LINE<br>STEP UP</div>';
    return;
  }
  let entries = state.queueData.map((e, i) => `
    <div class="queue-entry ${i === 0 ? 'next-up' : ''}">
      <div class="queue-pos">${i + 1}</div>
      <div class="queue-name">${e.name}</div>
      <div class="queue-record">${e.record}</div>
    </div>
  `).join('');

  if (state.inQueue) {
    entries += `<div class="queue-entry is-me">
      <div class="queue-pos">${state.queueData.length + 1}</div>
      <div class="queue-name">${state.myName || 'YOU'}</div>
      <div class="queue-record">0W-0L</div>
    </div>`;
  }
  list.innerHTML = entries;
}

export function updateQueueFromServer(serverQueue) {
  const list = document.getElementById('queueList');
  if (serverQueue.length === 0 && !state.inQueue) {
    list.innerHTML = '<div class="queue-empty">NO ONE IN LINE<br>STEP UP</div>';
    return;
  }
  // Records come from the player-cards cache. lobby.mjs prefetches on each
  // status tick so by the second pass through here they're populated.
  // First-time names show "NEW" until the cache fills.
  list.innerHTML = serverQueue.map((name, i) => {
    const isMe = state.signedIn && name.toUpperCase() === state.myName;
    const rec = getCachedRecord(name);
    const recStr = rec ? formatRecord(rec) : 'NEW';
    return `<div class="queue-entry ${i === 0 ? 'next-up' : ''} ${isMe ? 'is-me' : ''}">
      <div class="queue-pos">${i + 1}</div>
      <div class="queue-name">${String(name).toUpperCase()}</div>
      <div class="queue-record">${recStr}</div>
    </div>`;
  }).join('');
}

export function gotNext() {
  // Refuse to step up if we're already a player. Without this guard, a stale
  // click after a reload-resync would send a fresh `join` and double-book the
  // user into the *other* slot — which is exactly the bug that prompted this
  // refactor. The server-side ghost-slot eviction is a backstop; this is the
  // primary fix.
  if (state.mySlot >= 0) return;
  if (state.inQueue) return;

  // Hard gate: no gamepad, no game. The button should already be disabled
  // by updateCabinetControls, but guard the action too in case someone
  // finds an alternate trigger path.
  if (!state.gamepadConnected) return;

  // Anonymous players can play, just no stats. CRUCIAL: anon users get an
  // *ephemeral* session id minted per click — never the persistent state.myId
  // — so a tab close → reopen does NOT let the new tab silently reclaim the
  // old anon slot via syncSlotFromStatus(). The server sees the abandoned
  // anon slot's id and the new tab's id as different, the orphan slot gets
  // cleaned up by the close handler / idle kick, and the user starts fresh.
  if (!state.signedIn) {
    state.myName = ANON_NAMES[Math.floor(Math.random() * ANON_NAMES.length)] + '_' +
                   Math.floor(Math.random() * 999).toString().padStart(3, '0');
    state.myAvatar = '\u{1F47E}';
    state.isAnonymous = true;
    state.sessionId = 'anon-' + crypto.randomUUID().slice(0, 12);
    state.chatHistory.push({ name: null, system: `${state.myName} steps up (no stats — sign in to track!)` });
    renderChat();
  } else {
    state.isAnonymous = false;
    // Signed-in users join under their persistent id so reload recovery works.
    state.sessionId = state.myId;
  }

  state.inQueue = true;
  state.queuePosition = state.queueData.length + 1;
  const btn = document.getElementById('gotNextBtn');
  btn.textContent = `YOU'RE #${state.queuePosition} IN LINE`;
  btn.classList.add('in-queue');
  document.getElementById('leaveQueueBtn').style.display = 'block';
  document.getElementById('queueCount').textContent = state.queueData.length + 1;

  state.chatHistory.push({ name: null, system: `${state.myName} says I GOT NEXT!` });
  renderChat();
  renderQueue();

  // USB-only: always send the live gamepad id as the device label. If the
  // pad vanished between button click and now, fall back to a generic label.
  if (state.ws?.readyState === 1) {
    const gp = navigator.getGamepads()[0];
    const device = gp ? gp.id.substring(0, 30) : (state.gamepadId || 'Browser');
    state.ws.send(JSON.stringify({ type: 'join', id: state.sessionId, name: state.myName, device }));
  }
}

export function leaveQueue() {
  state.inQueue = false;
  state.queuePosition = -1;
  const btn = document.getElementById('gotNextBtn');
  btn.innerHTML = '&#x1FA99; I GOT NEXT';
  btn.classList.remove('in-queue');
  document.getElementById('leaveQueueBtn').style.display = 'none';
  document.getElementById('queueCount').textContent = state.queueData.length;
  renderQueue();

  if (state.ws?.readyState === 1) {
    state.ws.send(JSON.stringify({ type: 'leave' }));
  }
}

export function leaveGame() {
  state.leaving = true;
  if (state.ws?.readyState === WebSocket.OPEN) {
    if (state.mySlot >= 0) {
      state.ws.send(JSON.stringify({ type: 'leave', id: state.myId }));
    } else if (state.wsInQueue) {
      state.ws.send(JSON.stringify({ type: 'queue_leave' }));
    }
  }
  state.mySlot = -1;
  state.stickSlot = -1;
  state.wsInQueue = false;
  stopGamepadPolling();

  document.getElementById('leaveGameBtn').style.display = 'none';
  const btn = document.getElementById('gotNextBtn');
  btn.innerHTML = '&#x1FA99; I GOT NEXT';
  btn.classList.remove('in-queue');
  btn.disabled = false;
  btn.style.display = '';
  state.inQueue = false;
  document.getElementById('leaveQueueBtn').style.display = 'none';
  updateCabinetControls();
}

// Single source of truth for what the I GOT NEXT button says and whether it's
// shown. Called from sign-in/sign-out, assigned, after leave, and whenever a
// gamepad connects or disconnects.
//
//   - already in a slot     → button hidden, leaveGame button shown
//   - no gamepad detected   → "PLUG IN A GAMEPAD" (greyed, disabled)
//   - gamepad detected      → "I GOT NEXT" (enabled)
//
// USB-only for now. NOBD hardware stick support is parked — we show no
// NOBD UI and always send the gamepad id as the device label on join.
export function updateCabinetControls() {
  const btn = document.getElementById('gotNextBtn');
  if (!btn) return;
  const leaveBtn = document.getElementById('leaveGameBtn');

  // Already a player (or in the server-side queue) → hide step-up button.
  if (state.mySlot >= 0 || state.wsInQueue) {
    btn.style.display = 'none';
    if (leaveBtn) leaveBtn.style.display = 'block';
    return;
  }

  btn.style.display = '';
  btn.classList.remove('in-queue');

  if (!state.gamepadConnected) {
    btn.innerHTML = '&#x1F50C; PLUG IN A GAMEPAD';
    btn.title = 'Connect a USB gamepad to play. Press any button if it\u2019s plugged in but not detected.';
    btn.disabled = true;
    return;
  }

  btn.disabled = false;
  btn.innerHTML = '&#x1FA99; I GOT NEXT';
  btn.title = state.signedIn
    ? 'Step up to play'
    : 'Step up to play (sign in to track stats)';
}
