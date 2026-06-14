// local_gfx_overlay.mjs — Phase A LOCAL-ROM GFX overlay (render-replica).
//
// THESIS: the server still ships each body's GFX1/GFX2 (possibly STALE or TRUNCATED —
// the persistent scramble). This module OVERRIDES that art with the COMPLETE disc GFX
// fetched from web/render-replica/gfx/PL{NN}_gfx{1,2}.bin (NN = upper-hex char_id == PLxx
// index), written into the RAM image at the EXACT addresses the walker reads:
//     node+0x15C -> GFX1 (pixel/LZSS sheet, body_decoder reads here)
//     node+0x160 -> GFX2 (cell-record sheet, the walker reads here)
// So render_frame's walker + body_decoder read PERFECT, never-truncated art = no scramble.
//
// GROUND TRUTH (CONFIRMED 2026-06-13, tools/extract_replica_gfx.py --verify-ram vs
// _ryu_capture/mc_ram_dump.bin): the disc segments are BYTE-IDENTICAL to RAM at node+0x15C/
// 0x160 for the active chars (PL34 GFX1 1064736B@0x0c420040, PL17 GFX1 964736B@0x0c810040,
// both 0 diff; GFX2 likewise). char_id (struct +0x001) IS the PLxx index in hex (PL00=Ryu).
//
// COST: a char's GFX is fetched + cached ONCE (by char_id) in a JS Map (later IndexedDB for
// cross-session). After the first frame a char is on screen, the overlay is a memcpy of the
// (cached) GFX into the RAM image — sub-millisecond. The fetch is the local-ROM cache: the
// art rides the wire ZERO times after first load (Phase B removes it from the wire entirely).

const RAM_MASK = 0x00FFFFFF;                          // 0x8C../0x0C.. alias -> low 24 bits

// Char-struct layout (re_kb struct:char_struct / CLAUDE.md). Same SLOTS body_decoder uses.
const SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];
const OFF_ACTIVE = 0x000, OFF_CID = 0x001, OFF_GFX1 = 0x15C, OFF_GFX2 = 0x160;

const u8  = (ram, a) => ram[a & RAM_MASK];
const u32 = (ram, a) => (ram[a & RAM_MASK] | (ram[(a + 1) & RAM_MASK] << 8)
                         | (ram[(a + 2) & RAM_MASK] << 16) | (ram[(a + 3) & RAM_MASK] << 24)) >>> 0;

// Per-char_id fetch cache: char_id -> { gfx1:Uint8Array, gfx2:Uint8Array } | Promise | 'miss'.
// Shared across frames (and slots) — a char loads once, overlays every frame for free.
const _cache = new Map();
let _baseUrl = null;                                  // resolved against this module's URL

function gfxUrl(name) {
    if (!_baseUrl) _baseUrl = new URL('./gfx/', import.meta.url);
    return new URL(name, _baseUrl);
}

// Kick a non-blocking fetch of a char_id's two segments. On resolve, store the bytes in
// _cache so the NEXT frame overlays them. The frame that triggers the fetch overlays
// nothing for that char (shipped GFX stands one frame) — invisible at 60fps after connect.
function fetchChar(cid, hexName, log) {
    const entry = { gfx1: null, gfx2: null };
    const p = Promise.all([
        fetch(gfxUrl(`${hexName}_gfx1.bin`)).then(r => r.ok ? r.arrayBuffer() : Promise.reject(r.status)),
        fetch(gfxUrl(`${hexName}_gfx2.bin`)).then(r => r.ok ? r.arrayBuffer() : Promise.reject(r.status)),
    ]).then(([a1, a2]) => {
        const got = { gfx1: new Uint8Array(a1), gfx2: new Uint8Array(a2) };
        _cache.set(cid, got);
        if (log) log(`[local-gfx] ${hexName} cached: GFX1 ${got.gfx1.length}B + GFX2 ${got.gfx2.length}B`, 'ok');
        return got;
    }).catch(err => {
        _cache.set(cid, 'miss');
        if (log) log(`[local-gfx] ${hexName} fetch failed (${err}) — shipped GFX stands`, 'warn');
        return null;
    });
    _cache.set(cid, p);                               // mark in-flight (a Promise)
    return p;
}

// A char_struct slot's GFX node base is valid (points into guest area-3 RAM) iff it carries
// the 0x0C../0x8C.. tag. Tag-in bodies pre-seed this base at match load even while active=0.
const validBase = (g) => !!((g & 0x0C000000) || (g & 0x8C000000));

// ----------------------------------------------------------------------------
// PUBLIC: applyLocalGfx(ram, log) — call ONCE PER FRAME, BEFORE render_frame/transpiledTA.
//   Walks ALL SIX char structs (slot stride 0x5A4: P1C1/P2C1/P1C2/P2C2/P1C3/P2C3) — NOT the
//   slot-table count (which clamps to connect-active bodies and EXCLUDES tag-ins; re_kb 23b).
//   For every ACTIVE body, overlays the COMPLETE local disc GFX1/GFX2 (memoized by the body's
//   LIVE char_id @+0x001) at node+0x15C/0x160, OVERRIDING the shipped/seed bytes — so a tag-in
//   body (P1C2/P2C2/P1C3/P2C3) whose GFX was served stale from the frozen connect seed is fixed
//   exactly like a connect-active body (re_kb 25 finding:replica_live_stray_field_pin).
//
//   TAG-IN HARDENING: also PRE-FETCH the GFX of every INACTIVE-but-loaded body (valid GFX base,
//   active==0) so its art is cached BEFORE it tags in — this kills the one-frame async-fetch
//   stand that would otherwise show one garbage frame from the stale seed at the tag-in instant.
//   Returns { overlaid, pending } for diagnostics. Cheap after first load (cache hit = memcpy).
// ----------------------------------------------------------------------------
export function applyLocalGfx(ram, log) {
    let overlaid = 0, pending = 0;
    const done = new Set();                           // a char may occupy several slots — overlay once/frame
    for (const base of SLOTS) {
        const active = u8(ram, base + OFF_ACTIVE) !== 0;
        const cid = u8(ram, base + OFF_CID);
        const g1b = u32(ram, base + OFF_GFX1);
        const g2b = u32(ram, base + OFF_GFX2);
        // the node must point into guest RAM (0x0C../0x8C.. area-3) for an overlay target.
        // Applies to INACTIVE slots too: a tag-in body pre-seeds a valid GFX base before it
        // activates, so we can pre-warm its art now and overlay it the instant it tags in.
        if (!validBase(g1b)) continue;

        const hexName = 'PL' + cid.toString(16).toUpperCase().padStart(2, '0');
        let ent = _cache.get(cid);
        if (ent === undefined) {                      // first sighting (active OR pre-warm) -> fetch
            fetchChar(cid, hexName, log);
            if (active) pending++;                     // an active body with no art yet = 1 stale frame
            continue;
        }
        if (ent === 'miss')    { continue; }          // no local art — shipped GFX stands
        if (!(ent instanceof Uint8Array) && !ent.gfx1) {  // still a Promise (in-flight)
            if (active) pending++;
            continue;
        }
        // Only ACTIVE bodies get their GFX written into RAM (an inactive body isn't walked by
        // render_frame; we only needed its fetch warmed above).
        if (!active) continue;
        if (done.has(cid)) continue;                  // already overlaid this char this frame
        done.add(cid);

        // OVERRIDE: write the COMPLETE disc GFX over whatever the server shipped/seeded, at the
        // exact base the walker/decoder read from. (gfx1base & 0xFFFFFF) / (gfx2base & 0xFFFFFF).
        const o1 = g1b & RAM_MASK, o2 = g2b & RAM_MASK;
        if (o1 + ent.gfx1.length <= ram.length) ram.set(ent.gfx1, o1);
        if (o2 + ent.gfx2.length <= ram.length) ram.set(ent.gfx2, o2);
        overlaid++;
    }
    return { overlaid, pending };
}

// Test/headless hook: synchronously inject a char's GFX into the cache (skips fetch) so a
// node verifier can prove the overlay without a web server. cid = decimal char_id.
export function _injectCache(cid, gfx1, gfx2) {
    _cache.set(cid, { gfx1, gfx2 });
}
export function _cacheState() {
    const out = {};
    for (const [k, v] of _cache) out[k] = (v instanceof Promise) ? 'pending'
        : (v === 'miss' ? 'miss' : `gfx1=${v.gfx1.length} gfx2=${v.gfx2.length}`);
    return out;
}
