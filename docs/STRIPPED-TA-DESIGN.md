# Stripped-Down TA + Texture Cache Renderer

> **Status:** design. Clean-room. No copyrighted disassembly, no ROM-derived
> pixels committed. All texture bytes referenced here are produced at runtime
> from the live emulator's VRAM and never land in the repo.

## 1. The idea in one sentence

The current mirror ships the **full TA delta + every dirty VRAM page each
frame** (~1.7 MB/s, dominated by VRAM textures). MVC2 reuses a fixed set of
sprite textures, so if the client **caches each decoded texture after first
receipt and references it by id thereafter**, steady-state bandwidth collapses
to just the compact draw-list: geometry + texture-id + blend + z-order. It is
pixel-exact because we draw exactly the quads the game's TA produced — it
sidesteps every sprite-reconstruction / placement problem the ROM-asset client
fights.

### Why we believe the texture set is fixed (grounding)

This is not speculation. The production **WebGPU renderer already keys its
texture cache by `(TCW, TSP)`** and measured **0 steady-state cache misses
across 24,000 frames** (`docs/WEBGPU-RENDERER.md` §3). The renderer re-decodes a
texture only when a VRAM page overlapping it goes dirty. That is direct evidence
that within a match the live texture working set is small and stable; the only
reason we ship VRAM pages today is to *seed* and *occasionally refresh* that
cache — and we currently re-ship the page data over the wire even when the
client already holds the identical texture under a stable key.

The stripped-TA renderer formalizes that observation into the wire protocol:
**ship each unique texture once, keyed by content id; thereafter ship only the
draw-list.**

---

## 2. What exists today (read of the code)

### `core/network/maplecast_mirror.cpp` — `serverPublish()`

Per frame the server emits (delta-frame layout, `CLAUDE.md` "Wire Format"):

```
frameSize(4) frameNum(4) pvr_snapshot[16](64)
taSize(4) deltaPayloadSize(4) [TA delta runs]
checksum(4) dirtyCount(4)
[regionId(1) pageIdx(4) pageData(4096)] × N      // region 1=VRAM, 3=PVR
```

- **TA delta** (lines ~1548–1612): byte-level run diff of the raw TA command
  buffer against the previous frame's double buffer (`_taBuf[2]`), with an
  8-byte gap-merge and a `runLen` clamp to 65535. Keyframe every 60 frames.
- **Dirty pages** (lines ~1649–1667): `memcmp` of `vram[]` and `pvr_regs`
  against shadow copies, OR'd with a DMA force-dirty bitmap
  (`_vramDirtyBitmap`, `markVramDirty()`), shipped at 4 KB page granularity.
  **This is the ~1.7 MB/s cost — VRAM texture pages dominate.**
- **No texture-id caching exists.** Confirmed. The closest thing is the
  `EFCT`/`TXTR` experiment (lines ~1952–2020, gated behind `MAPLECAST_EFCT`):
  for *additive* (`DstInstr==ONE`) quads only, it hashes the texture's VRAM
  bytes, ships a decoded RGBA `TXTR` packet **once per content hash**, and a
  separate `EFCT` list of `(hash, cx, cy, w, h)`. That is a content-addressed,
  ship-once texture channel — exactly the primitive this design generalizes —
  but it covers only glow/effect quads and is not a full renderer.

### TA quad parameters (where they're parsed)

`ta_parse(ctx, true)` fills `rc.global_param_op / _pt / _tr` (opaque /
punch-through / translucent) with `PolyParam` records
(`core/hw/pvr/ta_ctx.h:33`):

```c
struct PolyParam { u32 first, count; TSP tsp; TCW tcw; PCW pcw; ISP_TSP isp; ... };
struct Vertex   { float x,y,z; u8 col[4]; u8 spc[4]; float u,v; ... };
```

- **Texture address:** `addr = (tcw & 0x1FFFFF) << 3`.
- **Pixel format:** `fmt = (tcw >> 27) & 7` (0=ARGB1555, 1=RGB565, 2=ARGB4444,
  others paletted/etc.); `twiddle = !((tcw>>26)&1)`; `vq = (tcw>>30)&1`;
  `mip = (tcw>>31)&1`.
- **Size:** `w = 8 << ((tsp>>3)&7)`, `h = 8 << (tsp & 7)`.
- **Blend:** `SrcInstr = (tsp>>29)&7`, `DstInstr = (tsp>>26)&7`; filter
  `(tsp>>12)&3`.
- **List type / z-order:** which of the three `global_param_*` arrays the poly
  came from (opaque, punch-through, translucent).
- **Palette selector** (paletted formats): `PAL_RAM_CTRL` + the palette bits in
  `tcw` / `pcw`.

The server already has working decoders for this:
`mcfx::decodeTex16(tcw, tsp, out)` (twiddle + VQ, fmt 0/1/2 → RGBA8888) and
`mcfx::texHash(addr, fmt, w, h, vq)` (FNV-1a over fmt/w/h + the raw 16-bit VRAM
region). **We reuse both verbatim.**

### `packages/renderer/src/wasm_bridge.cpp` — the client

`renderer_frame()` rebuilds a `TA_context` from the decoded TA buffer, applies
dirty VRAM/PVR pages into `vram[]` / `pvr_regs`, then runs
`renderer->Process()/Render()`. Texture decode happens inside flycast's
`TexCache` keyed off `(TCW,TSP)` reading `vram[]`. **The texture cache already
lives here** — today it is fed by a full local VRAM mirror; the stripped-TA
client feeds it from a content-id keyed GPU-texture cache instead.

---

## 3. Design

### 3.1 Texture identity (the cache key)

A texture must be uniquely identified so the client can detect when the game
**overwrites a VRAM address with different content** (the classic aliasing bug:
two different sprites occupy the same address at different times). VRAM address
alone is **not** a safe key. The key is:

```
tex_id (u64) = FNV1a( fmt | w | h | vq | scan ,  raw VRAM texel bytes ,  palette bytes )
```

- Reuse the existing `mcfx::texHash()` (FNV-1a over fmt/w/h/vq + the raw 16-bit
  VRAM region). Widen the accumulator to **64-bit** to push collision
  probability below 1e-12 for the few-thousand-entry working set (RDP/RemoteFX
  and LiveRender both use 64-bit content hashes for the same reason —
  `docs/OPTION6-MASTER-PLAN.md`).
- **Fold the palette into the hash for paletted formats** (fmt 5/6 = PAL4/PAL8).
  Same indexed pixels + different palette = different on-screen texture (this is
  literally how MVC2 skins work — see `CLAUDE.md` skin system). Without palette
  in the key, a skin change would silently keep the old colors. For non-paletted
  16-bit formats (fmt 0/1/2) the texel bytes already encode color, so palette
  contributes nothing and is skipped.

The **cache key is `tex_id` only** — a pure content hash. Address, size and
format are *carried in the texture-upload header* for decode, but they are **not
part of the key**, because the same content at a new address is the same drawn
texture and must hit the cache. (This is the LiveRender lesson: hash the
draw-call's *parameters/content*, never the surrounding state or transient
address.)

**Invalidation is implicit:** when the game writes new bytes to an address, the
next frame's quad referencing that address produces a *new* `tex_id`; the client
misses, the server ships the new texture once. The old `tex_id` ages out via
LRU. No explicit invalidation messages.

### 3.2 Server protocol (server → client)

Per frame, after `ta_parse(ctx, true)`:

1. Walk `rc.global_param_op`, `_pt`, `_tr` in that order (= z-order /
   list-type). For each `PolyParam` that is textured (`(pcw>>3)&1`):
   - Compute `tex_id` (§3.1). Skip formats the client can't decode yet
     (`mip`, unsupported fmt) — fall back to the legacy dirty-page path for
     those (§5).
   - If `tex_id` is **not** in the server's `_sentTextures` set (per-connection,
     see §5), decode it (`decodeTex16`) and emit a **`TXTR` record** into the
     texture channel; insert into `_sentTextures`.
   - Emit a **quad record** into the draw-list, referencing `tex_id`.
2. Compress and ship one `STAF` (Stripped-TA Frame) envelope.

#### Wire format — `STAF` envelope

Reuses the existing `ZCST` zstd outer envelope and the `_compressor`. Inner
layout:

```
"STAF"(4)  frameNum(4)  pvr_snapshot[16](64)
texCount(2)
  ── repeated texCount times (new textures this frame) ──
  tex_id(8)  fmt(1)  flags(1: vq|scan|paletted)  w(2)  h(2)
  rgbaLen(4)  rgba[rgbaLen]            // decoded RGBA8888, zstd handles entropy
quadCount(2)
  ── repeated quadCount times ──
  tex_id(8)            // 0 = untextured (flat/gouraud quad)
  blend(1)             // (SrcInstr<<3)|DstInstr
  flags(1)             // bit0 listtype lo, bit1 listtype hi, bit2 gouraud,
                       // bit3 textured, bit4 punch-through-alpha-test
  vcount(1)            // 3 or 4 (strip already triangulated to quads)
  ── per vertex (vcount times) ──
    x(2)  y(2)         // screen-space, fixed-point Q12.4  (640×480 → fits i16)
    z(2)               // quantized 1/w depth, monotonic per list (enough for
                       //   z-order within a list; lists are drawn in order)
    u(2)  v(2)         // UV in Q0.16  (0..1 → u16)
    argb(4)            // vertex color (col[4]); omitted when !gouraud (flag)
```

**Bytes per quad** (the steady-state cost):

| Field | Bytes |
|-------|-------|
| header (tex_id 8 + blend 1 + flags 1 + vcount 1) | 11 |
| 4 verts × (x,y,z,u,v = 10B) | 40 |
| 4 verts × argb (only if gourand) | 0–16 |
| **typical (flat-shaded sprite quad)** | **~51** |
| **worst (gouraud)** | **~67** |

zstd over the draw-list (highly repetitive: same blend/flags/tex_id runs, small
position deltas) realistically lands **~0.45×**, so **~23 B/quad on the wire**.

Note the draw-list is *already screen-space and quantized* — we deliberately do
**not** ship the raw TA command buffer. We ship the parsed, triangulated,
projected quad list. This is smaller and frees the client from running
`ta_parse` at all (a parse cost the WebGPU renderer measures at 0.19 ms/frame).

### 3.3 Client

State: a `Map<tex_id → GPUTexture>` LRU cache (cap ~2048 entries / ~64 MB).

Per `STAF` frame:
1. For each `TXTR` record: if `tex_id` not cached, `createTexture` +
   `writeTexture(rgba)`, insert into LRU. (Decode already done server-side, so
   the client just uploads — no twiddle/VQ logic needed client-side at all.)
2. For each quad: look up `GPUTexture` by `tex_id` (guaranteed present — server
   ships the `TXTR` before the quad that first references it, in the same
   frame), set blend pipeline from `blend`, set depth/list state from `flags`,
   write the quantized verts into the per-frame vertex buffer, draw.
3. Lists drawn in order op → pt → tr; within `tr`, preserve server-sent order
   (server already sorted). Vertex z is only a tiebreaker within a list.

The client never touches `vram[]`, never runs `TexCache`, never runs
`ta_parse`. It is a pure "bind cached texture, draw quad list" loop — far
simpler than today's `renderer_frame()`. This is implementable directly on the
existing **WebGPU** renderer (`web/webgpu/*`) whose texture manager is already
keyed by content; swapping the key from `(TCW,TSP)` to `tex_id` and the feed
from VRAM-pages to `TXTR`-packets is a localized change.

### 3.4 Untextured quads

Flat/gouraud quads (HUD bars, fades, shadows) carry `tex_id = 0` and just the
verts + colors. They cost the same ~51 B but never trigger a `TXTR`.

---

## 4. Bandwidth numbers

### 4.1 Per-frame draw-list (steady state)

MVC2 in-match scene composition (grounded against the WebGPU renderer's
measured per-frame parse load and typical 2D fighter draw counts):

| Component | Typical quad count |
|-----------|-------------------|
| Background layers (parallax, often 2–4 large textured quads) | 4–8 |
| 6 character sprites (body + a few sub-parts each) | 18–40 |
| Projectiles / effects (varies wildly: 0 idle, 30+ in supers) | 5–30 |
| HUD (health bars, meters, timer, portraits, combo text) | 20–40 |
| **Total textured + untextured quads/frame** | **~80–120 typical, ~180 peak (supers)** |

- **Pre-compression:** 100 quads × 51 B ≈ **5.1 KB/frame**.
- **Post-zstd (~0.45×):** ≈ **2.3 KB/frame**.
- **At 60 fps:** **~138 KB/s typical, ~250 KB/s during supers.**

Add `pvr_snapshot` (64 B) + envelope (~16 B) per frame ≈ 5 KB/s overhead.

> **Steady-state: ~140 KB/s (≈1.1 Mbps), peaks ~250 KB/s.**
> vs. today's **~1.7 MB/s** → **~12× reduction**, with **zero** image-quality
> loss (pixel-exact).

This sits between the full mirror (~1.7 MB/s) and the experimental ROM-asset
state client (~5–15 KB/s). The ROM-asset client is ~10× smaller again but is
**not** pixel-exact and fights sprite placement; stripped-TA is the
pixel-exact, low-risk middle.

### 4.2 Texture-cache warmup

Texture sizes in MVC2: character sprites are predominantly **64×64 to
128×128**, 16-bit source. As **decoded RGBA8888** on the wire:

| Texture size | RGBA bytes | zstd'd (~0.4×, sprites have flat alpha regions) |
|--------------|-----------|------------------------------------------------|
| 64×64 | 16 KB | ~6 KB |
| 128×128 | 64 KB | ~26 KB |
| 256×256 (rare, backgrounds) | 256 KB | ~100 KB |

**Per character across a full move-set:** MVC2 characters animate from a
per-character sprite pool. A representative working set actually *touched* by
the renderer over a full move-set is on the order of **150–300 unique textures**
(many small sub-part tiles + a few large body sprites). Taking ~200 textures
averaging ~96×96 (~36 KB RGBA, ~15 KB zstd'd):

- **Per character (RGBA on wire, zstd'd):** ~200 × 15 KB ≈ **~3 MB**.
- **Per character (decoded GPU residency, RGBA8888):** ~200 × 36 KB ≈ **~7 MB**.

**A 6-character match:**

- **Wire warmup (textures shipped once each):** ~6 × 3 MB ≈ **~18 MB**, plus
  shared backgrounds/HUD/projectile-common atlases ~3–5 MB → **~20–23 MB total**
  spread across the match (textures arrive lazily as moves are first used, not
  in a burst).
- **GPU cache residency:** ~6 × 7 MB + shared ≈ **~45–50 MB**, comfortably under
  a 64 MB LRU cap.

**Warmup shape:** it is *not* a cold-start cliff. The first ~3–5 s of a round
(both teams' point characters idling + walking) ship maybe ~80–120 textures
(~1.5 MB). New textures then trickle in as each *new move* is first performed —
a few KB per novel sprite, amortized over the whole match. A player who throws a
new super adds ~10–30 textures (~0.3–0.8 MB) in that one frame's `STAF`
(spreadable across a few frames if we cap `texCount`/frame).

> **Warmup: ~3 MB per character, ~20 MB per 6-character match, delivered
> lazily.** A returning client (same characters) that persists its cache to
> IndexedDB warms in **0 MB** — pure draw-list from frame 1.

### 4.3 Honest summary

| Metric | Value |
|--------|-------|
| Steady-state | **~140 KB/s** (peaks ~250 KB/s in supers) |
| Warmup, per character | **~3 MB** (lazy) |
| Warmup, 6-char match | **~20 MB** (lazy, trickled) |
| GPU cache residency | **~45 MB** (64 MB LRU cap) |
| vs. current mirror (~1.7 MB/s) | **~12× steady-state reduction** |
| Image quality | **pixel-exact** (we draw the game's own quads + textures) |

---

## 5. Reuse the mirror's dirty-page machinery, or a parallel channel?

**Recommendation: parallel texture-cache channel, NOT an extension of the
dirty-page path.** Reasons:

1. **Different keys, different lifetimes.** Dirty pages are keyed by *VRAM
   address* and are *ephemeral* (valid until the next write to that address).
   Cached textures are keyed by *content hash* and are *immortal* (good
   forever, across address reuse, across the whole session, even across
   sessions if persisted). Bolting "ship a page once, tagged" onto the dirty
   bitmap means tracking which client has which page — and a page can hold parts
   of several textures, or a texture can span pages, so page-granular shipping
   under-/over-ships at texture boundaries. The content hash is the correct
   granularity.

2. **The client gets simpler, not more complex.** The whole point is to stop
   the client maintaining a VRAM mirror + `TexCache` + `ta_parse`. Extending the
   dirty-page path keeps all of that alive. The parallel channel lets the client
   be a thin "upload RGBA by id, draw quads" loop.

3. **The primitive already exists and is proven in-tree.** The `EFCT`/`TXTR`
   experiment (`maplecast_mirror.cpp:1952`) is *exactly* a content-addressed
   ship-once texture channel with a `_sentHashes` set and a periodic re-ship
   (every 180 frames) to cover un-observable browser joins. Generalize it from
   "additive effect quads only" to "all textured quads," widen the hash to 64
   bits, and add the quad list. **Per-connection** `_sentTextures` is the one
   real change needed: the current experiment uses a process-global
   `_sentHashes` and papers over joins with a 3 s clear — fine for a handful of
   effects, wrong for a 2000-entry full cache. The relay hides joins, so either
   (a) the relay tracks per-downstream sent-sets and re-requests, or (b) the
   client sends a tiny "I have tex_ids [bloom filter]" hint on connect and the
   server primes from a fresh-set. (b) is cleanest and bounded.

4. **Coexistence / fallback.** Keep the legacy dirty-page mirror path compiled
   and selectable. Formats the texture channel can't yet decode
   (mipmapped, exotic) fall back to: ship the raw VRAM region as a `TXTR` with a
   `flags` bit saying "client-decode," reusing the WebGPU texture-manager's
   existing VQ/mip/palette decoders. This means **no quad is ever undrawable** —
   the worst case degrades to shipping that one texture's bytes once, still
   ship-once, still cached.

---

## 6. Phased build plan

Each phase has a concrete `verify:` check. The build is the per-step check
(`CLAUDE.md` code guidelines).

### Phase 0 — Instrument (no behavior change)
Add a `MAPLECAST_STAF_STATS` env-gated counter in `serverPublish()` after the
existing `ta_parse(ctx, true)`: per frame, count textured quads, untextured
quads, unique `tex_id`s, and bytes-if-quantized; log every 600 frames alongside
the existing `[MIRROR] TA DELTA` line.
- **verify:** run a real match, confirm typical quad count lands in the 80–180
  range and unique-`tex_id`/frame in steady state is ~0 (validates §1 and the
  §4.1 numbers against this build, not just priors).

### Phase 1 — Server: `STAF` emit alongside the mirror
In `maplecast_mirror.cpp`, behind `MAPLECAST_STAF=1`, after `ta_parse`:
- Widen `texHash` to u64; fold palette bytes for paletted formats.
- Build the `STAF` envelope (§3.2): walk `op/pt/tr`, quantize verts, emit
  `TXTR` for new `tex_id`s (per-connection set, Phase 1 can start process-global
  + periodic reship like the EFCT experiment), emit quad records.
- Compress with `_compressor`, `maplecast_ws::broadcastBinary`.
- Mirror still runs; `STAF` is additive so we can A/B on the wire.
- **verify:** capture `STAF` bytes for 600 frames, confirm steady-state KB/s
  matches §4.1 (~140 KB/s) and texCount→0 after warmup. Determinism rig
  (`MAPLECAST_DUMP_TA`) unaffected — `STAF` reads `rc` read-only after parse,
  never the mirror's TA double-buffer.

### Phase 2 — Client: `STAF` consumer on the WebGPU renderer
New path in `web/webgpu/*` (parallel to `renderer_frame`):
- `tex_id → GPUTexture` LRU. On `TXTR`: upload RGBA (or client-decode for the
  fallback flag). On quads: bind, set pipeline from blend/flags, draw.
- No `vram[]`, no `ta_parse`, no `TexCache`.
- **verify:** side-by-side a `STAF` client and the current mirror client on the
  same match; character select, stage select, in-match, and a super must be
  pixel-identical (the §3 fragile-scene test surface from `wasm_bridge.cpp`).

### Phase 3 — Join handling & persistence
- Replace process-global sent-set with per-connection (client sends a cache
  digest on connect; server primes `_sentTextures` and ships only missing
  `tex_id`s in the first few frames).
- Persist the client LRU to IndexedDB keyed by `tex_id` → 0-MB warmup on
  reconnect with the same characters.
- **verify:** reconnect mid-match; confirm no visual pop and near-zero texture
  re-ship; cold vs. warm client warmup MB matches §4.2.

### Phase 4 — Fallback coverage & cap tuning
- Wire the "client-decode" `TXTR` flag for mip/VQ/paletted edge formats through
  the WebGPU texture-manager's existing decoders.
- Tune LRU cap (start 2048 / 64 MB) and `texCount`/frame cap (spread super
  bursts over 2–3 frames).
- **verify:** full move-set sweep per character with no fallback misses logged;
  GPU residency stays under cap; no frame ships >1 super's worth of textures.

### Files touched
- `core/network/maplecast_mirror.cpp` — `STAF` emit (Phases 0,1,3,4); widen
  `mcfx::texHash` to u64; generalize the `_sentHashes`/`TXTR` primitive.
- `web/webgpu/*` (texture-manager + render loop) — `STAF` consumer (Phases 2–4).
- `relay/src/protocol.rs` — recognize `STAF`/`TXTR` magics for pass-through (and
  per-downstream sent-set if we do join-handling at the relay). **No decode** in
  the relay — it stays out of the hot path.
- The four-parser rule (`CLAUDE.md`) applies only to the *mirror* wire format;
  `STAF` is a new, parallel format, so it does **not** need the emulator.html /
  native-client parsers unless/until those clients adopt it.

---

## 7. Risks / honest caveats

- **Quad count variance:** supers and multi-projectile chaos can spike to ~180+
  quads → ~250 KB/s bursts. Acceptable (still <1/6 of today's bandwidth) and
  smooths under zstd. Worst realistic case (full-screen super flash with 30+
  additive layers) is bounded by the TA itself — the game never draws more than
  the hardware can, and the WebGPU renderer already handles those frames in
  1.9 ms.
- **First-use latency:** a brand-new move's textures ship in the same frame it's
  first performed (~0.3–0.8 MB one-shot). On a slow link this could delay that
  one frame; mitigate with the `texCount`/frame cap (Phase 4) spreading it, or
  pre-warming common-move textures during the round-start animation.
- **Hash collisions:** 64-bit content hash over a ~2000-entry set →
  collision probability ~1e-13. Negligible; if paranoid, carry a 32-bit
  secondary (size+addr) tiebreaker in the `TXTR` header (free, already known).
- **Paletted-format correctness:** the palette MUST be in the key, or skins/
  team-color swaps render stale. This is the one identity subtlety to get right
  and is called out in §3.1 and Phase 1.
