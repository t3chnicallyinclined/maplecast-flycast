// ============================================================================
// STATE.MJS — Shared mutable state container
//
// Single source of truth. Every module imports the same object reference.
// Mutations are direct property writes — zero overhead, zero events.
// ============================================================================

// Persistent client ID
let myId = localStorage.getItem('maplecast_id');
if (!myId) {
  myId = (crypto.randomUUID ? crypto.randomUUID()
    : ([1e7]+-1e3+-4e3+-8e3+-1e11).replace(/[018]/g, c =>
        (c ^ crypto.getRandomValues(new Uint8Array(1))[0] & 15 >> c / 4).toString(16)));
  localStorage.setItem('maplecast_id', myId);
}

export const state = {
  // Auth
  signedIn: false,
  myName: '',
  myAvatar: '',
  myId,
  // sessionId is the id we send to the server on `join`. For signed-in users
  // it equals myId (so reload-recovery via syncSlotFromStatus works). For
  // anonymous users gotNext() mints a fresh ephemeral one per click so an
  // abandoned anon slot is NOT silently reclaimed by a new tab. Initialised
  // to myId for back-compat with any non-gotNext join callers.
  sessionId: myId,
  authMode: 'signin',    // 'signin' or 'register'
  playerProfile: null,
  isAnonymous: false,

  // Queue
  inQueue: false,
  queuePosition: -1,

  // Connection
  ws: null,
  stickOnline: false,
  stickRegistered: false, // server confirmed this user owns a stick
  stickSlot: -1,          // slot the server says we already occupy (reload resync)
  connState: '',
  rendererStreaming: false,
  mySlot: -1,
  wsInQueue: false,       // server-side queue state
  leaving: false,

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
    _matchEnded: false, _wasRegistering: false,
  },

  // Stick registration
  stickCheckCounter: 0,
  registerTimeout: null,

  // Intervals
  gamepadInterval: null,
  testerInterval: null,

  // Chat — empty until real users connect
  chatHistory: [],

  // Placeholder leaderboard data (replaced by SurrealDB when connected)
  leaderboards: {
    streak: [
      { name: 'SHADOW_KING', stat: '12 WINS', king: true },
      { name: 'XECUTIONER', stat: '9 WINS' },
      { name: 'RUSH_DOWN', stat: '8 WINS' },
      { name: 'CABLE_GUY', stat: '7 WINS' },
      { name: 'MAGNETO_XX', stat: '6 WINS' },
      { name: 'STORM_CHSR', stat: '5 WINS' },
      { name: 'SENT_ARMY', stat: '5 WINS' },
      { name: 'IRON_FIST', stat: '4 WINS' },
      { name: 'PSYLOCKE_Q', stat: '3 WINS' },
      { name: 'DOOM_LOOP', stat: '3 WINS' },
    ],
    combo: [
      { name: 'RUSH_DOWN', stat: '87 HITS', king: true },
      { name: 'MAGNETO_XX', stat: '72 HITS' },
      { name: 'SHADOW_KING', stat: '63 HITS' },
      { name: 'STORM_CHSR', stat: '58 HITS' },
      { name: 'IRON_FIST', stat: '54 HITS' },
      { name: 'CABLE_GUY', stat: '47 HITS' },
      { name: 'DOOM_LOOP', stat: '41 HITS' },
      { name: 'XECUTIONER', stat: '38 HITS' },
      { name: 'SENT_ARMY', stat: '35 HITS' },
      { name: 'PSYLOCKE_Q', stat: '29 HITS' },
    ],
    speed: [
      { name: 'XECUTIONER', stat: '12 SEC', king: true },
      { name: 'SHADOW_KING', stat: '15 SEC' },
      { name: 'MAGNETO_XX', stat: '18 SEC' },
      { name: 'RUSH_DOWN', stat: '22 SEC' },
      { name: 'CABLE_GUY', stat: '24 SEC' },
      { name: 'STORM_CHSR', stat: '28 SEC' },
      { name: 'IRON_FIST', stat: '31 SEC' },
      { name: 'DOOM_LOOP', stat: '33 SEC' },
      { name: 'SENT_ARMY', stat: '37 SEC' },
      { name: 'PSYLOCKE_Q', stat: '42 SEC' },
    ],
    perfect: [
      { name: 'SHADOW_KING', stat: '8 ROUNDS', king: true },
      { name: 'XECUTIONER', stat: '6 ROUNDS' },
      { name: 'MAGNETO_XX', stat: '5 ROUNDS' },
      { name: 'RUSH_DOWN', stat: '4 ROUNDS' },
      { name: 'CABLE_GUY', stat: '3 ROUNDS' },
      { name: 'STORM_CHSR', stat: '3 ROUNDS' },
      { name: 'IRON_FIST', stat: '2 ROUNDS' },
      { name: 'DOOM_LOOP', stat: '2 ROUNDS' },
      { name: 'SENT_ARMY', stat: '1 ROUND' },
      { name: 'PSYLOCKE_Q', stat: '1 ROUND' },
    ],
    masher: [
      { name: 'CHAOS_AGENT', stat: '42 INP/S', king: true },
      { name: 'BUTTONZ', stat: '38 INP/S' },
      { name: 'RUSH_DOWN', stat: '35 INP/S' },
      { name: 'MAGNETO_XX', stat: '31 INP/S' },
      { name: 'SHADOW_KING', stat: '28 INP/S' },
      { name: 'IRON_FIST', stat: '25 INP/S' },
      { name: 'XECUTIONER', stat: '22 INP/S' },
      { name: 'STORM_CHSR', stat: '19 INP/S' },
      { name: 'CABLE_GUY', stat: '16 INP/S' },
      { name: 'DOOM_LOOP', stat: '14 INP/S' },
    ],
    surgeon: [
      { name: 'THE_SURGEON', stat: '94.2%', king: true },
      { name: 'CABLE_GUY', stat: '91.8%' },
      { name: 'SHADOW_KING', stat: '87.3%' },
      { name: 'XECUTIONER', stat: '85.1%' },
      { name: 'DOOM_LOOP', stat: '82.7%' },
      { name: 'STORM_CHSR', stat: '79.4%' },
      { name: 'MAGNETO_XX', stat: '76.2%' },
      { name: 'PSYLOCKE_Q', stat: '73.8%' },
      { name: 'IRON_FIST', stat: '71.5%' },
      { name: 'RUSH_DOWN', stat: '68.9%' },
    ],
  },

  // Queue — empty until real players join
  queueData: [],
};
