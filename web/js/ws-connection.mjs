// ============================================================================
// WS-CONNECTION.MJS — Dual WebSocket architecture
//
// CONNECTION 1 (Worker): Dedicated binary frame pipe. Runs on separate thread.
//   Worker receives WS binary frames and transfers ArrayBuffer to main thread
//   via postMessage with Transferable — ZERO COPY. Frame data never touches
//   the main thread event loop until it's ready to render.
//
// CONNECTION 2 (Main): JSON control messages (status, chat, queue, auth).
//   Handles all UI interactions. Binary frames from this connection are
//   ignored (Worker handles them).
//
// WHY: Main thread event loop handles DOM updates, gamepad polling, chat
//   rendering, JSON parsing — any of these can delay binary frame processing
//   by 1-5ms. The Worker thread has NOTHING on it except recv → transfer.
//
// OVERKILL IS NECESSARY.
// ============================================================================

import { state } from './state.mjs';
import { handleBinaryFrame } from './renderer-bridge.mjs';
import { renderChat } from './chat.mjs';
import { updateLobbyState, updateNameOverlay } from './lobby.mjs';
import { showNewChallenger, showMatchResults } from './demo.mjs';
import { startGamepadPolling, updateStickButtons } from './gamepad.mjs';
import { autoSignIn } from './auth.mjs';
import { leaveGame } from './queue.mjs';
import { avg } from './ui-common.mjs';

const WS_PORT = 7200;

// Relay mode: served from VPS (port 80/443) → use /ws proxy path
// Direct mode: served from home box (port 8000) → use :7200 direct
function getWsUrl() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const isRelay = location.port === '' || location.port === '80' || location.port === '443';
  if (isRelay) return `${proto}//${location.hostname}/ws`;
  return `${proto}//${location.hostname}:${WS_PORT}`;
}

let frameWorker = null;

export function connectWS() {
  state.connState = 'CONNECTING...';
  const wsUrl = getWsUrl();
  console.log('[ws] URL:', wsUrl);

  // === CONNECTION 1: Worker for binary frames (dedicated thread) ===
  try {
    // Workers need a classic script URL, not a module
    const workerBlob = new Blob([`
      let ws = null;
      self.onmessage = (e) => {
        if (e.data.type === 'connect') connect(e.data.url);
      };
      function connect(url) {
        if (ws) ws.close();
        ws = new WebSocket(url);
        ws.binaryType = 'arraybuffer';
        ws.onopen = () => self.postMessage({ type: 'open' });
        ws.onclose = () => {
          self.postMessage({ type: 'close' });
          setTimeout(() => connect(url), 2000);
        };
        ws.onmessage = (e) => {
          if (typeof e.data === 'string') return;
          self.postMessage({ type: 'frame', buffer: e.data }, [e.data]);
        };
      }
    `], { type: 'application/javascript' });
    const workerUrl = URL.createObjectURL(workerBlob);
    frameWorker = new Worker(workerUrl);

    frameWorker.onmessage = (e) => {
      if (e.data.type === 'frame') {
        // Binary frame transferred from Worker — zero copy, render immediately
        handleBinaryFrame(e.data.buffer);
      } else if (e.data.type === 'open') {
        console.log('[ws-worker] Frame pipe connected');
      } else if (e.data.type === 'close') {
        state.rendererStreaming = false;
      }
    };

    frameWorker.postMessage({ type: 'connect', url: wsUrl });
    console.log('[ws] Worker frame pipe started');
  } catch (err) {
    console.warn('[ws] Worker failed, falling back to single connection:', err.message);
    frameWorker = null;
  }

  // === CONNECTION 2: Main thread for JSON control ===
  const ws = new WebSocket(wsUrl);
  ws.binaryType = 'arraybuffer';
  state.ws = ws;

  ws.onopen = () => {
    console.log('[ws] Control connection open');
    state.connState = 'CONNECTED';
    const saved = localStorage.getItem('nobd_username');
    if (saved && !state.signedIn) {
      ws.send(JSON.stringify({ type: 'check_stick', username: saved }));
    }
  };

  ws.onclose = () => {
    console.log('[ws] Control disconnected, reconnecting...');
    state.connState = 'OFFLINE';
    setTimeout(connectWS, 2000);
  };

  ws.onmessage = (e) => {
    // If Worker is handling binary, skip binary frames on main connection
    if (typeof e.data !== 'string') {
      if (!frameWorker) {
        // Fallback: no Worker, handle binary on main thread
        handleBinaryFrame(e.data);
      }
      return;
    }

    // JSON messages → defer to microtask
    const raw = e.data;
    Promise.resolve().then(() => {
      try {
        handleJsonMessage(JSON.parse(raw));
      } catch (err) {}
    });
  };
}

function handleJsonMessage(msg) {
  const ws = state.ws;

  switch (msg.type) {
    case 'status':
      handleStatus(msg);
      break;

    case 'pong': {
      const rtt = performance.now() - msg.t;
      state.diag.pingSamples.push(rtt);
      if (state.diag.pingSamples.length > 10) state.diag.pingSamples.shift();
      state.diag.pingMs = avg(state.diag.pingSamples);
      break;
    }

    case 'stick_status':
      state.stickOnline = msg.online;
      if (msg.registered && !state.signedIn) autoSignIn(msg.username);
      break;

    case 'assigned':
      state.mySlot = msg.slot;
      if (state.mySlot >= 0) {
        state.chatHistory.push({ name: null, system: `${state.myName} is now P${state.mySlot + 1}!` });
        renderChat();
        startGamepadPolling();
        state.wsInQueue = false;
        document.getElementById('leaveGameBtn').style.display = 'block';
        document.getElementById('gotNextBtn').style.display = 'none';
        document.getElementById('leaveQueueBtn').style.display = 'none';
      } else if (state.leaving) {
        state.leaving = false;
      } else {
        state.wsInQueue = true;
        ws.send(JSON.stringify({ type: 'queue_join', name: state.myName }));
        const btn = document.getElementById('gotNextBtn');
        btn.textContent = 'IN LINE...';
        btn.classList.add('in-queue');
        btn.disabled = true;
        document.getElementById('leaveQueueBtn').style.display = 'block';
        document.getElementById('leaveGameBtn').style.display = 'block';
      }
      break;

    case 'register_started':
      break;

    case 'your_turn':
      showNewChallenger(state.myName);
      state.chatHistory.push({ name: null, system: msg.msg || `${state.myName} — IT'S YOUR TURN!` });
      renderChat();
      state.wsInQueue = false;
      {
        const device = localStorage.getItem('maplecast_stick') ? 'NOBD Stick' : 'Browser';
        ws.send(JSON.stringify({ type: 'join', id: state.myId, name: state.myName, device }));
      }
      break;

    case 'match_end':
      state.chatHistory.push({ name: null, system: `${msg.winner_name} WINS!` });
      renderChat();
      showMatchResults();
      if (msg.loser === state.mySlot) setTimeout(() => leaveGame(), 3000);
      break;

    case 'chat':
      state.chatHistory.push({ name: msg.name || '???', text: msg.text, king: false });
      renderChat();
      break;

    default:
      if (msg.type?.startsWith('relay_') && state.relay) {
        state.relay.handleMessage(msg);
      }
      break;
  }
}

function handleStatus(msg) {
  const d = state.diag;
  d.p1 = msg.p1 || {};
  d.p2 = msg.p2 || {};
  d.spectators = msg.spectators || 0;
  d.queue = msg.queue || [];
  d.serverFrame = msg.frame || 0;
  d.fps = msg.fps || 0;
  d.streamKbps = msg.stream_kbps || 0;
  d.publishUs = msg.publish_us || 0;
  d.dirtyPages = msg.dirty || 0;
  if (msg.game) d.game = msg.game;

  if (d._wasRegistering && !msg.registering) {
    localStorage.setItem('maplecast_stick', state.myId);
    clearTimeout(state.registerTimeout);
    const regStatus = document.getElementById('registerStatus');
    regStatus.textContent = 'Stick registered!';
    setTimeout(() => { regStatus.style.display = 'none'; updateStickButtons(); }, 3000);
  }
  d._wasRegistering = msg.registering;

  if (state.signedIn && localStorage.getItem('maplecast_stick')) {
    if (++state.stickCheckCounter >= 5) {
      state.stickCheckCounter = 0;
      state.ws.send(JSON.stringify({ type: 'check_stick', username: state.myName.toLowerCase() }));
    }
  }

  if (state.wsInQueue && state.mySlot < 0) {
    const p1open = !msg.p1?.connected;
    const p2open = !msg.p2?.connected;
    if (p1open || p2open) {
      state.wsInQueue = false;
      state.ws.send(JSON.stringify({ type: 'queue_leave' }));
      const device = localStorage.getItem('maplecast_stick') ? 'NOBD Stick' : 'Browser';
      const gp = navigator.getGamepads()[0];
      const devName = gp && !localStorage.getItem('maplecast_stick') ? gp.id.substring(0, 30) : device;
      state.ws.send(JSON.stringify({ type: 'join', id: state.myId, name: state.myName, device: devName }));
    }
  }

  updateLobbyState(msg);
  updateNameOverlay();
}
