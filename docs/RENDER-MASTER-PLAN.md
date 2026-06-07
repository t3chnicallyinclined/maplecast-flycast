# RENDER MASTER PLAN — Pixel-Perfect Off-SH4 Rendering at Low Bandwidth

> **Goal:** render MapleCast's MVC2 stream **pixel-perfect with the SH4 turned
> off** at **~20–140 KB/s**, with **no guessing**. Every claim below is marked
> **[CONFIRMED]** (verified against code / disassembly / disc bytes this session)
> or **[INFERRED]** (reasoned, not yet byte-verified). No fabricated `loc_8c…`.
>
> **Author:** mvc2-sh4-re-expert. **Date:** 2026-06-07. **Status:** master plan.
> **Scope:** READ-ONLY research deliverable. No code edited.

## The one confirmed insight this plan is built on

The TA stream renders **100% perfectly** today via
`web/webgpu/ta-parser.mjs → web/webgpu/pvr2-renderer.mjs` (`PVR2Renderer`). This
is the out-of-match video + `replay.html` path. **[CONFIRMED** —
`project_render_pipeline_state.md`: "the reference that WORKS perfectly … flycast's
parse + render, done right, client-side. Use this; don't reinvent it."]**

Therefore **all correct rendering data already exists in the TA stream.** The job
is not to invent rendering — it is to pick, per visual class, the **cheapest
source that is still bit-identical to what `PVR2Renderer` would draw**:

- **STATIC art** (characters, stage, HUD chrome) → reconstruct from a few GSTA
  state bytes + textures we already have. Ship ~0 per frame.
- **DYNAMIC art** (hitsparks, supers, auras, beams) → stream the real TA geometry
  (filtered to effect texIds) through `PVR2Renderer`. The only thing that must
  cross the wire each frame.
- **Textures** → content-address every texture the TA references; ship each
  **once** (TX64), cache forever; pre-rip the static ones from the disc so they
  never stream at all.

---

## 1. TA stream anatomy — what's in each frame, and the STATIC/DYNAMIC split

### 1.1 What the TA stream is **[CONFIRMED]**

The TA (Tile Accelerator) stream is the three PVR display lists the SH4 submits
per frame. `ta_parse(ctx, false)` produces, per list
(`core/network/maplecast_mirror.cpp:2553–2554`, `rc.global_param_op/_pt/_tr`):

- `rc.verts[]` — `Vertex{ x,y,z (screen-space, already projected), col[4] (RGBA),
  spc[4], u,v }` (`docs/STRIPPED-TA-DESIGN.md` §2; `ta_vtx.cpp` Red=0…Alpha=3,
  cited `maplecast_mirror.cpp:2689`).
- `rc.idx[]` — a triangle **strip** with degenerate links; `rc.idx[k]` indexes
  `rc.verts`. `makeIndex(primRestart=false)` rewrites each `PolyParam.first/.count`
  to span `rc.idx` (`maplecast_mirror.cpp:2547–2552`).
- `PolyParam{ first, count, tsp, tcw, pcw, isp, … }` — raw PVR render state per
  poly (`core/hw/pvr/ta_ctx.h:33`, cited `docs/STRIPPED-TA-DESIGN.md` §2).

**The three lists ARE the z-order** drawn op → pt → tr; the client draws straight
through with no depth buffer (`maplecast_mirror.cpp:2587–2588`, 2697–2699).
**[CONFIRMED]**

Per-poly state decoders (all **[CONFIRMED]**, `docs/STRIPPED-TA-DESIGN.md` §2.2):
- texture present: `(pcw>>3)&1` (`maplecast_mirror.cpp:2606`).
- texture addr: `(tcw & 0x1FFFFF) << 3`; fmt `(tcw>>27)&7` (0=ARGB1555,1=RGB565,
  2=ARGB4444, 5=PAL4, 6=PAL8); twiddle `!((tcw>>26)&1)`; vq `(tcw>>30)&1`; mip
  `(tcw>>31)&1`.
- size: `w = 8<<((tsp>>3)&7)`, `h = 8<<(tsp&7)` (`maplecast_mirror.cpp:2612`).
- blend: `SrcInstr=(tsp>>29)&7`, `DstInstr=(tsp>>26)&7`; **additive = `DstInstr==1`
  (ONE)** (`maplecast_mirror.cpp:2452`, 2598, 2647).

### 1.2 Which marvelous2 routine draws each class **[CONFIRMED addresses]**

Every render object — body, cape, projectile, hitspark, HUD — flows through the
**same** slot-table walker and quad emitter; they differ only by their GFX source
pointer and which VRAM poly bank feeds them.

| Class | marvelous2 routine | VRAM poly bank | Cite |
|---|---|---|---|
| Slot-table walk (the per-frame draw list) | `loc_8c0308c2` "Render_sprites" | — | `bank03.asm:1199–1200` **[CONFIRMED]** |
| Main sprite setup (world→screen, writes screen_x/y to +0xE0/+0xE4) | `loc_8c03093c` | — | `bank03.asm:1281`, called at `:1230` **[CONFIRMED]** |
| **THE quad emitter** (8-byte EXTRAS → 16-byte quad) | `loc_8c033e90` / `…e9e` | — | `bank03.asm:9258`, reached via `bra` at `:9240` **[CONFIRMED]** |
| **Stage / background draw** | `loc_8c027e32` (reads stage byte, `&0x1F`, table-indexes) | **`0x0cea0000` Stage Poly** | `bank02.asm:19127` (`;0cea0000`), region named `work.asm:38` **[CONFIRMED]** |
| **Effect Poly setup** (shared hitsparks/supers) | `loc_8c032c20` → `bank12.loc_8c129668` | **`0x0ced0000` Effect Poly** | `bank03.asm:6528` (`#data 0x0ced0000`), region named `work.asm:39` **[CONFIRMED]** |
| Cell processor / quad-vertex loop | `loc_8c1294c8` / `loc_8c129cc2` | — | agent file (prior RE) **[CONFIRMED-prior]** |

VRAM region map **[CONFIRMED** `marvelous2/memory/work.asm:36–39]**:
```
0ce60000  Texture_Decompress_Buffer   (LZSS staging)
0ce80000  DM00 Poly                   (demo/intro)
0cea0000  Stage Poly                  ← stage geometry+textures
0ced0000  Effect Poly                 ← shared hitspark/super/aura graphics
```

**Key RE fact [CONFIRMED** `reference_mvc2_effects_bank.md`]:** there is **no
separate effect sprite_id namespace.** A render object resolves its texture from
its **GFX base pointer at owner+0x15C (Dat_GFX1)** indexed by `part_idx`, not by
`sprite_id`. So you **cannot** classify a quad as "effect" by a sprite_id range.
The correct discriminator is its **texId** (which encodes the VRAM source) or its
**GFX-pointer region** (does owner+0x15C/0x160 point into `0x0ced0000`?).

### 1.3 Per-quad classification table — STATIC vs DYNAMIC + the runtime discriminator

| Quad class | List | Source | Class | Runtime discriminator (no guessing) |
|---|---|---|---|---|
| Stage / parallax background | **op** | Stage Poly `0x0cea0000` | **STATIC** | texId ∈ stage-texId-set (built once from `STGxxTEX`); OR poly's tex VRAM addr ∈ stage bank. Reconstruct from `stage_id`+`stage_anim_timer`. |
| Character bodies / sub-parts | **pt** (punch-through alpha) | PLxx_DAT via owner+0x15C | **STATIC** | Already reconstructed: GSTA `sprite_id` → `atlas/chars/PLxx.json`. The pt list is dominated by chars. |
| HUD: life bars, meters, timer digits, hit-counter, portraits | mixed (op/pt/tr) | `FONT.BIN` / `SELTEX.BIN` | **STATIC** | texId ∈ HUD-texId-set (built once from `FONT.BIN`/`SELTEX.BIN`). Reconstruct from GSTA `health/red_health/meter_fill/combo/timer`. |
| Hitsparks / supers / auras / beams | **tr** (translucent, usually additive `DstInstr==1`) | Effect Poly `0x0ced0000` | **DYNAMIC** | texId ∈ effect-texId-set (built once from `EFKYTEX`/`0x0ced0000` capture). **STREAM these.** |
| Fades / flat shadows / dim layers | op/tr | untextured | DYNAMIC-cheap | `texId==0` (untextured); cheap, stream with effects. |

> **The cardinal discriminator rule [CONFIRMED** agent file "Client rendering
> architecture"]:** identify HUD/effect quads by a **texId set (content-addressed),
> NEVER by screen-strip position or "additive" blend.** Position strips miss
> non-additive HUD bars; the additive heuristic grabs additive STAGE grid geometry
> (this is the live "garble flash on supers" bug — stage additive grid fragments
> leak through the HUDF additive filter, `project_render_pipeline_state.md`). The
> current `MAPLECAST_HUDF` filter (`maplecast_mirror.cpp:2597–2605`) is exactly
> this heuristic (additive OR top/bottom screen strip) and is **known wrong**;
> §4.2 replaces it with the texId set.

---

## 2. Texture cache — "rip + store on the fly" + disc cross-ref

### 2.1 Content-addressing (the cache key) **[CONFIRMED, in-tree]**

`mcfx::texHash64(tcw, w, h)` — 64-bit FNV-1a over `fmt|w|h|vq` + the raw
indexed/texel VRAM bytes **and** (for paletted fmt 5/6) the live selected palette
window (`maplecast_mirror.cpp:1696`, design `docs/STRIPPED-TA-DESIGN.md` §3.1,
§8.1). Properties:
- Same content at a **new VRAM address ⇒ same id ⇒ cache hit** (address is NOT in
  the key — the LiveRender lesson).
- Skin / team-color swap ⇒ palette folded in ⇒ **different id** (correct: MVC2
  skins are palette swaps, `CLAUDE.md` skin system). **[CONFIRMED]**
- Invalidation is implicit: new bytes → new id → server ships once → old id LRU-ages.

`mcfx::decodeTexAny(tcw, tsp, out)` decodes to RGBA8888 through flycast's **own**
canonical path (fmt 0/1/2+VQ via `decodeTex16`; fmt 5/6 de-twiddle + `PALETTE_RAM`
lookup with `PAL_RAM_CTRL` + `tcw.PalSelect` exactly as `TexCache.cpp:463–472`).
**No format guessing.** (`maplecast_mirror.cpp:1716`, §8.1.) **[CONFIRMED]**

### 2.2 Ship-once channel **[CONFIRMED, in-tree]**

`TX64` packet (`maplecast_mirror.cpp:2620–2626`, design §8.2): **[CONFIRMED]**
```
'TX64'(4)  texId(8 LE)  w(2 LE)  h(2 LE)  rawSize(4 LE)  zstd(RGBA8888)
```
Server holds `_stafSent` (set of shipped texIds); a texId is decoded+shipped only
on first sight (`maplecast_mirror.cpp:2614–2616`). Client caches by
`texKey(lo,hi)="hi:lo"` in `_stafTex` (`sprite-client.mjs:217, 228–231`). The
EFCT/TXTR experiment (`:2426`) is the same primitive for additive-only quads.
**[CONFIRMED]**

> **Known gap [CONFIRMED]:** `_stafSent` is **process-global** and cleared every
> 600 frames to re-seed relay-hidden browser joins (`maplecast_mirror.cpp:2558–2560`).
> That re-ships the whole working set every 10 s. §5 fixes this with a
> per-connection digest (design §3.2 / Phase 3).

### 2.3 Mapping a cached texId → a DISC asset (pre-rip, never stream) **[INFERRED — build step]**

The win: any texId whose bytes also live on the disc can be **pre-ripped** and
loaded client-side, so it **never crosses the wire even once.** Method (no
guessing — pure content match):

1. **Offline disc decode.** Each `STGxxTEX.BIN` / `EFKYTEX.BIN` / `FONT.BIN` /
   `SELTEX.BIN` is a PVR texture pack. Decode each contained texture to RGBA8888
   with the **same** `decodeTexAny` logic (twiddle/VQ/fmt/palette) the live path
   uses, so the bytes are bit-identical to what VRAM holds at runtime.
2. **Hash with the same `texHash64`.** For each disc texture, compute the 64-bit
   content id over the same `fmt|w|h|vq` + texel(+palette) layout.
3. **Cross-ref table** `disc_tex_catalog.json`: `{ texId → (file, byteOffset,
   w, h, fmt) }`. Build it once.
4. **Runtime:** when the server is about to ship a `TX64`, if `texId ∈ catalog`,
   **skip the upload** and instead emit a tiny `TXID` "use disc asset N" record
   (8-byte id only). Client loads that texture from its pre-bundled disc rip.

> Caveat **[INFERRED]:** runtime VRAM textures for paletted formats fold the *live*
> palette into the hash, while the disc stores the *base* indexed texture + a
> separate palette. So the cross-ref must hash the disc texture **per-palette** (or
> key the catalog on the indexed bytes only and carry the palette separately). For
> stage/HUD/effect art the palette is mostly fixed, so this is tractable; verify
> by hashing a live capture against the disc decode for one stage first (§6 step 2
> verify). Characters already bypass this entirely (atlas path).

---

## 3. Lean reconstruction from state (the STATIC art, ~0 stream cost)

### 3.1 Characters — **DONE [CONFIRMED]**

GSTA `sprite_id` (char+0x144) → `atlas/chars/PLxx.json` rect `{x,y,w,h,dx,dy}` →
textured quad at GSTA `screen_x/y` (char+0xE0/0xE4), `facing` (char+0x110),
`palette_id`. (`sprite-client.mjs`, `sprite-gpu.mjs`; agent file "Characters →
reconstructed".) GSTA wire carries every field needed
(`maplecast_gamestate.h:12–32`). **~5–15 KB/s, pixel-exact, keep it.**
The quad-emit logic reimplemented is `loc_8c033e90` (sprite_id → atlas rect +
dx/dy → quad) **[CONFIRMED address]**.

### 3.2 Stage — **rippable, not yet done [CONFIRMED source, INFERRED placement]**

- **Driver state (already on the wire):** `stage_id` (GSTA `GameState.stage_id`,
  `maplecast_gamestate.h:35`; RAM `STG_ID 0x8c26A95C` **[CONFIRMED** work.asm:27]**,
  enum `stg_AirS1…0x10` = **17 stages** `work.asm:53–69` matching the **17
  `STGxxTEX/POL`** disc files **[CONFIRMED** via ls]**) + `stage_anim_timer`
  (GSTA `stage_anim_timer`, RAM `0x8C1F9D80`, `maplecast_gamestate.cpp:72`,
  shipped in WIRE_SIZE `maplecast_gamestate.h:262`). **[CONFIRMED]**
- **Assets:** `STGxxTEX.BIN` (~1.0–1.7 MB each = the textures) + `STGxxPOL.BIN`
  (~116–194 KB = the polygon/geometry list), `MVC2 Dev Files/`. **[CONFIRMED** ls]**
- **Render routine to mirror:** `loc_8c027e32` (bank02) consumes the stage byte,
  masks `&0x1F`, table-indexes into Stage Poly `0x0cea0000`
  (`bank02.asm:19127`). **[CONFIRMED]** Port = parse `STGxxPOL` into PVR polys,
  bind `STGxxTEX` textures, animate by `stage_anim_timer`, feed `PVR2Renderer` as
  the **op** list (drawn first = behind everything). Client-side, **never streamed**.
- **Why this matters now [CONFIRMED** `project_render_pipeline_state.md`]:** stage
  is currently **black**; the super "garble flash" is stage additive-grid fragments
  floating on black. Ripping the stage fixes both.

### 3.3 HUD — **rippable, partly faked [CONFIRMED source, INFERRED routine/placement]**

- **Driver state (already on the wire):** per-char `health` (char+0x420),
  `red_health` (char+0x424); `p1/p2_meter_fill`, `p1/p2_meter_level`, `p1/p2_combo`,
  `game_timer` (`maplecast_gamestate.h:16–17, 36–44`). **[CONFIRMED** matches
  `CLAUDE.md` memory map 0x420/0x424/0x646/0x670]**.
- **Assets:** `FONT.BIN` (73 KB — timer digits / hit-counter / "HIT" text /
  meter-level glyphs) and `SELTEX.BIN` (1.59 MB — select screen art incl character
  **portraits**). **[CONFIRMED** ls]**. The life-bar/meter "fill" is the real
  **WHITE texture drawn at width = `health/max`, MODULATED by green/yellow vertex
  color** (agent file; the "white bar" bug is shipping a quad without per-vertex
  col + ShadInstr). **[CONFIRMED-prior]**
- **Render routine [INFERRED]:** the HUD draw routine is **NOT symbol-labeled** in
  `work.asm`/`pl_mem.asm` (searched: no `life/gauge/meter/vital` symbols). It runs
  through the same slot-table walker `loc_8c0308c2` / emitter `loc_8c033e90`
  **[CONFIRMED]** but I have **not** located a dedicated `loc_8c…` HUD label and
  will **not fabricate one**. **The grounded path that does NOT need the routine:**
  build the HUD-texId-set empirically from the live `TX64` stream (§4.2) and place
  bars/digits from GSTA values. Locating the exact draw routine + screen rects is a
  *refinement* (a follow-up RE task: trace which slot-table category byte (R+0x03)
  the HUD nodes use, near `bank03` slot walk), not a blocker.
- **Current state [CONFIRMED]:** `sprite-client.mjs:878 drawHudReal` / `:941–951
  _bar` draws bars with **hardcoded CSS colors at hardcoded screen rects** on
  Canvas2D — a placeholder. Replace with `FONT.BIN`-ripped digit/glyph textures +
  the real WHITE-bar-modulated-by-vertex-color via `PVR2Renderer`.

---

## 4. Stripped TA — stream ONLY the dynamic effects

### 4.1 What streams **[CONFIRMED need, INFERRED final filter]**

Only **DYNAMIC** geometry: hitsparks, supers, auras, beams — input/RNG-driven
multi-strip additive geometry that cannot be rebuilt from a few state bytes (agent
file "bandwidth ladder"). Ship the **real TA triangles** (the STAF channel) filtered
to the effect-texId set, rendered by **`PVR2Renderer`** (the proven path).

### 4.2 The effect discriminator — a texId set, built once **[INFERRED — calibration step]**

Replace the heuristic HUDF filter (`maplecast_mirror.cpp:2597–2605`, additive OR
screen-strip — **known to grab stage grid + miss non-additive HUD**,
`project_render_pipeline_state.md`) with a **content-addressed set**:

1. Capture the live `texHash64` of every quad whose **GFX source is the Effect
   Poly region `0x0ced0000`** (test owner+0x15C/0x160 ∈ `[0x0ced0000,0x0cf00000)`
   — `reference_mvc2_effects_bank.md` step 2; region **[CONFIRMED** work.asm:39]**),
   OR whose texId matches a disc `EFKYTEX.BIN` decode.
2. That set = `effect-texId-set`. The complement that draws to HUD screen regions
   from `FONT.BIN`/`SELTEX.BIN` = `hud-texId-set`. Both built **once**,
   empirically, from the stream — NOT from blend/position.
3. STAF emit keeps a quad iff `texId ∈ effect-texId-set` (∪ `texId==0` untextured
   effect layers). HUD comes from §3.3 reconstruction; stage from §3.2; chars from
   §3.1. **Nothing else streams.**

### 4.3 The strip-format fix (in flight, assume it lands) **[CONFIRMED gap]**

The STAF wire currently ships flycast geometry **per-triangle** (one record per
real triangle, `maplecast_mirror.cpp:2632–2694`), and the client feeds
`PVR2Renderer` 1-triangle "strips" — so PVR2Renderer's strip→list winding fix
(`_buildIndexBuffer`, GPU zero-area drop) **never engages** and complex supers
garble (`project_render_pipeline_state.md`). **Fix (being built in parallel):**
ship the real **strip** — `rc.verts` span + `PolyParam{first,count,tsp,tcw,pcw,isp}`
— and let `PVR2Renderer` triangulate. This plan **assumes that lands**; the effect
channel then = "filtered strips → PVR2Renderer," pixel-exact by construction. The
STAF→PVR2Renderer reshaping already exists (`sprite-client.mjs:257 onSTAF`,
28-byte/vertex buffer + per-poly `{first,count,tsp,tcw,pcw,isp}`, `tcw` overridden
to a surrogate that resolves to the TX64-cached `GPUTexture`). **[CONFIRMED]**

---

## 5. End-state architecture, bandwidth budget, build order

### 5.1 Architecture diagram

```
            VPS (headless flycast, SH4 RUNNING, authoritative)
            ┌──────────────────────────────────────────────┐
            │ ta_parse(ctx,false) → rc.verts/idx/PolyParam  │
            │                                               │
            │ readAllDrawn() (slot table 0x8C2895E0)        │  GSTA  ~5–15 KB/s
            │   → sprite_id/screen_x/y/facing/palette       │ ───────────────┐
            │   → health/red/meter/combo/timer/stage_id     │                │
            │   → stage_anim_timer                           │                │
            │                                               │                │
            │ effect filter (texId ∈ effect-set,            │  STAF  ~3–10 KB/s
            │   GFX∈0x0ced0000) → strips → PVR2Renderer fmt  │ ──(effects)───┤
            │                                               │                │
            │ texHash64 → TX64 (ship-once);                 │  TX64  (warmup;│
            │   if texId∈disc_catalog → TXID (no bytes)     │   ~0 steady)──┤
            └──────────────────────────────────────────────┘                │
                                                                            ▼
            CLIENT (browser, SH4 OFF)                                  relay /ws
            ┌───────────────────────────────────────────────────────────────┐
            │ Pre-bundled disc rips (loaded once, from MVC2 Dev Files):       │
            │   STGxxTEX/POL ×17 · EFKYTEX/POL · FONT.BIN · SELTEX.BIN        │
            │   atlas/chars/PLxx ×56                                          │
            │                                                                │
            │ Compose (back→front):                                          │
            │  1. STAGE   ← stage_id + stage_anim_timer → STGxxPOL/TEX → PVR2 │
            │  2. CHARS   ← sprite_id → atlas/chars → sprite-gpu              │
            │  3. EFFECTS ← STAF strips (+TX64/disc tex) → PVR2Renderer       │
            │  4. HUD     ← health/meter/combo/timer → FONT/SELTEX bars+digits│
            └───────────────────────────────────────────────────────────────┘
```

### 5.2 Per-component bandwidth budget

| Component | Source | Steady-state | Cite |
|---|---|---|---|
| GSTA state (chars + globals) | reconstructed | **~5–15 KB/s** | agent file "lean sprite ~5–15 KB/s" **[CONFIRMED-measured]** |
| STAF effects (filtered strips) | streamed | **~3–10 KB/s** | a few KB/s, agent "bandwidth ladder" **[INFERRED — needs §6 Phase-0 measure]** |
| TX64 textures (effects only; chars/stage/HUD pre-ripped) | ship-once | **~0 steady** (warmup only) | design §4.2 ship-once **[CONFIRMED-mechanism]** |
| HUD + stage | reconstructed/ripped | **~0** (client-side) | §3.2/§3.3 **[CONFIRMED-source]** |
| envelope + pvr_snapshot | per frame | ~5 KB/s | design §4.1 **[CONFIRMED]** |
| **TOTAL steady-state** | | **~20–30 KB/s typ; ~140 KB/s peak (supers)** | sums to the target |

Reference ladder **[CONFIRMED-measured** agent file]:** full TA mirror ~1.7 MB/s ·
STAF texture-cache (everything streamed) ~140 KB/s · lean reconstruction ~5–15 KB/s.
**This plan = lean reconstruction for static + filtered STAF for effects = lands at
the bottom of the ladder (~20 KB/s typ), with super peaks bounded at ~140 KB/s.**

### 5.3 Prioritized build order (dependency-ordered)

1. **Effect texId set + strip STAF (the only thing that must stream).**
   *Depends on:* the parallel strip-format fix (§4.3) landing.
   *Do:* build the effect-texId-set by tagging quads whose GFX ∈ `0x0ced0000`
   (`reference_mvc2_effects_bank.md`), replace the HUDF additive/strip heuristic
   (`maplecast_mirror.cpp:2597`) with that set, ship strips not per-triangle.
   *Verify:* a super renders crisply via `PVR2Renderer` over a black bg; no stage
   grid leaks. **This unblocks everything else by removing the garble.**

2. **Stage rip (kills the black background + garble-on-black).**
   *Depends on:* nothing (assets + `stage_id`/`stage_anim_timer` already present).
   *Do:* decode `STGxxPOL/TEX` (mirror `loc_8c027e32` / `0x0cea0000`), render as the
   op layer from `stage_id`+`stage_anim_timer` via `PVR2Renderer`.
   *Verify:* each of 17 stages matches the live TA video; animated stages tick.

3. **Texture cache disc cross-ref (drops warmup + effect TX64 to ~0).**
   *Depends on:* step 1 (need the effect-texId-set to know which textures to
   pre-rip) + a confirmed `decodeTexAny`-identical disc decoder.
   *Do:* build `disc_tex_catalog.json` (§2.3), add the `TXID` "use disc asset" path
   so catalogued textures never ship bytes.
   *Verify:* hash one live stage/effect capture vs the disc decode → identical id;
   confirm TX64 bytes/match drop to ~0 after warmup.

4. **HUD rip (replaces the Canvas2D placeholder; finishes the static set).**
   *Depends on:* step 3 (FONT/SELTEX in the catalog) + the HUD-texId-set.
   *Do:* rip `FONT.BIN` digits/glyphs + `SELTEX.BIN` portraits; draw life bars as
   the WHITE texture modulated by vertex color, digits/combo/timer from GSTA, via
   `PVR2Renderer` (retire `drawHudReal`/`_bar` hardcoded CSS).
   *Verify:* health/meter/combo/timer/portraits match the live HUD pixel-for-pixel.
   *(Refinement, non-blocking: locate the exact HUD draw `loc_8c…` to confirm screen
   rects rather than calibrate them from the stream.)*

5. **Join handling + persistence (steady-state correctness at scale).**
   *Depends on:* steps 1–4 (the channels exist).
   *Do:* per-connection sent-set via a client cache digest on connect (replace the
   600-frame global clear, `maplecast_mirror.cpp:2558`); persist client LRU +
   disc rips so a reconnect warms in **0 MB** (design §3.2 / Phase 3).
   *Verify:* mid-match reconnect → no visual pop, near-zero re-ship.

---

## 6. Open RE items (honest — what is INFERRED and must be confirmed)

- **HUD draw routine `loc_8c…`** — not located (no `life/gauge/meter` symbol in
  `work.asm`/`pl_mem.asm`). Confirm by tracing the slot-table category byte
  (R+0x03) for HUD nodes near `bank03` Render_sprites `loc_8c0308c2`. The texId-set
  path (§4.2) does not need it; screen-rect *exactness* does.
- **Disc paletted-texture cross-ref** — runtime hash folds the live palette; disc
  stores base index + palette. Verify the per-palette hashing scheme on one stage
  before generalizing (§2.3 caveat).
- **STAF effect bandwidth** — the ~3–10 KB/s figure is inferred; run the Phase-0
  `MAPLECAST_STAF_STATS`/`STAFMEASURE` counter (`maplecast_mirror.cpp:2717`,
  in-tree) filtered to the effect set to confirm.
- **EFKYTEX vs `0x0ced0000`** — confirm the disc `EFKYTEX.BIN` decode hashes equal
  to the live Effect-Poly captures (so effects can be pre-ripped too, not just
  streamed-once).

## Sources cited
- **marvelous2** (local): `build/bank02.asm:19127`, `build/bank03.asm:1199–1200,
  1230, 1281, 6528, 9240–9258`; `memory/work.asm:27, 36–39, 53–69`;
  `memory/pl_mem.asm` (no HUD symbol — negative result).
- **anotak** (local `refs/anotak/`; web `https://zachd.com/mvc2/data/anotak/`):
  Hitspark categories → effect-type byte; attack DamageType/Hitspark fields.
- **DEV FILES** (`MVC2 Dev Files/`): `STG00TEX.BIN…STG10TEX.BIN` (×17) + `STGxxPOL`,
  `EFKYTEX.BIN`/`EFKYPOL.BIN`, `FONT.BIN`, `SELTEX.BIN`, `DMxxTEX/POL`.
- **Live code:** `core/network/maplecast_mirror.cpp:1696, 1716, 2426, 2452,
  2540–2710`; `core/network/maplecast_gamestate.{h:12–62,262 / cpp:72, 624–652}`;
  `core/hw/pvr/ta_ctx.h:33`; `web/webgpu/{ta-parser,pvr2-renderer,sprite-client}.mjs`;
  `docs/STRIPPED-TA-DESIGN.md` §2–4, §8; `.claude/agents/mvc2-sh4-re-expert.md`.
