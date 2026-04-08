// ============================================================================
// LOBBY.MJS — Live lobby state, game state HUD, name overlay
// ============================================================================

import { state } from './state.mjs';
import { updateQueueFromServer } from './queue.mjs';

export function updateGameState(game) {
  const hud = document.getElementById('matchHud');
  const idle = document.getElementById('idleScreen');

  if (game && game.in_match) {
    idle.style.display = 'none';
    hud.classList.add('active');

    document.getElementById('hudTimer').textContent = game.timer || 99;

    if (game.p1_hp) {
      const pct = Math.max(0, Math.min(1, game.p1_hp[0] / 144));
      document.getElementById('hudP1Health').style.transform = `scaleX(${pct})`;
    }
    if (game.p2_hp) {
      const pct = Math.max(0, Math.min(1, game.p2_hp[0] / 144));
      document.getElementById('hudP2Health').style.transform = `scaleX(${pct})`;
    }

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

export function updateLobbyState(status) {
  document.getElementById('watchCount').textContent = status.spectators || 0;
  document.getElementById('playCount').textContent =
    (status.p1?.connected ? 1 : 0) + (status.p2?.connected ? 1 : 0);

  // NOW PLAYING — names + device type
  if (status.p1?.connected) {
    const name = (status.p1.name || 'P1').toUpperCase();
    const isHw = status.p1.type === 'hardware';
    document.getElementById('npP1').textContent = name;
    document.getElementById('hudP1Name').textContent = name;
    document.getElementById('npP1Record').textContent = isHw ? 'NOBD STICK' : 'BROWSER';
  }
  if (status.p2?.connected) {
    const name = (status.p2.name || 'P2').toUpperCase();
    const isHw = status.p2.type === 'hardware';
    document.getElementById('npP2').textContent = name;
    document.getElementById('hudP2Name').textContent = name;
    document.getElementById('npP2Record').textContent = isHw ? 'NOBD STICK' : 'BROWSER';
  }

  if (status.queue) {
    document.getElementById('queueCount').textContent = status.queue.length;
    updateQueueFromServer(status.queue);
  }

  if (status.game) updateGameState(status.game);

  if (status.p1?.connected) {
    document.getElementById('kingName').textContent = (status.p1.name || 'KING').toUpperCase();
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
