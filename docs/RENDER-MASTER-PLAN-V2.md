# RENDER MASTER PLAN V2 — Pixel-Perfect Off-SH4 MVC2 at ~20–140 KB/s (No Guessing)

> **Goal:** render the full MVC2 frame (stage + chars + HUD + effects) with the
> **SH4 turned OFF** on the client, **pixel-perfect**, at **~20–30 KB/s typical /
> ~140 KB/s super-peak**. Every visual class is sourced from the **cheapest bit-
> identical producer** — never from a server-side geometry re-map.
>
> **Author:** mvc2-sh4-re-expert (synthesis of T1–T6 + supersedes RENDER-MASTER-PLAN.md).
> **Date:** 2026-06-07. **Status:** master build plan. **Scope:** research +
> build spec; no code edited by this document.
>
> Every claim is **[CONFIRMED]** (verified against in-tree code / marvelous2
> disasm / disc bytes this session) or **[INFERRED]** (reasoned, not yet byte-
> verified — flagged with the exact verification step). No fabricated `loc_8c…`.

---

## 0. THE CARDINAL CONSTRAINT (frames every decision below)

The TA stream renders **100% pixel-perfect today** via
`web/webgpu/ta-parser.mjs → web/webgpu/pvr2-renderer.mjs` (`PVR2Renderer`) — this
is the out-of-match video + `replay.html` path. **[CONFIRMED]**

`PVR2Renderer` is validated against **exactly one producer shape**: a degenerate-
linked **triangle STRIP** in a 28-byte/vertex buffer (col/spc as **R,G,B,A bytes**),
`PolyParam{first,count,isp,tsp,tcw,pcw,tileclip}` spanning that strip, with **raw
PVR words** — and `_buildIndexBuffer` does the strip→list winding-correct
triangulation, `shaders.mjs` does the ShadInstr modulate. **[CONFIRMED** T1].

**THEREFORE — the invariant every asset generator (HUD/stage/effects) MUST obey:**

> Emit the **PVR2Renderer parsed object directly** (VBL-order strip verts + raw PVR
> words + surrogate `tcw` → texMgr shim) and call
> `PVR2Renderer.renderFrame(parsed, texMgrShim, pvrSnap, null, {singlePass:true,
> noSort:true, transparentClear:true})`.
>
> **NEVER** hand a server a different geometry layout to re-triangulate. A server-
> side C++ `ta_parse` re-map GARBLES because flycast's C++ `Vertex.col` channel
> order + strip walk + `pp.first/count` (which index `rc.idx`, not `rc.verts`)
> differ from this JS parser. **[CONFIRMED** — the retired `StafGL` produced
> stretched triangles + white HUD bars; mirror.cpp:2458-2459 documents the
> `rc.verts` mis-read]. The two proven garble causes were (a) server per-triangle
> re-emit and (b) the HUDF additive/screen-strip heuristic. Both are eliminated below.

The server's ONLY geometry job is to **de-index `rc.idx`→contiguous `rc.verts`**
(`ta_parse(ctx,false)`, mirror.cpp:2553, 2664-2688) and ship the strip **verbatim**;
ALL triangulation/winding/modulate happens in JS. This already ships in-tree
(STAF). **[CONFIRMED]**

### The PVR2Renderer input contract (the single target shape — T1, all CONFIRMED)

`parsed = { vertexData:Uint8Array (28B/vert), vertexCount, opaque:PP[],
punchThrough:PP[], translucent:PP[] }`. The five fields are the **whole** contract
(`pvr2-renderer.mjs:188`).

**28-byte vertex (VBL the GPU reads, `pvr2-renderer.mjs:41-46`), little-endian:**
```
off 0  f32 x        off 4  f32 y        off 8  f32 z   (= real 1/w depth)
off 12 u8 R,G,B,A    (col,  loc1 unorm8x4)   ← bytes in R,G,B,A order, NOT packed int, NOT BGRA
off 16 u8 R,G,B,A    (spc,  loc2 unorm8x4)
off 20 f32 u        off 24 f32 v   (uv, loc3)
```
Verts in **TRIANGLE-STRIP order** (e.g. quad = TL,BL,TR,BR); `_buildIndexBuffer`
expands `(count-2)` tris with `i&1` winding-swap. `count<3` ⇒ poly skipped. **[CONFIRMED]**

**`PolyParam{ first, count, isp, tsp, tcw, pcw, tileclip }`** — `first/count` are
**strip vertex indices** (NOT triangles). Ship **raw PVR words**; the renderer
derives everything itself (T1 §5, all `pvr2-renderer.mjs`):
- blend src/dst `(tsp>>29)&7` / `(tsp>>26)&7`; ShadInstr `(tsp>>6)&3` (1=modulate);
  useAlpha `(tsp>>20)&1`; textured `(pcw>>3)&1`; gouraud `(pcw>>1)&1`; offset
  `(pcw>>2)&1`; depthMode `(isp>>29)&7`; cull `((isp>>27)&3)^1`; zwrite `!((isp>>26)&1)`.
- **paraType MUST be 4:** `pcw=(4<<29)|(real&0x1FFFFFFF)` (`sprite-client.mjs:333`).
- `tcw` = **surrogate int** key (see texMgr shim); `tileclip:0` (no scissor) unless
  a real HUD element clips.

**Coord space:** `x,y` in **640×480 screen space**, pre-NDC. `_ndcMat` reads
`pvrSnap[0]`: `tx=g&0x3F, ty=(g>>16)&0x3F, w=(tx+1)*32, h=(ty+1)*32`. For MVC2 full
screen **`pvrSnap[0]` must encode tx=19,ty=14 → 640×480** (an all-zero snapshot
gives 32×32 — WRONG). The other 15 u32 are carried but unused by the current
renderer; zero them. **[CONFIRMED** T1].

**texMgr surrogate-binding protocol** (how a ripped `GPUTexture` binds, identical
to STAF, `webgpu-test.html:384-411`): give each ripped texture a stable int key,
set `tcw=key` on its polys, store `key→GPUTexture`, supply
`getTexture(tsp,key)→{texture, sampler-from-tsp}` (clamp-to-edge default for MVC2
sprites) + `getFallbackTexture()=1×1 white`. **No VRAM, no PVR decode, no ta_parse
needed** — a ripped disc texture binds byte-identically to a live one. **[CONFIRMED]**

**Composition rule:** keep characters on the lean `sprite-gpu` canvas; put
stage/HUD/effects on a `PVR2Renderer` **shared-device transparent-overlay** canvas
(`initShared` + `transparentClear`), drawn back→front in submission order. Keep the
split — a single unified op/pt/tr set across chars+HUD is **UNTESTED** (T1 gap 3).

---

## 1. ARCHITECTURE — cheapest bit-identical source per visual class

| Class | Producer | Stream cost | Renderer | Status |
|---|---|---|---|---|
| **Characters** | GSTA `sprite_id`→`atlas/chars/PLxx.json` rect @ `screen_x/y` | ~5–15 KB/s | `sprite-gpu` (lean canvas) | **DONE [CONFIRMED]** |
| **Stage** | `stage_id`→`STGxxPOL/TEX` (Ninja POL, pre-bundled) animated by `stage_anim_timer` | **~0** (client-side) | `PVR2Renderer` **op** layer | build (T4) |
| **HUD** | GSTA `health/red_health/meter_fill/meter_level/combo/timer`→white-bar + `FONT.BIN` digits, modulated by per-slot vertex color | **~0** (client-side) | `PVR2Renderer` overlay | build (T2/T3) |
| **Effects** | STAF de-indexed strips filtered to **effect-texId-set** (GFX∈`0x0ced0000`) | ~3–10 KB/s | `PVR2Renderer` overlay | build (T5) |
| **Textures** | content-addressed `texHash64`; ship-once `TX64`; pre-ripped disc → `TXID` (~0 bytes) | ~0 steady | uploaded once | build (T3/T6) |

**The ladder:** full TA mirror ~1.7 MB/s → STAF-everything ~140 KB/s → **this plan
~20–30 KB/s typ**. Pixel-exact for effects (real TA triangles via PVR2Renderer) and
for static art (disc-exact rips through the same renderer). **[CONFIRMED ladder** — agent file].

**Compose back→front each frame:** STAGE (op) → CHARS (lean canvas) → EFFECTS
(STAF overlay) → HUD (overlay, on top).

---

## 2. PER-COMPONENT BUILD

### 2.1 Characters — DONE [CONFIRMED]

GSTA `sprite_id` (char+0x144) → `atlas/chars/PLxx.json` `{x,y,w,h,dx,dy}` → textured
quad at `screen_x/y` (char+0xE0/0xE4), `facing` (char+0x110), `palette`. Reimplements
the `loc_8c033e90` quad-emit (`sprite_id→rect+dx/dy→quad`) **[CONFIRMED address]**.
**~5–15 KB/s, pixel-exact, keep it.** Not in the TX64 cache (separate atlas path).

### 2.2 Stage — Ninja POL decoder + op-layer render (T4)

**KEY BREAKTHROUGH [CONFIRMED]:** MVC2 stage data is **Sega NinjaLibrary / libspr
"NLOBJPUT"** display objects, **pre-relocated to load base `0x0cea0000`**. The
binary self-identifies (`bank14.asm:1-18`: "NLOBJPUT Ver 0.99 … SEGA", "libspr Ver
0.2 Build:Dec 16 1999"). So the POL files are a Ninja object tree with absolute
pointers + per-object PVR state — **decodable offline, no SH4**. The POL already IS
the projected geometry, which is exactly why PVR2Renderer (consumes projected
ta_parse output) is the right renderer.

**`stage_id`→file map [CONFIRMED exact]:** identity. enum `stg_AirS1…stg_Raft2` =
`0x00..0x10` (17 stages, `work.asm:53-69`) == filedic `STG00..STG10 POL/TEX`
(`filedic.asm:3122-3270`) == 17 disc pairs. `stage_id` on wire
(`maplecast_gamestate.h:38`), RAM `STG_ID 0x8C26A95C` (`work.asm:27`).

**STGxxPOL.BIN format [CONFIRMED by hexdump of all 17 + disasm]:** pre-relocated
Ninja object, base `0x0cea0000`. `+0x00` = `0x0cea0010` (self-reloc ptr to
TextureList); `+0x04` = objectCount; `+0x08..` = abs-pointer table (subtract
`0x0cea0000` for file offset). **TextureList** 16-byte recs `{u16 w, u16 h, u8
fmt/twiddle(0x0101), u8, u16 pad, u32 texVramAddr (0x0cc00000 family), u32 pad}`.
**Vertex block** 32-byte stride `{x f32, y f32, z f32, packed u16+u16, baseCol
ARGB8888, offsetCol ARGB8888, u f32, v f32}` — maps 1:1 onto PVR2Renderer's
(x,y,z,col,spc,u,v).

**STGxxTEX.BIN [CONFIRMED]:** RAW TWIDDLED PVR texels (no GBIX/PVRT magic), pre-
arranged to DMA into the VRAM addresses the POL TextureList names. T3's
`decodeTexAny`/de-twiddle applies verbatim.

**Render routine to mirror [CONFIRMED]:** dispatcher `loc_8c027b64`
(`bank02.asm:18666`) reads STG_ID, bounds<0x19, braf-jumps a 24-entry table. The
generic entry `loc_8c027e32` masks `&0x1F`, `r6=0x0cea0000`, enqueues the stage's
Ninja object via OBJPUT `loc_8c0279e4` (an 8-byte cmd/arg/dataPtr ring) — **no per-
poly transform in MVC2 code; verts are pre-projected in the POL**.

**Animation [CONFIRMED routine `loc_8c0338ec`, bank03.asm:8412]:** a 0.01/frame
accumulator at `0x8c1f9d8c` increments the u8 `stage_anim_timer` (`0x8C1F9D80`, on
wire) when it crosses 1.0; the timer's **low bit** drives a 2-phase A/B keyframe
toggle; a per-element loop (`loc_8c0339d0`) copies u16 words from keyframe lists
into `0x0cea0000`, overwriting UVs (packed-nibble unpack `loc_8c033a54`) or verts —
scrolling skies/waterfalls/parallax. Fully rip-able and reproducible from
`stage_anim_timer` alone.

**Camera [CONFIRMED on wire]:** `camera_x/y = 0x8C1F9CD8/0x8C1F9CDC`
(`maplecast_gamestate.cpp:63-64`), shipped in GSTA; combine with `pvrSnap` NDC matrix.

**BUILD:**
- **`tools/decode_stage_pol.py`** — parse STGxxPOL as pre-relocated Ninja: header
  `{relocPtr@0,count@4}`, walk abs-ptr table (−`0x0cea0000`), decode each sub-
  object's poly-group header + 32-B vertex blocks + TextureList. Emit per-stage JSON
  `{vertexData[28B/vert VBL], opaque:[{first,count,tsp,tcw,pcw,isp}], textureList:
  [{w,h,fmt,vramAddr,fileOffset}]}`. **PVR words SYNTHESIZED** from Ninja fields
  (textured/twiddle/fmt 0/1/2/opaque/gouraud) — validate against the public
  libspr/NL spec, mark each bit CONFIRMED only after a live-capture match.
- **TEX decode** (reuse T3 `decodeTexAny`): slice each TextureList region from
  STGxxTEX at its vramAddr offset, de-twiddle→RGBA8888, hash with `texHash64`. Pre-
  bundle as client stage-texture cache (never streamed).
- **Wire into PVR2Renderer as the OP layer** (draws first = behind): on `stage_id`
  change, load that stage's JSON+textures, feed `opaque[]` to `renderFrame` before
  chars/effects. This **replaces the black background** and kills "garble on black"
  (stage additive fragments now composite on a real stage).
- **Animation:** decode the stage's keyframe lists, drive UV/vert swaps in
  `opaque[]` by `stage_anim_timer` (low-bit A/B first; extend per stage).

**Verify:** render the ripped op-layer over black, A/B against the live out-of-match
TA video (PVR2Renderer renders that perfectly — diff against it) for each of 17
stages; confirm animated stages (Air Ship, Swamp, Carnival) tick at the right rate
against video frames.

### 2.3 HUD — asset-driven bars + FONT digits (T2 + T3)

**HUD is a self-contained object pool in bank0f [CONFIRMED]**, NOT the character
slot-table (resolving the "no HUD symbol" negative result). Dispatcher
`loc_8C0F0160` (`bank0f.asm:175`) reads the method-index byte at element+0x04,
indexes the jump table at `0x8C15FF78`, `jmp` to the per-element draw method.

**HUD element struct [CONFIRMED from bank0f methods]:** +0x04 draw-method index;
+0x18 ptr to OWNER CHARACTER struct (link to health/red_health); +0x1C anim counter
(life-bar drain, vs 0x28=40 frames); +0x20 element-type/char-index; **+0x50 FILL
RATIO float (0..1)**; +0x84 current GFX/sprite ptr.

**Bar-fill math [CONFIRMED with disasm constants]:**
- **Life bar, red/trailing layer:** `loc_8C0F123C` — fill = `red_health(owner+0x424)
  / 32.0` (max const `0x42000000=32.0`). Draws as a **second trailing quad** behind
  current HP, animating the gap over ~40 frames.
- **Life bar, main layer:** `loc_8C0F12B0` — fill = `currentHP / maxHP` where **maxHP
  is PER-CHARACTER** (u16 via ptr at element+0x6A). **The existing `_pointHealth`
  /144 is wrong on both counts** (`sprite-client.mjs:860`).
- **Super meter:** `loc_8C0F0FDC` — fill = `meter_fill[char] / 144.0` (base
  `0x8C289646`, P1/P2 = +0/+2; max const `0x43100000=144.0`); `meter_level` (0..5,
  `0x8C28964A/4B`) gates full segments, level-5 = MAX special render.
- **Timer:** `loc_8C0F24DA` — byte at `0x8C289630`, BCD-split /10 into 2 digits, each
  indexes a GFX table at `0x8C26A90C+(digit+0x55)*4` → two FONT digit sprites.

**White-texture-modulate vertex colors [CONFIRMED, actual disasm values]:** color
table `loc_8c15FFB0` (`bank15.asm:27238`), 12 ARGB words = 3 char-slots × 2-vertex
gradient. **CRITICAL CORRECTION to the old assumption — the life-bar color is PER-
TEAM-SLOT, NOT a green→yellow→red health gradient:**
- C1 bar: `#FF40FF` (pink) → `#FFFF00` (yellow)
- C2 bar: `#00FF00` (green) → `#FFFF00`
- C3 bar: `#00C0FF` (cyan) → `#FFFF00`

The bar is the **WHITE FONT.BIN texture MODULATED (ShadInstr=1) by these fixed per-
slot vertex colors** — so the generator emits white-tex + `col[4]` and lets
PVR2Renderer's ShadInstr modulate (the root cause of the "white bar" bug = dropping
the col/ShadInstr).

**Asset source — FONT.BIN [CONFIRMED, container fully reversed + bit-identical
decode, T3]:** self-contained, `0x40` header (2 recs stride 0x10) + 2 raw (non-VQ)
twiddled textures. tex0@`0x40` 256×128 ARGB1555 (glyph sheet A-Z+digits); tex1@
`0x10040` 64×64 ARGB4444 (HUD glyphs **including the solid-white life-bar rectangle**
— the "white texture modulated by vertex color" the plan needs). Offsets byte-exact
to file size `0x12040`. **Decode VERIFIED** = legible font + white bar.

**BUILD (replace `sprite-client.mjs` drawHUD/_bar/drawHudReal, ~lines 858-969):**
1. **Life bar** = white FONT tex, width = currentHP/maxHP (per-char maxHP via +0x6A
   vitality), red layer width = red_health/32.0, MODULATED by per-slot color from
   `loc_8c15FFB0` via PVR2Renderer ShadInstr. Draw red_health as a trailing quad,
   animate the gap over ~40 frames.
2. **Super meter** = white tex, width = meter_fill/144.0, `meter_level` gates full
   segments, level-5 = MAX.
3. **Timer** = two FONT digit sprites BCD-split from `game_timer`.
4. Emit each as a PVR2Renderer quad (VBL strip + synthesized raw words + surrogate
   tcw) per §0; **CAPTURE the real tsp/pcw/isp** of each HUD texId from a live HUDF
   capture for pixel-exact words (don't hand-author — T1 REC).

**Verify:** health/meter/timer match the live HUD. The one INFERRED step is the
exact **640×480 pixel rects** (bank16 stores CPS world verts; `loc_8c160100` meter
X±7.2 Y-49.2, `loc_8c160160` portraits X±33, `loc_8c1601F0` bar-ends X±52 — scaled
by `CpsXScale=1.6667`/`CpsYScale=2.1429`). Calibrate placement from ONE live HUD-
quad capture (texId+screen rect, cross-ref FONT.BIN). Everything else (fill formulas,
max constants, modulate colors, 5-level meter, dispatcher) is CONFIRMED.

**DEFER:** COMBO/HITS text — `0x8C289670` is NOT read anywhere in the disasm; the
combo draw routine source is unlocated (T2 gap 2). Do not fabricate it.

### 2.4 Effects — texId-set filter + de-indexed strip (T5)

**Discriminator [CONFIRMED principle]:** is the draw object's GFX source pointer in
the **Effect Poly region `[0x0ced0000, 0x0cf00000)`** (`work.asm:39`)? NOT
sprite_id, NOT blend, NOT position. Every render object resolves its texture from
**owner+0x15C (Dat_GFX1) / +0x160 (Dat_GFX2)** — there is **no effect sprite_id
namespace** (`reference_mvc2_effects_bank.md`), so a sprite_id range cannot classify
effects, and the current HUDF additive-OR-screen-strip heuristic
(`mirror.cpp:2627-2634`) is the **known live garble source** (it grabs the additive
STAGE grid → the "garble flash on supers", and misses non-additive effect layers).

**The emit path already exists and is correct** (de-indexed strip, mirror.cpp:2553,
2664-2688; client `onSTAF` mirror.cpp→`sprite-client.mjs:263-344`); **only the
filter predicate changes**.

**BUILD — 3 parts, smallest-risk first:**
- **PART A (the discriminator):** in `readAllDrawn()`
  (`maplecast_gamestate.cpp:317`) read `gfx1=read32(node+0x15C)`,
  `gfx2=read32(node+0x160)`; tag node when either ∈ `[0x0ced0000,0x0cf00000)`. In
  STAF `emitList` (`mirror.cpp:2616`), for each kept textured poly already computing
  `texId` at :2644, when its screen bbox matches a tagged effect node, insert texId
  into a persistent process-wide `_effectTexIds` set (seed over a warmup window;
  never cleared on the 600-frame reseed). **The only new logic.**
- **PART B (replace the heuristic):** swap the HUDF predicate (mirror.cpp:2627-2634)
  for: **keep iff `texId ∈ _effectTexIds` OR untextured effect layer
  (`(pcw>>3)&1==0`)**. Leave the de-index emit (2664-2708) and `onSTAF` client
  UNCHANGED — proven contract; only WHICH polys survive changes. Directly kills the
  garble flash + recovers non-additive layers.
- **PART C (measure + pre-rip):** add a 4th effect-only accumulator to STAFMEASURE
  (`mirror.cpp:2742`) gated on `texId∈_effectTexIds`, fix the cost model to
  `33+count*28`, run a live super, read `/dev/shm/mc_staf.log` to confirm ≤~10 KB/s.
  Then decode **EFKYTEX.BIN** for the TXID pre-rip.

**EFKYTEX.BIN [CONFIRMED container, T3]:** the directory lives in **EFKYPOL.BIN**
(main-RAM image at `0x0ced0000`): `word[1]=0xF1=241` objects, ptr table, 16-byte quad
recs `{w,h,attr,texptr,palptr}` of the `loc_8c033e90` emitter shape. **25 distinct
VQ textures**; `texptr = 0x0cc00000 + byteOffset`; gaps == VQ size `2048+w*h/4`; last
tex + size == file size `0x5b800` (byte-perfect no-gap proof). **Decode VERIFIED** =
coherent explosion/hitspark blob with smooth 16-level alpha. So the hitspark atlas =
EFKYTEX; **HIT_xx/HIT_DT/HIT_FM are anim/placement scripts, NOT pixels** (T3/T5) —
needed only to STATE-DRIVE effects later, not to stream them.

**Dependency:** A→B unblocks the visual immediately (effects render crisp over black
via PVR2Renderer); C is bandwidth/pre-rip. State-driving hitsparks from
`EffectTrigger`/HIT_xx is a separate later phase (T5 gap 4).

### 2.5 Textures — unified content-addressed cache + disc cross-ref (T6)

**Single namespace [CONFIRMED]:** `texId = mcfx::texHash64(tcw,w,h)` — 64-bit FNV-1a
(seed `1469598103934665603`, prime `1099511628211`) over `fmt|w|h|vq` + raw VRAM
texel bytes + (for paletted fmt 5/6 ONLY) the folded live palette window
(`mirror.cpp:1696-1712`). Address is NOT in the key (same content @ new addr → cache
hit). It is already the TX64 wire key AND the STAF poly key. **The offline disc
decoder MUST reproduce this byte-for-byte** (same `fmt|w|h|vq` prefix, same raw-VRAM
storage layout pre-twiddle, same FNV-1a constants) — the `tools/decode_raw_part.py`
twiddle is already ported from flycast `texconv.cpp`, so it is byte-portable.

**Disc cross-ref / TXID [INFERRED — new build]:** build `disc_tex_catalog.json`
`{texId(fmt0/1/2) OR indexHash(fmt5/6) → {discFile, bundleIndex, w, h, fmt, vq}}`
over EFKYTEX/EFKYPOL, STG00..10 TEX/POL ×17, FONT.BIN, SELTEX.BIN. **New wire record
`TXID`** (parallel to TX64): `'TXID'(4) texId(8 LE) discId(4 LE) [palOff(2)
palLen(2) palWindow]` — replaces the ~6-100 KB TX64 upload with ~12-40 bytes.
**Emit rule** (replaces mirror.cpp:2645): first sight of a texId not in the sent-set
→ if `texId∈catalog` broadcast TXID (tiny); else decode+broadcast TX64 (today's
path). STAF poly record unchanged. Client `onTXID` binds the pre-bundled disc
texture into `_stafTex` under `texKey` — identical cache slot to `onTX64`, render
loop untouched. **Net: every stage/HUD/effect texture crosses as ~12 bytes ONCE,
then 0.**

**Client pre-bundle:** ship STGxxTEX/POL×17 + EFKYTEX/POL + FONT.BIN + SELTEX.BIN
(+ existing `atlas/chars/PLxx`×56 on the separate sprite path); decode each once at
load, pre-populate `_stafTex` keyed by the catalog's precomputed texId → a TXID is a
pure cache hit. Under the existing ~64 MB LRU cap.

**Fix the 600-frame clear [CONFIRMED bug, mirror.cpp:2559-2560]:** `_stafSent` is a
process-global `unordered_set` cleared every 600 frames → re-ships the entire
working set every 10 s (the ~140 KB/s figure never decays to ~0). **Architectural
constraint [CONFIRMED, `fanout.rs`]:** the relay fans out via ONE shared
`broadcast::channel(16)`; flycast sees ONE upstream consumer, so it **cannot**
maintain a true per-TCP sent-set or ship a per-client TX64 subset over the shared
broadcast. **FIX A (recommended):** delete the 600-frame clear; **seed `_stafSent`
at startup from the disc catalog** (every client holds the pre-bundle), emit TXID
for catalogued ids / TX64 only for genuinely-novel textures (≈nothing for a fully-
ripped set); keep a SHORT periodic re-seed of ONLY the tiny non-catalogued working
set for relay-hidden joins. (FIX B — relay per-downstream sent-set — only if non-
catalogued textures prove significant; it forces the relay to parse the channel.)

**IndexedDB persistence [INFERRED — none exists yet]:** object store `texcache`
keyed by `texKey` storing `{w,h,rgba}` (the shape `onTX64` already caches); write-
through on TX64/TXID; hydrate before the connect digest → **0-MB reconnect warmup**.

**Relay [CONFIRMED pass-through, `fanout.rs:162-276`]:** TX64 (raw) and STAF (ZCST,
decompressed only for SYNC inspection) already forward original bytes verbatim — no
decode. Add `TX64`/`STAF`/`TXID` to the early short-circuit beside MCSV
(`fanout.rs:186`) to skip `apply_dirty_pages`, and add them to `is_state_frame`'s
keep-list (`protocol.rs:42-48`, currently GSTA/OBJF/MCSV only) **or** run STAF
clients in full-mirror mode (else a state-only subscriber drops STAF).

---

## 3. BANDWIDTH BUDGET (steady-state, post-warmup)

| Component | Source | Steady | Confidence |
|---|---|---|---|
| GSTA state (chars + globals) | reconstructed | ~5–15 KB/s | **CONFIRMED-measured** |
| STAF effects (filtered de-indexed strips) | streamed | ~3–10 KB/s | **INFERRED** — confirm via STAFMEASURE (Part C) |
| TX64/TXID textures | ship-once / disc-ref | **~0 steady** (few-KB warmup) | CONFIRMED-mechanism |
| HUD + stage | reconstructed/ripped | **~0** (client-side) | CONFIRMED-source |
| envelope + pvr_snapshot | per frame | ~5 KB/s | CONFIRMED |
| **TOTAL** | | **~20–30 KB/s typ; ~140 KB/s super-peak** | sums to target |

The decisive wins over the current build: deleting the 600-frame clear stops re-
shipping the working set every 10 s, and the TXID disc path makes stage/HUD/effect
textures cost ~0 on the wire forever. Per-poly STAF cost = `33B poly + verts×28B`,
zstd ~0.45×.

---

## 4. BUILD ORDER (dependency-ordered)

1. **Effect texId-set + filter swap (T5 A→B).** Depends on: the de-indexed strip
   (already landed in-tree). Do: tag GFX∈`0x0ced0000` in `readAllDrawn`, replace
   the HUDF heuristic (mirror.cpp:2627) with `texId∈_effectTexIds`. **Verify:** a
   super renders crisply via PVR2Renderer over black; no stage-grid leak. **Unblocks
   everything — removes the garble.**

2. **Stage rip (T4).** Depends on: nothing (assets + `stage_id`/`stage_anim_timer`
   present). Do: `decode_stage_pol.py` (Ninja POL) + `decodeTexAny` TEX → op layer.
   **Verify:** each of 17 stages matches the live TA video; animated stages tick.
   **Kills the black background + garble-on-black.**

3. **Texture cache disc cross-ref + TXID + kill 600-clear (T6 + T3/T5 Part C).**
   Depends on: 1 (effect-texId-set tells which to pre-rip) + a `decodeTexAny`-
   identical disc decoder. Do: `tools/rip_mvc2.py` builds `disc_tex_catalog.json`
   (reproduces `texHash64` byte-exactly); add TXID emit/handler; seed `_stafSent`
   from the catalog, delete mirror.cpp:2559-2560. **Verify:** hash one live STG/
   effect capture == disc decode (closes the paletted-hash risk); TX64 bytes → ~0
   after warmup.

4. **HUD rip (T2/T3).** Depends on: 3 (FONT/SELTEX catalogued) + HUD-texId-set. Do:
   replace `sprite-client.mjs` drawHUD/_bar/drawHudReal with white-bar + FONT digits
   modulated by per-slot color via PVR2Renderer; fix the /144 health bug. **Verify:**
   health/meter/timer match live; calibrate pixel rects from ONE live HUD capture.

5. **Join + persistence (T6).** Depends on: 1–4. Do: client-digest on connect
   (union of pre-bundle + IndexedDB keys); IndexedDB `texcache`. **Verify:** mid-
   match reconnect → no pop, near-zero re-ship.

---

## 5. GAP RESOLUTION (every gap the team surfaced)

| # | Gap | Resolution / remaining RE |
|---|---|---|
| 1 | HUD draw routine had no symbol | **RESOLVED** — it is a bank0f object pool, dispatcher `loc_8C0F0160` (T2). Fill math + colors + max constants CONFIRMED. **Remaining:** exact 640×480 pixel rects — one live HUD-quad capture (calibration, not RE). |
| 2 | Disc container formats not reversed | **RESOLVED for FONT.BIN + EFKYTEX (T3, bit-identical) + STGxxPOL/TEX (T4, Ninja/libspr, self-identified).** **Remaining:** SELTEX.BIN (1.59 MB, portraits — odd size, no companion POL; non-blocker for in-match HUD bars/digits). |
| 3 | Paletted cross-ref caveat unresolved | **RESOLVED-mechanism** — all in-scope HUD/effect/stage disc textures are **direct-color (fmt 0/1/2, palptr=0)** (T3), so `texHash64` folds ZERO palette → disc hash == live hash with no palette dependency. fmt5/6 (chars only, already on the atlas path) would use index-only sub-hash + palette in TXID. **Remaining:** per-stage verify any STGxxTEX isn't fmt5/6 (one capture). |
| 4 | STGxxPOL geometry/anim format unknown | **RESOLVED** — Ninja POL header/TextureList/32-B vertex/anim (`loc_8c0338ec`) decoded (T4). **Remaining single RE task:** decode one full Ninja sub-object end-to-end and **pixel-match the synthesized pcw/isp/tsp/tcw against a live STG00 capture** (the per-poly control-word synthesis is a lookup against the public libspr/NL spec, not from-scratch RE). |
| 5 | Effect-texId-set doesn't exist | **RESOLVED-path** — discriminator (GFX∈`0x0ced0000`), emit path, and filter-swap site all CONFIRMED (T5). **Remaining:** build the set live (warmup window) + measure effect-only KB/s (STAFMEASURE Part C). |
| 6 | Cardinal constraint (no C++ re-map) | **LOCKED** — T1 pins the exact PVR2Renderer contract (§0); every generator emits that shape; the server only de-indexes + ships verbatim. |

**THE SINGLE REMAINING FROM-SCRATCH RE TASK:** decode one full Ninja/libspr stage
sub-object (STG00 / Air Ship) end-to-end and confirm its **synthesized PVR control
words** render pixel-identical to a live TA capture (gap #4). The libspr/NLSPRITE
format is a **documented Sega SDK format** (the binary tells us so), so this is a
spec lookup + one pixel-match, not open-ended reverse engineering. Everything else
is CONFIRMED or a calibration-from-live-capture step.

## Sources
- **Team reports T1–T6** (this session): PVR2Renderer contract; HUD bank0f routine +
  fill math + modulate colors; FONT.BIN/EFKYTEX/EFKYPOL containers; STGxxPOL/TEX
  Ninja format; effect texId-set discriminator; unified texture-cache + TXID + join.
- **marvelous2** (local): `bank0f.asm:175,2331,2667,2743,5538`; `bank15.asm:27178-
  27250`; `bank16.asm`; `bank02.asm:18436-19136`; `bank03.asm:6485,8412`;
  `bank14.asm:1-18`; `memory/work.asm:27,36-39,53-69`; `filedic.asm:3122-3270`.
- **Live code:** `core/network/maplecast_mirror.cpp:1696,1716,2452,2540-2764`
  (texHash64/decodeTexAny/STAF emit/600-clear at 2559-2560);
  `core/network/maplecast_gamestate.{h:38,58 / cpp:63-64,317-373}`;
  `relay/src/{fanout.rs:137,162-276 / protocol.rs:42-48}`;
  `web/webgpu/{ta-parser,pvr2-renderer,sprite-client,shaders}.mjs`; `webgpu-test.html`.
- **DEV FILES** (`MVC2 Dev Files/`): `STG00..STG10 POL/TEX` ×17, `EFKYTEX.BIN`/
  `EFKYPOL.BIN`, `FONT.BIN`, `SELTEX.BIN`, `HIT_*`.
- **Docs:** `docs/STRIPPED-TA-DESIGN.md` §2-8; `docs/RENDER-MASTER-PLAN.md`
  (superseded by this V2); `.claude/agents/mvc2-sh4-re-expert.md`.
