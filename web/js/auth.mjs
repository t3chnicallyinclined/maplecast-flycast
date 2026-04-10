// ============================================================================
// AUTH.MJS — Sign in/out, user badge, auth modal, SurrealDB auth
// ============================================================================

import { state } from './state.mjs';
import { systemMessage } from './chat.mjs';
import { stopGamepadPolling } from './gamepad.mjs';
import { surrealQuery, surrealRegister, surrealUpdateLastSeen, surrealFetchProfile, resetAuthCache } from './surreal.mjs';
import { upgradeAuth } from './surreal-live.mjs';
import { updateCabinetControls, leaveGame, leaveQueue } from './queue.mjs';
import { AVATARS } from './ui-common.mjs';

// === User Badge + Dropdown ===

export function showUserBadge() {
  document.getElementById('signinBtn').style.display = 'none';
  const badge = document.getElementById('userBadge');
  badge.classList.add('active');
  document.getElementById('userAvatar').innerHTML = state.myAvatar;
  document.getElementById('userName').textContent = state.myName;
  if (state.playerProfile) {
    document.getElementById('userRank').textContent = state.playerProfile.rank_tier || 'WARRIOR';
  }
}

export function toggleUserMenu(e) {
  e.stopPropagation();
  document.getElementById('userDropdown').classList.toggle('open');
}

export async function signOut() {
  document.getElementById('userDropdown').classList.remove('open');

  // Vacate any DB-side claims FIRST so we don't leave orphaned rows
  // (queue row stuck on "TRISDOG up next", or slot row holding our name).
  // Both calls are idempotent and safe even when not in either state.
  try {
    if (state.mySlot >= 0) await leaveGame();
    else if (state.inQueue) await leaveQueue();
  } catch (e) {
    console.warn('[auth] signOut DB cleanup failed:', e.message);
  }

  state.signedIn = false;
  state.myName = '';
  state.myAvatar = '';
  state.mySlot = -1;
  state.inQueue = false;
  state.wsInQueue = false;
  state.playerProfile = null;
  state.dbToken = null;
  localStorage.removeItem('nobd_username');
  localStorage.removeItem('nobd_db_token');
  resetAuthCache(); // next query falls back to viewer role

  document.getElementById('signinBtn').style.display = '';
  document.getElementById('userBadge').classList.remove('active');
  document.getElementById('leaveQueueBtn').style.display = 'none';
  document.getElementById('leaveGameBtn').style.display = 'none';
  stopGamepadPolling();
  updateCabinetControls();
  systemMessage('A fighter has left the arcade.');
}

// === Auth Modal ===

export function openSignIn() {
  state.authMode = 'signin';
  updateAuthUI();
  document.getElementById('authError').style.display = 'none';
  document.getElementById('signInModal').classList.add('active');
  document.getElementById('modalName').focus();
}

export function closeSignIn() {
  document.getElementById('signInModal').classList.remove('active');
}

export function toggleAuthMode() {
  state.authMode = state.authMode === 'signin' ? 'register' : 'signin';
  updateAuthUI();
}

function updateAuthUI() {
  const title = document.getElementById('authModalTitle');
  const btn = document.getElementById('authSubmitBtn');
  const toggle = document.getElementById('authToggleBtn');
  if (state.authMode === 'register') {
    title.innerHTML = '\u2728 NEW FIGHTER';
    btn.textContent = 'ENTER THE ARCADE';
    toggle.textContent = 'ALREADY HAVE AN ACCOUNT? SIGN IN';
    document.getElementById('modalPass').setAttribute('autocomplete', 'new-password');
  } else {
    title.innerHTML = '\u26A1 INSERT COIN';
    btn.textContent = 'FIGHT';
    toggle.textContent = 'NEW? CREATE FIGHTER';
    document.getElementById('modalPass').setAttribute('autocomplete', 'current-password');
  }
}

function showAuthError(msg) {
  const el = document.getElementById('authError');
  el.textContent = msg;
  el.style.display = 'block';
}

export async function confirmSignIn() {
  const name = document.getElementById('modalName').value.trim().toUpperCase().replace(/\s+/g, '_');
  const pass = document.getElementById('modalPass').value;
  if (!name || name.length < 4) { showAuthError('NAME MUST BE 4+ CHARACTERS'); return; }
  if (!pass || pass.length < 4) { showAuthError('PASSWORD MUST BE 4+ CHARACTERS'); return; }
  if (!state.myAvatar) state.myAvatar = '\u{1F94A}';

  document.getElementById('authError').style.display = 'none';
  document.getElementById('authSubmitBtn').disabled = true;
  document.getElementById('authSubmitBtn').textContent = 'LOADING...';

  try {
    // Both register and signin go through the relay's /api endpoints, which
    // use the admin SurrealDB credentials server-side. The browser never
    // touches DB writes directly (the viewer-only role would block them).
    const endpoint = state.authMode === 'register' ? '/api/register' : '/api/signin';
    const res = await fetch(endpoint, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: name.toLowerCase(), password: pass }),
    });
    const data = await res.json().catch(() => ({ ok: false, error: 'invalid response' }));
    if (!data.ok) {
      const errMap = {
        'name already taken': 'NAME ALREADY TAKEN',
        'wrong password': 'WRONG PASSWORD',
        'fighter not found': 'FIGHTER NOT FOUND — REGISTER FIRST',
        'database unavailable': 'DATABASE OFFLINE — TRY AGAIN',
      };
      showAuthError(errMap[data.error] || (data.error || 'AUTH FAILED').toUpperCase());
      return;
    }
    state.playerProfile = data.profile;
    state.myName = name;
    state.signedIn = true;
    localStorage.setItem('nobd_username', name.toLowerCase());

    // Stash the browser-scoped SurrealDB JWT so subsequent REST/WS calls
    // run under the player's $auth record access instead of the shared
    // viewer fallback. Legacy accounts without pass_hash don't get a token
    // back from the relay — fall through to viewer in that case.
    if (data.db_token) {
      state.dbToken = data.db_token;
      localStorage.setItem('nobd_db_token', data.db_token);
      resetAuthCache();       // drop stale viewer REST token
      upgradeAuth().catch(() => {}); // re-auth the existing live-query WS
    }

    if (state.authMode === 'register') {
      systemMessage(`${name} registered! WARRIOR rank.`);
    } else {
      systemMessage(`Welcome back, ${name}!`);
    }

    showUserBadge();
    updateCabinetControls();
    closeSignIn();
  } catch (e) {
    showAuthError('CONNECTION ERROR: ' + (e.message || 'UNKNOWN'));
    console.error('[auth]', e);
  } finally {
    const btn = document.getElementById('authSubmitBtn');
    btn.disabled = false;
    btn.textContent = state.authMode === 'register' ? 'REGISTER' : 'FIGHT';
  }
}

export async function autoSignIn(username) {
  state.myName = username.toUpperCase();
  state.myAvatar = '\u{1F3AE}';
  state.signedIn = true;

  showUserBadge();
  updateCabinetControls();
  systemMessage(`${state.myName} has entered the arcade!`);

  await surrealRegister(state.myName);
  surrealUpdateLastSeen(state.myName);
}

// === Avatar Picker ===

export function renderAvatars() {
  const grid = document.getElementById('avatarGrid');
  grid.innerHTML = AVATARS.map((a, i) =>
    `<div class="avatar-pick" data-idx="${i}" onclick="pickAvatar(this)">${a}</div>`
  ).join('');
}

export function pickAvatar(el) {
  document.querySelectorAll('.avatar-pick').forEach(a => a.classList.remove('selected'));
  el.classList.add('selected');
  state.myAvatar = el.innerHTML;
}
