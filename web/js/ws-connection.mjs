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
import { startGamepadPolling, stopGamepadPolling, updateStickButtons } from './gamepad.mjs';
import { autoSignIn } from './auth.mjs';
import { leaveGame, updateCabinetControls } from './queue.mjs';
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

export async function connectWS() {
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
    // Always fire check_stick on connect so a reloaded tab learns its
    // current slot from the server (the reload-resync path). The handler
    // at case 'stick_status' below reclaims state.mySlot if appropriate.
    const username = state.signedIn
      ? state.myName.toLowerCase()
      : localStorage.getItem('nobd_username');
    if (username) {
      ws.send(JSON.stringify({ type: 'check_stick', username }));
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

  // Free our slot on tab close/reload so the next tab session doesn't see a
  // ghost player still occupying P1/P2. Server has its own 5s safety-net
  // eviction (see maplecast_ws_server.cpp), but explicit leave is instant
  // and avoids the race where the user reloads, sends `check_stick`, and
  // gets back the wrong slot from the dying connection.
  if (!_unloadHooked) {
    _unloadHooked = true;
    window.addEventListener('beforeunload', () => {
      try {
        if (state.ws?.readyState === WebSocket.OPEN && state.mySlot >= 0) {
          state.ws.send(JSON.stringify({ type: 'leave', id: state.myId }));
        }
      } catch {}
    });
  }
}

let _unloadHooked = false;

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
      state.stickRegistered = !!msg.registered;
      state.stickSlot = (typeof msg.slot === 'number') ? msg.slot : -1;
      if (msg.registered && !state.signedIn) autoSignIn(msg.username);

      // Reload resync: server says we already occupy a slot. Bind it back
      // without sending a fresh `join` (which would land us in the *other*
      // slot and double-book the user — the screenshot bug). Gamepad polling
      // is harmless if NOBD UDP is the actual input source: bufferedAmount
      // backpressure keeps the WS quiet when there's nothing to send.
      if (state.stickSlot >= 0 && state.mySlot < 0) {
        state.mySlot = state.stickSlot;
        startGamepadPolling();
        document.getElementById('leaveGameBtn').style.display = 'block';
        document.getElementById('gotNextBtn').style.display = 'none';
        document.getElementById('leaveQueueBtn').style.display = 'none';
      }
      updateCabinetControls();
      break;

    case 'stick_event':
      // Authoritative server-side stick lifecycle. We re-poll our own status
      // so the UI reflects "your stick just came online" without waiting for
      // the next status broadcast.
      if (state.signedIn && msg.events?.some(e => e.username === state.myName.toLowerCase())) {
        state.ws.send(JSON.stringify({ type: 'check_stick', username: state.myName.toLowerCase() }));
      }
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
        updateCabinetControls();
      } else if (state.leaving) {
        state.leaving = false;
        updateCabinetControls();
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
        // Use sessionId so anon and signed-in joins behave consistently with
        // gotNext(). Falls back to myId if gotNext was never called this session.
        const id = state.sessionId || state.myId;
        ws.send(JSON.stringify({ type: 'join', id, name: state.myName, device }));
      }
      break;

    case 'match_end':
      state.chatHistory.push({ name: null, system: `${msg.winner_name} WINS!` });
      renderChat();
      showMatchResults();
      if (msg.loser === state.mySlot) setTimeout(() => leaveGame(), 3000);
      break;

    case 'kicked':
      // Server-side eviction (safety net if our match_end self-disconnect missed).
      // Clear mySlot first so leaveGame() skips sending a redundant 'leave' back.
      console.log('[ws] kicked by server:', msg.reason);
      state.chatHistory.push({ name: null, system: 'You were kicked from the cabinet.' });
      renderChat();
      state.mySlot = -1;
      leaveGame();
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

// Reconcile state.mySlot against the authoritative server status. Called on
// every status tick — cheap when nothing changed (just two string compares).
function syncSlotFromStatus(msg) {
  // Skip during a deliberate leave so we don't re-bind to a slot the server
  // hasn't yet released. The 'assigned' {slot:-1} response will clear the flag.
  if (state.leaving) return;

  // Anonymous users have no persistent identity worth recovering — if their
  // tab dies, the next visit should be a clean spectator session. Reload-
  // recovery is a feature for signed-in players. The server-side close
  // handler / 30s idle kick will clean up the orphaned anon slot.
  //
  // This also defends against an old anon slot still bound to state.myId
  // from a pre-fix gotNext() click: a fresh tab in spectator mode would
  // otherwise silently inherit it. With this guard the orphan just times out.
  if (!state.signedIn) return;

  const myShortId = (state.myId || '').substring(0, 8);
  if (!myShortId) return;

  // Which slot does the server think we're in? (-1 = none)
  let serverSlot = -1;
  if (msg.p1?.connected && msg.p1.id === myShortId) serverSlot = 0;
  else if (msg.p2?.connected && msg.p2.id === myShortId) serverSlot = 1;

  if (serverSlot === state.mySlot) return;  // already in sync

  if (serverSlot >= 0 && state.mySlot < 0) {
    // We're holding a slot but our local state thinks we're not — page reload
    // or tab recovery. Adopt the server's view and surface the leave button.
    console.log('[ws] reconnect sync: adopting server slot', serverSlot);
    state.mySlot = serverSlot;
    state.wsInQueue = false;
    state.inQueue = false;

    // Make sure we have a name. If we signed in via localStorage, state.myName
    // is already set. If we joined anonymously and reloaded, we lost the
    // anon name — pull it from the server status (it knows the display name).
    const slotInfo = serverSlot === 0 ? msg.p1 : msg.p2;
    if (!state.myName && slotInfo?.name) {
      state.myName = slotInfo.name.toUpperCase();
    }

    // UI: hide queue/got-next, show leave-game
    const gotNextBtn = document.getElementById('gotNextBtn');
    const leaveQueueBtn = document.getElementById('leaveQueueBtn');
    const leaveGameBtn = document.getElementById('leaveGameBtn');
    if (gotNextBtn) gotNextBtn.style.display = 'none';
    if (leaveQueueBtn) leaveQueueBtn.style.display = 'none';
    if (leaveGameBtn) leaveGameBtn.style.display = 'block';

    // Resume sending gamepad input from this tab
    startGamepadPolling();

    state.chatHistory.push({
      name: null,
      system: `Reconnected to slot P${serverSlot + 1}.`,
    });
    renderChat();
    return;
  }

  if (serverSlot < 0 && state.mySlot >= 0) {
    // Server kicked us / our connection dropped without a clean leave.
    // Mirror the kicked-handler cleanup so we're back in spectator mode.
    console.log('[ws] reconnect sync: server dropped us from slot', state.mySlot);
    state.mySlot = -1;
    state.wsInQueue = false;
    state.inQueue = false;
    stopGamepadPolling();
    const gotNextBtn = document.getElementById('gotNextBtn');
    const leaveQueueBtn = document.getElementById('leaveQueueBtn');
    const leaveGameBtn = document.getElementById('leaveGameBtn');
    if (gotNextBtn) {
      gotNextBtn.innerHTML = '&#x1FA99; I GOT NEXT';
      gotNextBtn.classList.remove('in-queue');
      gotNextBtn.disabled = false;
      gotNextBtn.style.display = '';
    }
    if (leaveQueueBtn) leaveQueueBtn.style.display = 'none';
    if (leaveGameBtn) leaveGameBtn.style.display = 'none';
    return;
  }

  // serverSlot >= 0 && state.mySlot >= 0 && they differ — slot swap
  // (rare; e.g. p1 left, server promoted us from p2 to p1). Just adopt.
  if (serverSlot !== state.mySlot && serverSlot >= 0) {
    console.log('[ws] reconnect sync: slot swap', state.mySlot, '→', serverSlot);
    state.mySlot = serverSlot;
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

  // === RECONNECT SYNC ===
  // Server is the authority on which slot we're in. The status message
  // includes the first 8 chars of each occupant's id; if it matches ours,
  // we're already that player — make the UI reflect that. Covers two cases:
  //   1. Page reload while we still hold a slot
  //   2. Tab survived a transient WS disconnect/reconnect
  //
  // The opposite direction matters too: if we *think* we're in a slot but
  // the server doesn't agree, we got dropped — clean up our state.
  syncSlotFromStatus(msg);

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
      // sessionId for anon/signed-in parity (see gotNext rationale).
      const id = state.sessionId || state.myId;
      state.ws.send(JSON.stringify({ type: 'join', id, name: state.myName, device: devName }));
    }
  }

  updateLobbyState(msg);
  updateNameOverlay();
}
