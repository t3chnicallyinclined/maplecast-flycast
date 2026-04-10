// ============================================================================
// TICKER.MJS — Live marquee at the bottom of king.html
//
// Pulls recent match_end game_events from SurrealDB and renders them as a
// scrolling ticker. Refreshes every 30 seconds. Highlights matches with
// quality flags (perfect/ocv/comeback/clutch) with appropriate icons.
// ============================================================================

import { surrealQuery } from './surreal.mjs';

const REFRESH_MS = 30_000;
const FETCH_LIMIT = 12;
const FALLBACK = `<span class="ticker-crown">&#x1FA99;</span> CABINET OPEN — PUT YOUR QUARTER UP &nbsp;&bull;&nbsp; <span class="ticker-fire">&#x1F525;</span> NOBD.NET — KING OF MARVEL &nbsp;&bull;&nbsp;`;

let _interval = null;
let _lastRender = '';

function buildItems(rows) {
  // Each row is a match_end game_event. Output an array of HTML snippets,
  // most "interesting" first (quality flags get priority).
  const items = [];
  for (const r of rows) {
    const d = r.data || {};
    const winner = (d.winner || '').toUpperCase();
    const loser = (d.loser || '').toUpperCase();
    if (!winner) continue;

    // Quality moments — these get top billing
    if (d.perfect) {
      items.push(`<span class="ticker-fire">&#x1F525;</span> ${winner} just PERFECTED ${loser || 'someone'}`);
    }
    if (d.ocv) {
      items.push(`<span class="ticker-skull">&#x1F480;</span> ${winner} ran the OCV on ${loser || 'someone'}`);
    }
    if (d.comeback) {
      items.push(`<span class="ticker-fire">&#x1F525;</span> ${winner} made a 1-vs-3 COMEBACK on ${loser || 'someone'}`);
    }
    if (d.clutch) {
      items.push(`<span class="ticker-crown">&#x23F1;&#xFE0F;</span> ${winner} clutched it out vs ${loser || 'someone'}`);
    }

    // Big combos — only the bigger of the two
    const bigCombo = Math.max(d.p1_max_combo || 0, d.p2_max_combo || 0);
    if (bigCombo >= 20) {
      items.push(`<span class="ticker-fire">&#x1F525;</span> ${bigCombo}-HIT COMBO landed in ${winner} vs ${loser || '???'}`);
    }

    // Always at least mention the win
    if (!d.perfect && !d.ocv && !d.comeback && !d.clutch) {
      items.push(`<span class="ticker-crown">&#x1F451;</span> ${winner} beat ${loser || 'a challenger'}`);
    }
  }
  return items;
}

async function refreshTicker() {
  const el = document.getElementById('tickerContent');
  if (!el) return;

  try {
    const sql = `SELECT kind, ts, data FROM game_event WHERE kind = 'match_end' ORDER BY ts DESC LIMIT ${FETCH_LIMIT}`;
    const res = await surrealQuery(sql);
    const rows = res?.[0]?.result || [];
    const items = buildItems(rows);

    if (items.length === 0) {
      // No match_ends yet — keep the fallback marquee
      if (_lastRender !== FALLBACK) {
        el.innerHTML = FALLBACK;
        _lastRender = FALLBACK;
      }
      return;
    }

    // Join with bullet separator, repeat at least twice so the marquee
    // animation doesn't have a visible gap
    const joined = items.join(' &nbsp;&bull;&nbsp; ');
    const html = `${joined} &nbsp;&bull;&nbsp; ${joined} &nbsp;&bull;&nbsp;`;
    if (html !== _lastRender) {
      el.innerHTML = html;
      _lastRender = html;
    }
  } catch (e) {
    console.warn('[ticker] refresh failed:', e);
  }
}

export function startTicker() {
  if (_interval) return;
  refreshTicker();  // immediate first paint
  _interval = setInterval(refreshTicker, REFRESH_MS);
}

export function stopTicker() {
  if (_interval) {
    clearInterval(_interval);
    _interval = null;
  }
}
