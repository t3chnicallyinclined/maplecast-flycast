// ============================================================================
// CHAT.MJS — Trash Talk chat, backed by SurrealDB
//
// Phase 2 of the SurrealDB-first refactor: chat is no longer a browser-local
// `state.chatHistory` array forwarded over WS. It's a row in the `chat` table,
// fanned out to every browser via a LIVE SELECT subscription.
//
// Cross-browser, persistent across reloads, late-joiners get the last 50
// messages on init.
//
// Message kinds:
//   user     — free-form text from a signed-in user
//   reaction — preset emoji from any browser (anon or signed-in)
//   system   — server-emitted announcement (anyone can write today; Phase 6
//              tightens this with auth scopes)
//
// Render path: same DOM, same msgToHtml(), but the data source is the
// SurrealDB live query callback instead of state.chatHistory.push().
// ============================================================================

import { state } from './state.mjs';
import { escapeHtml } from './ui-common.mjs';
import { liveSubscribe, liveQuery } from './surreal-live.mjs';

// Local cache of rendered chat rows, in display order (oldest first).
// This replaces state.chatHistory as the in-memory shadow of the chat feed.
// It exists only so renderChatFull() (UI re-init) and incremental updates
// can share the same render code path. Source of truth is SurrealDB.
let _rows = [];

// Track ids we've already rendered so we can de-dupe between the initial
// SELECT and any LIVE notifications that fire while the SELECT is in flight.
const _rendered = new Set();

// ============================================================================
// Render helpers (unchanged from the previous implementation)
// ============================================================================

function rowToHtml(r) {
  if (r.kind === 'system') {
    return `<div class="chat-msg"><span class="msg-system">&gt; ${escapeHtml(r.body || '')}</span></div>`;
  }
  if (r.kind === 'reaction') {
    return `<div class="chat-msg"><span class="msg-hype">${escapeHtml(r.body || '')}</span></div>`;
  }
  // kind === 'user'
  const author = r.author || 'ANON';
  const isKing = state.kingName && author.toLowerCase() === state.kingName.toLowerCase();
  const cls = isKing ? 'msg-name king' : 'msg-name';
  return `<div class="chat-msg"><span class="${cls}">${escapeHtml(author)}</span>: <span class="msg-text">${escapeHtml(r.body || '')}</span></div>`;
}

function appendRow(r) {
  if (!r || !r.id) return;
  const rowKey = String(r.id);
  if (_rendered.has(rowKey)) return;
  _rendered.add(rowKey);
  _rows.push(r);
  const el = document.getElementById('chatMessages');
  if (!el) return;
  el.insertAdjacentHTML('beforeend', rowToHtml(r));
  el.scrollTop = el.scrollHeight;
}

// Drop a row by id — used by the live DELETE handler so the collector's
// 1-hour TTL sweep is reflected in any tab that's been open longer than
// the TTL. Without this, _rendered + _rows accumulate forever and old
// tabs keep showing chats the database has long since deleted.
function removeRow(id) {
  const key = String(id);
  if (!_rendered.has(key)) return;
  _rendered.delete(key);
  _rows = _rows.filter(r => String(r.id) !== key);
  // Re-render the chat box. We have at most 50-ish rows in memory at any
  // time (initial fetch limit + new arrivals); innerHTML rebuild is cheap.
  const el = document.getElementById('chatMessages');
  if (el) {
    const wasNearBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
    el.innerHTML = _rows.map(rowToHtml).join('');
    if (wasNearBottom) el.scrollTop = el.scrollHeight;
  }
}

// Full rebuild — for UI hot-reload or debug. Not normally called after init.
export function renderChatFull() {
  const el = document.getElementById('chatMessages');
  if (!el) return;
  el.innerHTML = _rows.map(rowToHtml).join('');
  el.scrollTop = el.scrollHeight;
}

// Back-compat shim — older callers (auth.mjs, queue.mjs, ws-connection.mjs)
// still call renderChat() expecting "render the most recent thing in
// state.chatHistory". Now those callers should write to SurrealDB instead and
// the live subscription will handle the render. But during the transition we
// keep this exported as a no-op so the imports don't break.
export function renderChat() {
  // No-op. Live query handles rendering. See appendRow().
}

// ============================================================================
// Init — load history, subscribe to live updates
// ============================================================================

let _initStarted = false;

export async function initChat() {
  if (_initStarted) return;
  _initStarted = true;

  try {
    // 1. Pull the last 50 messages, oldest-first, so the chat box shows
    //    immediately even before the live subscription activates.
    const initial = await liveQuery(
      'SELECT * FROM chat ORDER BY ts DESC LIMIT 50'
    );
    // SurrealDB WS RPC returns query results as either:
    //   [{ status: "OK", result: [...rows], time: "..." }]   ← envelope shape
    //   [[...rows]]                                          ← bare shape
    // Detect both and unwrap to the rows array.
    let rows = [];
    const first = initial?.[0];
    if (Array.isArray(first)) rows = first;
    else if (first?.result && Array.isArray(first.result)) rows = first.result;
    // SurrealDB returned newest-first; reverse for display order
    for (let i = rows.length - 1; i >= 0; i--) {
      appendRow(rows[i]);
    }
    console.log(`[chat] loaded ${rows.length} initial messages`);

    // 2. Subscribe to live updates for new rows AND for the TTL-sweep
    //    deletes that the collector runs every 500ms (rows older than 1h).
    //    Without DELETE handling here, an open tab accumulates rows forever
    //    while the DB is actually pruning them. Use the explicit recordId
    //    third arg because DELETE notifications may not include `row.id`.
    await liveSubscribe('chat', (action, row, recordId) => {
      if (action === 'CREATE' || action === 'UPDATE') {
        appendRow(row);
      } else if (action === 'DELETE') {
        const id = recordId || (row && row.id);
        if (id) removeRow(id);
      }
    });
    console.log('[chat] live subscription ready');
  } catch (e) {
    console.warn('[chat] initChat failed:', e.message);
  }
}

// ============================================================================
// Outbound — write a row instead of sending a WS message
// ============================================================================

export async function sendChat() {
  const input = document.getElementById('chatInput');
  const text = (input?.value || '').trim();
  if (!text) return;
  if (text.length > 280) {
    // Reject locally; server would reject too (ASSERT len <= 280).
    console.warn('[chat] message too long (max 280)');
    return;
  }

  // Anon users can chat too — the original "anon = reactions only" policy
  // was too restrictive when there's no signed-in chat to compare against.
  // Phase 6 may revisit this with proper auth scopes (e.g. anon names get a
  // visual distinction, or rate limits, or a content filter).
  const author = state.myName || 'ANON';
  if (input) input.value = '';

  try {
    await liveQuery(
      'CREATE chat SET author = $author, body = $body, kind = "user"',
      { author, body: text }
    );
    // No local push — the live subscription will deliver our own row back
    // and appendRow() will dedupe + render it.
  } catch (e) {
    console.warn('[chat] sendChat failed:', e.message);
  }
}

const REACTION_MAP = {
  HYPE:    '\u{1F525} HYPE!!!',
  BODIED:  '\u{1F480} BODIED',
  RESPECT: '\u{1F451} GGs RESPECT',
  SALTY:   '\u{1F9C2} SALTY',
  FRAUD:   '\u{1F6A8} FRAUD DETECTED',
};

export async function sendReaction(type) {
  const body = REACTION_MAP[type] || type;
  const author = state.myName || 'ANON';

  try {
    await liveQuery(
      'CREATE chat SET author = $author, body = $body, kind = "reaction"',
      { author, body }
    );
  } catch (e) {
    console.warn('[chat] sendReaction failed:', e.message);
  }
}

// ============================================================================
// System messages — convenience helper for other modules
//
// Replaces the previous pattern:
//   state.chatHistory.push({ name: null, system: 'foo' });
//   renderChat();
//
// With:
//   import { systemMessage } from './chat.mjs';
//   systemMessage('foo');
//
// This writes a kind='system' row that all browsers see. Today any client
// can write system messages because PERMISSIONS is FULL — Phase 6 will
// restrict this to the collector only via auth scopes.
// ============================================================================

export async function systemMessage(body) {
  if (!body) return;
  try {
    await liveQuery(
      'CREATE chat SET body = $body, kind = "system"',
      { body }
    );
  } catch (e) {
    console.warn('[chat] systemMessage failed:', e.message);
  }
}
