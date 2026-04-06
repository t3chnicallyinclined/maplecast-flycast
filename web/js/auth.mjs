// ============================================================================
// AUTH.MJS — Sign in/out, user badge, auth modal, SurrealDB auth
// ============================================================================

import { state } from './state.mjs';
import { renderChat } from './chat.mjs';
import { stopGamepadPolling, updateStickButtons } from './gamepad.mjs';
import { surrealQuery, surrealRegister, surrealUpdateLastSeen, surrealFetchProfile } from './surreal.mjs';
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

export function signOut() {
  document.getElementById('userDropdown').classList.remove('open');
  state.signedIn = false;
  state.myName = '';
  state.myAvatar = '';
  state.mySlot = -1;
  state.inQueue = false;
  state.wsInQueue = false;
  state.playerProfile = null;
  state.stickOnline = false;
  localStorage.removeItem('nobd_username');
  localStorage.removeItem('maplecast_stick');

  document.getElementById('signinBtn').style.display = '';
  document.getElementById('userBadge').classList.remove('active');
  document.getElementById('gotNextBtn').innerHTML = '&#x1FA99; I GOT NEXT';
  document.getElementById('gotNextBtn').classList.remove('in-queue');
  document.getElementById('gotNextBtn').disabled = false;
  document.getElementById('gotNextBtn').style.display = '';
  document.getElementById('leaveQueueBtn').style.display = 'none';
  document.getElementById('leaveGameBtn').style.display = 'none';
  document.getElementById('registerStickBtn').style.display = 'none';
  document.getElementById('unregisterStickBtn').style.display = 'none';
  stopGamepadPolling();
  state.chatHistory.push({ name: null, system: 'A fighter has left the arcade.' });
  renderChat();
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
    title.textContent = 'CREATE ACCOUNT';
    btn.textContent = 'REGISTER';
    toggle.textContent = 'ALREADY HAVE AN ACCOUNT? SIGN IN';
  } else {
    title.textContent = 'SIGN IN';
    btn.textContent = 'FIGHT';
    toggle.textContent = 'NEW? CREATE ACCOUNT';
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
    if (state.authMode === 'register') {
      // Check if username taken
      const check = await surrealQuery(`SELECT username FROM player WHERE username = '${name.toLowerCase()}'`);
      if (check === null) { showAuthError('DATABASE OFFLINE — TRY AGAIN'); return; }
      if (check?.[0]?.result?.length > 0) { showAuthError('NAME ALREADY TAKEN'); return; }

      // Create player — try with password hash, fall back to without
      let create = await surrealQuery(
        `CREATE player SET username = '${name.toLowerCase()}', pass_hash = crypto::argon2::generate('${pass.replace(/'/g, "\\'")}'), created_at = time::now(), last_seen = time::now()`
      );
      // If argon2 or schema fails, try simpler create
      if (!create?.[0]?.result?.length) {
        console.warn('[auth] Argon2 create failed, trying simple create');
        create = await surrealQuery(
          `CREATE player SET username = '${name.toLowerCase()}', created_at = time::now(), last_seen = time::now()`
        );
      }
      if (!create?.[0]?.result?.length) {
        showAuthError('REGISTRATION FAILED — CHECK DB');
        console.error('[auth] Create result:', JSON.stringify(create));
        return;
      }
      state.playerProfile = create[0].result[0];
      state.chatHistory.push({ name: null, system: `${name} registered! WARRIOR rank.` });
    } else {
      // Sign in
      const check = await surrealQuery(`SELECT * FROM player WHERE username = '${name.toLowerCase()}'`);
      if (check === null) { showAuthError('DATABASE OFFLINE — TRY AGAIN'); return; }
      if (!check?.[0]?.result?.length) { showAuthError('FIGHTER NOT FOUND — REGISTER FIRST'); return; }
      const player = check[0].result[0];
      if (player.pass_hash) {
        const verify = await surrealQuery(
          `RETURN crypto::argon2::compare('${player.pass_hash}', '${pass.replace(/'/g, "\\'")}')`
        );
        if (!verify?.[0] || verify[0].result !== true) { showAuthError('WRONG PASSWORD'); return; }
      }
      // No pass_hash = legacy account, let them in
      state.playerProfile = player;
      surrealUpdateLastSeen(name);
      state.chatHistory.push({ name: null, system: `Welcome back, ${name}!` });
    }

    state.myName = name;
    state.signedIn = true;
    localStorage.setItem('nobd_username', name.toLowerCase());

    showUserBadge();
    updateStickButtons();
    closeSignIn();
    renderChat();

    if (state.ws?.readyState === 1) {
      state.ws.send(JSON.stringify({ type: 'check_stick', username: name.toLowerCase() }));
    }
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
  updateStickButtons();
  state.chatHistory.push({ name: null, system: `${state.myName} has entered the arcade!` });
  renderChat();

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
