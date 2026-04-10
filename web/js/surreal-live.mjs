// ============================================================================
// SURREAL-LIVE.MJS — SurrealDB WebSocket client with LIVE SELECT support
//
// Companion to surreal.mjs (REST). This file owns the WS connection and
// dispatches LIVE SELECT notifications to per-subscription callbacks.
//
// Why a separate file: surreal.mjs is fire-and-forget REST for one-shot
// queries (leaderboard fetch, profile lookup). Live queries need a persistent
// WS, request/response correlation by JSON-RPC id, and a notification
// dispatcher. Different shape entirely.
//
// Wire format (SurrealDB 3.0+ JSON-RPC v2 over WS):
//   Request:  {id: "1", method: "signin",  params: [{user, pass, NS, DB}]}
//   Response: {id: "1", result: <token>}
//   Notify:   {result: {id: "<live-id>", action: "CREATE"|"UPDATE"|"DELETE", result: <row>}}
//
// LIVE notifications use the live query id (returned from `live` request) to
// route to the right subscriber callback.
//
// Reconnect: exponential backoff up to 30s, automatic re-subscribe of all
// active live queries on reconnect.
// ============================================================================

// SurrealDB endpoint — through nginx /db/rpc on production, direct ws://localhost:8000/rpc in dev
const isLocal = (location.hostname === 'localhost' || location.hostname === '127.0.0.1')
  && (location.port === '8000' || location.port === '8001');
const SURREAL_WS_URL = isLocal
  ? `ws://${location.hostname}:8000/rpc`
  : `${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.hostname}/db/rpc`;

import { state } from './state.mjs';
import { getAuthToken } from './surreal.mjs';

const SURREAL_NS = 'maplecast';
const SURREAL_DB = 'arcade';

// Fallback: viewer database user (shared with surreal.mjs REST path).
// Used only when state.dbToken is not set (signed-out / page-load-before-auth).
const VIEWER_USER = 'viewer';
const VIEWER_PASS = 'nobd_view_2026';

// ============================================================================
// Module-local state
// ============================================================================

let _ws = null;
let _wsReady = false;
let _connectingPromise = null;
let _nextRpcId = 1;
let _pending = new Map();        // rpcId → {resolve, reject}
let _liveCallbacks = new Map();  // liveQueryId → callback(action, row)
let _activeSubscriptions = [];   // [{table, where, callback, currentLiveId}] for re-subscribe on reconnect
let _reconnectAttempts = 0;
let _reconnectTimer = null;

// ============================================================================
// WebSocket lifecycle
// ============================================================================

function nextId() {
  return String(_nextRpcId++);
}

function rpc(method, params) {
  if (!_ws || _ws.readyState !== WebSocket.OPEN) {
    return Promise.reject(new Error('surreal-live: not connected'));
  }
  return new Promise((resolve, reject) => {
    const id = nextId();
    _pending.set(id, { resolve, reject });
    try {
      _ws.send(JSON.stringify({ id, method, params }));
    } catch (e) {
      _pending.delete(id);
      reject(e);
    }
    // 10s timeout — surreal calls should be sub-second
    setTimeout(() => {
      if (_pending.has(id)) {
        _pending.delete(id);
        reject(new Error(`surreal-live: timeout on ${method}`));
      }
    }, 10_000);
  });
}

function handleMessage(text) {
  let msg;
  try { msg = JSON.parse(text); }
  catch { return; }

  // RPC reply — correlated by id (check FIRST so live-query reply with
  // both id and result-with-action doesn't get misclassified)
  if (msg.id !== undefined) {
    const pending = _pending.get(String(msg.id));
    if (pending) {
      _pending.delete(String(msg.id));
      if (msg.error) pending.reject(new Error(msg.error.message || JSON.stringify(msg.error)));
      else pending.resolve(msg.result);
      return;
    }
    // Fall through — id present but no pending RPC (could be a stray)
  }

  // Live notification. SurrealDB 3.x notification shape over WS RPC:
  //   { result: {
  //       id:     <live query uuid>,         ← matches the id returned from LIVE SELECT
  //       action: 'CREATE'|'UPDATE'|'DELETE',
  //       record: 'tablename:id',            ← string record id (always present)
  //       result: { ...full row object... }, ← the actual row data (may be stale on DELETE)
  //       session: <session uuid>
  //   }}
  // The callback signature is `(action, row, recordId)` — most callers
  // only need (action, row), but DELETE handlers benefit from the explicit
  // recordId since `row.id` may not be reliably populated on delete.
  if (msg.result && typeof msg.result === 'object' && msg.result.action) {
    const live = msg.result;
    const liveId = live.id;
    const row = live.result; // the full row object (not `record` which is just the id string)
    const recordId = live.record || (row && row.id) || null;
    const cb = _liveCallbacks.get(String(liveId));
    if (cb) {
      try { cb(live.action, row, recordId); }
      catch (e) { console.warn('[surreal-live] subscriber error:', e); }
    } else {
      console.warn('[surreal-live] notification with no matching subscriber, liveId =', liveId, 'known =', Array.from(_liveCallbacks.keys()));
    }
    return;
  }
}

function scheduleReconnect() {
  if (_reconnectTimer) return;
  const delay = Math.min(30_000, 500 * Math.pow(2, _reconnectAttempts));
  _reconnectAttempts++;
  console.log(`[surreal-live] reconnecting in ${delay}ms (attempt ${_reconnectAttempts})`);
  _reconnectTimer = setTimeout(() => {
    _reconnectTimer = null;
    _connectingPromise = null;
    connect().catch(e => console.warn('[surreal-live] reconnect failed:', e.message));
  }, delay);
}

async function connect() {
  if (_wsReady) return;
  if (_connectingPromise) return _connectingPromise;

  _connectingPromise = new Promise((resolve, reject) => {
    console.log('[surreal-live] connecting to', SURREAL_WS_URL);
    let ws;
    try { ws = new WebSocket(SURREAL_WS_URL, ['json']); }
    catch (e) { _connectingPromise = null; reject(e); return; }

    let settled = false;

    ws.onopen = async () => {
      console.log('[surreal-live] WS open');
      _ws = ws;
      _wsReady = true;
      _reconnectAttempts = 0;

      try {
        // 1. Pick an auth method:
        //    - If state.dbToken is set (user is signed in with a browser-scoped
        //      JWT minted by the relay), use `authenticate` to bind the WS to
        //      that token. All subsequent queries run under $auth.id = the
        //      player record.
        //    - Otherwise fall back to `signin` with viewer credentials. The
        //      VIEWER role can SELECT globally but cannot write slot /
        //      live_match (blocked by PERMISSIONS NONE at the table level).
        if (state.dbToken) {
          await rpc('authenticate', [state.dbToken]);
          console.log('[surreal-live] authenticated with browser JWT');
        } else {
          const token = await rpc('signin', [{
            user: VIEWER_USER, pass: VIEWER_PASS,
            NS: SURREAL_NS, DB: SURREAL_DB,
          }]);
          if (!token) throw new Error('signin returned no token');
          console.log('[surreal-live] authenticated as viewer');
        }

        // 2. Use NS/DB explicitly — required for both auth paths in 3.x
        await rpc('use', [SURREAL_NS, SURREAL_DB]);

        // 3. Re-subscribe any live queries that were active before disconnect.
        // SurrealDB 3.x: the dedicated `live` RPC method is gone. We use a
        // `query` RPC with a `LIVE SELECT ...` SQL statement, and the live
        // query id comes back in the result envelope.
        for (const sub of _activeSubscriptions) {
          try {
            const newLiveId = await runLiveSelect(sub.table, sub.where);
            if (sub.currentLiveId) _liveCallbacks.delete(String(sub.currentLiveId));
            sub.currentLiveId = newLiveId;
            _liveCallbacks.set(String(newLiveId), sub.callback);
            console.log(`[surreal-live] re-subscribed ${sub.table} → live id ${newLiveId}`);
          } catch (e) {
            console.warn(`[surreal-live] re-subscribe ${sub.table} failed:`, e.message);
          }
        }

        if (!settled) { settled = true; resolve(); }
      } catch (e) {
        console.warn('[surreal-live] signin/use failed:', e.message);
        if (!settled) { settled = true; reject(e); }
        try { ws.close(); } catch {}
      }
    };

    ws.onmessage = (e) => handleMessage(e.data);

    ws.onerror = (e) => {
      console.warn('[surreal-live] WS error');
      if (!settled) { settled = true; reject(new Error('ws error')); }
    };

    ws.onclose = () => {
      console.log('[surreal-live] WS closed');
      _ws = null;
      _wsReady = false;
      _connectingPromise = null;
      // Reject any in-flight RPC calls
      for (const [id, p] of _pending) p.reject(new Error('connection closed'));
      _pending.clear();
      // Drop live callback ids — they'll be re-registered with new ids on reconnect
      _liveCallbacks.clear();
      for (const sub of _activeSubscriptions) sub.currentLiveId = null;
      scheduleReconnect();
    };
  });

  return _connectingPromise;
}

// ============================================================================
// Internal: run a LIVE SELECT and unwrap the live id from the result.
//
// SurrealDB 3.x WS RPC `query` returns:
//   { id: <rpcId>, result: [ { result: <liveId>, status: "OK", time: ".." type: "live" } ] }
// We dig out result[0].result to get the live query uuid.
// ============================================================================

async function runLiveSelect(table, where) {
  // Table name interpolation is safe here because callers control it (always
  // a hardcoded literal in our codebase). The WHERE clause uses bound vars.
  let sql = `LIVE SELECT * FROM ${table}`;
  let result;
  if (where) {
    sql += ' WHERE ' + where;
  }
  result = await rpc('query', [sql]);
  // Result envelope: [{result: <liveId>, status: 'OK', type: 'live'}]
  const first = Array.isArray(result) ? result[0] : null;
  const liveId = first && typeof first === 'object' ? first.result : null;
  if (!liveId || typeof liveId !== 'string') {
    throw new Error(`LIVE SELECT returned no live id: ${JSON.stringify(result).slice(0, 200)}`);
  }
  return liveId;
}

// ============================================================================
// Public API
// ============================================================================

/**
 * Subscribe to a SurrealDB LIVE SELECT. Auto-connects if needed.
 * The callback receives (action, row) for each notification:
 *   action: 'CREATE' | 'UPDATE' | 'DELETE'
 *   row:    the changed record (full row for CREATE/UPDATE, just id for DELETE)
 *
 * Returns a subscription handle with .kill() to unsubscribe.
 *
 * @param {string} table   Table name (e.g. 'chat', 'queue', 'slot')
 * @param {object} options Optional: { where: 'username = "tris"' } (raw SQL fragment)
 * @param {function} callback (action, row) => void
 */
export async function liveSubscribe(table, options, callback) {
  if (typeof options === 'function') {
    callback = options;
    options = {};
  }
  const where = options?.where || null;

  await connect();

  const liveId = await runLiveSelect(table, where);
  const sub = { table, where, callback, currentLiveId: liveId };
  _activeSubscriptions.push(sub);
  _liveCallbacks.set(String(liveId), callback);

  console.log(`[surreal-live] subscribed ${table}${where ? ` WHERE ${where}` : ''} → live id ${liveId}`);

  return {
    kill: async () => {
      const idx = _activeSubscriptions.indexOf(sub);
      if (idx >= 0) _activeSubscriptions.splice(idx, 1);
      if (sub.currentLiveId) {
        _liveCallbacks.delete(String(sub.currentLiveId));
        try { await rpc('kill', [sub.currentLiveId]); }
        catch { /* may already be dead */ }
      }
    },
  };
}

/**
 * Run an arbitrary SurrealDB query over the WS (for INSERT/UPDATE/DELETE).
 * Equivalent to surrealQuery() in surreal.mjs but uses the persistent WS so
 * the response shares the same auth token + connection state.
 *
 * Returns the raw result array from SurrealDB.
 */
export async function liveQuery(sql, vars) {
  await connect();
  const result = await rpc('query', vars ? [sql, vars] : [sql]);
  // SurrealDB's `query` RPC resolves with an array of statement results.
  // Per-statement failures (constraint violations, type errors, perms)
  // come back as `{status: 'ERR', result: '<message>'}` and the RPC layer
  // does NOT reject. Surface them as thrown errors so callers (gotNext,
  // leaveQueue, etc.) actually know their write was rejected instead of
  // silently flipping optimistic UI to a permanent stuck state.
  if (Array.isArray(result)) {
    for (const stmt of result) {
      if (stmt && typeof stmt === 'object' && stmt.status === 'ERR') {
        const msg = typeof stmt.result === 'string' ? stmt.result : JSON.stringify(stmt.result);
        throw new Error(msg);
      }
    }
  }
  return result;
}

/**
 * Force-connect (for callers that want to fail fast on auth issues at startup
 * rather than waiting for the first subscribe).
 */
export async function liveConnect() {
  return connect();
}

/**
 * Re-authenticate the existing WS with state.dbToken. Called by auth.mjs
 * after a successful signin/register or after auto-login restores a token
 * from localStorage — at that moment the WS may already be open under a
 * viewer session, and we want subsequent LIVE SELECT / query / CREATE calls
 * to run under the browser-scoped JWT instead.
 *
 * Idempotent. No-op if no token is available, or if the WS isn't open
 * (the next connect() attempt will pick up state.dbToken on its own).
 */
export async function upgradeAuth() {
  if (!state.dbToken) return;
  if (!_wsReady) return; // fresh connect will see state.dbToken naturally
  try {
    await rpc('authenticate', [state.dbToken]);
    console.log('[surreal-live] re-authenticated with browser JWT');
  } catch (e) {
    console.warn('[surreal-live] upgradeAuth failed:', e.message);
  }
}

/**
 * Status accessor for diagnostics overlay.
 */
export function liveStatus() {
  return {
    connected: _wsReady,
    pendingRpcs: _pending.size,
    activeSubscriptions: _activeSubscriptions.length,
    reconnectAttempts: _reconnectAttempts,
  };
}
