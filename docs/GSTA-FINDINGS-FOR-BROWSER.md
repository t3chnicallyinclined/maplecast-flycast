# GSTA native-client findings → WebGPU / sprite-client (browser) port notes

Running notes: measured render findings from the native GSTA client work (`build/flycast.exe`
GSTA mode, render_frame → flycast's own renderer) that apply to the **browser** render-replica
(`web/render-replica/replay.html` + `web/webgpu/*`). The browser is where the original garble
(HUD, bodies, bars) lives. Every item below is CONFIRMED-BY-MEASUREMENT against real flycast
(A/B TA diff / engine VRAM+palette diff / ASMTRACE) unless tagged.

Keep appending as the stage/z-order work lands.

---

## ★ THE BIG LEVER — switch the browser body drawer to render_frame
The browser defaults to the **EMITTER** (`window.__bodyMode='emitter'`, `sprite-client.mjs
buildEmitterDrawList`) — unvalidated flip/decode/roster, the source of the body/HUD garble.
The GSTA work has now **hardened `render_frame` + `body_decoder.mjs` into a pixel-perfect path**
(verified through flycast's own `twop`). The browser already has this path wired:
`replay.html?bodymode=render_frame`. **Switching the default to render_frame inherits ALL the
fixes below for free** — this is the single biggest browser win. (Wire-cost: render_frame needs
the rectab/idxtab dynamic regions, which the replica-live wire already ships.)

---

## Shared-code fixes (ALREADY in the browser's render_frame path)
These landed in files the browser render_frame path uses (`web/render-replica/body_decoder.mjs`,
the transpile in `tools/render-replica-poc/gen_*.c`). render_frame-mode browser gets them now;
the EMITTER path does NOT (see next section).

- **Palette bank PRESERVE** (`gen_submit_params.c finalize_body`, commit `f2e81a82f`): do NOT
  override the resident rectab TCW PalSelect with the static formula `16*(char_pair+1)+8*player_side`.
  The engine allocates banks dynamically (`loc_8c124bd8`) and uses ODD siblings (17,25); the static
  formula only makes even banks → Cable-all-blue. Preserve the wire'd bank.
- **Carve multi-row pitch** (`body_decoder.mjs ensureBodyTextures`, commit `90533abc4`): per-tile
  pitch `m = W/cols = H/rows ∈ {8,16,32}`, NOT a hardcoded 32. Fixed-32 over-steps for m<32 parts
  → grey rows. (re_kb/42)
- **Carve non-square** (`body_decoder.mjs`, commit `1ee855655`): the native-chunk (twTile) carve is
  correct ONLY for SQUARE grids (`Tw==Th`). Non-square parts (e.g. 64×128 2×4) must fall back to the
  linear-slice carve. Gate the chunk path on `Tw==Th`. (re_kb/44, corrects re_kb/43)
- **texU mirror = `facing XOR 0x4000`** (`render_frame.c`), NOT `0x4000`-alone (measured 35% vs 1.46%
  U-mismatch) and NOT `!facing`. (re_kb/24, vindicated by A/B diff)

## EMITTER-specific (only if the browser keeps the emitter instead of the lever above)
The emitter (`sprite-client.mjs` / `sprite-gpu.mjs`) has its OWN palette/carve/facing handling and
did NOT get the shared fixes. If the emitter stays the default, port each fix above into it:
palette-bank preserve, the m-carve, the non-square Tw==Th gate, facing XOR 0x4000.

## Z-ORDER / LISTS  — likely the browser HUD/bars garble  [IN PROGRESS, native]
Native `gstaEmitSpriteTA` forced ALL quads into one Translucent list, discarding the engine's
per-object OPAQUE/PUNCH-THROUGH/TRANSLUCENT structure. Bodies ARE genuinely TR (ListType=2,
TSP `0x949004D2`) — confirmed. The fix in progress: emit each object in its real engine list (from
the PCW ParaType/ListType) so OP→PT→TR draw in order with correct depth.
**Browser equivalent:** `buildHudTA` flattens every HUD-quad z to 1.0 and draw order = submit order
→ the para4 red backing covers the para5 team-color fill (the "all-red bars"). The per-object
list/z fix should port directly.

**CONFIRMED list assignments (native z-order pass, commit `ed11835b2`, re_kb/45):**
- **STAGE = OPAQUE** — ListType 0, ParaType 4 (Polygon). PCW high byte `0x80`.
- **BODIES = TRANSLUCENT** — ListType 2, ParaType 5 (Sprite). `gen_submit_params finalize_body`
  sets `PCW |= 0x02000000` (bit 25). TSP `0x949004D2`.
- flycast `ta_handle_cmd` (`core/hw/pvr/ta.cpp:222-249`) latches `pcw.ListType` on the first param of
  each list, resets on End_Of_List; the renderer draws **OP→PT→TR**.
- Native emit order that works: `[stage OP polys][EOL][body TR sprites]` → stage behind bodies.
  Live: `op=930 pt=0 tr=87` (was `op=0`). Browser: emit each object in its real PCW list + a proper
  EOL between lists, instead of one flat TR list with z=1.0.

## STAGE / BACKGROUND  [native FIRST VERSION landed — commit `ed11835b2`, re_kb/45]
Native `gsta_stage.cpp` is a faithful C++ PORT of `web/webgpu/stage-client.mjs _buildFromTA`
(loads `STGxx_ta.json`, reads `stage_id`@`0x8C289638` + camera M2@`0x8C2D6AD8`/M1@`0x8C2D6B18` from
the seeded RAM, re-projects world-authored meshes through the live camera, emits OP-list polys).
Stage verts 1881/1893 = 99.4% vs engine; bottom edge exact. NO wire gap (stage_id + camera already
shipped; other stages just need the OFFLINE `bake_stage_from_ta.py` per-stage TA bake).

**★ Browser-applicable bug found:** the native port inherited a **floor-to-white intensity override**
from `stage-client.mjs` that washed the dark grid bright and hid the blue floor — **disproven by the
engine-TA ground-truth raster** and replaced with the bake's real **per-vertex RGB**. The browser
`stage-client.mjs` very likely STILL has this floor-to-white override — check `_buildFromTA` and use
per-vertex colour, validated against `_stage_gt/GROUNDTRUTH_engine_ta_STG0B.png` (the engine TA bake).
## STAGE FLOOR/DECK black — ROOT-CAUSED + FIXED  [commit pending, re_kb/47, `finding:gsta_stage_floor_cull_fix`]
The "lower blue-floor deck renders dark/missing" item above was **NOT a per-vertex-colour problem** —
that earlier guess (deck bottom-row RGB ~0) was **wrong**. Per-mesh measurement vs the engine TA found
the real cause:

**The BLUE LOWER-DECK FLOOR (mesh3, texture t02 tcw `0xa0000`) was MARGIN-culled.** It is 2 huge
triangles spanning X ±6822 that cross the visible bottom band (baked screen Y 362..585). The native
emit rejected a whole triangle if ANY vertex left `[-MARGIN(800), 640/480+MARGIN]`, so the giant floor
quad was dropped entirely. The engine instead **submits the full quad and lets the PowerVR guard-band
clip it** (the captured engine TA carries mesh3 at full ±6822 extent). Also `mesh0` (green deck) has 8
grazing/behind-camera verts the engine clamps to `1/w=10` with screen XY ≈ −1.4e7 (`pos[2]==10`
sentinel), and the whole-tri reject killed every strip triangle touching one.

Measured lower-deck band coverage (y 330..410): engine GT **0.41**, native BEFORE **0.03** (93%
missing), native AFTER **0.49–0.58** across 17 frames, rgb now **blue-dominant** `[~40,~78,~148]`
matching engine `[3,18,70]`. All 4 stage bands hue-match the engine (grid green-dom, deck+floor
blue-dom).

**Fix (engine-faithful, applies to the browser too):** drop the whole-tri MARGIN reject. Reject a
vertex ONLY if non-finite or `|screen| > 1e6` (the `1/w=10` sentinel garbage); then keep a triangle
iff its screen **bounding box overlaps** the visible frame (±64px slack). Let the rasterizer clip the
rest. The browser `stage-client.mjs _buildFromTA` has the **same `MARGIN`-based per-vertex cull** —
apply the same change there or its blue floor is culled identically. The "~12 culled props" were these
world-authored deck/floor pieces, **not** the local props (69 props rendered 68/69; the 1 miss is a
deck piece entirely above the frame, engine-culled too).

## STAGE PATH must resolve from the BINARY dir  [commit pending, re_kb/47, `finding:gsta_stage_path_from_binary`]
Native `gstaStageEnsureLoaded` resolved `STGxx_ta.json` only via the env override + cwd-relative bases,
so launching `build/flycast.exe` from `$HOME` silently failed to load the stage. Fixed: prepend the
**executable's own dir** (`GetModuleFileNameA` / `readlink /proc/self/exe`) and `exeDir/../atlas/stages`
etc. to the candidate bases. Browser analogue: resolve stage assets relative to the **module URL**
(`import.meta.url`), not a page-relative path, so the page works regardless of where it's served from.

## META-LESSON — a hand-rolled validator can lie
`tools/render-replica-poc/_validate_all_multi.mjs` reported false `diff=0` because its reference
assumed single-blob storage (the native non-square carve was "self-consistent under the wrong
model"). It was only caught by diffing through **flycast's REAL `twop`** (`core/rend/texconv.cpp
ConvertTwiddlePal4`). **Validate browser decode against flycast's own twop / the engine VRAM, never
a hand-authored reference.** Same applies to `web/webgpu-test.html` DIFF tooling.

## NOT applicable to the browser
- **Texture↔TA threading race** (commit `b82834447`): native-only (WS thread decoded into shared
  `vram[]` while the render thread sampled a prior frame). The browser applies tiles synchronously
  per frame, so it never had this. Listed so it isn't mistakenly "ported."

## ★ "PURPLE CABLE" IS NOT A BUG — the engine itself draws this Cable purple  [CONFIRMED-BY-MEASUREMENT 2026-06-19]
The recurring "Cable's palette looks wrong (purple, not blue/grey military)" report was investigated
end-to-end against the engine arbiter and is **a correct render, not a defect**. Do NOT "fix" the
Cable palette in the browser — it would diverge FROM the engine.

**Identification (char struct, page-616, NOT the color read):** slot-0 in-match has
P1C1 = **Storm** `char_id 0x2A` (right, facing 0, palette 0, bank 16) and
P2C1 = **Cable** `char_id 0x17` (left, facing 1, **palette index 1**, body bank 24). The purple
left-side character with yellow boots is P2C1 Cable. (`+0x001` char_id, `+0x52D` palette,
`+0x110` facing, `+0xE0/E4` screen — read live from the GSTA prefix `ram16` region.)

**The decisive chain, all measured:**
1. **Engine real TA** (mirror client `MAPLECAST_DUMP_TA` on the SAME headless, 806 frames): Cable's
   PAL4 body sprites (`tcw` PalSelect) = **bank 24**, never bank 25. Full engine bank set across all
   frames = `{16,17,18,24,25}` — IDENTICAL to the GSTA client's set. No bank-selection divergence.
2. **Engine PVR PALETTE_RAM** (the `pvr_regs` shipped in the GSTA prefix, which IS the engine's own
   palette): **bank 24 = purple/yellow** (`fc7d`=rgb204,119,221 / `fff1`=yellow boots), **bank 25 =
   blue** (Cable's *standard* military palette). This Cable selected palette index 1 → bank 24 → purple.
   Banks 24 AND 25 are both correctly populated and shipped (this is NOT the re_kb/25 palette gap).
3. **Engine pixel ground truth**: rendered the engine's real TA + engine VRAM + engine palette through
   flycast's own pvr2 path (`tools/render-replica-poc/render_ta.mjs --mirror`, the DIFF "truth" canvas)
   → the engine draws **purple Cable with yellow boots**, pixel-matching the GSTA client.
4. **GSTA faithfulness**: `render_frame finalize_body` (`gen_submit_params.c`) PRESERVES the resident
   rectab TCW PalSelect verbatim (only injects when `resident_pal==0`), so the GSTA bank == the engine
   bank by construction. Costume-color histograms (Cable greys/peach, Storm white/grey/skin) match
   engine-vs-GSTA across the 12-frame spread.

**Lesson (matches the project's "DO NOT trust the color read" rule):** a palette that *looks* wrong to
a human can be exactly what the engine renders (alt/team-color skin via palette index). The arbiter is
the engine's TCW bank + its PALETTE_RAM contents + its own pixel render — never intuition about a
character's "canonical" colors. **Browser note:** the browser render_frame path (`body_decoder.mjs` +
`gen_submit_params.c finalize_body`) already PRESERVES the bank, so it inherits this correct behavior;
the EMITTER path's static-formula bank (`16*(char_pair+1)+8*player_side`, even-only) would force Cable
to bank 24's even sibling regardless of palette index and is the one that gets skins wrong — another
reason to switch the browser default to render_frame (the big lever at the top).

## STORM "better but not perfect" — no measurable body divergence found  [CONFIRMED-BY-MEASUREMENT 2026-06-19]
Diffed Storm (gfx1 `c420040`) GSTA-vs-engine: blend `(4,5)`, ListType 2, texfmt 5 IDENTICAL; pal-bank
set `{16,17,18}` IDENTICAL (bank 18 = her lightning-aura sub-palette, engine-authored, appears in 1484
engine frames). Carve: GSTA-decoded VRAM body band == engine VRAM **byte-exact (0.00% diff)** on the
frame-aligned pair; the 38% on non-aligned frames is pure pose drift (the engine re-decodes a different
pose's parts into the same VRAM addresses), NOT a carve defect. The three recent cape-carve fixes
(`90533abc4` m-pitch, `1ee855655`/`c2a89e5db` non-square Y-first twiddle) already closed the cape
garble. Across the 12-frame shot spread (390/420/450/480/510/540/570/600) Storm is clean — white/grey
costume, lightning-quill hair, cape, no grey blocks. Any *residual* "not perfect" is at the
pose-coverage/animation-timing level, not a per-part render-model bug in the bank/carve/facing/blend.

## METHOD NOTE — capturing the engine ground truth for a palette/part A/B  [reusable recipe]
1. GSTA wire (port 7212): capture the `MCRR` prefix → its `ram16` region = live char structs
   (identify chars by `+0x001`); its `pvr_regs` block = engine PALETTE_RAM (`+0x1000`, 1024 ARGB4444
   entries, `+0x108`=PAL_RAM_CTRL=2 → 4444). The GSTA client's `MAPLECAST_DUMP_GSTA_VRAM=<dir>` writes
   a per-quad manifest (`sel/gfx1/pal/tcw_addr/Ax/Ay/col/row/mir`) — the exact bank render_frame picks.
2. Engine real TA (mirror client, `MAPLECAST_MIRROR_CLIENT=1 --server …:7200 MAPLECAST_DUMP_TA=1`):
   parse Sprite (ParaType 5) TCW → `pal=(tcw>>21)&0x3F` for the engine's authoritative bank per object.
3. Engine pixels: `render_ta.mjs --mirror <captured .zcst>` (self-contained SYNC+TA) renders the
   engine's own frame through pvr2 — the byte-for-byte arbiter canvas. Diff GSTA shot vs this, never
   vs a hand-rolled reference.

## SUPER / HYPER-COMBO / PROJECTILE EFFECTS — measured state + the two open gaps  [CONFIRMED-BY-MEASUREMENT 2026-06-20]

User report: "super hyper combos aren't rendering properly" (garbled pink streaks + blue blocks on
Storm's special). Investigated the native GSTA client (7212) effect path vs the engine (mirror :7200).

### What I could measure (and the hard blocker)
Captured the live engine TA from :7200 over **60s + 3min** windows (`_fxwin_long.zcst`, `_fxwin_3min.zcst`)
AND walked the slot table across every available RAM capture (`_camcap.mcrr`, `_camcap2.mcrr`,
`_satwalk2.mcrr`). RESULT — **the slot-0 match fired NO super / projectile / hitspark in any window:**
- Engine TA blend histogram, 326,751 + more sprites: **100% `lt2 SRCA->INVSRCA`, ZERO additive** (no
  `DstInstr=ONE`). (`tools/render-replica-poc/_scan_blend.mjs`.)
- OBJF stream, 10,790 frames: constant 54 objects, categories **{1,2,3} only** (no projectile/effect
  cats 7/8/9), **`is_effect` == 0 on every record**, blend byte ∈ {0 opaque, 1 alpha} — never 2 additive.
  (`_scan_objs.mjs`.)
- Slot-walk of all RAM captures: every cat 1..4 node is a **body-sprite satellite** (cape/limb/drone)
  with GFX in a resident **fighter** bank (`0c420040`, `0c508680`, …) — **ZERO nodes with GFX1/GFX2 in
  the Effect-Poly bank `0x0CED0000`.** (`_find_efx_nodes.mjs`.)

**So the user's garble could not be reproduced passively — no effect-poly node was on screen in 3+ min.**
The reconstruction on the frames I DO have is faithful: render_frame on a real camcap frame emits 6 bodies
+ 4 satellites = 103 quads, **all `SRCA->INVSRCA`, matching the engine's measured blend exactly.** The
body-sprite *projectile/satellite* path (a Cable drone, an assist, a cape) is therefore correct today.

### GAP 1 (WIRE/DECODE) — Effect-Poly bank `0x0CED0000` has a DIFFERENT header than a char GFX1 table
The GSTA texture decode (`maplecast_mirror.cpp gstaDecodeBodies` → `gstaGfx1Offsets`) treats EVERY
`gfx1` as a GFX1 LZSS offset table: `n = u32[gfx1] >> 2`, then `parts[sel] = gfx1 + u32[gfx1+sel*4]`.
MEASURED header bytes (camcap RAM):
- A real char GFX1 (`0c420040`): `head=0x3984` → `n=3681` (valid); `off[0..3]=0x3984,0x3e44,…` = relative
  byte offsets. **Decodes correctly.**
- Effect-Poly bank (`0x0CED0000`): `head=0x0CED0010`, `u32[1]=0xF1(241)`, `u32[2]=0x0CED03D8` (= the
  directory base also at `0x0CED0008`), `u32[3+]=0x0CED0578,0x0CED0598,…` = **ABSOLUTE pointers, not a
  relative-offset table.** `n = 0x0CED0010>>2 = 0x033B4004` → tripped by the `n>0x40000 -> 0` sanity →
  `sel<n` false → **part decode produces NOTHING → effect quad gets no texture → it samples whatever
  stale VRAM sits at its TCW** = the pink-streak / blue-block garble signature.

So IF an effect node's GFX1 (`node+0x15C`) is the Effect-Poly bank base (or a directory pointer into it),
the body decoder mis-parses it. The Effect-Poly bank is a `*(0x0CED0008)` directory of `0x10`-byte
entries (per `maplecast_gamestate.cpp` EFFECTS DUMP), NOT a `count + relative-offset` GFX1 table — it
needs its own decode (resolve the per-effect art blob via the directory, then decode THAT blob, which may
itself be GFX1-format). **This is UNVALIDATED — it needs a live effect node to confirm whether `+0x15C`
for an effect is the bank base, a directory pointer, or a normal GFX1 blob elsewhere.**

### GAP 2 (BLEND) — additive is a gfx1-bank HEURISTIC, not engine-derived
`gstaEmitSpriteTA_append` forces `DstInstr=ONE` (additive) for any `gfx1 ∈ [0x0CED0000,0x0CEE0000)`.
The engine actually derives an effect's additive blend from a **different submit finalize branch**
(`type==4` cell-TSP path `loc_8c124740`, gated on `r13[0x30]`) that reads the effect cell's OWN
SrcInstr/DstInstr — the lean GSTA path only runs the BODY translucent finalize (`gen_submit_params.c
finalize_body`, which MUST force `SRCA->INVSRCA` because the resident rectab is a MIX of pre-finalized
tiles `0x949004D2` and raw-template tiles `0x000004C0`/blend-0; preserving the resident blend was tried
and REJECTED — it blanks the raw tiles to `ZERO->ZERO`). The gfx1-bank additive heuristic is kept (it is
a verified no-op on all current traffic and the best available approximation per
`finding:objs_effect_blend` / `reference_mvc2_effects_bank`), but the faithful path is to run the
`type==4` cell-TSP finalize for effect tiles. Both need the same live capture to validate.

### NEEDED TO CLOSE (precise): a CONTACT-FRAME capture with an effect-poly node on screen
Fire a super/hyper/projectile in slot-0 (or any matchup) and capture while an Effect-Poly node is live,
then: (a) `_find_efx_nodes.mjs` confirms a cat 1..4 node with GFX2 `node+0x160 ∈ 0x0CED0000`; (b) read
its `+0x15C` to learn the effect art layout; (c) A/B the GSTA-emitted effect quad vs the engine TA for
that frame. Server-side residency is ALREADY handled — `maplecast_replica_live.cpp collectFreshGfx`
ships cat 1..4 GFX1+GFX2 on-change (the cat-0-only gap of re_kb/29 is CLOSED), so the art reaches the
client; the OPEN work is purely the client decode (GAP 1) + blend (GAP 2).

## EFFECT CAPTURE ATTEMPT 2026-06-20 — STILL BLOCKED: the Windows headless emu thread does NOT advance frames  [CONFIRMED-BY-MEASUREMENT]
Goal: unblock GAP 1/2 using the "frozen super" in slot 1 (== the headless autoload base
`build-headless-win/data/<rom>.state`, md5 `d87b36b3…`, 9.56MB, **byte-identical to `_1.state`** — verified,
so the autoload IS loading the claimed state). Method: reboot headless autoloading the state and capture
BOTH wires (mirror :7200 full TA = ground truth, GSTA :7212 replica-live) from the FIRST frame.

**ROOT BLOCKER (new, measured) — the emulator never runs:**
- Across 4 reboots, with the recorder connected within ~0ms of the ports listening (`tools/render-replica-poc/_cap_persist.mjs`,
  reconnect-forever) for a full 35s window each: mirror :7200 sent **1 SYNC + only JSON lobby telemetry,
  ZERO TA delta frames**; GSTA :7212 prefix **NEVER built** (`onRenderFrame` gates on `rd8(0x8C289624)!=0`;
  it stayed 0 the whole window — no in-match render ever fired).
- Mirror lobby telemetry on every poll (fresh boot AND autoload boot): **`"frame":0 "fps":0 "dirty":0`**.
  The SH4 is at frame 0 and never advances.
- Process CPU over ~3s wall: **0.9219s → 0.9375s = +0.0156s (~0.5%)**. A running SH4 (even norend headless)
  pegs CPU; ~0.5% = the emu thread is **blocked, not executing**.
- Both headless logs (fresh + autoload) **halt at `[emulator] Flycast-emu thread running, about to call
  InitAudio()`** (`core/emulator.cpp` line ~1563) with no line after — the thread is almost certainly
  **stalled inside `InitAudio()`** on this Windows headless build (binary mtime Jun 15; mirror source last
  edited Jun 20 → the binary is also STALE).

**Consequence:** the dual-stream effect diff is IMPOSSIBLE until the headless actually steps frames — no
super can play out, no Effect-Poly node can ever appear in a live capture. This is an emu/build/environment
problem, NOT the GSTA effect code. The OPEN GAP 1/2 validation remains blocked for the SAME underlying
reason as the 2026-06-20 entry above (no live effect), now with a sharper cause: the emulator isn't running.

**OFFLINE savestate probe is unreliable:** the `.state` is `FLYSAVE1` + zlib-chunked sections (RAM in 1MB
zlib blocks interleaved with VRAM/regs); naive block-ordering by 0x789c scan reads page-616/649 as all-zero,
which is NOT trustworthy (likely mis-ordered, not truly empty). Do not conclude "no effect in the state"
from the offline decode — only a LIVE in-match read settles it.

**Code change shipped this session (safe, UNVALIDATED for the positive case):** `gstaDecodeBodies`
(`core/network/maplecast_mirror.cpp`) now **SKIPS** any gfx1 in `[0x0CED0000,0x0CEE0000)` instead of
mis-parsing the Effect-Poly absolute-pointer directory as a char GFX1 LZSS table. This turns the GAP-1
garble (corrupt 512B VRAM write at the effect's TCW) into a clean no-op — strictly better than corruption,
and a verified no-op on all current traffic (zero effect nodes ever observed). The FAITHFUL directory
decode (resolve `sel`→0x10-byte entry at `*(0x0CED0008)`, upload the absolute `e8` texels at the `e4`
format) + GAP 2 (the `type==4` cell-TSP effect blend) remain OPEN, blocked on a live effect.

**TO RESUME (precise):** FIRST fix the headless emu stall (rebuild the Win headless from current source;
investigate the `InitAudio()` hang on Windows headless — confirm the emu thread advances `frame>0` /
`fps>0` on a plain fresh boot BEFORE attempting any effect capture). Only once `frame=N>0` advances live
can the frozen-super autoload play out and the GAP 1/2 dual-stream diff proceed.
