// ============================================================================
// LEADERBOARD.MJS — Hall of Fame leaderboard tabs (SurrealDB-backed)
// ============================================================================

import { state } from './state.mjs';
import { surrealQuery } from './surreal.mjs';

// Cache fetched leaderboards by tab
const _liveCache = {};
let _currentTab = 'streak';

const QUERIES = {
  streak:  `SELECT username AS name, math::max([streak_current, streak_best]) AS streak FROM player ORDER BY streak DESC LIMIT 10`,
  combo:   `SELECT username AS name, max_combo AS combo FROM player WHERE max_combo > 0 ORDER BY combo DESC LIMIT 10`,
  speed:   `SELECT username AS name, fastest_match_sec AS sec FROM player WHERE fastest_match_sec > 0 ORDER BY sec ASC LIMIT 10`,
  perfect: `SELECT username AS name, perfects AS pf FROM player WHERE perfects > 0 ORDER BY pf DESC LIMIT 10`,
  masher:  `SELECT username AS name, max_inps AS ips FROM player WHERE max_inps > 0 ORDER BY ips DESC LIMIT 10`,
  surgeon: `SELECT username AS name, accuracy AS acc FROM player WHERE accuracy > 0 ORDER BY acc DESC LIMIT 10`,
};

const FORMATTERS = {
  streak:  (e) => `${e.streak || 0} WINS`,
  combo:   (e) => `${e.combo || 0} HITS`,
  speed:   (e) => `${e.sec || 0} SEC`,
  perfect: (e) => `${e.pf || 0} ROUNDS`,
  masher:  (e) => `${e.ips || 0} INP/S`,
  surgeon: (e) => `${(e.acc || 0).toFixed(1)}%`,
};

export function switchTab(btn, tab) {
  document.querySelectorAll('.lb-tab').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  _currentTab = tab;
  renderLeaderboard(tab);
}

export async function renderLeaderboard(tab) {
  _currentTab = tab;
  const list = document.getElementById('leaderboardList');

  // Render cached/placeholder immediately so UI doesn't flash
  let data = _liveCache[tab] || state.leaderboards[tab] || [];
  paintList(list, tab, data);

  // Fetch live in background
  try {
    const res = await surrealQuery(QUERIES[tab]);
    if (res?.[0]?.result?.length > 0) {
      const rows = res[0].result.map(r => ({
        name: (r.name || '').toUpperCase(),
        stat: FORMATTERS[tab](r),
      }));
      _liveCache[tab] = rows;
      // Only repaint if user is still on this tab
      if (_currentTab === tab) paintList(list, tab, rows);
    }
  } catch (e) {
    // Stay with placeholder/cache on failure
  }
}

function paintList(list, tab, data) {
  if (!data.length) {
    list.innerHTML = '<div class="lb-empty">NO RANKINGS YET — BE THE FIRST</div>';
    return;
  }
  list.innerHTML = data.map((e, i) => `
    <div class="lb-entry ${i === 0 ? 'rank-1' : i === 1 ? 'rank-2' : i === 2 ? 'rank-3' : ''}">
      <div class="lb-rank">${i === 0 ? '\u{1F451}' : i + 1}</div>
      <div class="lb-name">${e.name}</div>
      <div class="lb-stat">${e.stat}</div>
    </div>
  `).join('');
}
