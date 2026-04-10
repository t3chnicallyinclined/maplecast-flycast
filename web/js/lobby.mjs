// ============================================================================
// LOBBY.MJS — Live lobby state, game state HUD, name overlay
// ============================================================================

import { state } from './state.mjs';
import {
  prefetchMany, getCachedRecord, onRecordUpdate,
  formatRecord, formatStreak,
} from './player-cards.mjs';
import { liveSubscribe, liveQuery } from './surreal-live.mjs';

// Color tier thresholds for the per-player RTT badges in the HUD and the
// match-end overlay's WINNER PING stat. <20 / 20-49 / 50+ ms maps to
// green / yellow / red. 0 means "we haven't heard a ping_report from this
// player yet" — render as a dim em-dash.
//
// Exported because demo.mjs / showMatchResults() also paints #resultPing.
export function pingTier(rttMs) {
  if (!rttMs || rttMs <= 0) return 'tier-unknown';
  if (rttMs < 20) return 'tier-green';
  if (rttMs < 50) return 'tier-yellow';
  return 'tier-red';
}

export function formatPingMs(rttMs) {
  if (!rttMs || rttMs <= 0) return '— ms';
  return `${rttMs} ms`;
}

function paintHudPing(slotEl, rttMs) {
  if (!slotEl) return;
  slotEl.textContent = formatPingMs(rttMs);
  // Replace whatever tier-* class is currently set with the new one. Doing
  // it via classList instead of innerHTML keeps the element identity stable
  // so the cabinet doesn't repaint a flicker on every status tick.
  slotEl.classList.remove('tier-green', 'tier-yellow', 'tier-red', 'tier-unknown');
  slotEl.classList.add(pingTier(rttMs));
}

export function updateGameState(game) {
  const hud = document.getElementById('matchHud');
  const idle = document.getElementById('idleScreen');

  if (game && game.in_match) {
    idle.style.display = 'none';
    hud.classList.add('active');

    document.getElementById('hudTimer').textContent = game.timer || 99;

    const comboEl = document.getElementById('hudCombo');
    const maxCombo = Math.max(game.p1_combo || 0, game.p2_combo || 0);
    if (maxCombo > 1) {
      comboEl.textContent = maxCombo + ' HITS!';
      comboEl.classList.add('visible');
    } else {
      comboEl.classList.remove('visible');
    }
  } else {
    hud.classList.remove('active');
    if (!state.rendererStreaming) idle.style.display = 'flex';
  }
}

// Track active record-update subscriptions per slot so we can tear them down
// when the player in that slot changes (or empties). Subscriptions are
// stable across status ticks if the name doesn't change — we only re-bind
// on slot transitions to avoid churning the subscriber map at 1Hz.
let _p1Unsub = null;
let _p2Unsub = null;
let _kingUnsub = null;
let _p1BoundName = '';
let _p2BoundName = '';
let _kingBoundName = '';
// Cache the device label so the subscriber callback can keep showing it
// alongside the record without re-reading status.
let _p1Device = '';
let _p2Device = '';

function paintSlotRecord(slotEl, deviceLabel, record) {
  // "12W-3L • NOBD STICK"  /  "NOBD STICK" (until first fetch lands)
  const rec = formatRecord(record);
  if (rec) {
    slotEl.textContent = deviceLabel ? `${rec} • ${deviceLabel}` : rec;
  } else {
    slotEl.textContent = deviceLabel;
  }
}

// Paint a streak ("7W STREAK" / "3L STREAK" / "") into the cabinet HUD strip.
// Empty string for zero-streak so the :empty CSS rule collapses the row.
function paintHudStreak(streakEl, record) {
  if (!streakEl) return;
  streakEl.textContent = formatStreak(record);
}

function paintKingStreak(record) {
  const el = document.getElementById('kingStreak');
  const streak = formatStreak(record);
  el.textContent = streak || 'REIGNING KING';
}

export function updateLobbyState(status) {
  // SurrealDB live queries on the `slot` and `queue` tables (initSlotLive +
  // initQueueLive in queue.mjs) own the lobby slot/queue painting now.
  // updateLobbyState only handles the bits the status JSON still owns:
  //   - spectator count (until Phase 5 moves it to a session table)
  //   - HUD ping (RTT, comes from flycast directly via the relay broadcast)
  //   - in-game match state (HP/combo/timer — Phase 5 will move to live_match)
  //
  // Slot/queue painting goes through paintSlots() and paintQueueRows() in
  // queue.mjs, fired by SurrealDB live notifications.
  document.getElementById('watchCount').textContent = status.spectators || 0;

  // RTT pings still come from the JSON status broadcast (no DB equivalent yet)
  if (status.p1?.connected) {
    paintHudPing(document.getElementById('hudP1Ping'), status.p1.rtt_ms || 0);
  } else {
    paintHudPing(document.getElementById('hudP1Ping'), 0);
  }
  if (status.p2?.connected) {
    paintHudPing(document.getElementById('hudP2Ping'), status.p2.rtt_ms || 0);
  } else {
    paintHudPing(document.getElementById('hudP2Ping'), 0);
  }

  // Game state now flows via the live_match SurrealDB subscription
  // (see initLiveMatch below). status.game is ignored here.
}

// Real match-end overlay painter. Called from the ws 'match_end' handler
// with the rich payload the server now emits (winner_max_combo, duration_s,
// winner_rtt_ms, was_perfect, was_ocv, was_comeback, was_clutch, etc.).
// The demo path in demo.mjs still uses showMatchResults() with the hardcoded
// placeholder DOM — we leave that alone so `d` keypress demos still work.
//
// Auto-hides after 5s to match the prior behavior.
const _resultBadgeOrder = [
  ['was_perfect',  '\u2605 PERFECT \u2605'],
  ['was_ocv',      '\u2605 OCV \u2605'],
  ['was_comeback', '\u2605 COMEBACK \u2605'],
  ['was_clutch',   '\u2605 CLUTCH \u2605'],
];

let _resultsHideTimer = null;

export function paintMatchResults(payload) {
  if (!payload) return;
  const overlay = document.getElementById('matchResults');
  if (!overlay) return;

  const winnerName = (payload.winner_name || '').toUpperCase();
  document.getElementById('resultTitle').textContent =
    winnerName ? `${winnerName} WINS!` : 'MATCH OVER';

  // Stats — server gives us real numbers now. The display strings keep the
  // same shape as the placeholder mockup (e.g. "23 HITS", "42 SEC", "12 ms")
  // so the existing CSS sizing fits without tweaking.
  const combo = payload.winner_max_combo || 0;
  document.getElementById('resultCombo').textContent =
    combo > 0 ? `${combo} HITS` : '—';

  const dur = payload.duration_s || 0;
  document.getElementById('resultTime').textContent =
    dur > 0 ? `${Math.round(dur)} SEC` : '—';

  const winnerPing = payload.winner_rtt_ms || 0;
  const pingEl = document.getElementById('resultPing');
  pingEl.textContent = formatPingMs(winnerPing);
  pingEl.classList.remove('tier-green', 'tier-yellow', 'tier-red', 'tier-unknown');
  pingEl.classList.add(pingTier(winnerPing));

  // Badge: pick the highest-priority quality flag set on the payload, or
  // hide entirely for a vanilla win. Order matches arcade convention —
  // PERFECT outranks OCV outranks COMEBACK outranks CLUTCH.
  const badgeEl = document.getElementById('resultBadge');
  let badgeText = '';
  for (const [key, label] of _resultBadgeOrder) {
    if (payload[key]) { badgeText = label; break; }
  }
  if (badgeText) {
    badgeEl.textContent = badgeText;
    badgeEl.classList.add('active');
  } else {
    badgeEl.textContent = '';
    badgeEl.classList.remove('active');
  }

  overlay.classList.add('active');
  if (_resultsHideTimer) clearTimeout(_resultsHideTimer);
  _resultsHideTimer = setTimeout(() => overlay.classList.remove('active'), 5000);
}

// ============================================================================
// SurrealDB live_match subscription (Phase 5)
//
// Replaces the status.game → updateGameState path. The collector mirrors
// flycast's in-game RAM probe (HP, combos, timer, chars) into the
// live_match:current row every status tick. We subscribe to updates and
// paint the HUD the same way updateGameState always has.
// ============================================================================

let _liveMatchInitStarted = false;

export async function initLiveMatch() {
  if (_liveMatchInitStarted) return;
  _liveMatchInitStarted = true;

  try {
    // Initial snapshot
    const initial = await liveQuery('SELECT * FROM live_match:current');
    let rows = [];
    const first = initial?.[0];
    if (Array.isArray(first)) rows = first;
    else if (first?.result && Array.isArray(first.result)) rows = first.result;
    const current = rows[0];
    if (current) updateGameState(current);
    console.log('[live_match] initial state loaded');

    // Live subscription — the collector writes to live_match:current on every
    // flycast status tick (1Hz today). Deltas arrive here within ~100ms.
    await liveSubscribe('live_match', (action, row) => {
      if (!row) return;
      if (action === 'CREATE' || action === 'UPDATE') {
        updateGameState(row);
      }
      // DELETE is not a thing for the singleton, ignore
    });
    console.log('[live_match] live subscription ready');
  } catch (e) {
    console.warn('[live_match] init failed:', e.message);
  }
}

// Cache — skip innerHTML rebuild if names unchanged
let _lastP1 = '', _lastP2 = '';

export function updateNameOverlay() {
  const p1 = state.diag.p1 || {};
  const p2 = state.diag.p2 || {};
  const p1Name = p1.connected ? (p1.name || 'P1') : '';
  const p2Name = p2.connected ? (p2.name || 'P2') : '';

  if (p1Name === _lastP1 && p2Name === _lastP2) return; // no change, skip DOM
  _lastP1 = p1Name;
  _lastP2 = p2Name;

  const overlay = document.getElementById('nameOverlay');
  overlay.innerHTML =
    (p1Name ? `<div><div class="p-name p1">${p1Name}</div></div>` : '<div></div>') +
    (p2Name ? `<div><div class="p-name p2">${p2Name}</div></div>` : '<div></div>');
}
