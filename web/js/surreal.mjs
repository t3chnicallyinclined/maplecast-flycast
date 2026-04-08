// ============================================================================
// SURREAL.MJS — SurrealDB client
//
// HTTP API client for player auth and stats. Uses Basic auth for now.
// TODO: Migrate to DEFINE ACCESS TYPE RECORD with JWT tokens.
// ============================================================================

import { state } from './state.mjs';

// SurrealDB: via /db/ nginx proxy if served from VPS, direct port 8000 if local dev
const isLocal = (location.hostname === 'localhost' || location.hostname === '127.0.0.1')
  && (location.port === '8000' || location.port === '8001');
const SURREAL_BASE = isLocal
  ? `http://${location.hostname}:8000`
  : `${location.protocol}//${location.hostname}/db`;
const SURREAL_NS = 'maplecast';
const SURREAL_DB = 'arcade';
const VIEWER_USER = 'viewer';
const VIEWER_PASS = 'nobd_view_2026';

let _token = null;
let _signinPromise = null;

async function signin() {
  if (_token) return _token;
  if (_signinPromise) return _signinPromise;
  _signinPromise = (async () => {
    try {
      const res = await fetch(`${SURREAL_BASE}/signin`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Accept': 'application/json' },
        body: JSON.stringify({
          user: VIEWER_USER, pass: VIEWER_PASS,
          NS: SURREAL_NS, DB: SURREAL_DB,
        }),
      });
      const data = await res.json();
      if (data.token) {
        _token = data.token;
        // Refresh after 50 minutes (token TTL is 1h)
        setTimeout(() => { _token = null; }, 50 * 60 * 1000);
        return _token;
      }
    } catch (e) {
      console.log('[surreal] signin failed:', e.message);
    }
    _signinPromise = null;
    return null;
  })();
  return _signinPromise;
}

export async function surrealQuery(query, vars) {
  try {
    const token = await signin();
    if (!token) return null;
    const body = vars ? JSON.stringify({ query, vars }) : query;
    const res = await fetch(`${SURREAL_BASE}/sql`, {
      method: 'POST',
      headers: {
        'Content-Type': vars ? 'application/json' : 'text/plain',
        'Accept': 'application/json',
        'Surreal-NS': SURREAL_NS,
        'Surreal-DB': SURREAL_DB,
        'Authorization': 'Bearer ' + token,
      },
      body,
    });
    if (!res.ok) {
      // Token might be expired — clear and retry once
      if (res.status === 401 || res.status === 403) {
        _token = null;
      }
      return null;
    }
    return await res.json();
  } catch (e) {
    console.log('[surreal] Query failed:', e.message);
    return null;
  }
}

export async function surrealRegister(username) {
  // Read-only: check if player exists. Player records are auto-created by the
  // collector when matches happen, so first-time users will be "new" until
  // they play their first match.
  const check = await surrealQuery(`SELECT * FROM player WHERE username = '${username.toLowerCase()}'`);
  if (check?.[0]?.result?.length > 0) {
    state.playerProfile = check[0].result[0];
    return { exists: true, profile: check[0].result[0] };
  }
  // No existing record — return a stub profile so the UI can render
  const stub = { username: username.toLowerCase(), wins: 0, losses: 0, new_user: true };
  state.playerProfile = stub;
  return { exists: false, profile: stub };
}

export async function surrealUpdateLastSeen(username) {
  // No-op — last_seen is updated by the collector on match events.
  // Browsers don't have write access to the DB.
}

export async function surrealFetchProfile(username) {
  const res = await surrealQuery(`SELECT * FROM player WHERE username = '${username.toLowerCase()}'`);
  if (res?.[0]?.result?.length > 0) {
    state.playerProfile = res[0].result[0];
    return res[0].result[0];
  }
  return null;
}
