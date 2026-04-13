// ============================================================================
// QUEUE.MJS — Queue management, gotNext, leave
//
// SurrealDB-first refactor (Phase 3+4): the queue lives in SurrealDB now.
// "I GOT NEXT" inserts a row into the `queue` table; every browser sees it
// via a LIVE SELECT and renders the line. Promotion is driven by the
// collector watching the `slot` table — when a slot empties, the collector
// flips the head queue row to status='promoted', and the promoted browser's
// own live-query callback opens its direct controlWs to home flycast.
//
// flycast does not see the queue at all anymore. The legacy ws.send paths
// for queue_join / queue_leave / join (over the relay WS) remain only as
// backstops during the cutover and will be deleted in Phase 6.
// ============================================================================

import { state } from './state.mjs';
import { systemMessage } from './chat.mjs';
import { stopGamepadPolling, startGamepadPolling } from './gamepad.mjs';
import { ANON_NAMES } from './ui-common.mjs';
import { getCachedRecord, formatRecord } from './player-cards.mjs';
import { liveSubscribe, liveQuery } from './surreal-live.mjs';

// renderQueue() and updateQueueFromServer() lived here in the pre-SurrealDB
// world. They're gone now — paintQueueRows (driven by the queue live query)
// is the single render path. king.html and lobby.mjs callers were updated.
// ============================================================================
// SurrealDB queue live subscription
//
// Local cache of waiting queue rows, sorted by joined_at ASC. Updated by
// the live query callback. paintQueueRows() reads this and rebuilds the
// queue UI on every change.
//
// Order matters (head of line goes to first opened slot), so we sort on
// every mutation rather than relying on insertion order.
// ============================================================================

const _queueRows = new Map();   // id → row
let _queueInitStarted = false;

function paintQueueRows() {
  const list = document.getElementById('queueList');
  if (!list) return;

  // Filter to waiting only, sort by joined_at ascending (oldest first)
  const waiting = Array.from(_queueRows.values())
    .filter(r => r.status === 'waiting')
    .sort((a, b) => String(a.joined_at).localeCompare(String(b.joined_at)));

  // Update state.inQueue + state.queuePosition based on whether one of these
  // rows is mine. This drives the gotNext button label.
  const myRow = waiting.find(r => r.session_id === state.sessionId);
  const wasInQueue = state.inQueue;
  if (myRow) {
    state.inQueue = true;
    state.queuePosition = waiting.indexOf(myRow) + 1;
  } else {
    state.inQueue = false;
    state.queuePosition = -1;
  }
  // The cabinet button depends on both (a) whether WE are in the queue
  // and (b) whether our signed-in username is queued in another tab.
  // Either of those can flip on any queue delta, so just re-evaluate on
  // every paint. It's an idempotent DOM update so the cost is noise.
  const leaveQueueBtn = document.getElementById('leaveQueueBtn');
  if (leaveQueueBtn) leaveQueueBtn.style.display = state.inQueue ? 'block' : 'none';
  updateCabinetControls();

  // Update queue counter in the lobby
  const counter = document.getElementById('queueCount');
  if (counter) counter.textContent = waiting.length;

  if (waiting.length === 0) {
    list.innerHTML = '<div class="queue-empty">NO ONE IN LINE<br>STEP UP</div>';
    return;
  }

  list.innerHTML = waiting.map((r, i) => {
    const isMe = r.session_id === state.sessionId;
    const rec = getCachedRecord(r.username);
    const recStr = rec ? formatRecord(rec) : (r.is_anon ? 'ANON' : 'NEW');
    const upper = String(r.username).toUpperCase().replace(/'/g, "\\'");
    return `<div class="queue-entry ${i === 0 ? 'next-up' : ''} ${isMe ? 'is-me' : ''}"
                 onclick="showPlayerCard('${upper}')"
                 style="cursor:pointer;">
      <div class="queue-pos">${i + 1}</div>
      <div class="queue-name">${String(r.username).toUpperCase()}</div>
      <div class="queue-record">${recStr}</div>
    </div>`;
  }).join('');
}

// ============================================================================
// SurrealDB slot live subscription
//
// Mirrors the 2-row `slot` table into a local cache, repaints lobby UI on
// every change. The slot table is the canonical source for "who's currently
// playing P1/P2". Replaces the per-second status JSON broadcast.
// ============================================================================

const _slotRows = new Map(); // 'slot:p1' / 'slot:p2' → row
let _slotInitStarted = false;

// Auto-resume on page reload was REMOVED on purpose. Refresh = lose spot,
// modulo the 30s grace window enforced by the collector. See state.mjs
// `savedSessionId = null` and the project memory entry on presence design.

// A slot is "in grace" when its last_input_at is between 2s and 30s in
// the past — flycast stopped reporting frames for this player but the
// collector hasn't yet swept the row. 2s headroom absorbs normal status
// jitter so a single missed broadcast doesn't flash the grace UI.
const GRACE_START_SEC = 2;
const GRACE_TOTAL_SEC = 30;
function graceSecondsRemaining(row) {
  if (!row || !row.last_input_at) return 0;
  const last = Date.parse(row.last_input_at);
  if (!isFinite(last)) return 0;
  const elapsed = (Date.now() - last) / 1000;
  if (elapsed < GRACE_START_SEC) return 0;       // still live
  return Math.max(0, Math.ceil(GRACE_TOTAL_SEC - elapsed));
}
function isInGrace(row) {
  return graceSecondsRemaining(row) > 0;
}

function paintSlots() {
  for (const slotNum of [0, 1]) {
    const row = Array.from(_slotRows.values()).find(r => r.slot_num === slotNum);
    const isOccupied = !!(row && row.occupant_name);
    const npName = document.getElementById(slotNum === 0 ? 'npP1' : 'npP2');
    const npRec  = document.getElementById(slotNum === 0 ? 'npP1Record' : 'npP2Record');
    const hudName = document.getElementById(slotNum === 0 ? 'hudP1Name' : 'hudP2Name');

    if (isOccupied) {
      const name = row.occupant_name.toUpperCase();
      if (npName) {
        npName.textContent = name;
        npName.onclick = () => window.showPlayerCard && window.showPlayerCard(name);
        npName.style.cursor = 'pointer';
      }
      if (hudName) hudName.textContent = name;

      // Grace state — computed from last_input_at, not stored. When
      // flycast stops broadcasting frames for a player, their last_input_at
      // stops advancing, and the UI transparently flips to the countdown.
      const graceSecs = graceSecondsRemaining(row);
      if (graceSecs > 0) {
        if (npRec) {
          npRec.textContent = `WAITING ${graceSecs}s`;
          npRec.classList.add('grace');
        }
      } else {
        if (npRec) {
          const rec = getCachedRecord(name);
          npRec.textContent = rec ? formatRecord(rec) : (row.device || 'BROWSER');
          npRec.classList.remove('grace');
        }
      }
    } else {
      if (npName) {
        npName.textContent = 'OPEN';
        npName.onclick = null;
        npName.style.cursor = '';
      }
      if (hudName) hudName.textContent = '';
      if (npRec) {
        npRec.textContent = 'PRESS START';
        npRec.classList.remove('grace');
      }
    }
  }

  // Update the play count in the header
  const playCount = document.getElementById('playCount');
  if (playCount) {
    const filled = Array.from(_slotRows.values()).filter(r => r.occupant_name).length;
    playCount.textContent = filled;
  }

  // If MY slot just got freed by the collector grace sweep (e.g. tab was
  // backgrounded too long), surface that locally so the step-up button
  // comes back instead of the leave button hanging around.
  //
  // Match by username when signed in: session_id rotates per page load
  // and during reclaim the slot row still holds the previous tab's
  // session_id, but it's still "my" slot — the DB UNIQUE on occupant_name
  // guarantees at most one slot per user, so name match is authoritative.
  if (state.mySlot >= 0) {
    const myLower = (state.myName || '').toLowerCase();
    const stillMine = Array.from(_slotRows.values()).find(r =>
      r.slot_num === state.mySlot && (
        (myLower && r.occupant_name && r.occupant_name.toLowerCase() === myLower) ||
        r.session_id === state.sessionId
      )
    );
    if (!stillMine) {
      console.log('[slot] my slot was freed externally');
      state.mySlot = -1;
      stopGamepadPolling();
      try { state.controlWs && state.controlWs.close(); } catch {}
      state.controlWs = null;
    }
  }

  updateCabinetControls();
}

// Once-per-second ticker so the WAITING countdown decrements visibly even
// without a new live-query notification arriving. Cheap: a single repaint
// of two DOM nodes when grace is active, no-op when neither slot is in
// grace. Started lazily by initSlotLive.
let _graceTicker = null;
function ensureGraceTicker() {
  if (_graceTicker) return;
  _graceTicker = setInterval(() => {
    // paintSlots already calls updateCabinetControls internally, which
    // will swap RECLAIM → DISCONNECT → OPEN as grace ticks down.
    paintSlots();
  }, 1000);
}

export async function initSlotLive() {
  if (_slotInitStarted) return;
  _slotInitStarted = true;
  ensureGraceTicker();

  try {
    const initial = await liveQuery('SELECT * FROM slot');
    let rows = [];
    const first = initial?.[0];
    if (Array.isArray(first)) rows = first;
    else if (first?.result && Array.isArray(first.result)) rows = first.result;
    for (const r of rows) _slotRows.set(String(r.id), r);
    paintSlots();
    console.log(`[slot] loaded ${rows.length} slot rows`);

    await liveSubscribe('slot', (action, row) => {
      if (!row || !row.id) return;
      const key = String(row.id);
      if (action === 'CREATE' || action === 'UPDATE') {
        _slotRows.set(key, row);
      } else if (action === 'DELETE') {
        _slotRows.delete(key);
      }
      paintSlots();
    });
    console.log('[slot] live subscription ready');
  } catch (e) {
    console.warn('[slot] initSlotLive failed:', e.message);
  }
}

export async function initQueueLive() {
  if (_queueInitStarted) return;
  _queueInitStarted = true;

  try {
    // Initial fetch — current waiting list
    const initial = await liveQuery(
      'SELECT * FROM queue WHERE status = "waiting" ORDER BY joined_at ASC'
    );
    let rows = [];
    const first = initial?.[0];
    if (Array.isArray(first)) rows = first;
    else if (first?.result && Array.isArray(first.result)) rows = first.result;
    for (const r of rows) _queueRows.set(String(r.id), r);
    paintQueueRows();
    console.log(`[queue] loaded ${rows.length} waiting rows`);

    // Track ids we've already acted on as "promoted" so a UPDATE notification
    // on the same row doesn't re-fire handleMyPromotion (which would open a
    // second controlWs).
    const _handledPromotions = new Set();

    // Live subscription — push deltas
    await liveSubscribe('queue', (action, row) => {
      if (!row || !row.id) return;
      const key = String(row.id);
      if (action === 'CREATE' || action === 'UPDATE') {
        _queueRows.set(key, row);
        // If this is MY row and it just got promoted, fire the controlWs
        // open path — but only once per row id.
        if (row.session_id === state.sessionId && row.status === 'promoted' && !_handledPromotions.has(key)) {
          _handledPromotions.add(key);
          handleMyPromotion(row);
        }
      } else if (action === 'DELETE') {
        _queueRows.delete(key);
        _handledPromotions.delete(key);
      }
      paintQueueRows();
    });
    console.log('[queue] live subscription ready');
  } catch (e) {
    console.warn('[queue] initQueueLive failed:', e.message);
  }
}

// Called when MY queue row's status flips to 'promoted'. The collector did
// this on slot-open. Open the direct flycast WS and claim the slot.
async function handleMyPromotion(row) {
  console.log('[queue] promoted to slot', row.promoted_to, '— opening controlWs');
  systemMessage(`${state.myName} — IT'S YOUR TURN!`);

  // Claim the slot in local state IMMEDIATELY, before awaiting the WS open.
  // The slot live-query notification can arrive before this function
  // finishes — without an immediate mySlot assignment we'd race a second
  // promotion notification (e.g. P2 also empty) and open two controlWs.
  state.mySlot = row.promoted_to;

  try {
    const { ensureControlWs } = await import('./ws-connection.mjs');
    const { getPreferredLatchPolicy } = await import('./diagnostics.mjs');
    const ws = await ensureControlWs();
    const gp = navigator.getGamepads()[0];
    const device = gp ? gp.id.substring(0, 30) : (state.gamepadId || 'Browser');
    ws.send(JSON.stringify({
      type: 'join',
      id: state.sessionId,
      name: state.myName,
      device,
      slot: row.promoted_to,
      // Per-user latch policy: send the user's stored preference so the
      // slot inherits their choice (instead of whatever the previous
      // player left it at). Server applies this in its `join` handler.
      latch_policy: getPreferredLatchPolicy(),
    }));
    // Optimistic local state — flycast's `assigned` reply will confirm
    state.mySlot = row.promoted_to;
    state.wsInQueue = false;
    state.inQueue = false;
    startGamepadPolling();
    updateCabinetControls();  // flips "I GOT NEXT"/"PLUG IN" → "LEAVE GAME"
    document.getElementById('leaveGameBtn').style.display = 'block';
    document.getElementById('gotNextBtn').style.display = 'none';
    document.getElementById('leaveQueueBtn').style.display = 'none';
  } catch (e) {
    console.warn('[queue] failed to open controlWs after promotion:', e.message);
    systemMessage('Could not reach game server — try again.');
  }
}

export async function gotNext() {
  // Refuse to step up if we're already a player or already in line.
  if (state.mySlot >= 0) return;
  if (state.inQueue) return;

  // Hard gate: no gamepad, no game.
  if (!state.gamepadConnected) return;

  // SIGN-IN REQUIRED. Anonymous queueing is disabled — it was the source of
  // ghost queue entries (random NOMAD/WANDERER names auto-promoting from
  // stale rows, no accountability, no stats). Non-signed-in users see a
  // prompt to sign in when they click the button.
  if (!state.signedIn) {
    systemMessage('Sign in to step up to the cabinet.');
    try {
      const { openSignIn } = await import('./auth.mjs');
      openSignIn();
    } catch {}
    return;
  }
  state.isAnonymous = false;

  // DEFENSE IN DEPTH — one user, one slot, one queue row. The DB has UNIQUE
  // indexes on slot.occupant_name and queue.(username,status); these checks
  // just give the user a clean message instead of a constraint violation.
  const lower = state.myName.toLowerCase();
  const mySlotRow = Array.from(_slotRows.values())
    .find(r => r.occupant_name && r.occupant_name.toLowerCase() === lower);
  if (mySlotRow) {
    // Name is already in a slot (stale from previous tab/refresh). Instead of
    // blocking with a message, auto-reclaim — this is what the user wants.
    systemMessage(`${state.myName} reclaiming P${mySlotRow.slot_num + 1}...`);
    reclaimSlot(mySlotRow);
    return;
  }
  const alreadyQueued = Array.from(_queueRows.values())
    .some(r => r.status === 'waiting' && String(r.username).toLowerCase() === lower);
  if (alreadyQueued) {
    systemMessage(`${state.myName}, you're already in line.`);
    return;
  }

  // sessionId is stable per page-load (set in state.mjs); don't reroll it
  // here. Re-rolling would orphan a queue row written under the previous
  // id if the user clicked gotNext, then leaveQueue, then gotNext again.

  // Optimistic UI: flip the button immediately. Live query will reconcile
  // the queue list once the row lands.
  state.inQueue = true;
  const btn = document.getElementById('gotNextBtn');
  if (btn) {
    btn.textContent = "YOU'RE IN LINE...";
    btn.classList.add('in-queue');
  }
  document.getElementById('leaveQueueBtn').style.display = 'block';

  systemMessage(`${state.myName} says I GOT NEXT!`);

  // Insert into the queue table. The collector watches `slot` for
  // openings and will UPDATE my row to status='promoted' when my turn
  // arrives, which my own live-query callback (handleMyPromotion) catches
  // and uses to open the direct controlWs to flycast.
  const gp = navigator.getGamepads()[0];
  const device = gp ? gp.id.substring(0, 30) : (state.gamepadId || 'Browser');
  try {
    await liveQuery(
      'CREATE queue SET username = $username, device = $device, session_id = $session_id, is_anon = $is_anon, status = "waiting"',
      {
        username: state.myName,
        device,
        session_id: state.sessionId,
        is_anon: !state.signedIn,
      }
    );
  } catch (e) {
    console.warn('[queue] gotNext write failed:', e.message);
    // Roll back the optimistic UI
    state.inQueue = false;
    document.getElementById('leaveQueueBtn').style.display = 'none';
    updateCabinetControls();
    // Surface the actual reason — UNIQUE violation, perms, etc.
    const friendly = /unique|index|already/i.test(e.message)
      ? 'You already have a queue entry — wait it out or sign out.'
      : 'Could not join queue — try again.';
    systemMessage(friendly);
  }
}

export async function leaveQueue() {
  state.inQueue = false;
  state.queuePosition = -1;
  document.getElementById('leaveQueueBtn').style.display = 'none';
  updateCabinetControls();

  // Delete my queue row(s). We match BOTH on session_id (this tab's
  // optimistic write that may not have propagated through the live query
  // yet) AND on username (covers signOut after a refresh, where the row's
  // session_id has rotated but the username still identifies us). Username
  // wins because of the UNIQUE (username, status) index — at most one
  // 'waiting' row per name can exist at a time.
  try {
    if (state.signedIn && state.myName) {
      await liveQuery(
        'DELETE queue WHERE username = $username AND status = "waiting"',
        { username: state.myName }
      );
    } else {
      await liveQuery(
        'DELETE queue WHERE session_id = $session_id AND status = "waiting"',
        { session_id: state.sessionId }
      );
    }
  } catch (e) {
    console.warn('[queue] leaveQueue delete failed:', e.message);
  }
}

export async function leaveGame() {
  state.leaving = true;

  // Three things to clean up depending on what state we're in:
  //  1. Holding a slot → tell flycast (controlWs leave + close) AND hit
  //     /api/leave so the relay clears the slot row via admin SurrealDB
  //     creds. The browser cannot UPDATE slot directly (collector-only
  //     perms), but the relay can. Instant free — no 30s wait.
  //  2. In the queue → delete our queue row by username (signed in) or
  //     session_id (anon).
  //  3. Just spectating → no-op on the server, just reset local UI.
  if (state.mySlot >= 0) {
    // Order matters here. We want to clear the SurrealDB slot row BEFORE
    // the controlWs close races with flycast's next status broadcast:
    //   1. Hit /api/leave — relay wipes the slot row using admin creds.
    //      All browsers see the row empty within ms (live query push).
    //   2. Send `leave` over controlWs and close it. Flycast's next
    //      status says connected=false, collector mirror_slots is a
    //      no-op because the slot is already empty (WHERE !!occupant_name
    //      guard skips empty rows). No grace timer ever fires.
    if (state.signedIn && state.dbToken) {
      try {
        await fetch('/api/leave', {
          method: 'POST',
          headers: { 'Authorization': 'Bearer ' + state.dbToken },
        });
      } catch (e) {
        console.warn('[queue] /api/leave failed:', e.message);
      }
    }
    if (state.controlWs && state.controlWs.readyState === WebSocket.OPEN) {
      try { state.controlWs.send(JSON.stringify({ type: 'leave', id: state.sessionId })); } catch {}
      try { state.controlWs.close(); } catch {}
      state.controlWs = null;
    }
  } else if (state.inQueue) {
    try {
      if (state.signedIn && state.myName) {
        await liveQuery(
          'DELETE queue WHERE username = $username AND status = "waiting"',
          { username: state.myName }
        );
      } else {
        await liveQuery(
          'DELETE queue WHERE session_id = $session_id AND status = "waiting"',
          { session_id: state.sessionId }
        );
      }
    } catch (e) {
      console.warn('[queue] leaveGame queue clear failed:', e.message);
    }
  }

  state.mySlot = -1;
  state.wsInQueue = false;
  state.inQueue = false;
  state.leaving = false;
  stopGamepadPolling();

  document.getElementById('leaveGameBtn').style.display = 'none';
  document.getElementById('leaveQueueBtn').style.display = 'none';
  updateCabinetControls();
}

// Reclaim: resume a slot we already hold in the DB, after a refresh /
// browse-away / transient network blip. flycast's ghost-slot eviction on
// same-name join will disconnect whatever stale hdl is still registered
// and reassign us. Uses the same code path as handleMyPromotion.
async function reclaimSlot(slotRow) {
  if (!state.gamepadConnected) {
    systemMessage('Plug in a gamepad before reclaiming.');
    return;
  }
  const slot = slotRow.slot_num;
  console.log('[queue] reclaiming slot', slot);
  systemMessage(`${state.myName} reclaiming P${slot + 1}...`);

  state.mySlot = slot;
  state.leaving = false; // defensive
  try {
    const { ensureControlWs } = await import('./ws-connection.mjs');
    const { getPreferredLatchPolicy } = await import('./diagnostics.mjs');
    const ws = await ensureControlWs();
    const gp = navigator.getGamepads()[0];
    const device = gp ? gp.id.substring(0, 30) : (state.gamepadId || 'Browser');
    ws.send(JSON.stringify({
      type: 'join',
      id: state.sessionId,
      name: state.myName,
      device,
      slot,
      latch_policy: getPreferredLatchPolicy(),
    }));
    state.inQueue = false;
    state.wsInQueue = false;
    // Small delay to let server process join before sending input
    // (QUIC input arrives faster than TCP join — avoids race)
    setTimeout(() => startGamepadPolling(), 200);
    updateCabinetControls();
  } catch (e) {
    console.warn('[queue] reclaim failed:', e.message);
    state.mySlot = -1;
    systemMessage('Could not reach game server — try again.');
    updateCabinetControls();
  }
}

// Force-disconnect: frees a slot held by our signed-in name (this tab or
// another). The relay's /api/leave endpoint clears the slot row using
// admin SurrealDB creds — browsers can't UPDATE slot directly because of
// the collector-only permissions on the table.
async function forceDisconnect() {
  if (!state.dbToken) {
    systemMessage('Sign in required to disconnect.');
    return;
  }
  systemMessage(`Disconnecting ${state.myName}...`);
  try {
    const res = await fetch('/api/leave', {
      method: 'POST',
      headers: { 'Authorization': 'Bearer ' + state.dbToken },
    });
    if (!res.ok) throw new Error('http ' + res.status);
    // Local cleanup in case we're the tab holding the slot.
    if (state.controlWs && state.controlWs.readyState === WebSocket.OPEN) {
      try { state.controlWs.send(JSON.stringify({ type: 'leave', id: state.sessionId })); } catch {}
      try { state.controlWs.close(); } catch {}
      state.controlWs = null;
    }
    state.mySlot = -1;
    stopGamepadPolling();
    updateCabinetControls();
  } catch (e) {
    console.warn('[queue] forceDisconnect failed:', e.message);
    systemMessage('Could not disconnect — try again.');
  }
}

// Force-leave queue: delete our waiting queue row(s) by username, even
// if they were written under a different session_id (another tab or a
// rotated session). Fires when the user clicks LEAVE LINE while the UI
// shows "ALREADY IN LINE" because another tab's row is blocking us.
async function forceLeaveQueue() {
  try {
    await liveQuery(
      'DELETE queue WHERE username = $username AND status = "waiting"',
      { username: state.myName }
    );
    systemMessage(`${state.myName} left the line.`);
  } catch (e) {
    console.warn('[queue] forceLeaveQueue failed:', e.message);
    systemMessage('Could not leave queue — try again.');
  }
}

// Single source of truth for the I GOT NEXT button. Called whenever
// auth/slot/queue/gamepad state changes.
//
//   - already in a slot          → hidden, LEAVE GAME visible (LOCKED IN)
//   - already in line (waiting)  → hidden, LEAVE QUEUE visible
//   - signed-in name occupies a  → hidden (defense in depth — DB UNIQUE
//     slot under another tab        on occupant_name will reject too)
//   - no gamepad detected        → "PLUG IN A GAMEPAD" (greyed)
//   - gamepad detected           → "I GOT NEXT" (enabled)
//
// USB-only. No reload-recovery / resume button — refresh = lose your spot
// modulo the 30s collector grace window.
export function updateCabinetControls() {
  const btn = document.getElementById('gotNextBtn');
  if (!btn) return;
  const leaveBtn = document.getElementById('leaveGameBtn');

  // Already a player → hide step-up.
  if (state.mySlot >= 0) {
    btn.style.display = 'none';
    if (leaveBtn) leaveBtn.style.display = 'block';
    return;
  }
  if (leaveBtn) leaveBtn.style.display = 'none';

  btn.style.display = '';
  btn.onclick = gotNext;
  // Default: not in reclaim mode. The "name in slot" branch below adds the
  // class back when applicable. Always reset here so a stale class doesn't
  // leak into a different state on the next paint.
  btn.classList.remove('reclaim');

  // Already in line via the queue table — keep the in-queue label so the
  // user can see they're queued up. The leaveQueue button is shown by
  // gotNext() / paintQueueRows(); we just don't relabel the main button.
  if (state.inQueue) {
    btn.innerHTML = "YOU'RE IN LINE...";
    btn.classList.add('in-queue');
    btn.disabled = false;
    return;
  }
  btn.classList.remove('in-queue');

  // Defense in depth: signed-in user whose name is in a slot or queue
  // under a different tab (or a stale row we can't touch). The DB UNIQUE
  // indexes on slot.occupant_name and queue.(username,status) will reject
  // a duplicate, so we repurpose the main button as a force-disconnect
  // that frees the DB-side claim. Same pattern as the in-tab LEAVE GAME
  // button, just driven from a different code path.
  if (state.signedIn) {
    const lower = state.myName.toLowerCase();
    const mySlotRow = Array.from(_slotRows.values())
      .find(r => r.occupant_name && r.occupant_name.toLowerCase() === lower);
    if (mySlotRow) {
      // We get here only when state.mySlot < 0 (the early return at the
      // top of this function handles the "actively playing in this tab"
      // case). So if the slot is in our name, this tab does NOT have a
      // controlWs to flycast — we're "stranded": the DB row says we're
      // in P1 but no inputs are flowing. RECLAIM is always the right
      // action regardless of grace state, because:
      //
      //   - Within grace: classic "I dropped briefly, get me back in"
      //   - Past grace:   the collector should have swept us, but the
      //                   slot row is still here (collector lag, edge
      //                   case, manual DB write). Reclaim does the
      //                   right thing — flycast ghost-evicts the stale
      //                   hdl and re-binds us cleanly.
      //
      // The user-facing label changes based on whether the slot is
      // still warm so the language matches what's happening.
      const inGrace = isInGrace(mySlotRow);
      btn.innerHTML = '&#x21A9;&#xFE0F; ' + (inGrace ? 'RECLAIM P' : 'REJOIN P') + (mySlotRow.slot_num + 1);
      btn.title = inGrace
        ? 'You lost your connection. Click to get back in.'
        : 'Your slot is still being held for you. Click to take it back.';
      btn.disabled = !state.gamepadConnected;
      btn.classList.add('reclaim');
      btn.onclick = () => reclaimSlot(mySlotRow);
      return;
    }
    const inQueueElsewhere = Array.from(_queueRows.values())
      .some(r => r.status === 'waiting'
             && String(r.username).toLowerCase() === lower
             && r.session_id !== state.sessionId);
    if (inQueueElsewhere) {
      btn.innerHTML = '&#x274C; LEAVE LINE';
      btn.title = `${state.myName} is in the queue (maybe another tab). Click to remove your spot.`;
      btn.disabled = false;
      btn.onclick = forceLeaveQueue;
      return;
    }
  }

  if (!state.gamepadConnected) {
    btn.innerHTML = '&#x1F50C; PLUG IN A GAMEPAD';
    btn.title = 'Connect a USB gamepad to play. Press any button if it is plugged in but not detected.';
    btn.disabled = true;
    return;
  }

  btn.disabled = false;
  btn.innerHTML = '&#x1FA99; I GOT NEXT';
  btn.title = state.signedIn
    ? 'Step up to play'
    : 'Step up to play (sign in to track stats)';
}
