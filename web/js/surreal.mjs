// ============================================================================
// SURREAL.MJS — SurrealDB client
//
// HTTP API client for player auth and stats. Uses Basic auth for now.
// TODO: Migrate to DEFINE ACCESS TYPE RECORD with JWT tokens.
// ============================================================================

import { state } from './state.mjs';

// SurrealDB: via /db/ nginx proxy if served from VPS, direct port 8000 if local dev
// Local dev = localhost/127.0.0.1 on port 8000/8001. Everything else = VPS with nginx proxy.
const isLocal = (location.hostname === 'localhost' || location.hostname === '127.0.0.1')
  && (location.port === '8000' || location.port === '8001');
const SURREAL_URL = isLocal
  ? `http://${location.hostname}:8000/sql`
  : `${location.protocol}//${location.hostname}/db/sql`;
const SURREAL_NS = 'maplecast';
const SURREAL_DB = 'arcade';

export async function surrealQuery(query, vars) {
  try {
    const body = vars ? JSON.stringify({ query, vars }) : query;
    const res = await fetch(SURREAL_URL, {
      method: 'POST',
      headers: {
        'Content-Type': vars ? 'application/json' : 'text/plain',
        'Accept': 'application/json',
        'Surreal-NS': SURREAL_NS,
        'Surreal-DB': SURREAL_DB,
        'Authorization': 'Basic ' + btoa('root:root'),
      },
      body,
    });
    if (!res.ok) return null;
    return await res.json();
  } catch (e) {
    console.log('[surreal] Query failed:', e.message);
    return null;
  }
}

export async function surrealRegister(username) {
  const check = await surrealQuery(`SELECT * FROM player WHERE username = '${username.toLowerCase()}'`);
  if (check?.[0]?.result?.length > 0) {
    state.playerProfile = check[0].result[0];
    return { exists: true, profile: check[0].result[0] };
  }
  const create = await surrealQuery(
    `CREATE player SET username = '${username.toLowerCase()}', created_at = time::now(), last_seen = time::now()`
  );
  if (create?.[0]?.result?.length > 0) {
    state.playerProfile = create[0].result[0];
    return { exists: false, profile: create[0].result[0] };
  }
  return null;
}

export async function surrealUpdateLastSeen(username) {
  await surrealQuery(`UPDATE player SET last_seen = time::now() WHERE username = '${username.toLowerCase()}'`);
}

export async function surrealFetchProfile(username) {
  const res = await surrealQuery(`SELECT * FROM player WHERE username = '${username.toLowerCase()}'`);
  if (res?.[0]?.result?.length > 0) {
    state.playerProfile = res[0].result[0];
    return res[0].result[0];
  }
  return null;
}
