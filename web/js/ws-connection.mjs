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
import { nodeState } from './node-router.mjs';
// handleBinaryFrame import removed — binary frames are owned by render-worker.mjs
// (spawned by renderer-bridge.mjs). The main-thread JSON connection below
// drops any binary frames defensively in case the relay ever sends one here.
import { systemMessage } from './chat.mjs';
import { updateLobbyState, updateNameOverlay } from './lobby.mjs';
import { showMatchResults } from './demo.mjs';
import { startGamepadPolling, stopGamepadPolling } from './gamepad.mjs';
// updateStickButtons was removed when gamepad.mjs went USB-only. The legacy
// register_complete handler at line ~435 still calls it — stub to no-op so
// the module graph resolves.
const updateStickButtons = () => {};
import { autoSignIn } from './auth.mjs';
import { leaveGame, updateCabinetControls } from './queue.mjs';
import { avg } from './ui-common.mjs';

const WS_PORT = 7200;

// ============================================================================
// TWO WebSocket connections per browser, with very different lifecycles:
//
//   state.ws        — RELAY WS, ALWAYS-ON. Connects to wss://nobd.net/ws (the
//                     VPS relay on :7201). Carries the broadcast TA frame
//                     downstream + status JSON. Every browser opens this on
//                     page load and never closes it. Spectators only have
//                     this connection.
//
//   state.controlWs — DIRECT FLYCAST WS, LAZY. Connects to wss://<host>/play,
//                     which nginx proxies straight to flycast on
//                     127.0.0.1:7210 — bypassing the relay. Opens ONLY when
//                     the user clicks I GOT NEXT or RECLAIM, closes on
//                     leave/kick. Carries: join, leave, gamepad input
//                     (4-byte binary). Receives: assigned, kicked, status.
//
// Why two connections?
//
// flycast's `_connSlot[hdl]` registry keys on the upstream WS handle. The
// relay multiplexes ALL its clients onto a single shared upstream hdl, so
// two players joining via the relay would collide (both map to the same
// hdl → last-write-wins → one player can't send input). Direct per-browser
// connections on /play give each player their own hdl, so the existing
// single-hdl-per-slot logic just works.
//
// Why not always use /play?
//
// Spectators (95% of traffic) don't need to write to flycast at all, and
// the relay is what provides the frame cache + SYNC replay for late joiners.
// Opening a direct flycast WS for every spectator would burn flycast's
// connection budget and skip the SYNC cache. Lazy-open keeps spectators on
// the cheap relay path and only upgrades to a direct connection when the
// user actually steps up to the cabinet.
//
// Pre-2026-04-08 this pointed at wss://home.nobd.net/ws (a completely
// separate flycast instance on the home box). After the headless migration
// both WS endpoints terminate at the same nobd.net nginx — /ws goes to the
// relay, /play goes directly to loopback flycast.
// ============================================================================

// Use the same origin as the page. /play is a same-host nginx location that
// proxies directly to flycast:7210 (bypassing the relay). Dev (localhost:8000)
// doesn't have an nginx in front, so we fall back to the relay port there.
//
// If a distributed node is assigned (via node-router.mjs), use that node's
// /play endpoint instead of the page origin.
function getControlWsUrl() {
  // Distributed node assignment overrides origin
  if (nodeState.assignedNode) return nodeState.assignedNode.control_url;

  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const isProd = location.port === '' || location.port === '80' || location.port === '443';
  if (isProd) return `${proto}//${location.hostname}/play`;
  // Dev: no /play nginx block, talk straight to the relay — acceptable for
  // single-player local testing since there's only one browser anyway.
  return `${proto}//${location.hostname}:${WS_PORT}`;
}

// 30 second idle timer — if the browser is no longer playing/queued/resuming
// after this many ms, close the controlWs to free the flycast hdl.
const CONTROL_IDLE_CLOSE_MS = 30_000;

// Public: the render worker calls this to find its frame source. Async to
// match the previous LAN-race signature (the worker awaits it), even though
// the URL is now picked synchronously since the race is gone.
export async function getRendererWsUrl() {
  // If assigned to a distributed node, stream from that node's relay
  if (nodeState.assignedNode) return nodeState.assignedNode.relay_url;

  // Frame downstream stays on whatever served the page (the VPS relay in
  // production, localhost:8000 in dev). Browsers connect to wss://nobd.net/ws
  // (relay) or ws://localhost:7200 (dev), depending on origin.
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const isRelay = location.port === '' || location.port === '80' || location.port === '443';
  if (isRelay) return `${proto}//${location.hostname}/ws`;
  return `${proto}//${location.hostname}:${WS_PORT}`;
}

// Public: the render worker calls this to find the DEDICATED audio
// WebSocket source. Audio rides its own TCP socket to flycast's
// maplecast_audio_ws listener on its own io_service thread — nothing
// about this path can head-of-line block the TA mirror video pipe on
// /ws. In relay mode (wss://nobd.net/audio) it goes through an nginx
// proxy block that forwards directly to 127.0.0.1:7213 on the VPS.
export async function getRendererAudioWsUrl() {
  // If assigned to a distributed node, audio from that node
  if (nodeState.assignedNode) return nodeState.assignedNode.audio_url;

  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const isRelay = location.port === '' || location.port === '80' || location.port === '443';
  if (isRelay) return `${proto}//${location.hostname}/audio`;
  return `${proto}//${location.hostname}:7213`;
}


// The relay WS URL — same logic as the renderer URL since they share an
// origin. Spectators connect here on page load.
function getRelayWsUrl() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const isRelay = location.port === '' || location.port === '80' || location.port === '443';
  if (isRelay) return `${proto}//${location.hostname}/ws`;
  return `${proto}//${location.hostname}:${WS_PORT}`;
}

// ============================================================================
// state.controlWs lifecycle helpers
// ============================================================================

let _controlWsOpenPromise = null;
let _controlIdleTimer = null;

/**
 * Open the direct flycast control WS if it isn't already open. Returns a
 * promise that resolves to the WebSocket once it's ready to send. Idempotent:
 * subsequent calls while the connection is open return the same socket.
 * Subsequent calls while it's still opening return the same in-flight promise.
 *
 * Throws on failure (CGNAT user can't reach home.nobd.net, cert error, etc.)
 * — callers should catch and surface "cannot reach game server" UI.
 */
export function ensureControlWs() {
  // Already open
  if (state.controlWs && state.controlWs.readyState === WebSocket.OPEN) {
    return Promise.resolve(state.controlWs);
  }
  // In flight
  if (_controlWsOpenPromise) return _controlWsOpenPromise;

  _controlWsOpenPromise = new Promise((resolve, reject) => {
    console.log('[control-ws] Opening', getControlWsUrl());
    const ws = new WebSocket(getControlWsUrl());
    ws.binaryType = 'arraybuffer';
    state.controlWs = ws;

    let settled = false;
    const settle = (fn, val) => {
      if (settled) return;
      settled = true;
      _controlWsOpenPromise = null;
      fn(val);
    };

    ws.onopen = () => {
      console.log('[control-ws] open');
      // Personal-reply receive handler — flycast sends back assigned/kicked/pong
      // on the direct WS. The actual lobby/queue/slot UI updates come from
      // SurrealDB live queries, NOT from these messages.
      ws.onmessage = (e) => {
        if (typeof e.data !== 'string') return; // defensive: drop binary frames
        let msg;
        try { msg = JSON.parse(e.data); } catch { return; }
        switch (msg.type) {
          case 'assigned':
            // Flycast confirmed our slot. The slot row in SurrealDB was already
            // updated optimistically by queue.mjs handleMyPromotion. Nothing to
            // do here unless flycast rejected us (slot < 0).
            if (msg.slot < 0) {
              console.warn('[control-ws] flycast rejected join');
              state.mySlot = -1;
            }
            break;
          case 'kicked': {
            // Server-side eviction. Reasons we see in practice:
            //   - "idle"     : input was unchanged + no heartbeat for 30s
            //   - "ghost"    : another tab joined with the same name and
            //                  flycast's ghost-slot eviction kicked us
            //   - "leave"    : the user clicked DISCONNECT in another tab
            //                  which propagated to /api/leave → flycast
            //
            // In all cases this tab needs a full local cleanup: close the
            // controlWs, stop polling, drop mySlot, refresh the cabinet
            // controls so the right button comes back, and surface a
            // visible message in chat so the user knows what happened.
            console.log('[control-ws] kicked by server:', msg.reason);
            const wasInSlot = state.mySlot;
            state.mySlot = -1;
            stopGamepadPolling();
            try { ws.close(); } catch {}
            state.controlWs = null;
            // Lazy import to avoid the auth.mjs ↔ queue.mjs ↔ ws-connection
            // import cycle going stale.
            import('./queue.mjs').then(m => m.updateCabinetControls?.());
            const friendly = msg.reason === 'idle'
              ? `You were kicked from P${wasInSlot + 1} (idle).`
              : msg.reason === 'ghost'
                ? `You were taken over by another tab.`
                : `You were disconnected from P${wasInSlot + 1}.`;
            systemMessage(friendly);
            break;
          }
          case 'pong':
            // Optional latency measurement; ignore for now
            break;
          default:
            // Unknown messages from flycast — log and ignore
            console.log('[control-ws] msg:', msg.type);
        }
      };
      settle(resolve, ws);
    };

    ws.onerror = (e) => {
      console.warn('[control-ws] error', e);
      settle(reject, new Error('control ws error'));
    };

    ws.onclose = () => {
      console.log('[control-ws] closed');
      if (state.controlWs === ws) state.controlWs = null;
      settle(reject, new Error('control ws closed before open'));
      // Do NOT auto-reconnect. The relay WS reconnects because it's the
      // page-lifetime broadcast bus; the control WS only exists when the
      // user is actively playing and they'll explicitly reopen by clicking
      // RESUME or I GOT NEXT.
    };

    // 5s open timeout — if home.nobd.net is unreachable, fail loudly so the
    // caller can surface a "cannot reach game server" toast.
    setTimeout(() => settle(reject, new Error('control ws open timeout')), 5000);
  });

  return _controlWsOpenPromise;
}

/**
 * Close the direct flycast control WS if open. Idempotent.
 * Caller should ensure any final messages have drained first (or accept loss).
 */
export function closeControlWs(reason = 'manual') {
  if (_controlIdleTimer) { clearTimeout(_controlIdleTimer); _controlIdleTimer = null; }
  const ws = state.controlWs;
  if (!ws) return;
  console.log('[control-ws] closing —', reason);
  state.controlWs = null;
  try { ws.close(); } catch {}
}

/**
 * Reset the idle close timer. Called whenever the user is in an "active"
 * state (queued, playing, or pending resume). When activity stops, the timer
 * fires after CONTROL_IDLE_CLOSE_MS and closes the control WS.
 */
export function bumpControlIdle() {
  if (_controlIdleTimer) clearTimeout(_controlIdleTimer);
  _controlIdleTimer = setTimeout(() => {
    if (state.mySlot < 0 && !state.wsInQueue && !state.pendingResumeSlot) {
      closeControlWs('idle');
    }
  }, CONTROL_IDLE_CLOSE_MS);
}

// ============================================================================
// connectWS — opens the always-on relay WS (state.ws)
// ============================================================================

export async function connectWS() {
  state.connState = 'CONNECTING...';
  const wsUrl = getRelayWsUrl();
  console.log('[ws] Relay URL:', wsUrl);

  const ws = new WebSocket(wsUrl);
  ws.binaryType = 'arraybuffer';
  state.ws = ws;

  ws.onopen = () => {
    console.log('[ws] Relay connection open');
    state.connState = 'CONNECTED';
    // No check_stick here — that goes through controlWs when the user
    // explicitly tries to play (gotNext / RESUME). Spectators don't need it.
  };

  ws.onclose = () => {
    console.log('[ws] Control disconnected, reconnecting...');
    state.connState = 'OFFLINE';
    setTimeout(connectWS, 2000);
  };

  ws.onmessage = (e) => {
    // Binary frames are owned by render-worker.mjs (the WASM renderer worker
    // spawned by renderer-bridge.mjs) on its own dedicated WebSocket. Drop any
    // binary that lands here defensively.
    if (typeof e.data !== 'string') return;

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
        systemMessage(`${state.myName} is now P${state.mySlot + 1}!`);
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
      systemMessage(msg.msg || `${state.myName} — IT'S YOUR TURN!`);
      state.wsInQueue = false;
      {
        const device = localStorage.getItem('maplecast_stick') ? 'NOBD Stick' : 'Browser';
        // Use sessionId so anon and signed-in joins behave consistently with
        // gotNext(). Falls back to myId if gotNext was never called this session.
        const id = state.sessionId || state.myId;
        // Per-user latch policy ride-along — see queue.mjs for the full
        // rationale. Lazy import to avoid widening this module's imports.
        import('./diagnostics.mjs').then(m => {
          ws.send(JSON.stringify({
            type: 'join', id, name: state.myName, device,
            latch_policy: m.getPreferredLatchPolicy?.() || 'latency',
          }));
        }).catch(() => {
          ws.send(JSON.stringify({ type: 'join', id, name: state.myName, device }));
        });
      }
      break;

    case 'match_end':
      systemMessage(`${msg.winner_name} WINS!`);
      showMatchResults();
      if (msg.loser === state.mySlot) setTimeout(() => leaveGame(), 3000);
      break;

    case 'kicked':
      // Server-side eviction (safety net if our match_end self-disconnect missed).
      // Clear mySlot first so leaveGame() skips sending a redundant 'leave' back.
      console.log('[ws] kicked by server:', msg.reason);
      systemMessage('You were kicked from the cabinet.');
      state.mySlot = -1;
      leaveGame();
      break;

    case 'chat':
      // Dead path: chat now flows via SurrealDB live query, not WS broadcast.
      // Kept as a no-op so any in-flight WS chat messages from a stale flycast
      // build don't crash the dispatcher. Phase 6 cleanup will delete this case
      // when flycast stops broadcasting it.
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

    systemMessage(`Reconnected to slot P${serverSlot + 1}.`);
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
  // Phase A — per-slot input latch telemetry. Backed by the LatchStatsAccum
  // ring buffer in core/network/maplecast_input_server.cpp; see Phase A.4-5.
  // Null until the server includes the block (older servers / pre-Phase-A
  // builds).
  d.latchStats = msg.latch_stats || null;
  // Phase B — frame phase + per-slot latch policy. Drives the phase-aligned
  // gamepad sender (B.8) and the diagnostics overlay's policy display (B.9).
  d.framePhase = msg.frame_phase || null;
  d.latchPolicy = msg.latch_policy || null;
  // Wake the gamepad scheduler so it re-arms its phase-timer for this
  // frame. Lazy-import to avoid the gamepad ↔ ws-connection import cycle.
  if (d.framePhase) {
    import('./gamepad.mjs').then(m => {
      m.onServerFramePhase?.(d.framePhase, d.pingMs || 0);
    });
  }
  if (msg.game) d.game = msg.game;

  // Slot state used to be reconciled from this broadcast. DISABLED — the
  // relay WS status comes from the nobd.net fanout flycast, which does NOT
  // own player connections anymore. The canonical slot table lives in
  // SurrealDB (written by the collector from HOME flycast's status), and
  // the browser subscribes to it via queue.mjs initSlotLive. Letting
  // syncSlotFromStatus run here racks the UI in a loop:
  //    1. handleMyPromotion opens controlWs to home, sets mySlot = 0
  //    2. next relay-status tick says p1 is empty (home flycast has it,
  //       nobd.net fanout doesn't)
  //    3. syncSlotFromStatus "dropped us from slot 0", clears mySlot
  //    4. gamepad polling stops (gates on mySlot >= 0), player is stuck
  //
  // Do not re-enable without rewiring the relay status source.
  // syncSlotFromStatus(msg);

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
      // Per-user latch policy ride-along (see queue.mjs for the rationale).
      import('./diagnostics.mjs').then(m => {
        state.ws.send(JSON.stringify({
          type: 'join', id, name: state.myName, device: devName,
          latch_policy: m.getPreferredLatchPolicy?.() || 'latency',
        }));
      }).catch(() => {
        state.ws.send(JSON.stringify({ type: 'join', id, name: state.myName, device: devName }));
      });
    }
  }

  updateLobbyState(msg);
  updateNameOverlay();
}
