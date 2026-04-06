// ============================================================================
// QUEUE.MJS — Queue management, gotNext, leave
// ============================================================================

import { state } from './state.mjs';
import { renderChat } from './chat.mjs';
import { stopGamepadPolling } from './gamepad.mjs';
import { ANON_NAMES } from './ui-common.mjs';

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
  list.innerHTML = serverQueue.map((name, i) => {
    const isMe = state.signedIn && name.toUpperCase() === state.myName;
    return `<div class="queue-entry ${i === 0 ? 'next-up' : ''} ${isMe ? 'is-me' : ''}">
      <div class="queue-pos">${i + 1}</div>
      <div class="queue-name">${String(name).toUpperCase()}</div>
    </div>`;
  }).join('');
}

export function gotNext() {
  if (state.inQueue) return;

  // Anonymous players can play, just no stats
  if (!state.signedIn) {
    state.myName = ANON_NAMES[Math.floor(Math.random() * ANON_NAMES.length)] + '_' +
                   Math.floor(Math.random() * 999).toString().padStart(3, '0');
    state.myAvatar = '\u{1F47E}';
    state.isAnonymous = true;
    state.chatHistory.push({ name: null, system: `${state.myName} steps up (no stats — sign in to track!)` });
    renderChat();
  } else {
    state.isAnonymous = false;
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

  // Send join to server
  if (state.ws?.readyState === 1) {
    let device = localStorage.getItem('maplecast_stick') ? 'NOBD Stick' : 'Browser';
    const gp = navigator.getGamepads()[0];
    if (gp && !localStorage.getItem('maplecast_stick')) device = gp.id.substring(0, 30);
    state.ws.send(JSON.stringify({ type: 'join', id: state.myId, name: state.myName, device }));
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
}
