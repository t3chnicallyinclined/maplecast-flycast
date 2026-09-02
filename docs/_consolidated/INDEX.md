# MapleCast docs/ — Master Index

The authoritative map of `docs/`. Every markdown doc is listed once, by category, with a one-line summary and a **status tag** so you know whether to trust it.

**Status tags**
- **CURRENT** — still the live plan/truth. Trust it.
- **REFERENCE** — durable facts/spec (addresses, formats, RE). Trust the facts; not a "plan".
- **HISTORICAL** — a dated snapshot/handoff. Read for context, not for current state.
- **SUPERSEDED→X** — replaced. Do NOT act on it; go to X.

**Companion consolidated docs (same folder):**
- [CURRENT-STATE.md](./CURRENT-STATE.md) — what is live/true right now across all subsystems.
- [LESSONS-AND-GOTCHAS.md](./LESSONS-AND-GOTCHAS.md) — the hard-won traps and anti-patterns, deduped.
- INDEX.md — this file.

---

## ★ START HERE (read these first)

> **⚡ TDW ERA (2026-07-14/15): the wire changed.** The TDW dictionary wire is
> now the default/gold standard — read these FOUR first for anything
> wire/client/topology related:
> **[../TDW-PROTOCOL.md](../TDW-PROTOCOL.md)** (normative wire spec) ·
> **[../TDW-GOLD-STANDARD.md](../TDW-GOLD-STANDARD.md)** (gate audit + decommission ledger) ·
> **[../SYSTEM-MODEL.md](../SYSTEM-MODEL.md)** (process/wire topology + fan-out decision) ·
> **[../TA-DICT-WIRE-PLAN.md](../TA-DICT-WIRE-PLAN.md)** (the measured campaign record).
> Legacy wires (ZCST mirror / ZCS2 / GSTA side channels / replica-live) are
> transitional: serving old clients only, retiring per client class.

New contributor or AI agent: read these 5–8 before touching anything.

1. **[../ARCHITECTURE.md](../ARCHITECTURE.md)** — the whole-system mental model: 5 pillars, TA-mirror wire format, headless build, latency budgets, and the byte-perfect determinism guarantee. *(CURRENT)*
2. **[../RENDER-STATE.md](../RENDER-STATE.md)** — **the canonical render ledger.** If you are doing ANY render work, start here, not in the design docs. Authority ranking of all render paths, what is live, what is dead. *(CURRENT)*
   - **Current shipping render config (2026-07-11):** [../RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md](../RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md) — the live stage/bodies/effects split, prod env flags, and end-to-end frame flow for the default **render_frame + ZCS2 streaming-zstd** client (`webgpu-test.html`). *(CURRENT)*
3. **[LESSONS-AND-GOTCHAS.md](./LESSONS-AND-GOTCHAS.md)** — the six determinism fixes, the four-parser rule, the pull-model input law, and every trap that has cost time. *(CURRENT)*
4. **[../DEVELOPER-GUIDE.md](../DEVELOPER-GUIDE.md)** — run the whole stack locally in 3 terminals; components, ports, build targets. *(CURRENT)*
5. **[../DEPLOYMENT.md](../DEPLOYMENT.md)** — prod topology (rise3 `15.204.141.58` / nobd.net), from-scratch bring-up, and the deploy-discipline rules. *(CURRENT)*
6. **[../MVC2-MEMORY-MAP.md](../MVC2-MEMORY-MAP.md)** — the definitive MVC2 RAM address map (char structs, globals, object pool). The reference every RE task leans on. *(REFERENCE)*
7. **[../GSTA-FINDINGS-FOR-BROWSER.md](../GSTA-FINDINGS-FOR-BROWSER.md)** — the one CURRENT render *findings* doc: measured fixes that must port to the browser. *(CURRENT)*

Why these: #1–#2 orient you on system + render truth; #3 stops you re-hitting known traps; #4–#5 get you building/deploying safely; #6–#7 are the RE/render facts you will need constantly.

---

## Architecture & platform

| Doc | Summary | Status |
|-----|---------|--------|
| [../ARCHITECTURE.md](../ARCHITECTURE.md) | Canonical system model: 5 pillars, ZCST wire format, headless build, WebTransport, determinism guarantee. | **CURRENT** |
| [../DEVELOPER-GUIDE.md](../DEVELOPER-GUIDE.md) | Onboarding: run the stack locally in 3 terminals; 5 components, ports, 4-parser rule. | **CURRENT** |
| [../DEPLOYMENT.md](../DEPLOYMENT.md) | Public build/deploy overview + from-scratch nobd.net prod bring-up with migration gotchas. | **CURRENT** |
| [../MATCHMAKING.md](../MATCHMAKING.md) | Phase 1–8 design for a play.html matchmaker pairing strangers via the hub queue. | **CURRENT** |
| [../COMPETITIVE-CLIENT.md](../COMPETITIVE-CLIENT.md) | Vision + phase map for the native tournament client (lossless spectate, HUD, .mcrec, ROM verify, combo trainer). | **CURRENT** |
| [../MATCH-DATA-PLATFORM.md](../MATCH-DATA-PLATFORM.md) | Pivot vision: record full per-frame state, fan out via NATS, build leaderboards/stats/AI dataset. | **CURRENT** |

## Rollback, input & netcode

| Doc | Summary | Status |
|-----|---------|--------|
| [../ROLLBACK-PREDICTION.md](../ROLLBACK-PREDICTION.md) | GGPO-style client-side rollback over the TA-mirror stream; design + crash/desync investigation log. | **CURRENT** (predict arc active; note: rollback-ring shelved) |
| [../INPUT-LATCH.md](../INPUT-LATCH.md) | Dual-policy input latch (LatencyFirst vs ConsistencyFirst) bridging network input to MVC2's once-per-frame read. | **REFERENCE** |
| [../DC-SERIALIZE-AUDIT.md](../DC-SERIALIZE-AUDIT.md) | Static audit of flycast dc_serialize completeness; identifies the unsaved file-scope statics behind the ~3900B replay divergence. | **REFERENCE** |
| [../REPLAY-SIMPLIFICATION.md](../REPLAY-SIMPLIFICATION.md) | Plan to strip replay_reader/writer to the FBNeo/GGPO shape (power-on/disk-savestate + pull-model + version gate). | **CURRENT** (note: render-state/04 audit flags for tombstone — plan largely landed) |
| [../OPTIMIZATION-PLAN.md](../OPTIMIZATION-PLAN.md) | Sub-25ms button-to-pixel plan; three tiers of server/client tweaks with a live landed-status log. | **CURRENT** |
| [../TA-WIRE-V2-PLAN.md](../TA-WIRE-V2-PLAN.md) | Plan for TA wire v2 (dirty-diff / bandwidth reduction). Cross-check against measured lab results in render-state/07. | **REFERENCE** |
| [../PHASE-B-WIRE-PLAN.md](../PHASE-B-WIRE-PLAN.md) | Wire plan for the input-latch Phase B work. | **HISTORICAL** (Phase B landed; see INPUT-LATCH.md) |

## Render — the canonical ledger (read these for render truth)

| Doc | Summary | Status |
|-----|---------|--------|
| [../RENDER-STATE.md](../RENDER-STATE.md) | **Canonical render ledger.** Authority ranking of all render paths, subsystem status, pinned bandwidth, gates, dead campaigns. Start every render session here. | **CURRENT** |
| [../RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md](../RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md) | **The current shipping render config.** The stage/bodies/effects split + prod flags: stage & effects ride the ZCS2 streaming-zstd wire pixel-perfect (~0 cost, zstd dedup), character bodies are server-stripped and drawn locally via render_frame from the folded STM2 body state. Default client = `webgpu-test.html`; ~3 Mbps gameplay. Campaign record: HANDOFF-WIRE-THINNING-2026-07-11. | **CURRENT** |
| [../GSTA-FINDINGS-FOR-BROWSER.md](../GSTA-FINDINGS-FOR-BROWSER.md) | Measured render fixes from the native GSTA/render_frame client that must port to the browser. The one current render *findings* doc. | **CURRENT** |
| [../RENDER-REPLICA-RECORDING-FORMAT.md](../RENDER-REPLICA-RECORDING-FORMAT.md) | Live-wire spec for the off-SH4 render-replica: STATIC/DYNAMIC read-set partition, MCRR format, FRMx+tails frame record, HUDQ. | **CURRENT** |
| [../FRAME-ORACLE-SPEC.md](../FRAME-ORACLE-SPEC.md) | Spec for the live JIT dynarec hook attributing emitted quads to objects (the diagnostic that became the template-cache idea). | **REFERENCE** (note: its cursor-segmentation dismissal was corrected by PER-OBJECT-QUAD-SPEC) |
| [render-state/01-sprite-pipeline.md](../render-state/01-sprite-pipeline.md) | Client sprite/texture pipeline ledger: 5 body impls ranked, tombstones, full solved-bug catalog. | **CURRENT** |
| [render-state/02-native-client-phase2a.md](../render-state/02-native-client-phase2a.md) | Native GSTA client frame path + Phase 2a audit (gaps G1–G9) + first live A/B result. | **CURRENT** |
| [render-state/03-engine-re-ledger.md](../render-state/03-engine-re-ledger.md) | Engine SH4 RE ledger: what the char-pass driver renders, why the 3D-machine re-impl is dead, HUD/stage/camera map. | **CURRENT** |
| [render-state/04-docs-audit.md](../render-state/04-docs-audit.md) | Staleness audit + tombstone plan for every docs/*.md; the bandwidth-number-triad adjudication. | **CURRENT** |
| [render-state/05-gates-and-protocol.md](../render-state/05-gates-and-protocol.md) | Gate catalog (G1–G15) + the Phase 2a live A/B pixel-gate protocol + missing-tooling list. | **CURRENT** |
| [render-state/06-bandwidth-prior-art.md](../render-state/06-bandwidth-prior-art.md) | Prior-art survey for a sub-1-Mbps TA stream (meshopt, streaming zstd, quad-delta ring, LiveRender). | **REFERENCE** |
| [render-state/07-bandwidth-lab-results.md](../render-state/07-bandwidth-lab-results.md) | MEASURED compression lab: wire is 6.875 Mbps, 78.7% VRAM pages; a measured stack hits 0.788 Mbps zero-loss. | **CURRENT** |

## MVC2 reverse-engineering & assets (durable reference)

| Doc | Summary | Status |
|-----|---------|--------|
| [../MVC2-MEMORY-MAP.md](../MVC2-MEMORY-MAP.md) | Definitive MVC2 DC memory map: char structs (0x8C268340/stride 0x5A4), globals, object pool (0x8C26AA54), per-frame dirty pages. | **REFERENCE** |
| [../MVC2-FRAMEDATA-FIELDS.md](../MVC2-FRAMEDATA-FIELDS.md) | Every anotak attack-record (0x1C) + anim-cell (0x14) field mapped to byte offset + SH4 read site. | **REFERENCE** |
| [../MVC2-RECONSTRUCTION-SPEC.md](../MVC2-RECONSTRUCTION-SPEC.md) | The exact per-frame MVC2 render algorithm traced from disasm, as an implementable from-state spec. | **REFERENCE** |
| [../MVC2-WIRE-GAP-ANALYSIS.md](../MVC2-WIRE-GAP-ANALYSIS.md) | Field-by-field audit of GSTA/OBJS wire vs on-screen state not shipped; the real hit-flash field (0x12e). | **REFERENCE** |
| [../MARVELOUS2-RE-HANDOFF.md](../MARVELOUS2-RE-HANDOFF.md) | marvelous2 disasm findings: cracked the sprite LZSS codec + fully mapped the object pool. Durable RE facts. | **REFERENCE** |
| [../MARVELOUS2-GFX-NOTES.md](../MARVELOUS2-GFX-NOTES.md) | Clean-room notes on how MVC2 draws a char from parts + assembly (cumulative-pen geometry, logical crop). | **REFERENCE** |
| [../MVC2-RIPPER-DESIGN.md](../MVC2-RIPPER-DESIGN.md) | Design for an offline ripper: PLxx_DAT + EFKY effects → cross-referenced atlas + catalog, no emulator. | **REFERENCE** |
| [../SKIN-SYSTEM.md](../SKIN-SYSTEM.md) | Live palette-swap skins via per-frame PVR palette-RAM overrides shipped through the TA mirror. | **CURRENT** |
| [../NAOMI-SPIKE.md](../NAOMI-SPIKE.md) | Naomi/arcade MVC2 ROM migration spike (naomi-mvc2-spike branch) + live palette-swap skin system exploration. | **REFERENCE / HISTORICAL** (spike) |
| [render-state/03-engine-re-ledger.md](../render-state/03-engine-re-ledger.md) | (also render) Engine-side RE ledger — see Render section. | **CURRENT** |

## Build & deploy guides

| Doc | Summary | Status |
|-----|---------|--------|
| [../WINDOWS-CLIENT-BUILD.md](../WINDOWS-CLIENT-BUILD.md) | Build a native Windows flycast.exe mirror-client (MAPLECAST_CLIENT_ONLY=ON). | **REFERENCE** |
| [../WASM-BUILD-GUIDE.md](../WASM-BUILD-GUIDE.md) | Build the two browser WASM clients: standalone renderer (king.html, primary) + EmulatorJS core (legacy). | **CURRENT** |
| [../WINDOWS-HEADLESS-BUILD.md](../WINDOWS-HEADLESS-BUILD.md) | Build the Windows headless flycast.exe (MAPLECAST_HEADLESS=ON) used as the local rollback predictor / GSTA client. | **REFERENCE** |
| [../WEBGPU-RENDERER.md](../WEBGPU-RENDERER.md) | Architecture/reference for the pure-JS WebGPU renderer of the TA mirror stream (zero WASM, zero build). | **CURRENT** |

## Ideas & feasibility (research logs — deferred, not plans)

| Doc | Summary | Status |
|-----|---------|--------|
| [../SATURN-FEASIBILITY.md](../SATURN-FEASIBILITY.md) | Study: a MapleCast-for-Saturn port via Ymir is viable (IVDPRenderer is already wire-shaped). One blocker: FB readback. | **REFERENCE** |
| [research/graphify-evaluation.md](../research/graphify-evaluation.md) | Evaluation of graphify knowledge-graph tool: complement only, do NOT duplicate the re_kb RE graph. | **REFERENCE** |
| [../FLYCAST-RENDERER-BROWSER-SCOPE.md](../FLYCAST-RENDERER-BROWSER-SCOPE.md) | Scope comparing 3 paths to render MVC2 in-browser via flycast's real code (emitter→TA, full WASM SH4, hybrids). | **REFERENCE** |
| [../PER-OBJECT-QUAD-SPEC.md](../PER-OBJECT-QUAD-SPEC.md) | RE spec to attribute each rendered quad to its object via the game's own display-list count table (0x8C26AA24). | **REFERENCE** |

---

## 🪦 Archive / historical — DO NOT trust as current plans

These are superseded plans or dated snapshots. Read for context/provenance only. Each notes what replaced it.

### Superseded render plans (the pixel-shipping family — all dead)

The whole mirror/STAF/VCACHE pixel-and-geometry-shipping line was abandoned for reconstruct-from-state. See [../RENDER-STATE.md](../RENDER-STATE.md) and [render-state/04-docs-audit.md](../render-state/04-docs-audit.md) for the tombstone chain.

| Doc | Summary | Status |
|-----|---------|--------|
| [../STREAMING-OPTIONS.md](../STREAMING-OPTIONS.md) | Apr-2026 survey of 7 streaming options; chose TA mirror primary + H.264 secondary. | **SUPERSEDED→ RENDER-STATE.md** (Option 6 seeded the render-reconstruction arc) |
| [../RENDER-MASTER-PLAN.md](../RENDER-MASTER-PLAN.md) | First "pixel-perfect off-SH4 at ~20–140 KB/s" plan (reconstruct static, stream dynamic effects). | **SUPERSEDED→ RENDER-MASTER-PLAN-V2 → RENDER-STATE.md** |
| [../RENDER-MASTER-PLAN-V2.md](../RENDER-MASTER-PLAN-V2.md) | T1–T6 synthesis pinning the PVR2Renderer input contract; stage/HUD/effects build details. | **SUPERSEDED→ RENDER-DECISION → RENDER-STATE.md** |
| [../RENDER-TIER1-PLAN.md](../RENDER-TIER1-PLAN.md) | Argues full-frame STAF (content-hash texId + TX64 ship-once) is already shipping; promote to full-frame. | **SUPERSEDED→ RENDER-DECISION (Option B rejected)** |
| [../RENDER-DECISION.md](../RENDER-DECISION.md) | Verdict: STAF surrogate machinery is redundant; ship VCACHE + STAF-effects + sprite-gpu. | **SUPERSEDED→ PIXEL-PERFECT-PLANS → RENDER-STATE.md** |
| [../PIXEL-PERFECT-PLANS.md](../PIXEL-PERFECT-PLANS.md) | Reframe: prior attempts share one wrong assumption (re-derive offline); proposes the Frame Oracle + template cache. | **SUPERSEDED / HISTORICAL** (Frame Oracle landed; template cache → render_frame) |
| [../STRIPPED-TA-DESIGN.md](../STRIPPED-TA-DESIGN.md) | Design + build of stripped-TA/STAF: cache textures by content-hash, ship draw-list only. | **SUPERSEDED→ RENDER-DECISION → GSTA render replica** |

### Superseded / historical render & asset handoffs

| Doc | Summary | Status |
|-----|---------|--------|
| [../ASSEMBLY-DRIVEN-DESIGN.md](../ASSEMBLY-DRIVEN-DESIGN.md) | Assembly-driven part renderer feasibility. ⚠️ Its central "can't decode offline" blocker was DISPROVEN. | **SUPERSEDED** (offline LZSS+detwiddle closed byte-exact; re_kb/08) |
| [../GSTA-MAPPING-HANDOFF.md](../GSTA-MAPPING-HANDOFF.md) | 38-byte GSTA/OBJS wire→address map, ~90% verified with 3 flagged ambiguities. | **HISTORICAL** (resolved by wire-gap analysis + re_kb) |
| [../ROM-ASSET-CLIENT.md](../ROM-ASSET-CLIENT.md) | Proves (char_id, sprite_id)→byte-identical pixels; atlas + ~253B state instead of TA video. | **HISTORICAL** (early verification era → render-replica) |
| [../ROM-ASSET-CLIENT-PLAN.md](../ROM-ASSET-CLIENT-PLAN.md) | Forward build plan for the ROM-asset client (bake-anchor gap, PVR2 transparency, ModNao). | **HISTORICAL** (predates transpiled-SH4 approach) |
| [../HANDOFF-WIRE-THINNING-2026-07-11.md](../HANDOFF-WIRE-THINNING-2026-07-11.md) | Wire-thinning campaign record: STM2 size-tolerant delta + KEY-defer, the super-spike root cause (84% render-STATE floor), NO_SCENE_SYNC, the reverted clean-strip. Its shipping outcome = [RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md](../RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md). | **HISTORICAL** (campaign record; current config in the CHECKPOINT) |
| [../HANDOFF-2026-06-08.md](../HANDOFF-2026-06-08.md) | Session handoff announcing the Frame Oracle breakthrough (live JIT hook → ground-truth screen quads). | **HISTORICAL / SUPERSEDED→ RENDER-STATE.md** |
| [../HANDOFF-EMITTER-2026-06-09.md](../HANDOFF-EMITTER-2026-06-09.md) | Session handoff for the off-SH4 emitter; pipeline proven from disasm, blocked on build host. | **HISTORICAL / SUPERSEDED** (emitter retired as drawer → render_frame) |
| [../CHARQ-PLAN.md](../CHARQ-PLAN.md) | CHARQ programmable Oracle probe plan (per-part PVR sprite-quad capture). | **SUPERSEDED→ RENDER-STATE.md** (CHARQ era; per render-state/04 audit) |
| [../STATE-REPLICA-PLAN.md](../STATE-REPLICA-PLAN.md) | Plan to inject per-frame GSTA state into a live SH4 replica. | **SUPERSEDED** (injection dead-end; goal met by controlled-core + lockstep) |
| [../WINDOWS-REPLICA-TEST.md](../WINDOWS-REPLICA-TEST.md) | Windows local render-replica test rig notes. | **SUPERSEDED** (per render-state/04 audit) |
| [../BAKE-HARNESS-PLAN.md](../BAKE-HARNESS-PLAN.md) | Plan for the sprite bake/export harness. | **SUPERSEDED** (per render-state/04 audit) |
| [../PART-ASSEMBLY-PLAN.md](../PART-ASSEMBLY-PLAN.md) | Plan for offline part-assembly reconstruction. | **SUPERSEDED** (per render-state/04 audit) |

### Superseded / abandoned ideas & feasibility

| Doc | Summary | Status |
|-----|---------|--------|
| [../OPTION6-INSANE-IDEAS.md](../OPTION6-INSANE-IDEAS.md) | 15 blue-sky bandwidth/latency ideas; none implemented under Option 6. | **HISTORICAL** (pivoted to latency-first, OPTIMIZATION-PLAN) |
| [../OPTION6-MASTER-PLAN.md](../OPTION6-MASTER-PLAN.md) | Post-mortem: state-keyed frame dedup hit 0% hit rate; three pivots proposed. | **SUPERSEDED→ OPTIMIZATION-PLAN** (branch retired) |

> **Note on bandwidth numbers:** three figures circulate (4.1 Mbps anchor / 1.7 MB/s unlabeled / 36–88 Mbps unrecovered-conditions). Cite only the Apr-2026 **~4.1 Mbps zstd anchor** for historical context; the fresh 2026-07-08 measurement is **6.875 Mbps** (see [render-state/07](../render-state/07-bandwidth-lab-results.md)). Details in [LESSONS-AND-GOTCHAS.md](./LESSONS-AND-GOTCHAS.md).

---

## Docs that steer you WRONG (flagged for tombstoning)

Per [render-state/04-docs-audit.md](../render-state/04-docs-audit.md), watch for these actively-misleading docs if you encounter them:
- **[../ROLLBACK-SHELVED.md](../ROLLBACK-SHELVED.md)** — ⚠️ its un-shelve condition #1 is now TRIGGERED (lockstep client runs full local SH4 bit-exact); the predict/lockstep arc is ACTIVE. Actively wrong; links a nonexistent ROLLBACK-RING-DESIGN.md. **SUPERSEDED**.
- **[../ASSEMBLY-DRIVEN-DESIGN.md](../ASSEMBLY-DRIVEN-DESIGN.md)** — states a disproven blocker as fact (offline decode was later closed byte-exact). **SUPERSEDED** (also listed above).
- The bandwidth-triad docs — cite the ~4.1 Mbps anchor only; latest measured is 6.875 Mbps.
- **[../CHANGELOG.md](../CHANGELOG.md)** — frozen at 2026-06-08; a month of render history is absent. The ledger continues in [../RENDER-STATE.md](../RENDER-STATE.md). **HISTORICAL**.
