# RENDER-STATE appendix 04 — docs/*.md staleness audit + tombstone plan

> Produced 2026-07-08 by the senior-re-generalist during the RENDER-STATE ledger sweep.
> Verdicts anchored to: 189544592 (texels byte-exact), 1dc03e611 (Step-3 realcore), 483511fef (Phase 2a),
> bc16af338 (lockstep-mirror client, bit-exact ~2.4 KB/s), predict arc 3cf92ca83..3cf861b33, re_kb/25 + 40-65.

## 1. The bandwidth-number triad (most-propagated stale claim — PIN THIS)

Three incompatible "mirror bandwidth" numbers circulate:
- **~4 Mbps zstd / ~12 Mbps uncompressed** — the only tabulated, conditions-stated measurement (ARCHITECTURE.md ~line 341, Apr 2026, keyframe-heavy stream). **The anchor.**
- **~1.7 MB/s (~13.6 Mbps)** — STRIPPED-TA-DESIGN.md:10,54; RENDER-TIER1-PLAN.md:5,359; RENDER-DECISION.md:164,189; RENDER-MASTER-PLAN.md:327; V2:106. No compression state/match-phase stated; consistent with the uncompressed rate presented as "the" cost.
- **36–88 Mbps in-match** — PIXEL-PERFECT-PLANS.md:37, HANDOFF-2026-06-08.md:20, FLYCAST-RENDERER-BROWSER-SCOPE.md:67,307. Drove the "pixel-shipping DEAD" verdict; measurement conditions (resolution/compression/upscale) not stated anywhere.

INFERRED: different compression states + match phases, not a wire contradiction — but no doc says so, and the spread was decision-load-bearing. Rule: cite the anchor; the 36–88 figure is "conditions unrecovered; re-measure deterministically before citing."

## 2. Verdict table

S = SUPERSEDED (🪦 tombstone), PS = PARTIALLY-STALE (status banner), C = current, C-ref = current as reference.

| Doc | Era | Verdict | Key stale claims / superseded-by |
|---|---|---|---|
| STREAMING-OPTIONS.md | Apr 5 | **S** | visual cache "YES" (file deleted); H.264/NVENC "SECONDARY" (prod has no GPU); Options 2/3/5 "NOT IMPLEMENTED" — lockstep bc16af338 + predict now implement the client-sim family; Option 6 realized differently (GSTA ~7 KB/frame + sprite machine) |
| STRIPPED-TA-DESIGN.md | Jun 7 | **S** | 1.7 MB/s baseline; STAF/TX64 family demoted then whole pixel/geometry-shipping killed; replaced by state-only GSTA → 189544592/1dc03e611/483511fef |
| RENDER-MASTER-PLAN.md | Jun 7 | **S** | superseded same-day by V2, no banner |
| RENDER-MASTER-PLAN-V2.md | Jun 7 | **S** | PVR2Renderer-shaped-objects+TX64 constraint is not the shipped architecture (render_frame + native render won) |
| RENDER-TIER1-PLAN.md | Jun 7 | **S** | 1.7 MB/s; STAF/TX64 not the live path |
| RENDER-DECISION.md | Jun 7 | **S** | "ship MAPLECAST_VCACHE" — VCACHE later measured into the dead family; its negative verdicts (StafGL delete) stand |
| PIXEL-PERFECT-PLANS.md | Jun 8 | **S** | Option 3 template-cache never built; winners = transpiled render_frame + lockstep; 36–88 Mbps |
| FLYCAST-RENDERER-BROWSER-SCOPE.md | Jun 8 | **S** | 2-option framing; winner was option (C) not in the doc (native real render code + lockstep); Test 1 never run — mooted; 36–88 Mbps |
| CHARQ-PLAN.md | Jun 9 | **S** | CHARQ wire never product; ground-truth role → Oracle probe → ASMTRACE; MAPLECAST_CHARQ legacy |
| FRAME-ORACLE-SPEC.md | Jun 8 | **PS** | MVP correlation superseded by GenCall block-entry + charPassCapture + realBody; the instrument itself CURRENT |
| PER-OBJECT-QUAD-SPEC.md | Jun 8 | **PS** | count-based cursor segmentation not shipped mechanism; loc_8c033e90 load-time question RESOLVED (doc presents as open) |
| HANDOFF-2026-06-08.md / HANDOFF-EMITTER-2026-06-09.md | Jun 8/9 | **S** | session handoffs; whole-sprite/emitter arcs superseded |
| STATE-REPLICA-PLAN.md / WINDOWS-REPLICA-TEST.md | Jun 6 | **S** | live-SH4 injection DEAD-END (crashes); goal later achieved WITHOUT injection (1dc03e611 controlled core; bc16af338 lockstep) — tombstone must distinguish injection-dead from goal-alive |
| ROM-ASSET-CLIENT.md / -PLAN.md | Jun 4 | **S** | whole-sprite bake superseded; keep char_id→PLxx map (Cable=PL17) + prod IP note |
| BAKE-HARNESS-PLAN.md | Jun 4 | **S** | (char,sprite)→whole-image bake superseded |
| PART-ASSEMBLY-PLAN.md | Jun 6 | **S** | thesis vindicated, implementation superseded by transpiled walker |
| ASSEMBLY-DRIVEN-DESIGN.md | Jun 6 | **S** | **central blocker claim OVERTURNED**: "~86% of parts need live scratch — cannot build atlas offline" was disproven (offline LZSS closed, full roster baked from disc, re_kb/08). Classic doc-steering hazard |
| GSTA-MAPPING-HANDOFF.md | Jun 5 | **S** | "~90% verified" long done; superseded by wire-gap analysis + re_kb 40-65 |
| MVC2-WIRE-GAP-ANALYSIS.md | Jun 7 | **PS** | field inventory good; gap STATUSES stale (stage done, blend deployed, hit-flash shipped, effects closed) |
| MVC2-RECONSTRUCTION-SPEC.md | Jun 7 | **C-ref** | engine spec render_frame implements; banner: re_kb resolves its UNKNOWNs |
| MVC2-RIPPER-DESIGN.md | Jun 7 | **PS** | ripper built in variant form (extract_gfx1_atlas.py full-span); coverage 100% |
| MARVELOUS2-GFX-NOTES.md / -RE-HANDOFF.md | Jun 10/6 | **C-ref** | facts stand; re_kb supersedes on conflict |
| MVC2-MEMORY-MAP.md / MVC2-FRAMEDATA-FIELDS.md | Jun 6-7 | **C-ref** | ground truth |
| PHASE-B-WIRE-PLAN.md | Jun 13 | **PS→PARKED** | B2 gen_desc_build never built (absent from tree); wire still ~7 KB/frame; re-evaluate vs lockstep (~2.4 KB/s) before resuming |
| RENDER-REPLICA-RECORDING-FORMAT.md | Jun 14 | **PS** | wire-spec ancestor; read-set superseded by re_kb/25 + extended by 40-65 |
| GSTA-FINDINGS-FOR-BROWSER.md | Jun 29 | **C** | the one current render doc; its "big lever" (browser default emitter→render_frame) STILL OPEN (replay.html:1811 default 'emitter') |
| ARCHITECTURE.md | May 8 | **PS** | canonical for wire/pillars; entire Jun–Jul arc absent; line ~341 = the bandwidth anchor |
| COMPETITIVE-CLIENT.md | May 3 | **PS** | vision; phases 0-8 landed; doesn't know GSTA/lockstep/predict |
| WEBGPU-RENDERER.md | May 3 | **C-ref** | browser TA-mirror renderer doc; (TCW,TSP) cache measurement is load-bearing prior art |
| WASM-BUILD-GUIDE.md | Apr 6 | **PS** | third wasm (render_frame.wasm) absent |
| MATCH-DATA-PLATFORM.md | May 8 | **PS→VISION** | unstarted; premise strengthened by GSTA |
| OPTIMIZATION-PLAN.md | May 7 | **PS** | item #6 moonshot is NOW THE ACTIVE ARC (predict commits); baseline table dated |
| OPTION6-MASTER-PLAN.md / OPTION6-INSANE-IDEAS.md | May 7 | **C (archived)** | the model tombstones; optional note: idea #2 now implemented natively |
| ROLLBACK-PREDICTION.md | May 8 | **PS→REVIVED** | being implemented as the predict arc; ".mcrec PENDING — crash" row stale (validated 2026-05-07) |
| **ROLLBACK-SHELVED.md** | May 9 | **PS→UN-SHELVED (URGENT)** | un-shelve condition #1 TRIGGERED (lockstep client runs full local SH4 bit-exact); shelving rationale no longer describes the product; links nonexistent docs/ROLLBACK-RING-DESIGN.md. **Highest-risk steering doc for the current arc** |
| REPLAY-SIMPLIFICATION.md | May 7 | **S (executed)** | done, historical |
| DC-SERIALIZE-AUDIT.md | May 8 | **C-ref + PS** | findings stand; restore-into-live family re-fixed in predict GATE 0 (1045ee19a, 1a7a5bb80) — cross-link |
| SATURN-FEASIBILITY.md / NAOMI-SPIKE.md | Jun 6/May 3 | **C (parked)** | |
| CHANGELOG.md | Jun 8 | **PS (frozen)** | stops 2026-06-08; a month of render history absent; banner: ledger continues in RENDER-STATE.md |
| DEPLOYMENT / DEVELOPER-GUIDE / INPUT-LATCH / MATCHMAKING / SKIN-SYSTEM / WINDOWS-*-BUILD | ops | ops/C | no render-decision claims |

## 3. Priority order for stamping

1. **ROLLBACK-SHELVED.md** (says the current active arc doesn't fit the architecture — actively wrong).
2. **ASSEMBLY-DRIVEN-DESIGN.md** (states a disproven blocker as fact).
3. **The bandwidth triad docs** (STRIPPED-TA-DESIGN + RENDER-* + scope/handoffs citing 36-88).
4. Everything else per the table.

## 4. The serverPublish pass-contradiction — adjudicated

Two adjacent comments in `core/network/maplecast_mirror.cpp` disagree:
- **:1842-1851 (re_kb/50 era): serverPublish gets the CHARACTER pass ctx** (realBody=22..172 in [ORACLE-PASS] logs); charPassCapture (rend_start_render) sees only the HUD pass (realBody=0). **This is RIGHT for the current build/config** — later empirical measurement (re_kb/50, rebuilt 2026-06-29 headless), and the fix built on it (0be71e220/fe3bc9a44) shipped and closed.
- :1856-1864 (CHARQ era, 2026-06-09): claims the opposite ("serverPublish holds the SURVIVING HUD pass"). Stale for the current config.

INFERRED reconciliation: both were true on their respective threading configs (QueueRender single-slot drop depends on threading mode; non-threaded serverPublish runs synchronously inside EACH STARTRENDER write). Falsifiable test: count [ORACLE-PASS] flushes per vframe + realBody values under threaded vs non-threaded rendering.

Fix: era-stamp the :1856 CHARQ comment — "[2026-06-09 CHARQ-era observation; on the current headless config serverPublish DOES see the char pass — see re_kb/50 and the comment above. CHARQ mode is legacy.]"

## 5. Paste-ready tombstone headers

**STRIPPED-TA-DESIGN.md** (pattern for RENDER-MASTER-PLAN, V2, TIER1, RENDER-DECISION with superseded-by swapped):

```markdown
> **🪦 OUTCOME (2026-07-08): Superseded — never shipped as designed.**
> The stripped-TA/STAF + TX64 ship-once-texture family was demoted (RENDER-DECISION.md, 2026-06-07)
> and the whole geometry/pixel-shipping approach was then abandoned in favor of the state-only
> GSTA render replica: transpiled SH4 render (tools/render-replica-poc/render_frame.c) proved
> byte/pixel-exact vs the 7200 mirror (commit 189544592), then the engine's real render code was
> run off-SH4 from resident-only state (commit 1dc03e611) and in-process in the native client
> (commit 483511fef, MAPLECAST_GSTA_NATIVE_CHARPASS).
> **Known-wrong number in this doc:** "~1.7 MB/s" mirror cost is unlabeled-uncompressed; the
> measured wire is ~4.1 Mbps zstd / ~12 Mbps raw (ARCHITECTURE.md §wire, Apr 2026).
> Canonical ledger: docs/RENDER-STATE.md.
```

**FLYCAST-RENDERER-BROWSER-SCOPE.md**:

```markdown
> **🪦 OUTCOME (2026-07-08): Superseded — a third option not in this doc won.**
> This scoped (A) browser emitter vs (B) full wasm SH4. The shipped answer is (C): the
> transpiled/real SH4 render compiled into the NATIVE flycast client — byte gate closed at
> commits 1dc03e611 (controlled-SH4 from resident-only state) and 483511fef (in-process
> char-pass) — plus the native lockstep-mirror client (bc16af338, bit-exact at ~2.4 KB/s),
> which realizes this doc's path (B) natively instead of in wasm. Test 1 (wasm fps) was
> never run; it was mooted, not answered. The 36–88 Mbps figure at :67 carries unstated
> measurement conditions — do not cite (see RENDER-STATE.md §bandwidth).
```

**STATE-REPLICA-PLAN.md** (+ WINDOWS-REPLICA-TEST.md shortened):

```markdown
> **🪦 OUTCOME (2026-06-06, confirmed 2026-07): Injection approach dead; goal achieved otherwise.**
> Per-frame GSTA injection into a LIVE SH4 always crashes (corrupt engine-owned pointers).
> Do NOT resume this design. The goal (client renders the server's truth pixel-exact from state)
> was later achieved WITHOUT live injection: (1) the engine's real render opcodes run in a
> CONTROLLED core from resident-only GSTA state, byte-exact (commit 1dc03e611, realcore);
> (2) the lockstep-mirror client runs a full local SH4 from inputs, bit-exact (commit bc16af338).
```

**ASSEMBLY-DRIVEN-DESIGN.md**:

```markdown
> **🪦 OUTCOME (2026-07-08): Superseded; central blocker claim OVERTURNED.**
> §0's claim that ~86% of parts cannot decode offline (live scratch dependency) was later
> disproven: offline LZSS + de-twiddle closed byte-exact and the full roster was baked
> offline from disc (re_kb finding emitter_render_model, tools/re_kb/08; 2026-06-10/11).
> The renderer itself was then superseded by the transpiled render_frame path
> (commit 189544592). Treat all feasibility conclusions here as historical.
```

**ROLLBACK-SHELVED.md** — status banner, not tombstone:

```markdown
> **⚠️ STATUS (2026-07-08): UN-SHELVED.** Un-shelve condition #1 (client-side SH4 predictor)
> triggered: the lockstep-mirror client (commit bc16af338) runs a full local SH4 bit-exact,
> and the predict/rollback arc is ACTIVE on feat/render-replica-live (STAGE 0–c + CAPSTONE,
> commits 3cf92ca83..3cf861b33; MAPLECAST_PREDICT_LIVE=1). The "central server, thin clients"
> rationale below no longer describes the native client. NOTE: the link to
> docs/ROLLBACK-RING-DESIGN.md is broken (file never existed in docs/).
```

**ROLLBACK-PREDICTION.md**: `> **⚠️ STATUS (2026-07-08): REVIVED in modified form** — implemented as the predict arc (STAGE 0–c, 3cf92ca83..3cf861b33). The ".mcrec PENDING — crash blocks" row is stale: validated 2026-05-07 (Step D). Read the commits, not §phases.`

**STREAMING-OPTIONS.md**: `> **🪦 OUTCOME (2026-07-08): Historical survey.** Option 4 (TA mirror) remains the spectator wire. The visual cache (:21) was deleted. H.264/NVENC (:56) is unavailable on the GPU-less prod VPS. Options 2/3/5/6 were all eventually realized in different forms: state-only GSTA replica ~7 KB/frame (189544592), native lockstep ~2.4 KB/s (bc16af338), predict-live (3cf861b33). Current ledger: RENDER-STATE.md.`

**Session/plan docs** (HANDOFF-*, CHARQ-PLAN, GSTA-MAPPING-HANDOFF, ROM-ASSET-CLIENT*, BAKE-HARNESS-PLAN, PART-ASSEMBLY-PLAN, REPLAY-SIMPLIFICATION): one-liner — `> **🪦 OUTCOME (date): session/plan superseded — kept as history.** Superseded by <X>. Do not derive current architecture from this doc; see docs/RENDER-STATE.md.`

**PS-banner docs** (ARCHITECTURE, COMPETITIVE-CLIENT, OPTIMIZATION-PLAN, WASM-BUILD-GUIDE, CHANGELOG, MVC2-WIRE-GAP-ANALYSIS, FRAME-ORACLE-SPEC, PER-OBJECT-QUAD-SPEC, RENDER-REPLICA-RECORDING-FORMAT, PHASE-B-WIRE-PLAN, DC-SERIALIZE-AUDIT): `⚠️ PARTIALLY STALE (2026-07-08)` banner enumerating the table's stale rows, pointing at RENDER-STATE.md / re_kb.

## 6. Carried-forward OPEN item

The browser render-replica default is still `'emitter'` (web/render-replica/replay.html:1811) despite the standing decision to promote render_frame (docs/GSTA-FINDINGS-FOR-BROWSER.md) — carry as OPEN in RENDER-STATE.md, not history.
