// ============================================================================
// LEADERBOARD.MJS — Hall of Fame leaderboard tabs (SurrealDB-backed)
// ============================================================================

import { state } from './state.mjs';
import { surrealQuery } from './surreal.mjs';

// Cache fetched leaderboards by tab
const _liveCache = {};
let _currentTab = 'streak';

// Tab keys are kept stable so the HTML doesn't need new onclick handlers,
// but each one now maps to a real field the collector actually writes to
// the player table. See web/schema.surql + web/collector/src/main.rs.
const QUERIES = {
  streak:  `SELECT username AS name, best_streak AS streak FROM player WHERE best_streak > 0 ORDER BY streak DESC LIMIT 10`,
  combo:   `SELECT username AS name, best_combo AS combo FROM player WHERE best_combo > 0 ORDER BY combo DESC LIMIT 10`,
  speed:   `SELECT username AS name, ocvs FROM player WHERE ocvs > 0 ORDER BY ocvs DESC LIMIT 10`,        // tab labelled "OCV"
  perfect: `SELECT username AS name, perfects AS pf FROM player WHERE perfects > 0 ORDER BY pf DESC LIMIT 10`,
  masher:  `SELECT username AS name, comebacks AS cb FROM player WHERE comebacks > 0 ORDER BY cb DESC LIMIT 10`,  // tab labelled "COMEBACK"
  surgeon: `SELECT username AS name, clutch_wins AS cw FROM player WHERE clutch_wins > 0 ORDER BY cw DESC LIMIT 10`, // tab labelled "CLUTCH"
};

const FORMATTERS = {
  streak:  (e) => `${e.streak || 0} WINS`,
  combo:   (e) => `${e.combo || 0} HITS`,
  speed:   (e) => `${e.ocvs || 0} OCVs`,
  perfect: (e) => `${e.pf || 0} ROUNDS`,
  masher:  (e) => `${e.cb || 0} COMEBACKS`,
  surgeon: (e) => `${e.cw || 0} CLUTCH`,
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
    <a class="lb-entry ${i === 0 ? 'rank-1' : i === 1 ? 'rank-2' : i === 2 ? 'rank-3' : ''}"
       href="/player.html?name=${encodeURIComponent(e.name.toLowerCase())}"
       style="text-decoration:none;color:inherit;display:flex;">
      <div class="lb-rank">${i === 0 ? '\u{1F451}' : i + 1}</div>
      <div class="lb-name">${e.name}</div>
      <div class="lb-stat">${e.stat}</div>
    </a>
  `).join('');
}
