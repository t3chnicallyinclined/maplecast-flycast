# RENDER DECISION — Stop Rebuilding Renderers; Build From `pvr2-renderer`

> **The question the user asked, answered straight:** *"Why are we rebuilding
> renderers we already have? Is the STAF re-serializer double work?"*
>
> **Verdict (one line):** **YES — the STAF re-serializer's surrogate-texture
> machinery is redundant, and there is a cheaper, lower-risk path
> (`MAPLECAST_VCACHE`) that is already 80% built and leaves both the proven
> renderer AND the proven mirror decode untouched.** Ship that. Delete StafGL,
> demote STAF to an effects-only fallback, keep `sprite-gpu` (it is a different,
> good technique).
>
> **Author:** mvc2-sh4-re-expert. **Date:** 2026-06-07. **Scope:** decision +
> scope + build order. READ-ONLY except this file.
>
> Each claim is **[CONFIRMED]** (verified against in-tree code this session,
> `file:line`) or **[INFERRED]** (reasoned; verification step named).

---

## 0. THE TWO FACTS THAT DECIDE EVERYTHING

1. **The proven renderer (`pvr2-renderer.mjs`, fed by `ta-parser.mjs`) renders
   the raw mirror frame pixel-perfect today, decoding textures straight from a
   client-side VRAM mirror** — `PVR2Renderer` calls `texMgr.getTexture(tsp, tcw,
   vram)` (`pvr2-renderer.mjs:325, 408`), and the production `texture-manager.mjs`
   decodes the texture from `vram` by TCW address with a full twiddle/VQ/paletted
   decoder + dirty-page invalidation (`texture-manager.mjs:106-178`). This is the
   out-of-match video + `replay.html` path. **[CONFIRMED]** It needs **no** STAF,
   **no** surrogate, **no** TX64.

2. **The one and only reason the STAF path needs a `tcw` SURROGATE + a `TX64`
   texture channel + a `texMgr` shim is that the STAF wire deliberately threw VRAM
   away.** STAF re-parses + re-serializes the frame on the SERVER into a geometry
   strip with **no VRAM region** (`maplecast_mirror.cpp:2582-2715`), so the client
   has nothing to decode from and must instead be fed textures out-of-band (TX64),
   keyed by a per-frame integer surrogate that stands in for the 64-bit content
   hash (`sprite-client.mjs:323-336`; shim `webgpu-test.html:392-409`).

> **Therefore the surrogate machinery is redundant *as a way to feed
> pvr2-renderer*** — it exists only to compensate for the server re-serialize.
> If you never throw VRAM away, `texMgr.getTexture(tsp,tcw,vram)` already returns
> the right texture and there is nothing to surrogate. The cheap win is not "ship
> textures a better way," it is **"don't restructure the frame at all — just stop
> re-sending VRAM bytes the client already holds."**

---

## 1. IS THE STAF RE-SERIALIZER REDUNDANT WORK? — **YES** [CONFIRMED]

**The surrogate-texture machinery is necessary ONLY because we re-serialized.**
Trace it:

| Step | Code | Why it exists |
|---|---|---|
| Server re-parses the frame (`ta_parse(ctx,false)`) and writes a **VRAM-free** strip wire | `maplecast_mirror.cpp:2553, 2582-2715` | STAF is "a parallel channel, NOT an extension of the dirty-page path" — zero VRAM by construction (`STRIPPED-TA-DESIGN.md §5`) |
| Server content-hashes each texture and ships its decoded RGBA once as `TX64` | `maplecast_mirror.cpp:2651-2664` | client has no VRAM to decode from, so textures must arrive out-of-band |
| Client assigns a per-frame **surrogate int** and **overrides `pp.tcw = surr`** | `sprite-client.mjs:327-336` | the real 64-bit `texId` doesn't fit a TCW field, and pvr2-renderer keys textures by `tcw`; so a small int stands in |
| `texMgr` **shim** resolves `surr → texKey → TX64 RGBA`, ignoring the `vram` arg | `webgpu-test.html:392-409` | replaces the real VRAM decoder because there is no VRAM |

Contrast the **mirror** path's `getTexture` (`texture-manager.mjs:106`): same
`PVR2Renderer.renderFrame` call, same `getTexture(tsp,tcw,vram)` signature
(`pvr2-renderer.mjs:325`), but `tcw` is the **real** TCW and the texture is
decoded from `vram`. **No surrogate. No TX64. No shim.** [CONFIRMED]

**Conclusion (CONFIRMED):** feeding `pvr2-renderer` does **not** require the
server re-parse/re-serialize/surrogate. The surrogate→texture mismap that keeps
painting HUD textures on characters is a bug that *only exists in a layer we added
to work around a problem we created*. The mirror path has no such layer and no
such class of bug. **The STAF re-serializer is redundant work for the full-frame
goal.** (It is *not* redundant for one narrow job — see Option C / effects, §3.)

---

## 2. OPTION A — CONTENT-ADDRESSED VRAM (the "build from what works" path)

**Premise:** keep the working TA stream + VRAM mirror + `frame-decoder.mjs` +
`texture-manager.mjs` + `pvr2-renderer.mjs` **exactly as today**. Reduce the wire
by **content-addressing the VRAM dirty pages**: hash each 4 KB page; ship its
4096 bytes only when that content is new; ship a tiny reference when it's a repeat.
The client fills references from a `hash→page` cache, reconstructs a **standard**
delta frame, and feeds the **unmodified** renderer.

### 2.1 THE BIG FINDING: this is already built server-side — `MAPLECAST_VCACHE`

This is not a new design. It ships in-tree, env-gated, **off by default**:

- **Design block** `maplecast_mirror.cpp:1566-1597` — verbatim the prompt's
  Option A: FNV-1a page hash (`vcacheHashPage` :1589-1594), per-stream "already
  sent" set (`_vcacheSent`), novel page ships data once + is recorded, repeat page
  ships "a compact reference (NO 4096 bytes)". **[CONFIRMED]**
- **Emit** `maplecast_mirror.cpp:2045-2099` — when `_vcacheOn`, the dirty-page
  count is preceded by sentinel `0xFFFFFFFF`; each page is
  `regionId(1) pageIdx(4) hash(8) hasData(1) [data(4096) if hasData]`. A repeat
  page emits `hasData=0` and **13 bytes instead of 4101** (:2082-2090). **[CONFIRMED]**
- **Accounting** `:2106-2114` — already logs `[VCACHE] ... saved N KB this frame,
  M MB total | sent-set=K`. **[CONFIRMED]**

**What's MISSING is the client half.** `frame-decoder.mjs:applyFrame` reads the
page count as a plain u32 (`frame-decoder.mjs:125`) and has **no** sentinel peek,
**no** `hash→page` cache, **no** reference-fill. Pointing the current WebGPU client
at a VCACHE stream → it reads `dirtyPages = 0xFFFFFFFF` and loops catastrophically.
**[CONFIRMED — `frame-decoder.mjs` has no VCACHE code; only the unrelated TA-delta
sentinel at :114]**

> **So Option A is ~80% done: the server emit + hash + sent-set + accounting all
> exist; the only build is a ~40-line client decoder that reconstructs a standard
> delta frame before handing it to the untouched `applyFrame`.**

### 2.2 Server change (mostly DONE)

- **Already built:** the entire emit (`:2045-2099`), `vcacheHashPage` (:1589),
  `_vcacheSent` + 600-frame reseed (:2037-2040). **[CONFIRMED]**
- **One honest gap:** the reseed `_vcacheSent.clear()` every 600 frames
  (`:2039`) re-ships the whole working set every 10 s for relay-hidden joins —
  same flaw as STAF's 600-clear. Replace with a connect-time client digest later
  (§5). For a first measurement, leave it. **[CONFIRMED gap]**

### 2.3 Client change (the only real build — ~40 lines, localized)

In `frame-decoder.mjs`, **before** the dirty-page loop (`:125`):

1. Peek the page-count field; if it's `0xFFFFFFFF`, read the real count from the
   next u32 and enter VCACHE mode. [INFERRED — matches server wire `:2046-2048`]
2. Maintain `this._pageCache = Map<hashStr, Uint8Array(4096)>`.
3. Per page: read `regionId(1) pageIdx(4) hash(8) hasData(1)`. If `hasData`, read
   the 4096 bytes, **store in `_pageCache[hash]`**, and apply to `this.vram` /
   `this.pvrRegs` exactly as today (`:132-139`). If not, **look up `_pageCache[hash]`**
   and apply those bytes the same way.
4. Everything downstream is byte-identical to a standard delta frame:
   `texture-manager.getTexture(tsp,tcw,vram)` decodes from the now-correct `vram`,
   `pvr2-renderer` renders. **No surrogate, no TX64, no shim.** [CONFIRMED downstream is untouched]

**Native + emulator.html clients:** VCACHE is gated by env and only affects the
mirror wire's dirty-page section. If those clients ever consume a VCACHE stream
they need the same ~40-line decode (four-parser rule). For the WebGPU client we
target, it's one file. [CONFIRMED — `CLAUDE.md` four-parser rule applies to the
*mirror* wire; VCACHE modifies the mirror wire, so the rule applies *if* other
clients adopt it]

### 2.4 Dedup / warmup behavior

- **Implicit invalidation, like STAF:** a page's identity is its content hash, so
  when the game DMAs new texels into an existing VRAM page, the new content hashes
  differently → ships once → the client's existing `texture-manager` dirty-page
  invalidation (`:92-104, 116-123`) re-decodes that page's textures correctly.
  **The aliasing problem is handled by the SAME mechanism STAF uses, one level
  lower (page vs texture).** [CONFIRMED mechanism]
- **Warmup:** first visit to a screen ships its pages once; revisits (menus, the
  same stage, repeated sprites at the same VRAM slot) ship 13-byte refs.

### 2.5 HONEST bandwidth — does MVC2's VRAM churn really repeat enough?

This is the make-or-break question, and the honest answer is **"strong for the
static/repeating set, weak for the per-frame animation set — net win is real but
NOT to ~0."**

- **Evidence the working set is small + stable [CONFIRMED-prior]:** the production
  renderer keys its texture cache by `(addr,fmt,…)` and measured **0 steady-state
  cache misses across 24,000 frames** (`STRIPPED-TA-DESIGN.md §1.1`, citing
  `WEBGPU-RENDERER.md §3`). That means *within a match the live texture working set
  is small and rarely changes* — i.e. the same VRAM pages hold the same content
  frame to frame. **This is exactly the condition VCACHE exploits.** [CONFIRMED-prior]
- **But the mirror's ~1.7 MB/s is dominated by VRAM dirty pages** that re-ship
  even when content is unchanged (`STRIPPED-TA-DESIGN.md §2 line 54`,
  `.claude/agents/mvc2-sh4-re-expert.md` bandwidth ladder). A texture cache with 0
  misses but a wire that still re-ships pages means **the bytes are mostly
  redundant re-sends** — precisely what content-addressing kills. [CONFIRMED-prior]
- **Why it does NOT trend to ~0 (the honest caveat):** MVC2 DMAs *new* sprite
  parts into VRAM staging every time a character advances an animation frame
  (`loc_8c03552a` LZSS decode → staging `0x0CE60000`; the engine recomputes
  GFX pointers per frame — agent file "render data path"). Those pages have
  **genuinely new content** each animation step, so they ship full the first time
  each unique sprite appears. Over a match the *unique* sprite set is bounded
  (~150–300 textures/char, `STRIPPED-TA-DESIGN.md §4.2`), so VCACHE trends toward
  "ship each unique page once," but **active animation keeps a non-zero novel-page
  rate** (new moves, new supers). [INFERRED from the decode/VRAM reality + the
  §4.2 working-set sizes]
- **The numbers we actually have:** there is **no committed VCACHE measurement
  log** in-tree (the `[VCACHE]` printf exists but no captured run). **This is the
  single most important thing to measure before committing** (§4 step 1). The
  STAF probe (`MAPLECAST_STAFMEASURE`, `maplecast_mirror.cpp:2562-2565`) gives the
  STAF comparison; VCACHE's own `[VCACHE] saved … total` line
  (`:2111-2114`) gives Option A's. **[CONFIRMED both probes exist; CONFIRMED no
  captured data]**

**Honest estimate [INFERRED]:** VCACHE removes the redundant re-send of the stable
working set (stage, HUD, idle sprites, repeated menu art) — likely a **large
fraction** of the 1.7 MB/s, plausibly landing **mid-hundreds of KB/s steady with
spikes on new-move first-use**, i.e. in the **same order as STAF's ~140–400 KB/s**
but with a different shape (page-granular novelty vs texture-granular). It will
**not** reach the ~15 KB/s lean-sprite tier — that requires throwing away pixels
for reconstruction (Option D). **Measure before believing any specific number.**

### 2.6 Why re-evaluate the prior preference for STAF-per-texture

The prior plan (`RENDER-TIER1-PLAN.md §4`) rejected Option 2 (content-addressed
VRAM) in favor of STAF 1(b) on two grounds. Re-examined against *what actually
happened this session*:

| Prior argument for STAF over VCACHE | Re-evaluation |
|---|---|
| "Page dedupe is wrong granularity — a page holds parts of several textures, a texture spans pages" (`TIER1 §4 Opt 2`) | **True but it doesn't cause incorrectness** — the client reconstructs exact VRAM, so the unchanged `texture-manager` decodes pixel-perfect regardless of how textures straddle pages. It's a *bandwidth* tightness argument (texture-granular is tighter), not a correctness one. And texture-granular STAF **keeps breaking** (surrogate mismap), so its theoretical tightness is unrealized. [CONFIRMED reasoning] |
| "STAF deletes the 8 MB VRAM mirror + twiddle/VQ decoder + dirty-page invalidation" (`TIER1 §4`) | **This 'simplification' is exactly what cost us.** Deleting the proven decoder forced building TX64 + surrogate + shim — *more* moving parts, and the ones that break. VCACHE **keeps** the proven decoder (a feature, not a liability — it already has 0 steady misses over 24k frames). [CONFIRMED — the deleted machinery is the working machinery] |
| "STAF is lower bandwidth" | **Unproven on this build, and STAF's own as-built strip is ~250–400 KB/s pre-quantization** (`TIER1-PLAN §7, §9 risk 1`). VCACHE vs STAF steady-state is an **open measurement**, not a settled fact. [CONFIRMED STAF is not yet at 140; INFERRED the comparison is open] |

**The decisive shift:** STAF's advantage was *theoretical bandwidth tightness*;
its realized cost is *a fragile re-serialize layer that breaks the proven
renderer's contract*. VCACHE's advantage is *it cannot break the renderer or the
decoder because it touches neither* — it only changes which bytes cross the wire,
and the client reconstructs a byte-identical standard frame. For a project whose
cardinal lesson is "never hand-roll flycast's renderer" (`agent file §"Client
rendering architecture"`), **the path that touches neither the renderer nor the
decoder is the correct default.**

---

## 3. ALL VIABLE OPTIONS

| Opt | What | Steady BW | Complexity | Risk | Touches proven renderer? | Reuses | Throws away |
|---|---|---|---|---|---|---|---|
| **A. Content-addressed VRAM (`VCACHE`)** | hash 4 KB pages, ship novel once + 13-B refs; client rebuilds exact VRAM → unchanged decoder+renderer | **~mid-hundreds KB/s [INFERRED, measure]** | **Low** (server DONE; ~40-line client decoder) | **Lowest** — renderer + decoder + `frame-decoder` downstream all untouched | **No** (and not the decoder either) | mirror wire, `frame-decoder`, `texture-manager`, `pvr2-renderer`, server emit (`:2045-2099`), `vcacheHashPage` | nothing |
| **A+. A + command filtering** | A, plus drop TA commands for off-screen / never-referenced regions before diffing | A minus a bit | Medium (must not desync the TA-delta double-buffer) | Medium — TA-delta is determinism-load-bearing (`CLAUDE.md` 6 bugs) | No | all of A | nothing |
| **B. STAF full-frame** (`RENDER-TIER1-PLAN`) | server re-parses → VRAM-free strip + TX64 + surrogate; `pvr2-renderer` via shim | ~140 KB/s *after* per-vertex quantization (else ~250–400) | **High** — re-serialize + surrogate + relay handling + quantization still to build | **High (realized)** — surrogate→texture mismap paints HUD on chars; breaks repeatedly | renderer object-shape contract, `decodeTexAny`, `texHash64` | re-serialize, TX64, surrogate, shim | the proven VRAM decoder + dirty-page invalidation (rebuilt as TX64+shim) |
| **C. STAF effects-only** (`HUDF` / RENDER-MASTER-PLAN-V2 §2.4) | STAF filtered to **effects** (GFX∈`0x0ced0000`), composited over A | +~3–10 KB/s | Medium (the effect-texId-set filter, V2 §2.4) | Medium — small surface, real triangles | renderer, the STAF emit | — | (keeps STAF but only for the one visual that genuinely needs streaming) |
| **D. Sprite reconstruction** (`sprite-gpu.mjs`) | GSTA `sprite_id`→`atlas/chars/PLxx.json` rect → quad; SH4 off | ~5–15 KB/s | Built; ongoing per-visual RE | Medium — not pixel-exact for effects/HUD; placement RE | `sprite-gpu.mjs`, atlases, GSTA wire | — | pixel-exactness for non-char visuals (by design) |
| **E. Stage/HUD disc-rip** (V2 §2.2/2.3) | client-side from disc assets, state-driven | **~0** (client-side) | High (Ninja POL decode, HUD bank0f RE) | Medium — calibration | `pvr2-renderer`, `stage-client.mjs`, disc assets | — | nothing (additive to A or D) |

**Anything else?** A "diff the *parsed* command list instead of raw TA bytes" idea
collapses into B (re-serialize). A "ship VRAM as content-addressed *textures* but
keep the raw TA stream" is B without the geometry re-serialize — but the client
still needs the surrogate because the raw TA references textures by TCW/VRAM it no
longer has, so it's strictly worse than A (more parts, same problem). **A is the
floor of the "don't break the renderer" family.**

---

## 4. THE VERDICT

**Primary architecture: Option A (`MAPLECAST_VCACHE`) for the full frame +
Option C (STAF effects-only) for dynamic effects, both rendered by the UNTOUCHED
`pvr2-renderer.mjs` from the UNTOUCHED `texture-manager.mjs` VRAM decoder. Keep
Option D (`sprite-gpu`) as the independent lean-character layer. Pursue Option E
(disc-rip stage/HUD) later for the ~0-KB/s tail.**

**Why A over B, opinionated and final:**
- B's entire value proposition was bandwidth tightness, and **it has cost more
  than it saved**: a re-serialize layer that breaks the renderer's contract
  (surrogate mismap), a relay it confuses, a quantization step still unbuilt, and
  a ~250–400 KB/s as-built reality that isn't even at its own 140 KB/s target yet
  (`TIER1-PLAN §7, §9`).
- A **cannot** produce the surrogate-mismap class of bug, because there is no
  surrogate — the client reconstructs real VRAM and the **proven** decoder runs.
- A is **80% already built** (`:1566-2114`); the remaining work is a small,
  localized, self-contained client decoder, not a new renderer.
- A leaves the determinism-critical mirror wire's TA-delta untouched (it only
  changes the dirty-page section's encoding), so the six regression bugs and the
  determinism rig are not at risk.

**The chosen wire change:** the `VCACHE` dirty-page encoding (sentinel
`0xFFFFFFFF` + real count, then per page `regionId(1) pageIdx(4) hash(8)
hasData(1) [data(4096)?]`) — **already on the wire** (`:2046-2090`). The build is
the client decode of it.

### KEEP
- **`pvr2-renderer.mjs`** — the proven renderer. Untouched. Both A and C feed it. **[KEEP]**
- **`ta-parser.mjs`** — the reference parser shape; mirror path uses it. **[KEEP]**
- **`frame-decoder.mjs` + `texture-manager.mjs`** — the proven VRAM decode path.
  A *adds* a ~40-line VCACHE branch to `frame-decoder`; the decoder is untouched. **[KEEP]**
- **The `MAPLECAST_VCACHE` server emit** (`:1566-1597, 2037-2114`). **[KEEP — finish it]**
- **`sprite-gpu.mjs` + the character atlases** — Option D, the genuinely different
  and *correct* lean-character technique (~15 KB/s, CONFIRMED good). It is **not**
  redundant with A/C; it's a separate, cheaper layer for the one visual class that
  reconstructs cleanly. **[KEEP]**
- **STAF emit, but DEMOTED to effects-only (Option C / `HUDF`)** — the de-indexed
  strip + `decodeTexAny` + `texHash64` are correct; the *full-frame* surrogate use
  is what breaks. Confine STAF to effects (small, real-triangle, genuinely needs
  streaming), composited over A. **[KEEP (scoped down)]**

### DELETE / RETIRE
- **`sprite-staf-gl.mjs` (StafGL)** — already retired in use
  (`webgpu-test.html:269-270`); the hand-rolled WebGL knockoff of `pvr2-renderer`.
  **Remove the file** (it "stays on disk for reference" — that reference is this
  doc + the retired-note; the file is dead weight and an attractive nuisance). **[DELETE]**
- **The full-frame STAF surrogate/TX64/shim *as the full-frame strategy*** — stop
  promoting STAF to full-frame (`RENDER-TIER1-PLAN` Tier-1 goal). The surrogate
  (`sprite-client.mjs:327-336`) + shim (`webgpu-test.html:392-409`) + the
  full-frame BG-poly port (`:2718-2769`) are kept **only** for the effects overlay
  (Option C), where the texId set is small and stable. **Do not invest further in
  full-frame STAF.** **[DELETE the full-frame ambition; keep the effects subset]**

### Build-order note on STAF
You do **not** have to rip STAF out today — it's env-gated and additive. The
decision is to **stop spending effort making full-frame STAF work** and redirect
that effort to (1) measuring VCACHE and (2) building its client decoder. STAF
survives as the effects channel.

---

## 5. BUILD ORDER (dependency-ordered, opinionated)

1. **MEASURE VCACHE first (no new code).** Run the headless server with
   `MAPLECAST_VCACHE=1` on a real match; read the `[VCACHE] … saved … total |
   sent-set` line (`maplecast_mirror.cpp:2111-2114`). Capture steady-state KB/s
   and the novel-vs-ref page split. **Gate the whole plan on this number.** If the
   saved fraction is large (expected), proceed; if not, the static set isn't
   repeating and Option E (disc-rip) jumps the queue. **[CONFIRMED probe exists;
   this closes the §2.5 open question.]**

2. **Build the VCACHE client decoder** (~40 lines, `frame-decoder.mjs`,
   §2.3). Peek the `0xFFFFFFFF` sentinel; maintain `hash→page` cache; fill refs;
   reconstruct a standard delta frame; hand to the **unchanged** downstream.
   **Verify:** A/B a VCACHE client vs the current mirror client on the same match —
   **pixel-identical** (the proven decoder runs in both), with the VCACHE wire
   measurably smaller. **This is the load-bearing build step.**

3. **Replace the 600-frame VCACHE reseed with a connect-time digest** (`:2039`).
   Client sends the set of page hashes it holds on connect; server seeds
   `_vcacheSent` and ships only novel pages. Persist `_pageCache` to IndexedDB for
   0-MB warmup on reconnect. **Verify:** mid-match reconnect → near-zero re-ship,
   no visual pop. **[INFERRED — mirrors STAF Phase 3]**

4. **Demote STAF to effects-only (Option C).** Wire the effect-texId-set filter
   (GFX∈`0x0ced0000`, RENDER-MASTER-PLAN-V2 §2.4) so STAF ships only effects;
   composite the STAF overlay over the VCACHE full frame. **Verify:** a super
   renders crisply over the real VCACHE frame; no surrogate mismap (the texId set
   is tiny and stable, so the failure mode that plagued full-frame STAF is gone).
   **Delete `sprite-staf-gl.mjs`.**

5. **(Tail) Option E disc-rip** for stage/HUD to push the static set to ~0 KB/s
   (RENDER-MASTER-PLAN-V2 §2.2/2.3). Additive to A; the ladder's last rung.

---

## 6. CONFIRMED vs INFERRED LEDGER

**CONFIRMED (this session, against in-tree code):**
- Surrogate machinery exists only to feed pvr2-renderer without VRAM:
  `sprite-client.mjs:327-336`, shim `webgpu-test.html:392-409`, vs the mirror's
  `getTexture(tsp,tcw,vram)` `texture-manager.mjs:106` / `pvr2-renderer.mjs:325`.
- STAF re-serializes a VRAM-free strip: `maplecast_mirror.cpp:2553, 2582-2715`.
- `texHash64` is content-addressed (address excluded, palette folded for fmt 5/6):
  `maplecast_mirror.cpp:1696-1712`.
- **Option A is built server-side as `MAPLECAST_VCACHE`:** design `:1566-1597`,
  emit `:2045-2099`, hash `:1589-1594`, sent-set + 600-reseed `:2037-2040`,
  accounting `:2106-2114`.
- **Option A has no client decoder:** `frame-decoder.mjs:125` reads page count as
  a plain u32; no sentinel/cache/ref-fill anywhere in the file.
- Proven mirror decode path is untouched by A: `frame-decoder.mjs:89-145`,
  `texture-manager.mjs:106-178, 92-104`.
- StafGL retired in use: `webgpu-test.html:269-270`. `sprite-gpu` is the active
  lean-char renderer: `webgpu-test.html:268, 430`.
- 0 steady-state texture-cache misses over 24,000 frames (prior measurement):
  `STRIPPED-TA-DESIGN.md §1.1` ← `WEBGPU-RENDERER.md §3`.
- STAF as-built strip is ~250–400 KB/s pre-quantization, not yet at its 140 target:
  `RENDER-TIER1-PLAN.md §7, §9 risk 1`.

**INFERRED (reasoned; verification named):**
- VCACHE steady-state lands ~mid-hundreds KB/s, not ~0 (MVC2 animates new VRAM
  per move) — **verify by build step 1** (the `[VCACHE]` log). This is the one
  number that gates the plan.
- The ~40-line VCACHE client decoder is sufficient and correct — verify by build
  step 2's pixel-identical A/B.
- Connect-time digest + IndexedDB give 0-MB warmup — verify by step 3.

---

## 7. SOURCES (file:line)
- **Surrogate / STAF redundancy:** `web/webgpu/sprite-client.mjs:218, 229-233,
  264-345` (onSTAF, surrogate, onTX64); `web/webgpu-test.html:384-423` (shim +
  renderStaf); `web/webgpu/pvr2-renderer.mjs:325, 408` (`getTexture(tsp,tcw,vram)`).
- **STAF server re-serialize:** `core/network/maplecast_mirror.cpp:2540-2769`
  (emit, BG poly, 600-clear at :2567); `:1696-1712` (texHash64); `:1716-1730`
  (decodeTexAny).
- **Option A / VCACHE (built):** `core/network/maplecast_mirror.cpp:1566-1597`
  (design + vcacheHashPage); `:2037-2040` (sent-set/reseed); `:2045-2099` (emit);
  `:2106-2114` (accounting).
- **Proven mirror decode (untouched by A):** `web/webgpu/frame-decoder.mjs:89-145`
  (applyFrame; page count `:125`); `web/webgpu/texture-manager.mjs:106-178`
  (VRAM decode), `:92-104` (dirty-page invalidation).
- **Renderers:** `web/webgpu/pvr2-renderer.mjs` (proven); `web/webgpu/sprite-gpu.mjs`
  (lean chars, kept); `web/webgpu/sprite-staf-gl.mjs` (StafGL, delete).
- **Prior plans re-evaluated:** `docs/RENDER-TIER1-PLAN.md` (§4 Opt-2 rejection,
  §7/§9 STAF bandwidth); `docs/RENDER-MASTER-PLAN-V2.md` (§2.4 effects filter,
  §2.2/2.3 disc-rip); `docs/STRIPPED-TA-DESIGN.md` (§1.1 0-miss, §4.2 working set,
  §5 parallel-channel rationale).
- **Grounding:** `.claude/agents/mvc2-sh4-re-expert.md` (render data path, VRAM
  churn via `loc_8c03552a`, bandwidth ladder); `docs/WEBGPU-RENDERER.md §3`.
