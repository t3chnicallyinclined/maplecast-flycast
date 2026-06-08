# Pixel-Perfect Lean Stream — Architecture Analysis & Paths Forward

Written 2026-06-08 after the projectile-anchor attempts stalled. The recurring failures (anchor drift, `sid→assembly` index, LZSS scratch buffer, coverage gaps) are symptoms of ONE wrong assumption. This doc reframes the problem and lays out the options with honest distances.

---

## The fundamental thing we've been missing

**We keep trying to RE-DERIVE the game's render OFFLINE** — rip the sprites (whole-sprite bake), rip the EXTRAS (sid→assembly), decode the LZSS — and re-implement the placement ourselves. Every wall is a symptom of that: the offline data is ambiguous/incomplete, and our re-implementation is an approximation.

**But the server runs the actual game.** It has, live in RAM/VRAM every frame:
- every **decoded texture** (VRAM, already LZSS-expanded by the SH4),
- every **exact quad** the game emits (the TA display list it builds — the mirror already reads it),
- every object's **exact position/scale/palette**.

We don't need to re-derive any of it offline. We need to **observe what the game drew and ship the minimum to reproduce it.**

---

## The irreducible truth (what the GPU actually needs)

Frame N for the GPU = a **list of quads**: `{screen pos, uv, texId, blend}`. That's the TA display list. Two facts:

1. **A quad list = a per-sprite TEMPLATE × a position.** The same sprite drawn at different places is the same relative-quad template offset by its screen pos (× scale, × flip). The display list is mostly *repeated templates*.
2. **The template is position-independent and static per `sprite_id`.** Ship it ONCE, reference it forever by `sprite_id`.

So the minimum per-frame wire is just the **object list**: `sprite_id + screen_x/y + scale + flip + palette-ref` per drawn object (~8 B each). Everything else (the templates, the textures) is **shipped once and cached.**

**This is the piece VCACHE missed.** VCACHE content-addressed the *textures* (27% of the wire) but still shipped the *geometry* (73%). The geometry is *also* dedupable — by `sprite_id`. Template-cache the geometry + VCACHE the textures = the whole wire collapses.

---

## The options (wire math + pixel-perfect? + distance)

| # | Architecture | Per-frame wire | Pixel-perfect? | Distance from today |
|---|---|---|---|---|
| 1 | **TA mirror** — ship the full display list | **36–88 Mbps** | ✅ yes | 0 (works, too heavy) — the baseline |
| 2 | **Whole-sprite reconstruct** (current) — ship state, redraw from ripped atlases | ~0.05 Mbps | ❌ no (anchor drift, coverage, approximation) | shipped, but has a hard ceiling |
| 3 | **Display-list template cache** — server observes the game's emitted quads, dedups by `sprite_id`, ships templates+textures once + object-refs per frame | **~0.1 Mbps** | ✅ **yes** (it's the game's actual quads) | **moderate — recommended** |
| 4 | **Client-side flycast** — ship inputs, client runs the SH4+render locally | **~0.001 Mbps** (inputs) | ✅ yes (SH4 is byte-deterministic, proven) | needs ROM client-side + WASM emulator |

Per-frame data, least → most: **inputs (4) < state/template-ref (3≈2) ≪ display list (1)**. Option 3 is the leanest *browser-friendly* pixel-perfect; option 4 is the absolute leanest but ROM-gated.

---

## ⭐ RECOMMENDED: Option 3 — Display-List Template Cache

**Idea:** the server already builds the TA display list every frame (the mirror reads it). Instead of shipping the whole thing, **extract per-`sprite_id` quad templates, dedup, and ship each once; per frame ship only the object references.** The client replays cached templates at the given positions.

**Why it dissolves every wall we hit:**
- **Anchor drift → gone.** The template's relative quads = the game's emitted absolute quads *minus the object's own screen pos*. That IS the exact anchor, derived from ground truth. No `+0x178`, no EXTRAS rip, no bake-relative guess.
- **`sid→assembly` index → gone.** We never decode the assembly. We observe the *output* the game already assembled, keyed by the `sprite_id` it drew.
- **LZSS scratch buffer → gone.** The server ships *decoded* textures (content-addressed, VCACHE-style). The client never decodes.
- **Coverage gaps → gone.** Templates fill the cache as sprites appear on screen. Nothing is "missing" — it streams in once, then it's free.
- **Effects-by-type → mostly gone.** Hitsparks/supers are also `sprite_id`-templated; the chaotic remainder (truly unique per-frame geometry) is the only thing we ever stream raw — the deliberate hybrid, and it's small.

**Data budget (steady state, caches warm):** per drawn object ≈ `sid(2) + x/y(4) + scale(1) + flip/pal(1)` ≈ 8 B × ~20 objects ≈ 160 B/frame ≈ **~0.08–0.12 Mbps**. One-time: ~8 MB VRAM textures + the templates, amortized over the match. **~400–800× leaner than the mirror, and pixel-identical.**

**Build plan (builds on the EXISTING mirror, not from scratch):**
1. **Server template extractor.** In the TA display-list capture (where the mirror reads it — `maplecast_mirror.cpp`, the TA staging pages 813–839 per `re-catalog`), group emitted quads by the object/`sprite_id` that produced them (the slot-table walk already enumerates objects → their quads). For each, store quads **relative to the object's `screen_x/y`** → a position-independent template keyed by `sprite_id` (+ a flip/scale-normalized form).
2. **Template dedup + ship-once.** Hash each template; ship a new one once with its `sprite_id` key; on repeat, the per-frame record is just the reference. (Same pattern as VCACHE pages.)
3. **Texture cache.** Reuse VCACHE (content-addressed VRAM pages) for the templates' textures — ship once, client reconstructs byte-identical.
4. **Per-frame object list.** Replace the heavy display list with `[{sid, x, y, scale, flip, palRef}]` per drawn object (the slot-table output we already read in `readAllDrawn`).
5. **Client replay.** For each object: fetch cached template by `sid`, apply `x/y/scale/flip`, bind cached texture, apply palette LUT (#2 already done) → emit quads → render via the existing `PVR2Renderer`/sprite-gpu. No render-code port, no asset RE.
6. **Hybrid escape hatch.** Any object whose per-frame quads DON'T match its cached template (truly dynamic geometry) → stream its quads raw that frame. Measure how often this fires; it should be the small chaotic tail.

**Risk/unknowns:** (a) how cleanly the display-list capture attributes quads to objects (the slot-table walk should give it); (b) templates that vary per frame beyond scale/flip (animated UVs, vertex-color ramps) — those fall to the hybrid path; (c) the one-time template/texture burst at scene start (amortizes fast). None are RE walls — they're integration + measurement.

---

## Alternative: Option 4 — Client-Side flycast (BYO-ROM)

**Idea:** SH4 execution is **byte-deterministic across machines/OSes** (empirically validated — see memory `reference_determinism_validated`). So the client can run the same flycast (WASM) from the same inputs and get the byte-identical frame. Ship **only inputs** (~2 B/player/frame).

**Pros:** the absolute leanest (~0.001 Mbps), trivially pixel-perfect, no render RE at all.
**Cons / why it's not the default:**
- **ROM client-side.** Browsers can't be shipped the ROM (DMCA). Viable only as **bring-your-own-ROM** (user loads their own dump) — fine for a native/competitive client, not a public spectator page.
- **Compute.** A full SH4 emulator in the browser is heavy (the headless is ~12% CPU on a server core; WASM in a tab is rougher). Rollback/resync adds complexity.
- The GGPO-style rollback path was **shelved** for the central-server topology (memory `project_rollback_shelved`), but for a **replica/competitive client that re-sims from inputs**, this is exactly the model.

**Where it fits:** the native **competitive client** (`docs/COMPETITIVE-CLIENT.md`) — user has the ROM, wants frame-perfect + lowest bandwidth. Not the browser spectator path.

---

## Honest distances & recommendation

- **Option 2 (today):** ~90% visually, *never* pixel-perfect — the ripped-asset ceiling. Good enough for "watchable," not for "perfect."
- **Option 3 (template cache):** pixel-perfect + ~0.1 Mbps, **reachable from where we are** because it *reuses* the mirror's display-list capture + VCACHE + the palette LUT we already built. The work is extraction/dedup/replay integration, not RE. **This is the path.**
- **Option 4 (client flycast):** leanest + perfect, but a different product (native, BYO-ROM). Park for the competitive client.

**Recommended sequence:**
1. **Prove the template hypothesis cheaply:** instrument the server to log, for one match, how many *unique* per-`sprite_id` quad-templates appear and how often per-frame quads match a cached template (the dedup ratio). If templates dedup hard (expected), Option 3's budget holds.
2. If confirmed, build the **server template extractor + ship-once protocol** on top of the existing mirror/VCACHE, and a **client replay** path alongside the current whole-sprite client (A/B).
3. Keep the whole-sprite client as the fallback/coverage net while Option 3 fills in.

The "fundamental miss" in one line: **stop re-deriving the render offline; let the server — which runs the real game — hand the client the exact quads and textures, deduped, and ship only positions per frame.**

---

## The Frame Oracle — the diagnostic that BECOMES Option 3

The single best next move, and the bridge from "diagnose" to "ship": **a frame-by-frame differ between the game's ground-truth TA output and our reconstruction.** This is the instrument to build first. It pays off twice — it tells us exactly what we're missing, and its output *is* the template cache.

**What it does, per frame:**
1. **Server — capture the truth, attributed.** The SH4 builds the TA display list every frame (the mirror already reads it). Add a **PC-hook at the per-object render routine** (`loc_8c0308c2` slot walk → `loc_8c03093c` per-object draw, `bank03`): on each hit, read the object (`slot, sprite_id, screen_x/y, scale, flip`) and mark the current TA-buffer write cursor. The quads written between consecutive hits **belong to that object** → exact `sprite_id → quad-list` attribution. (This is literally "snapshot the SH4 as it builds the next frame, tagged.")
2. **Server — emit two aligned streams per frame:** (a) the **state** (the object list we already ship), and (b) the **ground-truth quads per object** (positions relative to the object's `screen_x/y` = the template).
3. **Client — capture our reconstruction's draw list** for the same frame.
4. **Differ — align by frame + object and report:** quads in the truth that we don't draw (MISSING — coverage/effect gaps), quads we draw at the wrong place (ANCHOR/scale error, with the exact pixel delta), quads we draw that the game doesn't (EXTRA). Overlay it visually (truth in green, ours in red) for instant read.

**Both payoffs from one instrument:**
- **Diagnostic:** every remaining defect (the projectile anchor, the missing `0x29c–0x2a4` sprites, effect routing) shows up as a precise per-object delta against ground truth — no more eyeballing or guessing. Combined with reading the RAM state + the `marvelous2` disasm at each hit, we get the full causal chain: *quad → object → RAM field → disasm routine → why.* That's "traverse the ROM/memory to fully understand," closed-loop.
- **The product:** the attributed `sprite_id → relative-quad template` is **exactly Option 3's template.** Cache it (ship once per `sid`), pair with VCACHE textures, ship the object list per frame → pixel-perfect at ~0.1 Mbps. The differ's "MISSING" set is the cache-fill worklist; its "ANCHOR delta" is zero by construction (templates are the game's own output).

**Why this beats every prior attempt:** we never re-derive anything. We observe the SH4 building the frame, tag what each object produced, and either (diagnostic mode) compare it to ours or (ship mode) cache + replay it. The anchor/`sid→assembly`/LZSS/coverage walls all vanish because they were all "re-derive the asset offline" problems, and this reads the asset live from the running game.

**First build step:** the server PC-hook + per-frame attributed-quad dump (to a debug capture or a side packet), plus a client draw-list dump, plus an offline aligner that prints the per-object deltas. Start with ONE character's projectile (Ryu hadouken `0x18c…` / Storm typhoon) to validate the attribution, then widen.
