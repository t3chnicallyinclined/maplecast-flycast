// ============================================================================
// CHAT.MJS — Trash Talk chat system
//
// PERF: renderChat() appends only the last message (insertAdjacentHTML).
//       renderChatFull() does full rebuild (init only).
// ============================================================================

import { state } from './state.mjs';
import { escapeHtml } from './ui-common.mjs';

function msgToHtml(m) {
  if (m.system) return `<div class="chat-msg"><span class="msg-system">&gt; ${m.system}</span></div>`;
  if (m.hype) return `<div class="chat-msg"><span class="msg-hype">\u{1F525} ${m.hype}</span></div>`;
  const cls = m.king ? 'msg-name king' : 'msg-name';
  return `<div class="chat-msg"><span class="${cls}">${m.name}</span>: <span class="msg-text">${escapeHtml(m.text)}</span></div>`;
}

// Full rebuild — call ONCE at init
export function renderChatFull() {
  const el = document.getElementById('chatMessages');
  el.innerHTML = state.chatHistory.map(msgToHtml).join('');
  el.scrollTop = el.scrollHeight;
}

// Incremental append — call on each new message (no DOM nuke)
export function renderChat() {
  const el = document.getElementById('chatMessages');
  const last = state.chatHistory[state.chatHistory.length - 1];
  if (!last) return;
  el.insertAdjacentHTML('beforeend', msgToHtml(last));
  el.scrollTop = el.scrollHeight;
}

export function sendChat() {
  const input = document.getElementById('chatInput');
  const text = input.value.trim();
  if (!text) return;
  const chatName = state.myName || 'ANON';
  if (state.ws?.readyState === 1) {
    state.ws.send(JSON.stringify({ type: 'chat', name: chatName, text }));
  }
  state.chatHistory.push({ name: chatName, text, king: false });
  renderChat();
  input.value = '';
}

export function sendReaction(type) {
  const chatName = state.myName || 'ANON';
  const reactions = {
    'HYPE': '\u{1F525} HYPE!!!',
    'BODIED': '\u{1F480} BODIED',
    'RESPECT': '\u{1F451} GGs RESPECT',
    'SALTY': '\u{1F9C2} SALTY',
    'FRAUD': '\u{1F6A8} FRAUD DETECTED',
  };
  const text = reactions[type] || type;
  if (state.ws?.readyState === 1) {
    state.ws.send(JSON.stringify({ type: 'chat', name: chatName, text }));
  }
  state.chatHistory.push({ name: chatName, text, king: false });
  renderChat();
}
