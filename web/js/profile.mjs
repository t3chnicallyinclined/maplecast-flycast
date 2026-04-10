// ============================================================================
// PROFILE.MJS — Inline player card (replaces modal profile flow)
//
// Single-page-app model: clicking a player name anywhere (leaderboard, lobby
// P1/P2 slots, queue entries, chat handles, ticker) shows their stats in the
// player card under Hall of Fame. No navigation, no modal, no state loss.
//
// The card has two faces:
//   - FRONT: rank, ELO, W-L, streak, combo, perfects, matches
//   - BACK:  character usage + win rate  (click card to flip)
//
// Exports:
//   openProfile(name?)    — inline card (back-compat shim for existing
//                           window.openProfile bindings). If `name` omitted,
//                           shows the signed-in user's own card.
//   showPlayerCard(name)  — primary entry point for "show this player"
//   closePlayerCard()     — hides the card
//   initPlayerCard()      — called at page load to show the top player from
//                           a random category as the default card
//
// The old modal export names (closeProfile, switchProfileTab) remain as
// no-ops so king.html's `window.*` binding list doesn't have to change.
// ============================================================================

import { state } from './state.mjs';
import { surrealQuery } from './surreal.mjs';

let _currentName = null;
let _flipped = false;

// Shown when the card is open. Drives the close button + click-outside logic.
export function showPlayerCard(name) {
  if (!name) return;
  const upper = String(name).toUpperCase();
  _currentName = upper;
  _flipped = false;

  // Paint placeholders immediately so it feels snappy
  document.getElementById('pcName').textContent = upper;
  document.getElementById('pcNameBack').textContent = upper;
  document.getElementById('pcRank').textContent = 'LOADING…';
  document.getElementById('pcElo').textContent = '—';
  document.getElementById('pcRecord').textContent = '—';
  document.getElementById('pcStreak').textContent = '—';
  document.getElementById('pcBestStreak').textContent = '—';
  document.getElementById('pcCombo').textContent = '—';
  document.getElementById('pcPerfects').textContent = '—';
  document.getElementById('pcOcv').textContent = '—';
  document.getElementById('pcMatches').textContent = '—';

  const wrap = document.getElementById('playerCardWrap');
  const card = document.getElementById('playerCard');
  if (wrap) wrap.style.display = '';
  if (card) card.classList.remove('flipped');

  // Fetch the real data
  fetchPlayerStats(upper).catch(e => console.warn('[player-card] fetch failed:', e.message));
}

export function closePlayerCard() {
  const wrap = document.getElementById('playerCardWrap');
  if (wrap) wrap.style.display = 'none';
  _currentName = null;
}

async function fetchPlayerStats(name) {
  const lower = name.toLowerCase().replace(/'/g, "\\'");
  const res = await surrealQuery(`SELECT * FROM player WHERE username = '${lower}'`);
  const p = res?.[0]?.result?.[0];

  // If the card was closed or swapped to another name while we were fetching,
  // bail out without painting stale data.
  if (_currentName !== name) return;

  if (!p) {
    document.getElementById('pcRank').textContent = 'NEW FIGHTER';
    document.getElementById('pcElo').textContent = '1000';
    document.getElementById('pcRecord').textContent = '0-0';
    document.getElementById('pcStreak').textContent = '0';
    document.getElementById('pcBestStreak').textContent = '0';
    document.getElementById('pcCombo').textContent = '0';
    document.getElementById('pcPerfects').textContent = '0';
    document.getElementById('pcOcv').textContent = '0';
    document.getElementById('pcMatches').textContent = '0';
    return;
  }

  document.getElementById('pcRank').textContent     = p.rank_tier || 'WARRIOR';
  document.getElementById('pcElo').textContent      = p.rating ?? 1000;
  document.getElementById('pcRecord').textContent   = `${p.wins || 0}-${p.losses || 0}`;
  document.getElementById('pcStreak').textContent   = p.streak || 0;
  document.getElementById('pcBestStreak').textContent = p.best_streak || 0;
  document.getElementById('pcCombo').textContent    = p.best_combo || 0;
  document.getElementById('pcPerfects').textContent = p.perfects || 0;
  document.getElementById('pcOcv').textContent      = p.ocvs || 0;
  document.getElementById('pcMatches').textContent  = p.total_matches || 0;

  // Win rate (for the back)
  const total = (p.wins || 0) + (p.losses || 0);
  const wr = total > 0 ? Math.round((p.wins || 0) * 1000 / total) / 10 : 0;
  document.getElementById('pcWinrate').textContent = total > 0 ? `${wr}%` : '—';

  // Character usage (top 3 most-played with their W-L)
  fetchCharStats(lower).catch(() => {});
}

async function fetchCharStats(lowerName) {
  const res = await surrealQuery(
    `SELECT char_name, games, wins, losses FROM char_stats WHERE username = '${lowerName}' ORDER BY games DESC LIMIT 3`
  );
  const rows = res?.[0]?.result || [];
  const host = document.getElementById('pcChars');
  if (!host) return;
  if (!rows.length) {
    host.innerHTML = '<div class="pc-char-empty">no matches yet</div>';
    return;
  }
  host.innerHTML = rows.map(r => {
    const total = (r.wins || 0) + (r.losses || 0);
    const wr = total > 0 ? Math.round((r.wins || 0) * 100 / total) : 0;
    return `<div class="pc-char-row">
      <span class="pc-char-name">${r.char_name}</span>
      <span class="pc-char-games">${r.games}g · ${wr}%</span>
    </div>`;
  }).join('');
}

// Click anywhere on the card body flips it (but not on the close button —
// that's handled separately). Wire this up once at init.
let _flipHooked = false;
function hookFlip() {
  if (_flipHooked) return;
  _flipHooked = true;
  const card = document.getElementById('playerCard');
  if (!card) return;
  card.addEventListener('click', (e) => {
    // Ignore clicks on the close button (which lives in the header above)
    if (e.target.closest('.player-card-close')) return;
    _flipped = !_flipped;
    card.classList.toggle('flipped', _flipped);
  });
}

// On page load, pick a random leaderboard category and show its top player
// as the default card content. Gives the card a reason to exist before the
// user has clicked anything.
export async function initPlayerCard() {
  hookFlip();
  // SurrealDB 3.x requires fields used in ORDER BY to appear in the SELECT
  // list. Including the sort field (and filtering > 0 so we don't surface
  // brand-new accounts with all-zero stats as the "top" of any category)
  // gives the card real signal instead of a 400 + closed card.
  const categories = ['best_streak', 'best_combo', 'rating', 'ocvs', 'perfects', 'comebacks'];
  const pick = categories[Math.floor(Math.random() * categories.length)];
  try {
    const res = await surrealQuery(
      `SELECT username, ${pick} FROM player WHERE ${pick} > 0 ORDER BY ${pick} DESC LIMIT 1`
    );
    const top = res?.[0]?.result?.[0];
    if (top?.username) {
      showPlayerCard(top.username);
    } else {
      closePlayerCard();
    }
  } catch (e) {
    closePlayerCard();
  }
}

// ============================================================================
// MY PROFILE modal — opened from the user dropdown ("MY PROFILE" item).
//
// Distinct from the inline player card: the modal has tabs (STATS / MY PADS /
// TESTER) for managing your own gamepad setup. Other-player clicks (from
// leaderboard, lobby slots, queue entries) go through showPlayerCard above
// instead and never see the modal.
//
// The modal DOM lives in king.html (#profileModal). loadProfileStats /
// updatePadList / startButtonTester / stopButtonTester are the same helpers
// the old standalone modal used.
// ============================================================================

export function openProfile(name) {
  // With a name → inline card for that player (other-player flow)
  if (name) {
    showPlayerCard(name);
    return;
  }
  // No-arg → MY PROFILE modal (requires signed-in)
  if (!state.signedIn || !state.myName) {
    // Bounce to sign-in for unauthenticated users
    import('./auth.mjs').then(m => m.openSignIn());
    return;
  }
  document.getElementById('profileTitle').textContent = state.myName;
  document.getElementById('profileModal').classList.add('active');
  loadProfileStats(state.playerProfile);
  updatePadList();
  startButtonTester();
}

export function closeProfile() {
  document.getElementById('profileModal').classList.remove('active');
  stopButtonTester();
}

export function switchProfileTab(btn, tab) {
  document.querySelectorAll('.profile-tab').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  document.querySelectorAll('.profile-section').forEach(s => s.classList.remove('active'));
  const sec = document.getElementById('profile' + tab.charAt(0).toUpperCase() + tab.slice(1));
  if (sec) sec.classList.add('active');
  if (tab === 'tester') startButtonTester();
  else stopButtonTester();
}

// Modal stats panel — uses the MODAL DOM ids (profRank, profElo, …) which
// are distinct from the inline-card ids (pcRank, pcElo, …).
function loadProfileStats(profile) {
  const p = profile || {};
  const set = (id, v) => {
    const el = document.getElementById(id);
    if (el) el.textContent = v;
  };
  set('profRank', p.rank_tier || 'WARRIOR');
  set('profElo', p.rating ?? 1000);
  set('profRecord', `${p.wins || 0}W - ${p.losses || 0}L`);
  set('profStreak', p.streak || 0);
  set('profBestStreak', p.best_streak || 0);
  set('profCombo', p.best_combo || 0);
  set('profPerfects', p.perfects || 0);
  set('profMatches', p.total_matches || 0);
  set('profSince', p.created_at ? new Date(p.created_at).toLocaleDateString() : '-');
}

function updatePadList() {
  const list = document.getElementById('padList');
  if (!list) return;
  let html = '';
  const gamepads = navigator.getGamepads();
  for (let i = 0; i < gamepads.length; i++) {
    const gp = gamepads[i];
    if (!gp) continue;
    html += `<div class="pad-list-item">
      <div><div style="color:var(--neon-cyan);">${gp.id.substring(0, 30)}</div>
      <div class="pad-type">Gamepad ${i} / ${gp.buttons.length} btns / ${gp.axes.length} axes</div></div>
      <div class="pad-status online">CONNECTED</div>
    </div>`;
  }
  if (!html) {
    html = '<div style="text-align:center;padding:20px;color:var(--gray);font-size:7px;">NO GAMEPAD DETECTED<br><br>Plug in a USB gamepad and press any button</div>';
  }
  list.innerHTML = html;
}

// === Button Tester (requestAnimationFrame for sync with display) ===

function startButtonTester() {
  stopButtonTester();
  function loop() {
    updateButtonTester();
    state.testerInterval = requestAnimationFrame(loop);
  }
  state.testerInterval = requestAnimationFrame(loop);
}

function stopButtonTester() {
  if (state.testerInterval) {
    cancelAnimationFrame(state.testerInterval);
    state.testerInterval = null;
  }
}

function updateButtonTester() {
  // Walk all 4 slots — Chrome can report a connected pad at any index, not
  // just [0]. Pick the first non-null one.
  const pads = navigator.getGamepads();
  let gp = null;
  for (let i = 0; i < pads.length; i++) {
    if (pads[i]) { gp = pads[i]; break; }
  }
  const nameEl = document.getElementById('testerPadName');
  if (!nameEl) return;
  if (!gp) { nameEl.textContent = 'NO GAMEPAD DETECTED'; return; }
  nameEl.textContent = gp.id.substring(0, 40);

  const toggle = (id, pressed) => {
    const el = document.getElementById(id);
    if (el) el.classList.toggle('pressed', pressed);
  };
  toggle('dpadU',  !!gp.buttons[12]?.pressed);
  toggle('dpadD',  !!gp.buttons[13]?.pressed);
  toggle('dpadL',  !!gp.buttons[14]?.pressed);
  toggle('dpadR',  !!gp.buttons[15]?.pressed);
  toggle('faceA',  !!gp.buttons[0]?.pressed);
  toggle('faceB',  !!gp.buttons[1]?.pressed);
  toggle('faceX',  !!gp.buttons[2]?.pressed);
  toggle('faceY',  !!gp.buttons[3]?.pressed);
  const ltEl = document.getElementById('testerLT');
  const rtEl = document.getElementById('testerRT');
  if (ltEl) ltEl.style.width = ((gp.buttons[6]?.value || 0) * 100) + '%';
  if (rtEl) rtEl.style.width = ((gp.buttons[7]?.value || 0) * 100) + '%';
  toggle('btnSelect', !!gp.buttons[8]?.pressed);
  toggle('btnStart',  !!gp.buttons[9]?.pressed);
  toggle('btnLB',     !!gp.buttons[4]?.pressed);
  toggle('btnRB',     !!gp.buttons[5]?.pressed);
}
