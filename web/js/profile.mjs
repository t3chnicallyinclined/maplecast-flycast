// ============================================================================
// PROFILE.MJS — User profile modal, pad list, button tester
// ============================================================================

import { state } from './state.mjs';
import { openSignIn } from './auth.mjs';

export function openProfile() {
  if (!state.signedIn) { openSignIn(); return; }
  document.getElementById('profileTitle').textContent = state.myName;
  document.getElementById('profileModal').classList.add('active');
  loadProfileStats();
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
  document.getElementById('profile' + tab.charAt(0).toUpperCase() + tab.slice(1)).classList.add('active');
  if (tab === 'tester') startButtonTester();
  else stopButtonTester();
}

export function loadProfileStats() {
  const p = state.playerProfile;
  if (!p) return;
  document.getElementById('profRank').textContent = p.rank_tier || 'WARRIOR';
  document.getElementById('profElo').textContent = p.rating || 1000;
  document.getElementById('profRecord').textContent = `${p.wins || 0}W - ${p.losses || 0}L`;
  document.getElementById('profStreak').textContent = p.streak || 0;
  document.getElementById('profBestStreak').textContent = p.best_streak || 0;
  document.getElementById('profCombo').textContent = p.best_combo || 0;
  document.getElementById('profPerfects').textContent = p.perfects || 0;
  document.getElementById('profMatches').textContent = p.total_matches || 0;
  document.getElementById('profSince').textContent = p.created_at ? new Date(p.created_at).toLocaleDateString() : '-';
}

function updatePadList() {
  const list = document.getElementById('padList');
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

export function startButtonTester() {
  stopButtonTester();
  function loop() {
    updateButtonTester();
    state.testerInterval = requestAnimationFrame(loop);
  }
  state.testerInterval = requestAnimationFrame(loop);
}

export function stopButtonTester() {
  if (state.testerInterval) {
    cancelAnimationFrame(state.testerInterval);
    state.testerInterval = null;
  }
}

function updateButtonTester() {
  const gp = navigator.getGamepads()[0];
  const nameEl = document.getElementById('testerPadName');
  if (!gp) { nameEl.textContent = 'NO GAMEPAD DETECTED'; return; }
  nameEl.textContent = gp.id.substring(0, 40);

  document.getElementById('dpadU').classList.toggle('pressed', !!gp.buttons[12]?.pressed);
  document.getElementById('dpadD').classList.toggle('pressed', !!gp.buttons[13]?.pressed);
  document.getElementById('dpadL').classList.toggle('pressed', !!gp.buttons[14]?.pressed);
  document.getElementById('dpadR').classList.toggle('pressed', !!gp.buttons[15]?.pressed);
  document.getElementById('faceA').classList.toggle('pressed', !!gp.buttons[0]?.pressed);
  document.getElementById('faceB').classList.toggle('pressed', !!gp.buttons[1]?.pressed);
  document.getElementById('faceX').classList.toggle('pressed', !!gp.buttons[2]?.pressed);
  document.getElementById('faceY').classList.toggle('pressed', !!gp.buttons[3]?.pressed);
  document.getElementById('testerLT').style.width = ((gp.buttons[6]?.value || 0) * 100) + '%';
  document.getElementById('testerRT').style.width = ((gp.buttons[7]?.value || 0) * 100) + '%';
  document.getElementById('btnSelect').classList.toggle('pressed', !!gp.buttons[8]?.pressed);
  document.getElementById('btnStart').classList.toggle('pressed', !!gp.buttons[9]?.pressed);
  document.getElementById('btnLB').classList.toggle('pressed', !!gp.buttons[4]?.pressed);
  document.getElementById('btnRB').classList.toggle('pressed', !!gp.buttons[5]?.pressed);
}
