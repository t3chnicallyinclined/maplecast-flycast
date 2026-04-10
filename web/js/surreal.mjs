// ============================================================================
// SURREAL.MJS — SurrealDB REST client (one-shot queries + signin fallback)
//
// Authentication model (Phase 6a+):
//   1. If state.dbToken is set (user is signed in, relay minted a scoped JWT
//      via DEFINE ACCESS browser), use it directly.
//   2. Otherwise fall back to the viewer database user — which can SELECT
//      anything but cannot write slot/live_match (blocked by PERMISSIONS NONE).
//
// Companion module surreal-live.mjs handles WS-based LIVE SELECT with the
// same auth precedence. Both share this token cache via getAuthToken().
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

// Fallback: unauthenticated browsers (and signed-in users whose db_token
// expired) use the `viewer` database user. VIEWER role can SELECT but is
// gated from writing `slot` / `live_match` by the table-level PERMISSIONS
// NONE rule, and can write `chat` / `queue` (PERMISSIONS FULL).
const VIEWER_USER = 'viewer';
const VIEWER_PASS = 'nobd_view_2026';

let _viewerToken = null;
let _viewerSigninPromise = null;

/**
 * Public: fetch the current best token to use for a SurrealDB call.
 * Prefers state.dbToken (browser-scoped JWT from auth.mjs) over the
 * cached viewer token. Exported so surreal-live.mjs can share it.
 */
export async function getAuthToken() {
  if (state.dbToken) return state.dbToken;
  return viewerSignin();
}

/**
 * Reset the cached viewer token. Called after a sign-in/out event so the
 * next REST call picks up state.dbToken (or drops back to viewer if the
 * user just signed out).
 */
export function resetAuthCache() {
  _viewerToken = null;
  _viewerSigninPromise = null;
}

async function viewerSignin() {
  if (_viewerToken) return _viewerToken;
  if (_viewerSigninPromise) return _viewerSigninPromise;
  _viewerSigninPromise = (async () => {
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
        _viewerToken = data.token;
        // Refresh after 50 minutes (token TTL is 1h)
        setTimeout(() => { _viewerToken = null; }, 50 * 60 * 1000);
        return _viewerToken;
      }
    } catch (e) {
      console.log('[surreal] viewer signin failed:', e.message);
    }
    _viewerSigninPromise = null;
    return null;
  })();
  return _viewerSigninPromise;
}

export async function surrealQuery(query, vars) {
  try {
    const token = await getAuthToken();
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
      // Token might be expired — clear both caches and let next call retry
      if (res.status === 401 || res.status === 403) {
        resetAuthCache();
        // If it was a scoped browser token that expired, drop it so we fall
        // back to viewer until the user signs in again. auth.mjs is
        // responsible for refreshing on explicit re-signin.
        if (state.dbToken) {
          console.warn('[surreal] db_token rejected — clearing');
          state.dbToken = null;
          localStorage.removeItem('nobd_db_token');
        }
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
