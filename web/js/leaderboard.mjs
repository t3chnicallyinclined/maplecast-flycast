// ============================================================================
// LEADERBOARD.MJS — Hall of Fame leaderboard tabs
// ============================================================================

import { state } from './state.mjs';

export function switchTab(btn, tab) {
  document.querySelectorAll('.lb-tab').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  renderLeaderboard(tab);
}

export function renderLeaderboard(tab) {
  const list = document.getElementById('leaderboardList');
  const data = state.leaderboards[tab] || [];
  list.innerHTML = data.map((e, i) => `
    <div class="lb-entry ${i === 0 ? 'rank-1' : i === 1 ? 'rank-2' : i === 2 ? 'rank-3' : ''}">
      <div class="lb-rank">${i === 0 ? '\u{1F451}' : i + 1}</div>
      <div class="lb-name">${e.name}</div>
      <div class="lb-stat">${e.stat}</div>
    </div>
  `).join('');
}
