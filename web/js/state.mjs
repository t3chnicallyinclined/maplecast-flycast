// ============================================================================
// STATE.MJS — Shared mutable state container
//
// Single source of truth. Every module imports the same object reference.
// Mutations are direct property writes — zero overhead, zero events.
// ============================================================================

// Persistent client ID — survives page closes (localStorage)
let myId = localStorage.getItem('maplecast_id');
if (!myId) {
  myId = (crypto.randomUUID ? crypto.randomUUID()
    : ([1e7]+-1e3+-4e3+-8e3+-1e11).replace(/[018]/g, c =>
        (c ^ crypto.getRandomValues(new Uint8Array(1))[0] & 15 >> c / 4).toString(16)));
  localStorage.setItem('maplecast_id', myId);
}

// Ephemeral session id — fresh per page load. We INTENTIONALLY don't
// persist this anywhere. Tab refresh / navigation = lose your spot
// (modulo the 30s grace window the collector enforces on slot rows).
// This matches the user-facing rule: "if you leave, you're out, but
// you have ~30 seconds to come back if you're quick."
const savedSessionId = null;

export const state = {
  // Auth
  signedIn: false,
  myName: '',
  myAvatar: '',
  myId,
  // SurrealDB JWT scoped to the `browser` record access. Set on successful
  // signin/register by auth.mjs (from the relay's /api/signin response),
  // cleared on sign-out. surreal.mjs and surreal-live.mjs prefer this
  // token when present and fall back to the viewer role otherwise.
  dbToken: localStorage.getItem('nobd_db_token') || null,
  // sessionId: a fresh per-page-load uuid. NOT persisted (sessionStorage
  // path was removed when we adopted the 30s grace presence model). Two
  // tabs of the same browser get distinct sessionIds, so the "you're
  // already playing in another tab" defense actually works.
  sessionId: 'sess-' + (crypto.randomUUID ? crypto.randomUUID().slice(0, 12) : Math.random().toString(36).slice(2, 14)),
  authMode: 'signin',    // 'signin' or 'register'
  playerProfile: null,
  isAnonymous: false,

  // Queue
  inQueue: false,
  queuePosition: -1,

  // Connection
  ws: null,             // RELAY WS (always-on, broadcast: TA frames + downstream JSON)
  controlWs: null,      // DIRECT FLYCAST WS (lazy, opens on queue promotion, closes on leave)
  connState: '',
  rendererStreaming: false,
  mySlot: -1,
  wsInQueue: false,       // legacy server-side queue state, retained for older callers
  leaving: false,

  // Reload-recovery: when the server still has us bound to a slot but the
  // browser tab is fresh, we DO NOT auto-adopt the slot. We surface a
  // "RESUME P1/P2" button instead and wait for the user to click it.
  // Reasoning: silently restoring a slot from a stale session can put a
  // player back in a match they didn't mean to rejoin (especially if they
  // reloaded for an unrelated reason on the leaderboard or chat tab).
  // -1 = no pending resume, 0 = P1 pending, 1 = P2 pending.
  pendingResumeSlot: -1,

  // Gamepad detection — reactive, updated by gamepad.mjs on connect/disconnect.
  // UX gate: can't join the game without one plugged in. Set by the initial
  // navigator.getGamepads() sweep and by the gamepadconnected/disconnected
  // browser events.
  gamepadConnected: false,
  gamepadId: '',          // trimmed gamepad.id (for device label on join)

  // WASM renderer
  glCtx: null,
  wasmModule: null,
  frameBuf: null,
  setOpt: null,           // function ref, set by renderer-bridge after init

  // Relay
  relay: null,

  // Diagnostics
  diag: {
    fps: 0, serverFrame: 0, bytesPerFrame: 0, totalBytes: 0,
    p1: {}, p2: {}, spectators: 0, queue: [], game: null,
    pingMs: 0, pingSamples: [],
    streamKbps: 0, publishUs: 0, dirtyPages: 0,
    gamepadActive: false, inputSendCount: 0,
    _matchEnded: false,
  },

  // Intervals
  gamepadInterval: null,
  testerInterval: null,

  // Chat / leaderboards / queue all live in SurrealDB now. The arrays
  // here used to be the local cache; everything reads directly from
  // live queries instead. Kept the queueData stub because a few legacy
  // code paths still poke it (queue.mjs renderQueue() — itself dead code
  // post-Phase 6, but the import graph would need a sweep before removal).
  queueData: [],
};
