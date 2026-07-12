# RENDER-STATE appendix 01 — Client sprite/texture pipeline ledger

> Produced 2026-07-08 by the mvc2-sprite-render expert during the RENDER-STATE ledger sweep.
> Line numbers pinned to `feat/render-replica-live` HEAD ~9f86257e3 as read that day (lines drift; function names also given).

> **2026-07-11 — shipping-config update (docs/RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md).**
> The browser DEFAULT (https://nobd.net/webgpu-test.html, `bodysrc=wasm`) now ships **render_frame
> (the transpile chain in §1 Authoritative)** as the body drawer, live on the ZCS2 streaming-zstd
> wire. The **whole-sprite / emitter "sprite machine" was A/B-rejected (re_kb/74, re-verified
> 2026-07-11)** — it renders worse and its wire-size edge is gone, so the tombstones below stand
> confirmed. Body split: the server char-strips the para5 body quads (banks {82,83,88,89},
> `CHARSTRIP=1`); render_frame draws them locally from the folded STM2 body state (`STATE_MERGE=1`),
> byte-exact. **Stage is NOT stripped (`STAGESTRIP=0`) — it rides the wire TA/VRAM pixel-perfect
> (static ⇒ ~0 cost under zstd streaming-window dedup)**; effects/projectiles/supers/HUD ride the
> wire too. Ranks 1–2 (native charpass / lockstep) unchanged as the native frontier.

## 1. CHARACTER BODIES

### Authoritative
- **Phase 2a native char-pass** — `core/network/gsta_charpass.cpp` (`run`/`run_live`/`selftest_from_env`; ENTRY_PC 0x8C030858, SP 0x8C00F3EC, RET 0x8C039648), wired at `maplecast_mirror.cpp:4812-4823` (`gstaApplyFrame`, `MAPLECAST_GSTA_NATIVE_CHARPASS=1`, default OFF). Commit 483511fef. Declared supersessor of render_frame + gstaDecodeBodies hand-assembly + P3D injection for the char pass (stage + HUD stay; HUD = Phase 2b).
- Because the flag defaults OFF, the **default-shipping** body drawer is the transpile chain: `render_frame` (`core/network/gsta_render_frame.c/.h` in the native client, commit 94b518328; `web/render-replica/render_frame.{mjs,wasm}` in browser) + texel decode `gstaDecodeBodies` (`maplecast_mirror.cpp:3930`) / `ensureBodyTextures` (`web/render-replica/body_decoder.mjs:326`) — kept in explicit LOCKSTEP (comments at mirror.cpp:4092/4130/4181).

### Gate status
- Phase 2a: byte gate CLOSED in-process (md5 be1377d28b3d4bf624c18590dae21ce5 == engine == standalone runner, 4519 parcels, ~5.8ms). Precursors: Step-3 gate 1dc03e611; `--drop-scratch` falsifier. NOT pixel-validated live (see appendix 02 live A/B result: bodies invisible under the flag as of 2026-07-08).
- Transpile chain: sprite machine CLOSED 5/5 at 189544592 — TEXEL 100.0% (WRONG=0 ZERO=0, 893 quads), ANCHOR 99.0% cov LAG=0, BIT15 emission 100% on out-of-sample `_live4`. Geometry: positions byte-exact vs ASMTRACE (2b6f74167, re_kb/10); PCW/ISP/TSP/TCW 9/9 byte-exact (re_kb/42 citations).

### Superseded alternatives (tombstones)

| Superseded impl | What | Superseded by / tombstone |
|---|---|---|
| Whole-sprite path `sprite-client.mjs buildDrawList` (:1042) + baked PLxx.{json,png} | one composite quad per sprite_id; prod lean path | Not dead (still prod lean client) but for FIDELITY superseded by transpile/charpass — can never be byte-exact (RGB bake, tint-approx flash, `_held` fallback :1046-1057). re_kb/54; memory project_render_pipeline_state |
| Emitter part-assembly `buildEmitterDrawList` (:1506) / `buildAssemblyDrawList` (:1257) / `_buildAssemblyDrawListLegacy` (:1263) | JS port of body walker, geometry 0.00px (re_kb/08) | Retired as drawer by render_frame.wasm at 364f9ce1e; whole hand-assembly class superseded by 483511fef |
| Legacy assembly builder (`window._emitterPort===false` A/B) | pre-port builder | buildEmitterDrawList (faithful loc_8c033e90 port), a30aa406b |
| body_decoder hand-modeled carve generations (x-first twiddle / linear-slice / hardcoded-32 pitch) | pre-2026-06-14 | FAITHFUL verbatim transpile (re_kb/28, 4b729a6f3) → per-tile RE-TILE → carve-fix series → 189544592 |
| replay.html drawer flip-flops (render_frame → emitter 8695e907f/59095d725 → render_frame 364f9ce1e) | | Final: transpile draws; native endgame = Phase 2a → lockstep |
| GSTA injection into a live SH4 | per-frame state inject | DEAD-END (crashes; memory project_state_replica_injection_deadend; docs/STATE-REPLICA-PLAN.md) |

### Solved-bug catalog (bodies) — do not re-solve

Geometry / facing:
1. **Facing polarity is ROM-derived, facing=1 = faces RIGHT** — setter loc_8c0d97ee; sprite-client.mjs:1535-1564; re_kb/09.
2. **texU mirror = raw `facing XOR 0x4000`**, decoupled from calibrated posReflect — the "detached forearm/half-body-swap" bug; sprite-client.mjs:1692-1705, commit 6a5574e2b; engine loc_8c0346c4 `neg r8` (re_kb/08). render_frame: X-span couples to facing (facing=1 spans bx−W..bx), 1f5f31f04.
3. **Reflect the pen ORIGIN only (`tlx = 2A − tlx`), never the rect** — the ±50-70px side offset; re_kb/08 emitter_reflection_axis.
4. **sprite_id bit15 = scale-walker dispatch (loc_8c0348c8), NOT flip** — sprite-client.mjs:856 (sid_xform); ROM-confirmed 42fdf4a54; emission-gate dac30acfd.
5. **tileScale = 1.0 (full CPS 5/3 × 15/7), not 0.5** — sprite-client.mjs:1522-1532; re_kb/08.
6. **Atlas parts stored bottom-up ⇒ V-flip in sampling only** (`_emitFlipY`, re_kb/08).
7. **Full-span (sw·8×sh·8), not logical-crop** — logical-crop was the upside-down regression (memories reference_roster_logical_crop_rebake + project_emitter_facing_fix). Baker = tools/extract_gfx1_atlas.py.
8. **Intra-assembly z: record 0 = FRONT** (engine Z=1/W, W bumps per part) — sprite-client.mjs:1815-1819, 260663171, re_kb/38.
9. **Per-object depth z=1/W in replica-live** — d7a5d88c1 (cape-through-body).
10. **Body col-rank ASC for BOTH facings** (617ec6f86); wide-part storage-order re_kb/21 (27cc1dd1f).

Texel carve lineage (the single most re-hit bug family — FINAL answer is #5):
1. Carve pitch = m=W/cols ∈ {8,16,32}, not hardcoded 32 — 90533abc4, re_kb/42. Fix sites: gstaDecodeBodies + ensureBodyTextures.
2. W>32∧H>32 SQUARE parts: copy native 512B chunk at tile-grid twiddle index — 274d09474, re_kb/43.
3. re_kb/44's "non-square → linear slice" (1ee855655) was WRONG, corrected by:
4. Non-square multi-tile grids: **Y-FIRST tile twiddle** (`gstaTwTileYFirst` mirror.cpp:3904 / `twTileYFirst` body_decoder.mjs:172) — c2a89e5db, re_kb/46; square grids ALSO Y-first + **facing-mirror column reversal** (`storageCol = mir ? cols-1-screenCol : screenCol`) — ff39ee18f.
5. **FINAL: engine chunk order = 2-ROW BANDS** (`by=row&~1; k=by*Tw+col*bh+(row-by)`) — body_decoder.mjs:478-487 + gstaDecodeBodies lockstep; 189544592 item 3. Band==Y-first for any grid with a dim ≤2 — why every earlier "validation" self-confirmed.
6. **Desc-keyed carve** (per-quad emit-time [m,cx,ry,flags] via `render_frame_quad_srcdesc`/`gsta_quad_srcdesc`, mirror.cpp:3950) — kills same-sel multi-instance scramble; 8a1debead item 3.
7. **LZSS = faithful loc_8c0354c0 transpile** (`decodeA` body_decoder.mjs:201 == extract_gfx1_atlas.py decodeA), byte-exact 6/6 live + 1533/1533 sels — re_kb/28. The older "offline LZSS is a dead end" claim is SUPERSEDED (decoder-polarity bug, re_kb/08).

Palette / TA:
1. **Sprite BASE COLOR at +16 must be written** (zero face color × MODULATE ⇒ full discard, black canvas) — bc348be0d, wasm_entry_frame.c.
2. **Preserve resident rectab TCW PalSelect; never override with the static even-bank formula** (Cable-blue) — f2e81a82f, gen_submit_params.c finalize_body, re_kb/41 (0.00% mismatch over 41k tiles).
3. **"Purple Cable" is engine-faithful, not a bug** — re_kb/48 (bank24 IS purple; identify chars by char_id never color).
4. **Body double-buffer parity pin, SCOPED to [0x88000,0x8C000)** — blanket pin corrupted effect bands; mirror.cpp:4429-4475, 189544592 item 1.
5. **BTCW tail consumed verbatim by render_frame** — mirror.cpp:4619-4630; extended 2026-07-04 to satellite/effect nodes.
6. **PALSEL carry-on-nearmiss** in the recorder — 189544592 item 4; root fix (current-parity entry read) still OPEN; pipeline mapped re_kb/62.
7. **Char-pass capture, not HUD pass** — read-set/idxtab must snapshot on the CHARACTER StartRender; server capture-gate attempt REVERTED (fc45c5532); fix was client-side (2181d1356, re_kb/50). Snapshot BEFORE publish (dac30acfd — ±1-frame tearing).
8. **Super over-tile: stale node+0xDC/tiledesc during hitstop ⇒ per-frame effect rectab-template + bit15 scale-walker dispatch client-side** — re_kb/50 (7dd9347e8, 0be71e220, a2efae106); 0x85xxx never-engine body blocks culled (8342a574d, re_kb/51).

Whole-sprite path fixes (kept even though fidelity-superseded): anisotropic CPS (a4a9133ce; :1060-1067), per-char zoom +0x50/54 with `_sane` guard, mirror-asymmetric anchor on flip, sort by cid then z (:1225 unshift scattering), flicker-bridge default OFF (e75054f75), sprite_id render-phase latch on the wire (c7c25ac44).

### Open gaps (bodies)
- Phase 2a-live: entry ctx in prefix / palette repoint / VRAM coherence / splice order / frozen live pixel gate (483511fef NEXT) — see appendix 02.
- Palsel root fix (current-parity entry read) — 189544592.
- re_kb/51 gsta_body_part_transposition (2-part clean transposition) — OPEN; mooted if 2a defaults.
- Emitter historical opens (moot while retired): emitter_flip_unvalidated, emitter_roster_rebake_pending (re_kb/08).

## 2. SATELLITES / PROJECTILES / EFFECTS

### Authoritative
- Sprite-machine satellites (cat 1..4): same transpile chain — `gen_render_satellite.c` (transpiled loc_8c030af8, 1b8021e2e); slot-table walk = the draw list (readAllDrawn; re_kb/61 proves the slot table only holds cats 0-4). Texels: same desc-keyed carve; **bit15/effect quads never staged** (resident-VRAM-backed; 189544592 item 2, body_decoder.mjs:374).
- 3D-machine (NaomiLib) effects: Phase-A P3D parcel injection — oracle capture (MAPLECAST_POLY3D/P3D_CLS) → wire tail → OP-prefix injection (mirror.cpp:4641-4648, 5012-5103). Draw path RE'd in re_kb/64. **Under Phase 2a these ride the native TA natively — see appendix 03 §2.**

### Gate status
- Sprite-machine satellites: inside the 5/5 close (189544592: pal17 projectile texels 243/243; LAG 348→0 via dac30acfd); hit-flash byte-exact (8a1debead); projectiles <0.005px vs ASMTRACE (e3bcde3dc).
- 3D effects: NOT byte-gated — live measure only: olive-wedge occupancy 36.9%→2.30% (12bb53a57); TR capture drop root-caused (re_kb/65: TR = slot2/class 0xAC deferred; default P3D_CLS={0x10} rejected them). Declared NEXT ARC pre-2a; DEAD as a campaign under 2a.

### Superseded (tombstones)
| Superseded | Superseded by |
|---|---|
| OBJS pool-walker (7ab274fb3) → caps 48/200/255 | slot-table readAllDrawn (fd4a68e95) — the game's real draw list |
| Owner-relative/proximity 'auto' anchor + far>130 heuristic | own-origin rule (node+0xE0/E4 + baked dx/dy), sprite-client.mjs:1130-1188; PATH A node+0x178 hotspot REJECTED (?v=54, :1171-1175) |
| Owner-facing for satellites | object's OWN flip node+0x130 in sid bit 0x8000 (3fc1fe130) |
| Emitter isEffect skip stub (re_kb/37 foot-garble; 1251ffd81) → fx_atlas dispatch (ad4fdd915, re_kb/40, FX_CID=0xFE0) | Effect-Poly class DISSOLVED: re_kb/49 v3 — zero Effect-Poly nodes in measured supers; garble was slot-walk over-tiling (re_kb/50). Real effects = sprite-machine bit15 quads (closed) + 3D-machine |
| EFCT packet path (f7122cd9e) + live TA effects pass (b09a79d58) + hit-spark pass (e56a71c48) | dev overlays; default OFF (a9c7c1339, VRAM phase skew) |
| Effect quads via idxtab fallback | BTCW-for-effects unconditional + phantom cull (8a1debead item 1; revised dac30acfd item 2) |

### Solved-bug catalog (satellites/effects)
1. **Visibility gate = node+0x12C != 0** (engine clears it FIRST on free) + coherent slot-table snapshot — 53e14f7e0; re_kb/61 (FREE loc_8c0450c0).
2. BTCW override unconditional for effects + over-emit cull — 8a1debead/dac30acfd.
3. **Palette repoint discriminator**: repoint to private Dat_Pal bank ONLY when engine palsel == slot base bank {16,24,32,40,48,56} — else hit-flash invisible in victim's colors; mirror.cpp:4670-4703.
4. Super-effect palette + UV span — f1de957a1.
5. **TA-list legality: injected OPAQUE parcels go in the OP prefix before its EOL, never appended into an open TR list** (flycast FSM swallows the boundary EOL mid-strip) — 12bb53a57.
6. sel==0xFF blank-record over-emit (7d2ee95d1); fx effect_key range guard ≥0x3D8 (1eaa7f348).
7. P3D class filter: TR effects = ListType-2 → slot2/class 0xAC — capture must include 0xAC (re_kb/65).
8. Whole-sprite object rules: sentinel sids 0x7fff/0xffff/0 skipped, OOB cull −64..704/−64..544, z = slot-table layer, `_objCfg` nudge defaults identity.

### Open gaps
- 3D-machine injection completeness (~2% residual olive) — DEAD as campaign if 2a defaults; alive only while transpile is the shipping path.
- fx_atlas node→directory binding UNPROVEN offline (re_kb/40) — moot while the class stays dissolved.
- Whole-sprite `_objMiss` shared-effect sprites never fully baked (diagnostic by design).

## 3. TEXTURE / PALETTE DERIVATION

### Authoritative
- Body/satellite texels: faithful transpile decode — decodeA LZSS → per-tile re-tile carve (desc-keyed [m,cx,ry,flags], Y-first twiddle, 2-row-band chunk order, facing-mirror column reversal) in LOCKSTEP pair gstaDecodeBodies / ensureBodyTextures. Effect/bit15 texels: resident VRAM, never decoded. Closure = 189544592.
- Palettes: live PVR palette from the wire (on-change PVR tail da8642e6d/0df4cb889); TCW PalSelect preserved from resident rectab (f2e81a82f); repoint discriminator; palsel carry. Residency/patch pipeline mapped re_kb/62.
- Offline atlas production: tools/extract_gfx1_atlas.py (full-span, byte-exact; 100% roster coverage per tools/scan_atlas_coverage.py), tools/rip_gfx2_assembly.py, web/webgpu/bake.mjs (STATIC_HOLD=3). ROM-derived outputs scp-only, never committed.
- Exact-palette LUT (whole-sprite): tools/rgb_to_indexed.py → PLxx_idx.png + PLxx_lut.json; sprite-gpu.mjs setCharLUT/:310, setIndexedAtlas/:320, LUT_SHADER/:97. 33be8b30b (Ryu verified pixel-identical; roster gate never run — open).
- Skins: setSkin(charId,bodyColors16) sprite-gpu.mjs:315 overrides the body bank per group. NOTE: the engine allocates odd sibling banks dynamically (re_kb/41) — any client forcing the static formula is wrong.

### Superseded (tombstones)
| Superseded | Superseded by |
|---|---|
| PARTDUMP/DECODEHOOK/QUADCAPTURE live pixel capture as sole pixel source + "offline LZSS dead end" claim | offline faithful LZSS (re_kb/08 REVERSES; re_kb/28) |
| min-square/tile_to_indices twiddle in rip_gfx2_assembly.py | ConvertTwiddlePal4 full-part detwiddle (re_kb/08) |
| Hand-modeled carve generations | desc-keyed + Y-first + 2-row-band (re_kb/42→43→44-corrected→46→ff39ee18f→189544592) |
| TX64/STAF content-addressed texture channel + on-demand VRAM streaming plan | pixel-shipping dead (36-88 Mbps); GSTA prefix ships full 8MB VRAM once (re_kb/54) |
| VCACHE (3bdac4da2, f724c4d6c) | same verdict |
| Real-ESRGAN HD upscaler (e28c29a86) | orthogonal, unused in byte-exact chain |

### Extra solved bugs
- In-RAM PLDAT is relocated/re-packed at load — validate offline decode against DISC GFX1 or in-RAM node+0x15C base, never disc==RAM offsets (re_kb/28).
- GFX selector at record +6, not +4 (5dcf53e39).
- GFX1/GFX2 must ship at REAL extent + fresh on body load/tag-in (f03f3675d, b3823ba28, 9a5b128d2) — the tag-in scramble family.
- Single-frame signoff fails on pose-dependent carve bugs — verify ≥12 poses (re_kb/46 META).

## 4. WEBGPU CONSUMERS

### Authoritative
- **pvr2-renderer.mjs (PVR2Renderer :48)** — flycast's PVR2 port; the ONE trusted browser rasterizer (TA video, replay.html, DIFF truth, STAF surface, render_frame's TA consumer).
- **sprite-gpu.mjs (SpriteGPU :136)** — whole-sprite/emitter quad drawer (RGB recolor + tint, LUT shader, additive/spark pipelines, maxGroups=8/maxInst=64). Prod lean path; NOT byte-exact by design.
- **STAF path** (webgpu-test.html :398 + texMgr surrogate shim): de-indexed strips so PVR2Renderer triangulates (4499d8195); in-match gating (2de376cb4, dfd308f36, 9e670f452 — char-select 5fps flood). Parked/hidden in UI (f44cd7a96).
- **DIFF v7** (webgpu-test.html :709-1076): green=TA/red=ours/yellow=match; WebGPU READBACK FIX — differ reads 2D mirror canvases fed at each present site (:966, :506), never the presented swap-chain.
- **Native consumer (endgame):** flycast's own renderer via clientReceiveGsta → renderer->Process (94b518328, mainui.cpp).

### Consumer solved-bug catalog
1. Never read the presented WebGPU canvas for diffing — 2D mirrors at present sites.
2. STAF must ship de-indexed strips (raw indexed verts garbled supers).
3. In-match gating of STAF/HUDF emit + filter-before-decode.
4. STAF snap[0] carries render size (all-zero snapshot renders 32×32).
5. One canvas per mode (47240ba44); auto-switch debounce on tag-in (68330ba59); skip TA video decode in sprite mode (e6fcd6e3a).
6. sprite-gpu oversized-atlas crash-proofing (8548d047d); LUT MAXB=8 must match shader.
7. Renderer groups CONSECUTIVE same-cid sprites and drops chars past maxGroups(8) — draw lists must sort by cid.

### Open gaps
- Browser replica-live HUD/overlay pt4/pt7 class (re_kb/54 hybrid verdict) + Phase 2b HUD.
- Cockpit knobs (`_asmCfg`,`_objAnchor`,`_objCfg`,`_emitterPort`,`_emitFaceInv`,`_emitFlipY`) belong to the RETIRED emitter path — mark historical so nobody "re-calibrates" it.
- Browser replay.html default is still `'emitter'` (:1811) despite the standing render_frame promotion decision (docs/GSTA-FINDINGS-FOR-BROWSER.md).

## Cross-cutting

1. **The five body implementations, ranked by authority:** (1) Phase 2a native charpass [byte-gated offline, flag-OFF, live pixel gap] > (2) render_frame transpile + lockstep decoders [byte-gated, DEFAULT] > (3) emitter [geometry 0.00px, RETIRED as drawer] > (4) whole-sprite [prod lean, approximation by design] > (5) legacy A/B relic. Any rebuild session starts at (1)/(2). [Post-sweep note: lockstep-mirror bc16af338 sits above all of these — see RENDER-STATE.md §1.] **[2026-07-11: render_frame (2) is the CONFIRMED live BROWSER default on the ZCS2 streaming-zstd wire; the whole-sprite/emitter "sprite machine" was A/B-rejected (re_kb/74 — renders worse, wire edge gone). Stage rides the wire pixel-perfect (STAGESTRIP=0); bodies are char-stripped + drawn locally. docs/RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md.]**
2. **Lockstep invariant (most dangerous re-hit):** gstaDecodeBodies (mirror.cpp) and ensureBodyTextures (body_decoder.mjs) implement the SAME carve and must be edited together (stated in-code mirror.cpp:4092/4130/4181, body_decoder.mjs:478-487).
3. **Gates per subsystem:** bodies/texels = replay + texel_gate on `_live*` corpus vs 7200 mirror; charpass = selftest md5; emitter (if revived) = validate_emitter_geom.py 0.00px; browser = DIFF v7. Eyeballing is never the gate (≥12 poses, re_kb/46).
4. **Deploy rules that bit us:** ROM-derived atlases scp-only; `?v=` bump every module change (e8fcb8011, 6cf93d513 were real regressions); prod web has been ahead of git (c38dc7163 snapshot discipline).
