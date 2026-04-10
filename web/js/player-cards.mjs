// ============================================================================
// PLAYER-CARDS.MJS — Cached SurrealDB lookups for player records.
//
// Single source of truth for "what's this player's W-L / streak / etc.".
// Other modules call getCachedRecord() to read, prefetchMany() to populate.
// Subscribe with onRecordUpdate(name, cb) to get notified when a fetch lands.
//
// Cache TTL is 60s — fresh enough that a finished match shows up within a
// minute, lazy enough that we don't hammer the DB on every status broadcast.
// ============================================================================

import { surrealQuery } from './surreal.mjs';

const TTL_MS = 60_000;

// name (lowercased) → { record, fetchedAt }
const _cache = new Map();
// name (lowercased) → Set<callback>
const _subscribers = new Map();
// names being fetched right now (so we don't double-fetch)
const _inflight = new Set();

// Anonymous prefixes match the collector's skip list — no point looking up
// rows the collector deliberately doesn't write.
const ANON_PREFIXES = [
  'wanderer_', 'drifter_', 'nomad_', 'ronin_', 'ghost_', 'shadow_',
  'vagrant_', 'stranger_', 'outlaw_', 'rogue_', 'exile_', 'phantom_',
  'unknown_', 'nameless_', 'faceless_',
];

function isAnon(name) {
  if (!name) return true;
  const lower = name.toLowerCase();
  return ANON_PREFIXES.some(p => lower.startsWith(p));
}

function notify(name) {
  const subs = _subscribers.get(name);
  if (!subs) return;
  const rec = _cache.get(name)?.record;
  for (const cb of subs) {
    try { cb(rec); } catch (e) { console.warn('[player-cards] subscriber error:', e); }
  }
}

/**
 * Returns the cached record for a name, or null if we haven't fetched it.
 * Does NOT trigger a fetch — call prefetchRecord/prefetchMany for that.
 * Returns null for anonymous players (they have no DB row).
 */
export function getCachedRecord(name) {
  if (!name || isAnon(name)) return null;
  const key = name.toLowerCase();
  const entry = _cache.get(key);
  if (!entry) return null;
  return entry.record;
}

/**
 * Subscribe to record updates for a player. Callback fires immediately if
 * we already have the record cached, then again whenever it's refreshed.
 * Returns an unsubscribe function.
 */
export function onRecordUpdate(name, cb) {
  if (!name || isAnon(name)) return () => {};
  const key = name.toLowerCase();
  if (!_subscribers.has(key)) _subscribers.set(key, new Set());
  _subscribers.get(key).add(cb);

  // Fire immediately with current cache state, if any
  const cached = _cache.get(key)?.record;
  if (cached !== undefined) {
    try { cb(cached); } catch {}
  }

  return () => {
    const subs = _subscribers.get(key);
    if (subs) {
      subs.delete(cb);
      if (subs.size === 0) _subscribers.delete(key);
    }
  };
}

/**
 * Format a record as "12W-3L" or "0W-0L" for display in cards/queue rows.
 * Returns empty string for null records.
 */
export function formatRecord(record) {
  if (!record) return '';
  const w = record.wins || 0;
  const l = record.losses || 0;
  return `${w}W-${l}L`;
}

/**
 * Format a streak as "12 WIN STREAK" / "3 LOSS STREAK" / "" for zero.
 */
export function formatStreak(record) {
  if (!record) return '';
  const s = record.streak || 0;
  if (s > 0) return `${s} WIN STREAK`;
  if (s < 0) return `${-s} LOSS STREAK`;
  return '';
}

/**
 * Fetch records for a list of names in a single SurrealDB query, populate
 * the cache, and notify subscribers. Skips anonymous names and names that
 * are already cached and fresh.
 */
export async function prefetchMany(names) {
  if (!names || names.length === 0) return;

  const now = Date.now();
  const toFetch = [];
  for (const raw of names) {
    if (!raw) continue;
    const key = raw.toLowerCase();
    if (isAnon(key)) continue;
    if (_inflight.has(key)) continue;
    const entry = _cache.get(key);
    if (entry && (now - entry.fetchedAt) < TTL_MS) continue;
    toFetch.push(key);
  }
  if (toFetch.length === 0) return;

  for (const k of toFetch) _inflight.add(k);

  // SurrealDB SQL: WHERE username IN [...]
  // We build the literal array — viewer is read-only and these names come
  // from server status JSON (player names that already passed the lobby's
  // own validation), so injection surface is minimal. Still, defang quotes.
  const list = toFetch.map(n => `'${n.replace(/'/g, "")}'`).join(',');
  const sql = `SELECT username, wins, losses, streak, best_streak, best_combo, perfects, ocvs, comebacks, clutch_wins FROM player WHERE username IN [${list}]`;

  try {
    const res = await surrealQuery(sql);
    const rows = res?.[0]?.result || [];
    const seen = new Set();
    for (const row of rows) {
      const key = (row.username || '').toLowerCase();
      if (!key) continue;
      _cache.set(key, { record: row, fetchedAt: now });
      seen.add(key);
      notify(key);
    }
    // Names we asked about that came back empty: cache a null record so we
    // don't keep re-fetching every status tick.
    for (const k of toFetch) {
      if (!seen.has(k)) {
        _cache.set(k, { record: null, fetchedAt: now });
        notify(k);
      }
    }
  } catch (e) {
    console.warn('[player-cards] prefetchMany failed:', e);
  } finally {
    for (const k of toFetch) _inflight.delete(k);
  }
}

/**
 * Convenience: fetch a single name. Just delegates to prefetchMany.
 */
export function prefetchRecord(name) {
  return prefetchMany([name]);
}

/**
 * Test/dev helper: clear all cache + subscribers. Not used in production.
 */
export function _resetCache() {
  _cache.clear();
  _subscribers.clear();
  _inflight.clear();
}
