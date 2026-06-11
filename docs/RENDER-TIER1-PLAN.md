# RENDER TIER 1 — Full Frame, Pixel-Perfect, ~140 KB/s via TA-commands + Ship-Once Texture Cache (No VRAM stream, No Server Re-parse)

> **Goal.** Render the FULL MVC2 frame (stage + chars + HUD + effects) **pixel-perfect**
> at **~140 KB/s** by shipping flycast's **parsed TA command stream + a content-addressed,
> ship-once texture cache** instead of streaming the ~1.7 MB/s VRAM dirty-page firehose.
> The frame is drawn through the PROVEN client path
> `web/webgpu/ta-parser.mjs` → `web/webgpu/pvr2-renderer.mjs` (`PVR2Renderer`) — the
> out-of-match video / `replay.html` renderer — with **NO server-side geometry re-parse**
> (the re-parse is what garbled every prior STAF/StafGL attempt).
>
> **Author:** mvc2-sh4-re-expert. **Date:** 2026-06-07. **Status:** scope + build spec.
> **Scope of "TIER 1":** the FULL-frame stripped-TA renderer at ~140 KB/s. Disc-rip
> tiers (stage/HUD/effects from the disc → ~20 KB/s) are a SEPARATE later step covered by
> `docs/RENDER-MASTER-PLAN-V2.md`; TIER 1 is the pixel-exact, low-risk, no-asset-rip rung.
>
> Each claim is **[CONFIRMED]** (verified against in-tree code this session, cited
> `file:line`) or **[INFERRED]** (reasoned; flagged with the verification step).

---

## 0. EXECUTIVE BOTTOM LINE

**The crux — "how does the client get the right texture for each TA command without the
VRAM?" — is ALREADY SOLVED and SHIPPING in-tree.** It is **Option 1(b)**: the server ships
a per-poly **content hash `texId`** inside each STAF poly record, plus each unique texture
**once** as a decoded-RGBA `TX64` packet; the client maps `texId → GPUTexture` through a
**texId-keyed texMgr shim**, and the proven `PVR2Renderer` samples that cache instead of
decoding from `D.vram`. **The client never needs VRAM and the `ta-parser → PVR2Renderer`
math is untouched.**

- **CONFIRMED in-tree:** server emit `core/network/maplecast_mirror.cpp:2540-2725`; texture
  identity `texHash64` `maplecast_mirror.cpp:1696-1712`; decode `decodeTexAny`
  `maplecast_mirror.cpp:1716-1730`; client reshape `web/webgpu/sprite-client.mjs:264-345`;
  the texId-keyed texMgr shim `web/webgpu-test.html:384-411`; render call
  `web/webgpu-test.html:416-423` → `PVR2Renderer.renderFrame` `web/webgpu/pvr2-renderer.mjs:187`.
- **TIER 1 is therefore NOT a from-scratch build — it is a PROMOTION of the existing STAF
  path** from "HUDF-filtered effects overlay" to "full-frame renderer", plus 3 correctness/
  bandwidth fixes (kill the 600-frame re-ship, fix the relay pass-through, run the BG poly).

**Option 1(b) beats 1(a) and Option 2 decisively** (§4). Use it as-is.

---

## 1. THE CRUX, STATED PRECISELY, AND WHY IT'S ALREADY SOLVED

### 1.1 Why caching by VRAM address is WRONG (the constraint that forces content-addressing)

The client `texture-manager.mjs` decodes every quad's texture **from `D.vram` by the quad's
TCW address**: `getTexture(tsp, tcw, vram)` computes `addr=(tcw&0x1FFFFF)<<3` and reads
`vram[addr…]` with a twiddle/VQ walk (`texture-manager.mjs:106-178`). Two hard facts make
the raw address unusable as a cache key:

1. **VRAM addresses are REUSED over time.** The same TCW address holds *different* texture
   content across frames (the game DMAs new sprites into the same VRAM region). The
   production renderer already keys its cache by `(addr,fmt,texU,texV,palSel,vq,mip)` and
   **invalidates on dirty-page overlap** precisely because address alone aliases
   (`texture-manager.mjs:112-123, 92-104`). [CONFIRMED]
2. **Textures are twiddled / VQ-walked across a region.** Decode is not a linear blit: it
   de-twiddles via `s_twTab` (`maplecast_mirror.cpp:1605-1613`, twiddle table identical to
   the client `detwiddle` in `texture-manager.mjs:4-14`) and for VQ walks a 2048-byte
   codebook + index region (`decodeTex16` `maplecast_mirror.cpp:1635-1665`; client
   `_decodeVQ` `texture-manager.mjs:180-213`). The "texture" is a *content+format* object,
   not a byte range. [CONFIRMED]

Therefore the only correct identity is a **content hash that folds format + size + VQ +
the raw texel bytes + (for paletted formats) the live palette** — which is exactly
`mcfx::texHash64`.

### 1.2 `texHash64` already makes address-reuse safe [CONFIRMED — maplecast_mirror.cpp:1696-1712]

```c
static uint64_t texHash64(uint32_t tcw, int w, int h) {
    int fmt=(tcw>>27)&7, vq=(tcw>>30)&1;
    uint32_t addr=(tcw&0x1FFFFF)<<3;
    uint64_t hsh=FNV1a_OFFSET;
    mix(fmt); mix(w); mix(w>>8); mix(h); mix(h>>8); mix(vq);   // format/size/VQ FOLDED IN
    n = fmt==5 ? w*h/2 : fmt==6 ? w*h : vq ? 2048+w*h/4 : w*h*2;
    for (i<n) mix(vram[addr+i]);                               // RAW texel bytes
    if (fmt==5) fold 16 live palette entries;                  // PALETTE folded for PAL4/PAL8
    if (fmt==6) fold 256 live palette entries;
    return hsh;
}
```

- **Address is NOT in the key** — only `addr` is used to *locate* the bytes, then the bytes
  themselves are hashed. Same content at a new address → **same `texId` → cache hit**;
  different content at the same address → **different `texId` → server ships the new texture
  once.** Invalidation is implicit (a new content at an old address yields a new hash); no
  invalidation messages, no dirty-page tracking on the client. [CONFIRMED]
- **Palette is folded for fmt 5/6** (`maplecast_mirror.cpp:1709-1710`) — a skin / team-color
  swap (which is *exactly* how MVC2 skins work, `CLAUDE.md` skin section) changes the
  `texId`, so stale colors are impossible. fmt 0/1/2 encode color in the texels, so palette
  contributes nothing and is skipped. [CONFIRMED]

This is the same content-addressing lesson the design docs cite (`docs/STRIPPED-TA-DESIGN.md`
§3.1, `.claude/agents/mvc2-sh4-re-expert.md` §2.5). **CONFIRMED: address reuse + twiddle/VQ
are correctly handled by content-addressing; the key is already 64-bit and already the wire key.**

### 1.3 How the client obtains the texture WITHOUT VRAM — the shipping mechanism [CONFIRMED]

The poly record on the STAF wire already carries the `texId`
(`maplecast_mirror.cpp:2578-2580`, 8 bytes per poly). On the client:

1. **`onTX64(key,w,h,rgba)`** caches the decoded RGBA by content key
   (`sprite-client.mjs:229-232`); key = `texKey(lo,hi)` = `"hi:lo"`
   (`sprite-client.mjs:218`). The server ships the `TX64` packet **once** per `texId`
   (`maplecast_mirror.cpp:2645-2657`, guarded by `_stafSent`).
2. **`onSTAF(d)`** reshapes the wire into PVR2Renderer's exact parsed object, and for each
   textured poly assigns a **per-frame surrogate int** `surr` (1:1 with the 64-bit `texId`)
   and **overrides `pp.tcw = surr`** (`sprite-client.mjs:307-336`). `_stafSurrTex[surr] = texKey`.
3. **The texMgr shim's `getTexture(tsp, surr)`** resolves `surr → texKey → _stafTex RGBA`,
   uploads it to a `GPUTexture` once, and returns `{texture,sampler}` with the sampler built
   from the real TSP wrap/filter bits (`web/webgpu-test.html:392-409`). **No VRAM, no
   `D.vram`, no PVR decode, no `ta_parse` on the client.**
4. **`PVR2Renderer.renderFrame(_stafParsed, shim, snap, null, …)`** runs the *identical*
   strip→list winding fix, ShadInstr modulate, and blend/depth/cull derivation it uses for
   the live TA video — it just calls `texMgr.getTexture(tsp,tcw)` where `tcw` is the
   surrogate (`pvr2-renderer.mjs:325`). [CONFIRMED — `pvr2-renderer.mjs:187, 325`]

**This is Option 1(b) verbatim, already in the tree.** The crux is solved.

---

## 2. WHAT'S ALREADY BUILT AND REUSABLE (cite file:line)

| Piece | Where | Status |
|---|---|---|
| **Texture identity** `texHash64(tcw,w,h)` — 64-bit FNV-1a, folds fmt/w/h/vq + texels + palette | `core/network/maplecast_mirror.cpp:1696-1712` | **CONFIRMED, reuse verbatim** |
| **Decode** `decodeTexAny(tcw,tsp,out)` → RGBA8888; fmt 0/1/2(+VQ) via `decodeTex16`, fmt 5/6 paletted; mip/exotic → false | `core/network/maplecast_mirror.cpp:1716-1730` (+ `decodeTex16` :1635-1665) | **CONFIRMED, reuse** |
| **Server STAF emit** — `ta_parse(ctx,false)`, de-index `rc.idx`→contiguous strip, per-poly `{firstVert,vertCount,texId,tcw,tsp,pcw,isp,listType}`, ship `TX64` once | `core/network/maplecast_mirror.cpp:2540-2725` | **CONFIRMED, reuse; un-gate the HUDF filter for full frame** |
| **TX64 wire** `'TX64'(4) texId(8) w(2) h(2) rawSize(4) zstd(RGBA)` | `maplecast_mirror.cpp:2651-2657` | **CONFIRMED** |
| **STAF wire** `'STAF'(4) frameNum(4) pvr_snapshot[16](64) vertCount(4) polyCount(4) verts[28×N] polys[33×M]` | `docs/STRIPPED-TA-DESIGN.md §8.2`; emit `maplecast_mirror.cpp:2582-2715` | **CONFIRMED** |
| **Client reshape** `onSTAF` → `_stafParsed{vertexData,vertexCount,opaque,punchThrough,translucent}` + surrogate-tcw | `web/webgpu/sprite-client.mjs:264-345` | **CONFIRMED, reuse** |
| **Client TX64 cache** `onTX64`, `texKey` | `web/webgpu/sprite-client.mjs:218, 229-232` | **CONFIRMED** |
| **texId-keyed texMgr shim** `getTexture(tsp,surr)→GPUTexture`, `getFallbackTexture` | `web/webgpu-test.html:384-411` | **CONFIRMED — this IS Option 1(b)** |
| **Proven renderer** `PVR2Renderer.renderFrame` (strip→list, ShadInstr, blend, op/pt/tr) | `web/webgpu/pvr2-renderer.mjs:187-…, 325` | **CONFIRMED, UNTOUCHED** |
| **Reference parser shape** (the object `_stafParsed` must match byte-for-byte) | `web/webgpu/ta-parser.mjs:36-250` | **CONFIRMED contract** |
| **SYNC-invalidate fix** — SYNC replaces all VRAM but carries no dirty list → flag `syncPending` to invalidate the tex cache once | `web/webgpu/frame-decoder.mjs:78-87` | **CONFIRMED (mirror path); see §6 note** |
| **Routing** — GSTA/EFCT/TXTR/OBJS bail out of `applyFrame`; TX64 raw route; STAF decompress-peek route | `frame-decoder.mjs:77`; `webgpu-test.html:1094-1114` | **CONFIRMED** |

**Net:** every primitive the crux needs already exists and is proven. TIER 1 is promotion +
3 fixes, not new architecture.

---

## 3. IS THE TA-DELTA WIRE SHIPPABLE WITHOUT VRAM AS-IS? — two different answers

There are **two distinct "command streams"** in the tree; only one is relevant to TIER 1.

### 3.1 The MIRROR delta wire — NO, it assumes VRAM arrives together [CONFIRMED]

`frame-decoder.mjs:applyFrame` parses `frameSize|frameNum|pvr_snapshot|taSize|deltaPayloadSize|
[TA delta runs]|checksum|dirtyCount|[regionId|pageIdx|pageData×4096]×N`
(`frame-decoder.mjs:89-141`). The TA delta patches `this.prevTA` (the raw TA command buffer)
**and the same frame carries the dirty VRAM pages** that the downstream `texture-manager`
decodes from. The returned object (`frame-decoder.mjs:144`) hands `taBuffer` + `vramDirty` +
`dirtyPageList` to the renderer, which calls `texMgr.setDirtyPages(...)` then
`getTexture(tsp,tcw,vram)` reading `D.vram`. **If you stop shipping VRAM pages on this wire,
the mirror client decodes garbage** — `applyFrame` and `texture-manager` are inseparable from
VRAM. So the answer for the *mirror* wire is: **NOT shippable without VRAM.** (Confirmed:
`frame-decoder.mjs:128-141` is the only place VRAM is populated for the mirror path.)

### 3.2 The STAF wire — YES, it is VRAM-free BY DESIGN [CONFIRMED]

STAF is a **separate, parallel format** (`docs/STRIPPED-TA-DESIGN.md §6 "parallel channel,
NOT an extension of the dirty-page path"`). It ships the **parsed, de-indexed, projected
geometry + per-poly `texId`** and the textures via `TX64` — there is **no VRAM region in the
STAF envelope at all** (`maplecast_mirror.cpp:2582-2715`). The client `onSTAF` never touches
`D.vram` (`sprite-client.mjs:264-345`); the texMgr shim resolves textures from the `TX64`
cache (`webgpu-test.html:392-409`). **TIER 1 rides the STAF wire, which is VRAM-free by
construction.** This is the whole point of choosing STAF over extending the mirror.

**Conclusion:** Do NOT try to strip VRAM from the mirror delta wire (3.1) — that path is
load-bearing on VRAM. Use STAF (3.2), which already ships zero VRAM.

---

## 4. OPTION DECISION — 1(b) vs 1(a) vs 2

### Option 1(b) — texId-keyed GPU texture cache (CHOSEN, already built)

Server ships per-poly `texId` + `TX64` once; client maps `texId→GPUTexture` via the texMgr
shim; `PVR2Renderer` samples it. **This is the shipping STAF path.**

- **Cleanest / least invasive to the proven path.** `PVR2Renderer` is **byte-for-byte
  untouched** — it still calls `getTexture(tsp,tcw)`; only the `texMgr` *implementation*
  changes (shim vs VRAM decoder) and `tcw` is a surrogate. No `D.vram`, no client-side
  twiddle/VQ, no `ta_parse`. [CONFIRMED — `webgpu-test.html:392-409`, `pvr2-renderer.mjs:325`]
- **Decode happens once, server-side**, in flycast's canonical path (`decodeTexAny`), so the
  client uploads ready RGBA — no format-guessing, no client decoder maintenance. [CONFIRMED]

### Option 1(a) — write RGBA back into a client-side `D.vram` at the TCW address

Reconstruct a fake `D.vram`, blit each cached texture's RGBA to its TCW address, and let the
**unchanged** `texture-manager.getTexture(tsp,tcw,vram)` decode it. **REJECTED:**

- `texture-manager` decodes **twiddled/VQ source bytes**, not RGBA (`texture-manager.mjs:151-213`).
  To feed it you'd have to **re-encode** RGBA back to twiddled fmt-0/1/2/5/6 source bytes — a
  brand-new client encoder, the inverse of the decode we just did server-side. Pure waste.
- TCW addresses **reuse/alias** (§1.1), so a client `D.vram` keyed by address re-introduces
  the exact aliasing bug content-addressing exists to kill — you'd need dirty-page
  invalidation again, i.e. you've rebuilt the mirror.
- It keeps an 8 MB `D.vram` mirror + the whole `texture-manager` decode path alive — the
  opposite of "less invasive." **1(b) deletes all of that.**

### Option 2 — content-addressed VRAM-page delta (dedupe pages by page-content hash)

Ship only VRAM pages whose **content** hasn't been seen, keeping `D.vram` correct so the
mirror `ta-parser`/`texture-manager` are untouched. **REJECTED on bandwidth + correctness:**

- **Wrong granularity.** A 4096-byte page can hold parts of several textures, and a texture
  can span pages (`docs/STRIPPED-TA-DESIGN.md §5.1`). Page dedupe under-/over-ships at
  texture boundaries; one changed texel dirties a whole 4 KB page. Content-addressing at the
  **texture** boundary (1b) is strictly tighter.
- **No client simplification.** It keeps the 8 MB VRAM mirror, the twiddle/VQ decoder, and
  dirty-page invalidation alive (the very machinery 1b removes) — `docs/STRIPPED-TA-DESIGN.md
  §5.2`.
- **Bandwidth.** STAF still ships the geometry it ships in 1(b); Option 2 *adds* page bytes
  on top of needing the raw TA delta too. The 1(b) per-poly `texId` (8 B) replaces the
  texture's entire page footprint after first send; pages would re-ship on any neighboring-
  texel churn. **1(b) is the lower-bandwidth, lower-complexity, pixel-identical choice.**

**Decision: ship Option 1(b) (the existing STAF path). Promote it to full-frame.** [CONFIRMED rationale]

---

## 5. THE EXACT CHANGES TO REACH FULL-FRAME TIER 1

The crux is solved; what's missing is **(i)** the full frame (today STAF is HUDF-filtered to
effects+HUD, or full only behind an unfiltered debug flag), **(ii)** steady-state bandwidth
(the 600-frame re-ship keeps it from decaying), and **(iii)** correct relay pass-through and
the opaque background. None touch `PVR2Renderer` or the parsed-object contract.

### 5.1 Server (`core/network/maplecast_mirror.cpp`) — un-filter + de-flutter

1. **Full-frame emit (un-gate HUDF).** `MAPLECAST_STAF` already emits **all** of op/pt/tr
   (`maplecast_mirror.cpp:2709-2711`); the HUDF additive/screen-strip filter
   (`:2627-2634`) only runs under `MAPLECAST_HUDF`. For TIER 1 **run with `MAPLECAST_STAF`
   (no `HUDF`)** → the full frame already ships. **No code change** beyond config; the
   filter is the known garble source and is bypassed. [CONFIRMED — `:2627` is `if(_hudfOn)`]
2. **Kill the 600-frame re-ship [CONFIRMED bug, `maplecast_mirror.cpp:2558-2560`].**
   ```c
   static uint32_t _stafClear = 0;
   if ((++_stafClear % 600) == 0) _stafSent.clear();   // re-ships the whole working set every 10s
   ```
   This re-ships every on-screen texture every 600 frames, so the wire **never decays to
   ~0** — it's why the figure is ~140 KB/s, not lower. **Fix:** delete the clear; instead
   maintain `_stafSent` for the session and cover relay-hidden joins via a connect-time
   client digest (§5.3 Phase 3) OR a *short* periodic re-seed of only the small working set.
   [CONFIRMED bug; INFERRED fix — verify steady texCount→0 after warmup, §7]
3. **Background polygon.** The full frame's opaque background comes from the PVR `ISP_BACKGND`
   regs + a VRAM strip (the mirror client synthesizes it in `ta-parser.fillBGP`
   `ta-parser.mjs:252-392`). STAF runs `ta_parse(ctx,false)` and emits `global_param_op`
   directly, so any background poly *in the op list* already ships; the implicit
   `FillBGP` background (not in `rc`) does not. **For TIER 1, emit the BG poly explicitly**
   by porting `fillBGP`'s ISP_BACKGND read server-side into one extra op poly, OR accept the
   stage's own op geometry as the backdrop. [INFERRED — verify the out-of-match black/stage
   background renders; the SYNC-invalidate note `frame-decoder.mjs:78-87` is the mirror-path
   analog of this concern.]

### 5.2 Relay (`relay/src/fanout.rs`, `protocol.rs`) — stop mis-parsing STAF/TX64 [CONFIRMED need]

Today the relay forwards original wire bytes (so browsers DO get STAF/TX64), **but**:
- **STAF is ZCST-wrapped**, so `is_compressed` is true → it gets **decompressed for
  inspection** (`fanout.rs:193-194`), is not SYNC, falls to the `else` branch, and runs
  `apply_dirty_pages` on it (`fanout.rs:229`). `apply_dirty_pages` reads `frame[76]` as
  `delta_payload_size` (`protocol.rs:123-125`) — for STAF that's `polyCount`, producing a
  garbage offset that **corrupts the relay's cached SYNC VRAM** or wastes the decompress.
- **TX64 is raw** (`is_compressed` false) → inspect=original → not SYNC → `else` → same
  `apply_dirty_pages` garbage on a `TX64` packet.

**Fix [CONFIRMED site]:** add `STAF` and `TX64` (and future `TXID`) to the early
short-circuit beside `MCSV` (`fanout.rs:186`) — forward verbatim, skip decompress +
`apply_dirty_pages`. Also add them to `is_state_frame`'s keep-list (`protocol.rs:42-47`,
currently `GSTA|OBJF|MCSV`) **or** run STAF clients in full-mirror mode, else a state-only
subscriber drops STAF (`fanout.rs:540`). [CONFIRMED — code paths cited]

### 5.3 Client — already wired; remaining work is the render route + persistence

- **Render route exists** (`webgpu-test.html:1094-1114` TX64+STAF routing;
  `:416-423` render). For full-frame TIER 1, **remove the HUDF filtering on the server** (5.1)
  and the same client path renders the whole frame on the `scs` overlay. The lean character
  canvas can be turned off (full STAF draws characters too) or kept for the hybrid. [CONFIRMED routes]
- **Phase 3 — join digest + IndexedDB [INFERRED, not built].** On connect, client sends the
  set of `texId`s it already holds (bloom/digest); server seeds `_stafSent` and ships only
  missing textures. Persist `_stafTex` to IndexedDB keyed by `texKey` → 0-MB warmup on
  reconnect with the same characters. Replaces the deleted 600-frame clear for relay-hidden
  joins. [INFERRED — `docs/STRIPPED-TA-DESIGN.md §6.3, Phase 3`]

**`PVR2Renderer` and `ta-parser.mjs` are NOT modified in any phase.** The parsed-object
contract (`{vertexData(28B/vert), vertexCount, opaque, punchThrough, translucent}`,
`pvr2-renderer.mjs:188`) is produced identically by `ta-parser.parse` (mirror) and
`onSTAF` (STAF) — that identity is the whole "no re-parse" guarantee. [CONFIRMED]

---

## 6. HOW VRAM-REUSE / TWIDDLE / VQ IS HANDLED (summary, all CONFIRMED)

- **Reuse:** `texId` excludes the address (§1.2). Same content @ new addr → cache hit;
  changed content @ same addr → new `texId` → ship once. No address keying anywhere on the
  STAF path. [CONFIRMED `maplecast_mirror.cpp:1696-1712`]
- **Twiddle:** server decodes twiddled source to linear RGBA via `s_twTab`
  (`maplecast_mirror.cpp:1605-1665`); the client receives **already-linear RGBA** in `TX64`
  and uploads it directly (`webgpu-test.html:401-402`) — the client does zero twiddle work
  on the STAF path. [CONFIRMED]
- **VQ:** `decodeTex16` walks the 2048-byte codebook + index region server-side
  (`maplecast_mirror.cpp:1648-1660`); `texHash64` sizes the hashed region as `2048+w*h/4`
  for VQ (`:1705`) so the codebook is part of the identity. Client never sees VQ. [CONFIRMED]
- **Paletted (fmt 5/6):** `decodeTexAny` resolves the index→palette server-side
  (`:1722-1730`); `texHash64` folds the live palette window (`:1709-1710`) so a skin swap is
  a new texture. [CONFIRMED]
- **marvelous2 grounding:** the engine recomputes the GFX/texture pointers
  (owner `+0x15C`/`+0x160` Dat_GFX1/2) every frame and DMAs decompressed parts into VRAM
  staging at `0x0CE60000` (`loc_8c03552a` LZSS decoder;
  `.claude/agents/mvc2-sh4-re-expert.md` "render data path"), which is *why* VRAM addresses
  cycle — confirming the reuse reality content-addressing defends against. [CONFIRMED grounding]

---

## 7. BANDWIDTH MATH (sums to the ~140 KB/s claim)

**Per-poly STAF cost (as built, `maplecast_mirror.cpp` wire §8.2):** 33-byte poly record +
**28 B per strip vertex** (no per-vertex delta/quantization yet — correctness-first).
The in-tree STAFMEASURE probe models strip cost as `11 + count*10` per poly and applies a
zstd factor of **0.45** (`maplecast_mirror.cpp:2745, 2756`).

**Steady-state geometry (zstd 0.45×), grounded in `docs/STRIPPED-TA-DESIGN.md §4.1` quad
counts (~80–120 typ, ~180 super-peak):**

| Component | Per frame | Notes |
|---|---|---|
| ~100 polys × (33 + ~4 strip-verts × 28) | ~14.5 KB raw | de-indexed strip, full f32 verts |
| × zstd 0.45 | **~6.5 KB/frame** | repetitive blend/tcw runs compress well |
| × 60 fps | **~390 KB/s raw-strip** | **this is the un-optimized worst case** |
| `pvr_snapshot`(64 B) + `STAF`/`ZCST` envelope | ~5 KB/s | per-frame overhead |

The **~140 KB/s** target in `docs/STRIPPED-TA-DESIGN.md §4.1` and the agent file
("texture-cache STAF ~140 KB/s") is the **measured/modeled figure with per-vertex
quantization** (the `~51 B/quad → ~23 B/quad post-zstd` model, `§4.1`): `100 quads × 23 B ×
60 = ~138 KB/s`. The **as-built de-indexed strip is heavier** (full f32 verts, one vert per
strip step incl. degenerate links) — so **TIER 1 lands ~140 KB/s only AFTER the per-vertex
quantization in `docs/STRIPPED-TA-DESIGN.md §8.5` "known limits"** (Q12.4 x/y, Q0.16 uv,
strip-share). Without it, expect ~250–400 KB/s. **[CONFIRMED model; the ~140 KB/s is
post-quantization — flag this as the bandwidth gap to close.]**

**Textures (TX64) — ~0 steady, lazy warmup:**

| | |
|---|---|
| Per unique texture | `w*h*4` RGBA, zstd'd (~0.4×): 64×64 ≈ 6 KB, 128×128 ≈ 26 KB (`§4.2`) |
| Per character full move-set | ~150–300 unique → ~3 MB lazy (`§4.2`) |
| 6-char match warmup | **~20 MB total, trickled** as moves are first used (`§4.2`) |
| Steady-state, post-warmup | **~0** (ship-once) — **IFF the 600-frame clear is deleted (§5.1.2)** |

**TIER 1 TOTAL:**
- **With the as-built strip (no quantization):** ~250–400 KB/s geometry + ~0 steady textures.
- **With per-vertex quantization (the §8.5 optimization):** **~140 KB/s steady** (the target),
  ~250 KB/s super-peaks, + lazy ~20 MB texture warmup.
- **vs. current mirror ~1.7 MB/s → ~12× steady reduction, pixel-exact.** [CONFIRMED ladder —
  agent file + `STRIPPED-TA-DESIGN.md §4.3`]

**Verify against the live build:** run `MAPLECAST_STAFMEASURE=1`, read
`/dev/shm/mc_staf.log` (`maplecast_mirror.cpp:2757-2759`) — it already prints
`STRIP all/char-only/stage` and `uniqTex` KB/s split by list. Confirm steady KB/s and
`uniqTex`→plateau before trusting the number on this build. [CONFIRMED probe exists]

---

## 8. BUILD ORDER (dependency-ordered; the crux step is DONE)

0. **[DONE / CONFIRMED] The crux** — texId-without-VRAM via Option 1(b) ships in-tree
   (`maplecast_mirror.cpp:2540-2725`, `sprite-client.mjs:264-345`, `webgpu-test.html:384-423`).
   Nothing to build; this is the load-bearing fact of TIER 1.

1. **Relay pass-through fix (§5.2).** Add `STAF`/`TX64` to the `fanout.rs:186` short-circuit
   + `protocol.rs:42-47` keep-list. **Verify:** relay no longer runs `apply_dirty_pages` on
   STAF/TX64 (log); SYNC cache uncorrupted; browser receives STAF/TX64 intact. **Unblocks a
   clean wire.** [CONFIRMED sites]

2. **Full-frame emit (§5.1.1).** Run `MAPLECAST_STAF` (no HUDF). **Verify:** the full frame
   (stage + chars + HUD + effects) renders on the `scs` `PVR2Renderer` overlay,
   pixel-identical to the out-of-match TA video; no garble (the HUDF heuristic is bypassed).

3. **Background poly (§5.1.3).** Emit the ISP_BACKGND opaque quad server-side (port
   `fillBGP`) or confirm stage op-geometry covers it. **Verify:** no black-background bleed;
   backdrop matches the mirror client.

4. **Kill the 600-frame clear + steady-state (§5.1.2).** Delete `maplecast_mirror.cpp:2559-2560`.
   **Verify (STAFMEASURE / texCache stat `sprite-client.mjs:348`):** `texCache` plateaus,
   TX64 KB/s → ~0 after warmup; geometry KB/s holds.

5. **Per-vertex quantization (§7 / `STRIPPED-TA-DESIGN.md §8.5`).** Q12.4 x/y, Q0.16 uv,
   strip-share, drop full f32. **Verify:** wire drops from ~250–400 KB/s to **~140 KB/s**;
   A/B pixel-diff vs the un-quantized strip shows no visible loss.

6. **Join digest + IndexedDB persistence (§5.3 Phase 3).** Connect-time `texId` digest;
   persist `_stafTex`. **Verify:** mid-match reconnect → no texture pop, near-zero re-ship,
   0-MB warmup for returning same-character clients.

---

## 9. RISKS / GAPS

| # | Risk | Confidence / mitigation |
|---|---|---|
| 1 | **As-built strip is ~250–400 KB/s, not 140** until per-vertex quantization (step 5) | **CONFIRMED gap** (§7). The 140 KB/s is the post-quantization figure; the un-optimized strip is correctness-first (`STRIPPED-TA-DESIGN.md §8.5`). Step 5 closes it. |
| 2 | **600-frame clear keeps steady-state high** | **CONFIRMED bug** `maplecast_mirror.cpp:2559-2560`. Step 4 deletes it; step 6 covers relay-hidden joins. Until then steady-state never decays. |
| 3 | **Relay corrupts SYNC cache / wastes decompress on STAF** | **CONFIRMED** (`fanout.rs:193-229`). Step 1. Browsers still receive STAF today (verbatim forward), so this is a relay-hygiene + state-only-subscriber fix, not a "STAF doesn't arrive" blocker. |
| 4 | **Background poly** not in `rc` (FillBGP is synthesized) | **INFERRED** (§5.1.3). Step 3; verify against mirror. Low risk — stage op geometry often covers it. |
| 5 | **mip / exotic formats** draw untextured (vertex color) on STAF | **CONFIRMED limit** `maplecast_mirror.cpp:2640-2641`, `STRIPPED-TA-DESIGN.md §8.5`. MVC2 uses fmt 0/1/2/5/6(+VQ); mip is rare. Phase-4 client-decode fallback if any surface. |
| 6 | **CLAMP_TO_EDGE + per-TSP wrap/filter** not fully honored on the shim sampler | **CONFIRMED limit** `STRIPPED-TA-DESIGN.md §8.5`; shim builds sampler from TSP bits (`webgpu-test.html:403-405`) so wrap/filter ARE derived — verify Flip/Clamp edge cases. Low risk for MVC2 sprite quads. |
| 7 | **No depth buffer** — relies on op→pt→tr list order | **CONFIRMED** `webgpu-test.html:421-422` (`noSort`), how 2D fighters compose. Matches the live video path. |
| 8 | **Hash collision** over a ~2000-entry set with 64-bit FNV-1a | ~1e-13 (`STRIPPED-TA-DESIGN.md §7`). Negligible. |
| 9 | **Four-parser rule** (`CLAUDE.md`) | STAF is a **parallel** format, not the mirror wire — only needs the WebGPU client parser. The native/emulator.html parsers are untouched unless those clients adopt STAF (`STRIPPED-TA-DESIGN.md §6 Files`). [CONFIRMED] |

---

## 10. SOURCES (file:line)

- **Crux mechanism (Option 1b):** `core/network/maplecast_mirror.cpp:2540-2725` (STAF emit,
  TX64 ship-once, `_stafSent` 600-clear at 2559-2560); `:1696-1712` (`texHash64`);
  `:1716-1730` (`decodeTexAny`); `:1635-1665` (`decodeTex16`); `:1605-1613` (twiddle table).
- **Client:** `web/webgpu/sprite-client.mjs:218, 229-232` (texKey/onTX64), `:264-345` (onSTAF
  + surrogate-tcw), `:348` (texCache stat); `web/webgpu-test.html:384-411` (texId-keyed
  texMgr shim), `:416-423` (renderStaf), `:1094-1114` (TX64/STAF route).
- **Proven renderer (untouched):** `web/webgpu/pvr2-renderer.mjs:187-216, 325`;
  contract `:188`; reference shape `web/webgpu/ta-parser.mjs:36-250`.
- **Mirror path assumes VRAM (why STAF, not Option 2):** `web/webgpu/frame-decoder.mjs:89-145`
  (applyFrame), `:128-141` (VRAM populate), `:78-87` (SYNC-invalidate fix);
  `web/webgpu/texture-manager.mjs:106-178` (VRAM decode by TCW), `:92-104` (dirty-page invalidate).
- **Relay:** `relay/src/fanout.rs:186` (MCSV short-circuit site), `:193-229` (decompress +
  apply_dirty_pages), `:540` (state-only drop); `relay/src/protocol.rs:42-47` (is_state_frame),
  `:113-159` (apply_dirty_pages).
- **Bandwidth:** `core/network/maplecast_mirror.cpp:2727-2764` (STAFMEASURE,
  `/dev/shm/mc_staf.log`); `docs/STRIPPED-TA-DESIGN.md §4.1-4.3, §8.2, §8.5`.
- **Design / grounding:** `docs/STRIPPED-TA-DESIGN.md`; `docs/RENDER-MASTER-PLAN-V2.md §0`;
  `.claude/agents/mvc2-sh4-re-expert.md` (render data path, §2.5 textures, bandwidth ladder).
