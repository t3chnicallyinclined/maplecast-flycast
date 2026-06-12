# MVC2 RENDER-ONLY SH4 REPLICA — Implementation Plan

> Run MVC2's **real render code** on the client over a frozen snapshot + per-frame GSTA
> patches, emit the game's own exact TA command stream, and feed it to the gold-standard
> `pvr2-renderer`. The goal is to **delete the JS render-rule whack-a-mole**
> (assembly/facing/blend/tiling/palette/flip) by replacing every hand-derived rule with
> the authoritative ROM routine that produced it.

Status: **PROPOSAL — gated on Phase 0.** Date: 2026-06-11.
Evidence base: SH4 RE read-set analysis (mvc2-sh4-re-expert), client SH4-in-browser
integration scoping (mvc2-sprite-render-expert), server snapshot + wire scoping (general).

---

## 1. TL;DR / Recommendation

**Viable as a research bet, with one decisive caveat — and Phase 0 is GO.** The server,
wire, and back-half rendering are already built: the headless ships GSTA (~48 kbps) and
a full-machine MCSV snapshot, the relay caches both, and the browser already turns a raw
TA buffer into pixels via `ta-parser.mjs` → `pvr2-renderer.mjs` (the gold-standard
rasterizer). The replica's **output is byte-identical in kind** to the existing mirror
stream, so the entire parse+rasterize back-half is reused verbatim. The load-bearing
unknown is **read-set hermeticity**: the SH4 render call-tree reads ~20 char-struct
fields plus an **engine-owned pointer cluster (`+0x154…+0x184`)** and the **slot-table
node wiring (`0x8C2895E0`/`0x8C287DE0`)** that GSTA does not carry. The honest finding is
that the replica is **provably sound and crash-free for re-projecting a snapshot's poses
at new GSTA positions/scales/facing**, but **changing pose from `sprite_id` re-enters the
exact corruption class that killed live injection** unless we also re-derive
`+0x154/+0x160` (deterministic from `character_id`+`sprite_id`+static) and rebuild the
slot table. **Phase 0 is the gate that decides everything**: an env-gated, determinism-safe,
server-side freeze→re-render→diff-TA experiment that empirically proves (or kills) the
read-set ⊆ {GSTA} ∪ {static snapshot} claim — at near-zero cost because it reuses the
already-armed Oracle hook's `ta_parse(ctx,true)` + Stop/ResetCache primitives. **Do not
write a single line of client SH4 code until Phase 0 is green.**

---

## 2. Architecture

The replica ships **state, not pixels** (or geometry). The client re-derives geometry by
running MVC2's own render code over a one-time snapshot, then hands the resulting TA
stream to the renderer the project already trusts.

```
 ┌──────────────────────────── SERVER (VPS, headless flycast, full SH4) ────────────────────────────┐
 │                                                                                                   │
 │   real SH4 runs MVC2 ──► serverPublish() once/frame                                               │
 │        │                      │                                                                   │
 │        │                      ├── ONCE @ match start:  MCSV snapshot (RAM 16MB + VRAM 8MB + PVR)  │
 │        │                      │        content-addressed by snapshot_id (char-set × stage)        │
 │        │                      │                                                                   │
 │        │                      └── EVERY FRAME:  GSTA (~388B) + OBJF (N×16B)  ≈ 48 kbps            │
 │        │                                                                                          │
 │   [Oracle hook = Phase-0 validator: ta_parse(ctx,true) ground-truth quads]                       │
 └───────────────────────────────────────────────┬──────────────────────────────────────────────────┘
                                                  │
                          flycast :7200 → relay :7201 → nginx /ws :443
                          (relay caches MCSV by snapshot_id; GSTA/OBJF on keep-list)
                                                  │
 ┌────────────────────────────────────────────────▼─────────── CLIENT (browser / native) ───────────┐
 │                                                                                                    │
 │   ONCE:   recv MCSV ──► hydrate { ram[16MB], vram[8MB], pvr_regs }   (cache in IndexedDB by id)    │
 │                                │                                                                   │
 │   FRAME:  recv GSTA/OBJF ──► patch char structs in ram[] @ documented offsets                     │
 │                                │       (+ re-derive +0x154/+0x160 + rebuild slot table — Tier 2)   │
 │                                ▼                                                                   │
 │           ┌────────────── render-only SH4 slice (wasm) ──────────────┐                            │
 │           │  ctx.pc = RENDER_ENTRY_PC; ctx.pr = SENTINEL              │                            │
 │           │  reset display-list cursor; restore 0x8C26A974           │                            │
 │           │  while (pc != SENTINEL) { op=Read16(pc); pc+=2; Op[op] } │                            │
 │           │  memory = flat ram/vram + handler tables (NO virtmem)    │                            │
 │           └───────────────────────────┬───────────────────────────────┘                          │
 │                                        │ captures the game's own TA command stream                │
 │                                        ▼   (RAM display list @ ~0x0C56xxxx, OR SQ→TA bursts)       │
 │                                  taBuffer (raw TA)                                                 │
 │                                        │                                                           │
 │                       ta-parser.mjs (REUSE VERBATIM) ──► parsed { verts, opaque/pt/tl, passes }   │
 │                                        │                                                           │
 │                       pvr2-renderer.mjs renderFrame(parsed, texMgr, pvrSnap, vram) ──► PIXELS      │
 │                                        (GOLD STANDARD CONFIG, ~1.88ms)                              │
 └────────────────────────────────────────────────────────────────────────────────────────────────┘
```

**Key invariant:** the SH4 slice is run **only** for the render subtree, from a snapshot,
then the context is thrown away. There are no engine invariants to violate across frames —
which is exactly why this is structurally safer than the dead "inject GSTA into a live SH4"
approach.

---

## 3. Why — vs JS reconstruction and vs the dead TA mirror

### 3a. vs current JS reconstruction (the whack-a-mole)
Today the client re-implements the ROM's render rules by hand in JS (`sprite-client.mjs`):
sprite assembly, facing/flip, blend modes, tiling, palette banks. Each is a separate
reverse-engineering battle (the "detached forearm" facing bug, the upside-down logical-crop
regression, the emitter flip-unvalidated open item). **Every one of these rules is a
behavior the ROM render code already implements exactly.** The replica replaces the entire
rule-set with the authoritative routine — there is nothing left to guess, because the game
draws itself. The pure-JS emitter has geometry **numerically closed to 0.00px**, but it
still owns every render rule by hand and cannot reach effects/supers the offline atlas
doesn't cover. The replica's unique win is **exactness for the cases the emitter can't
reach**, at GSTA bandwidth.

### 3b. vs the dead TA mirror
The dead mirror shipped the **pixels/geometry** (full TA delta + dirty VRAM pages) at
36–88 Mbps and was killed because the wire is ~73% geometry. The replica ships **state**
(388B GSTA + OBJF, ~48 kbps) and re-derives the identical TA stream on the client. Same
pixel fidelity, ~3 orders of magnitude less bandwidth.

### 3c. Trade table

| Approach | Fidelity | Steady BW | Client CPU | ROM on client | Render-rule maintenance |
|---|---|---|---|---|---|
| **JS reconstruction (current)** | rules hand-derived; gaps remain | ~48 kbps | low (re-bake on sprite_id change) | none | **HIGH — perpetual whack-a-mole** |
| **Pure-JS emitter** | geometry 0.00px; no effects/supers | ~48 kbps | low | none | high (still hand-rules) |
| **Dead TA mirror** | pixel-exact | **36–88 Mbps** | very low (parse+raster) | none | none |
| **Hybrid (server emits geometry)** | pixel-exact | ~100–500 KB/s | very low (parse+raster) | none | none |
| **Render-only SH4 replica (this)** | **pixel-exact (runs real code)** | **~48 kbps + 1× MCSV** | **HIGH (wasm SH4 interp)** | **16–26 MB ROM-derived blob** | **NONE** |

The replica is the only row that is **both** pixel-exact **and** GSTA-bandwidth — bought
with client CPU risk and a ROM-derived snapshot shipped to every client. The hybrid
(server-emits-geometry) is the lower-risk way to get pixel-exactness if those two costs
prove unacceptable (see §6).

---

## 4. Phase 0 — VALIDATION EXPERIMENT (THE GATE — IMMEDIATE NEXT ACTION)

**Purpose:** empirically prove the per-frame render read-set is closed over
{GSTA} ∪ {static snapshot}, *before* building any client. This is the one experiment that
decides go/no-go. It runs entirely **server-side**, in-process, env-gated, and
determinism-safe — reusing the three already-proven-safe primitives in the tree.

### 4.1 The precise sufficiency condition to test
From the read-set analysis, the analytic gap reduces to exactly this. The render read-set
is closed over static ∪ GSTA **except**:
`{+0x154…+0x184 engine pointer cluster, +0x12C cull gate, +0x24 zoom-idx, +0x3C pos_z,
+0x14D pal-select, +0x48/+0x4C/+0x4E words, +0x158 anim-group, slot-table wiring}`.
Of these, `+0x160` (GFX2) / GFX1 / anim ptrs are **deterministically derivable from
`character_id` + static PLxx_DAT base**, and `+0x154` (current_cell) is derivable from
`(character_id, sprite_id)` via the same `GFX2 + *(u32)(GFX2 + (sid&0x7FFF)*4)` formula the
body walker `loc_8c0344d4` itself uses. So the honest sufficiency condition is:

> **Re-derive `+0x154/+0x160` from (character_id, sprite_id) + static, rebuild the slot
> table from the GSTA active-object set, and freeze everything else from the snapshot.
> Everything not on that list is GSTA or static.**

Phase 0 tests this in two tiers.

### 4.2 Concrete steps (server-side, in-process A/B)

1. **Add env gate `MAPLECAST_REPLICA_PHASE0`** (default OFF → byte-stock prod path, zero
   change when off — same convention as every hook in `maplecast_oracle_hook.cpp`).

2. **Freeze point = `serverPublish()`** (`maplecast_mirror.cpp:1748`), the existing
   once-per-frame post-draw boundary. At that point the live `TA_context* ctx` is the
   just-completed authoritative frame (the **(a) Authoritative** reference).

3. **Snapshot the static regions** to a side buffer at a chosen in-match frame (gate on
   `0x8C289624` in-match, same as CHARQ/Oracle):
   - Char/object structs page `0x8C268000…0x8C26B000` (6 char structs + object pool +
     tables `0x8C26A518`/`0x8C26A974`).
   - Slot table `0x8C2895E0` (16×0x180) + `0x8C287DE0` (×4 node-pointer array).
   - `GameGlobalPointer` block `0x8C26823C` (+several KB; reads `+0x24`,`+0x2E`,`+0x8E`,`+0x98`).
   - Template tables `0x8C1F9D80…0x8C1FA000`, `0x8C1F9F9C`.
   - VRAM (8MB) + PALETTE_RAM (existing SYNC path already copies these).

4. **Neutralize → restore full snapshot, then overwrite ONLY the GSTA-shipped fields** on
   each active node from the wire values (pos `+0x34/+0x38`, scale `+0x50/+0x54`, facing
   `+0x110`, flip `+0x130`, sprite_id `+0x144`, palette `+0x52D/+0x14D`). **Leave
   `+0x154…+0x184` at snapshot values — NOT zeroed** (zeroing crashes the `+0x160` deref).

5. **Re-invoke render at the SH4-paused boundary** (right after `runInternal()` returns —
   never inside a compiled block, never racing the SH4 thread). Reuse the proven
   Stop()→boundary→ResetCache()→Start() dance (`maplecast_oracle_hook.h:48-59`
   `mc_probeApplyReload`; the rollback deferred-rewind boundary). Before the call:
   - reset display-list cursor `*(u32*)(GameGlobalPointer+0x24) = list_base`;
   - **restore `0x8C26A974` from snapshot** (its `fmac` accumulate is NOT idempotent);
   - restore `0x8C1F9D80` template scratch to be safe;
   - call the recompiled block for the render entry a **second** time, capturing into a
     **second** display-list buffer.

6. **Two-tier test:**
   - **Tier 1 (re-project identical pose):** patch ONLY pos/scale/facing; leave `sprite_id`
     at snapshot. **Expect a byte-identical display list.**
   - **Tier 2 (change pose):** patch `sprite_id` to a different value, leave the cluster
     stale. **Expect mismatch or crash** — confirming the cluster is on the per-frame read
     path. Then **re-run the anim-load `loc_8c034e8c`** (the minimal unavoidable "logic")
     with the patched sprite_id/group to rebuild `+0x154/+0x160`, and re-test → **expect
     match**.

7. **Diff + capture:** diff the re-run display list against the live frame's, **byte-for-byte**,
   OR parse both via `ta_parse(ctx,true)` and diff `rc.verts`/`PolyParam` per-quad using the
   `validate_emitter_geom.py` numeric pattern (0.00px relative = exact). Use the Oracle's
   `mc_oracle_quadObjMap` / `screen_quads` per-object attribution to localize mismatches.
   Emit per-frame stats `{matched, mismatched, unassigned, max_px_delta}` to
   `/dev/shm/mc_replica_phase0.jsonl` (append+fflush+rolling-tail, same as `mc_probe.log`).

### 4.3 Success criteria
- **Tier 1 byte-identical** (or 0.00px per-quad) over a sustained in-match window across
  several characters/poses → **PROVES** the replica is sound for position/scale/facing
  re-projection with **zero game logic**. *Minimum bar to proceed.*
- **Tier 2 byte-identical after the single `loc_8c034e8c` re-run** → **PROVES** the full
  read-set is closed over {GSTA} ∪ {static} **provided one deterministic anim-load call**.
  This is the strong result that makes the replica fully general.

### 4.4 What kills the idea
- **Tier 1 does NOT match** (and the deltas are not a fixable cursor/accumulator reset
  bug) → the render reads per-frame state we cannot identify or patch → **NO-GO**, fall
  back to the hybrid (§6).
- **Tier 2 crashes or mismatches even after `loc_8c034e8c`** → pose changes require more
  than one anim-load call; the replica re-enters the live-injection corruption class for
  any pose change → **viable only for the re-projection sub-case** (downgrade scope).
- **The slot-table rebuild owner cannot be identified** and snapshot-and-patch-pointers
  doesn't hold across the GSTA active set → satellite/projectile rendering is out of reach;
  bodies-only replica.

### 4.5 Why this is determinism-safe
It is the same three primitives the wire-format race-fix contract already protects:
(1) env-gated branch that is a literal no-op when off; (2) `ta_parse(ctx,true)` read-only
re-parse that never writes guest state, never enqueues, never touches rqueue; (3)
Stop/ResetCache at the SH4-paused boundary. The re-render operates on a **separate replica
context / snapshot copy**, READ-ONLY w.r.t. the authoritative guest. Run the
`MAPLECAST_DUMP_TA=1` determinism rig at the end of Phase 0 to confirm prod is byte-stock.

---

## 5. Workstream (AFTER a green Phase 0)

Each phase: **goal · key files · risk · exit criteria.** Ordered so the highest-uncertainty
work is retired earliest (see §7).

### Phase 1 — Snapshot export (ship-once static regions)
- **Goal:** ship the full static memory the render code reads, content-addressed so it
  amortizes to ~0 across matches with the same character-set/stage.
- **Mechanism:** reuse **MCSV** (full `dc_serialize`: RAM 16MB + VRAM 8MB + PVR + ctx),
  NOT SYNC — SYNC deliberately skips main-RAM GFX/cell tables (`initRegions` comment
  "SKIP RAM"), which the code replica needs. Hash the MCSV body (or just the GFX1/GFX2/code
  regions) → `snapshot_id`; extend the relay's existing MCSV/SyncCache to key by
  `snapshot_id` and skip the 6–10MB resend when the client advertises a known id in its WS
  connect query. Browser caches the hydrated blob in IndexedDB by `snapshot_id`.
- **Key files:** `maplecast_mirror.cpp` (MCSV build/broadcast `serverSaveSync`/
  `doForcedSaveStateBroadcast`; SYNC/region setup :318-352), `relay/src/fanout.rs`
  (MCSV cache :47-55, replay-on-connect :558-562), `relay/src/protocol.rs` (MCSV magic).
- **Risk:** LOW — primitive already ships; ~30 lines server, ~20 relay for content-address.
- **Exit:** client receives MCSV once, hydrates `ram[16MB]+vram[8MB]+pvr_regs`; a second
  match with the same char-set skips the resend.

### Phase 2 — GSTA field additions (close the wire gap)
- **Goal:** carry the 2–3 per-char fields the render reads that GSTA lacks.
- **Adds:** `+0x12C` cull/visibility byte (reuse the existing `_pad` byte at +56);
  `+0x154` cur-cell (append one u16). Stride 57→59, total 376→388 bytes. **Append-only** —
  parsers already tolerate trailers via `len >= off+N` guards (`gamestate.cpp:2283`); relay
  doesn't parse GSTA bodies, so no relay change.
- **Key files:** `core/network/maplecast_gamestate.cpp` (GSTA serialize :2142-2215),
  `maplecast_gamestate.h` (CharacterState/WIRE_SIZE). Update **all four parsers** per the
  wire-format rule.
- **Risk:** LOW — append-only, no parser break.
- **Exit:** GSTA carries the cull byte + cur-cell; existing clients unaffected.
- **Note:** `+0x12C`/`+0x154` may be redundant with the re-derive path proven in Phase 0
  Tier 2 (`+0x154` is derivable from `character_id`+`sprite_id`). If Phase 0 Tier 2 proves
  re-derivation, prefer **deriving on the client** over widening the wire. Decide based on
  the Phase-0 result.

### Phase 3 — Client SH4 render slice (wasm)
- **Goal:** a minimal render-only SH4 interpreter in wasm that runs the render subtree over
  the snapshot and captures the TA stream.
- **Build:** carve a clean compile unit — `interpr/sh4_opcodes.cpp` + `sh4_opcode_list.cpp`
  + `sh4_fpu.cpp` (FPU mandatory — projection/scale math is FP-heavy) + `Sh4Context`
  (512B, portable). **Do NOT use `Run()`** (scheduler/interrupts); write a driver:
  `ctx.pc=ENTRY; ctx.pr=SENTINEL; while(pc!=SENTINEL){op=Read16(pc); pc+=2; OpPtr[op](ctx,op);}`.
  Replace `addrspace`/`virtmem` with a flat `ram[16MB]+vram[8MB]` + handler tables;
  `translate(a)=a & 0x1FFFFFFF` with area routing (area3→RAM, area1/4→VRAM) per `GetMemPtr`.
  **Drop entirely:** dynarec, MMU (MVC2 runs MMU-off), interrupts, scheduler, AICA, Maple,
  Holly DMA, BIOS, store-queue→TA unless needed. Stub `UpdateSystem_INTC`, icache/ocache
  (STRICT_MODE off).
- **EXECUTOR SPECTRUM (A→C) — we do NOT need the whole SH4 core or the full ISA at runtime.**
  Three ways to *execute* the render block, leanest end-state last:
  - **A. Full flycast SH4 core → wasm** (call only the render entry). Biggest, but **proven**
    (upstream flycast already runs SH4 in-browser via emscripten). Fastest path to a *working*
    Phase-3 prototype because the runtime executor resolves indirect jumps (the cell-processor
    jump tables `loc_8c033d78` etc.) automatically.
  - **B. Minimal render-only interpreter** (the build above): only the ~50–100 opcodes the
    render subtree uses + FPU, driven by the custom `while(pc!=SENTINEL)` loop. Much smaller
    than A; still interpreter-speed.
  - **C. AOT-transpile the render call-tree → wasm/JS** (static binary translation of *just*
    `loc_8c0344d4`/dispatch/tiling/bank12 projection/submit, offline, once). **No interpreter
    at runtime — native speed, smallest footprint.** This is the literal "reproduce the block"
    option and the real answer to the §6.2 / Phase-5 perf risk (it removes the per-op
    interpreter overhead the plan otherwise eats). Cost: the transpiler must enumerate all
    indirect-branch targets — they're **data-driven jump tables in the ROM, so enumerable**,
    just real work. flycast's existing dynarec is a runtime version of exactly this.
  - **Recommended sequencing:** prove with **A** (or B), then drop to **C** only if Phase 5's
    measured op-count blows the 60fps budget. Do NOT build C up front — it's the optimization,
    not the prototype.
- **Per-frame patch:** translate `0x8C268340 + slot*0x5A4 → ram[addr & 0x00FFFFFF]`, memcpy
  GSTA fields into char-struct offsets before each invocation. **Reset display-list cursor
  + restore `0x8C26A974`** before each run (per Phase 0). Tier-2 path: re-derive
  `+0x154/+0x160` and re-run `loc_8c034e8c` if pose changed; rebuild slot table from the
  GSTA/OBJF active set.
- **Key files:** `core/hw/sh4/interpr/sh4_interpreter.cpp` (Run/Step/ExecuteOpcode — the
  driver you replace), `sh4_if.h` (Sh4Context), `sh4_mem.cpp` (ReadMem*/GetMemPtr/SQ→TA),
  `addrspace.cpp` (virtmem vs table fork), `packages/renderer` (proves emscripten builds
  flycast render code), `maplecast_oracle_hook.cpp` (confirmed entry PCs + ABI).
- **Risk:** **HIGH** — call-tree hermeticity (the load-bearing unknown, retired by Phase 0);
  carving a unit free of `addrspace`/`virtmem`/holly; entry-PC + live-register ABI (owned
  by SH4 RE expert). **bank11/bank12 leaf bodies are ABSENT from the disasm checkout** —
  transform `loc_8c1216c0`, vertex builders `loc_8c11e2e0/e460/e860`, submit
  `loc_8c1244b0`; their exact globals/PVR-template reads are inferred, not read. Confirm via
  fuller disasm or Oracle probe on those PCs before relying on them.
- **Exit:** wasm slice runs the render entry over a hydrated snapshot + GSTA patch and
  produces a `taBuffer` matching the server's captured TA for the same frame (Tier 1).

### Phase 4 — Output → pvr2-renderer wiring
- **Goal:** turn the captured TA stream into pixels.
- **Mechanism:** the emit is **a RAM display list** (cursor-delimited from
  `GameGlobalPointer+0x24`, lands ~`0x0C56xxxx`) — the draw routines do **no SQ/`0x10000000`
  writes (KB-confirmed)**, a separate later pass bulk-DMAs it to the TA. So capture the
  list **from RAM** after the render pass (do NOT need a real TA submit). Feed the captured
  buffer straight into `TAParser` (**reuse verbatim**) → `renderFrame(parsed, texMgr,
  pvrSnap, vram)`. Transform required: **essentially none** — the replica's output is
  byte-identical in kind to the mirror stream.
- **Key files:** `web/webgpu/ta-parser.mjs` (`TAParser` → `parsed` shape, :10/:249 —
  reuse), `web/webgpu/pvr2-renderer.mjs` (`renderFrame` :187, GOLD STANDARD CONFIG),
  `packages/renderer/src/wasm_bridge.cpp` (TA-buffer → Process()/Render() + PVR-snapshot
  wiring to mirror).
- **Risk:** LOW — back-half is built and gold-standard. One open caveat retired in Phase 0:
  confirm emit is RAM display list vs SQ bursts vs Holly DMA list (capture at the right
  place). RE notes say RAM display list.
- **Exit:** end-to-end pixels on screen from snapshot + GSTA, visually + numerically
  matching the live frame for the body slots.

### Phase 5 — Perf hardening (the 60fps risk)
- **Goal:** sustain 60fps for the full render subtree, worst-case included.
- **Budget reality:** interpreter is ~15-40 native instr/op, ~1.5-3× in wasm → ~50-100
  wasm instr/op. Render subtree plausibly 50k–500k SH4 ops/frame. At 100k ops ≈ 7.5M wasm
  instr ≈ ~1.5-7.5ms (fits 16.67ms). At 500k ≈ ~5-37ms (blows budget worst-case). **60fps
  is plausible if the subtree is ~50-150k ops AND load-time decode is excluded.**
- **Mandatory exclusions:** snapshot **after** part decode (pixels already in VRAM) — never
  let the slice re-run the LZSS/part decoder (the Oracle saw the decoder fire ~84k×/match;
  that is load-time work to skip). No dynarec available in this tree (wasm-dynarec is a
  research project, out of scope) → stuck at interpreter speed.
- **Risks:** FP-heavy path + FPSCR/rounding-mode juggling per op; worst-case frames
  (supers, full screen of projectiles) spike op count exactly when least affordable; full
  render-tree cost every frame even when nothing moved (vs current re-bake-on-change).
- **Mitigations:** measure op-count first (instrument the Phase-0 server re-render to count
  ops/frame across the worst observed frames). If too slow, interpret only the geometry/
  quad-emit subtree and let the proven 0.00px JS emitter do the parts it already does
  correctly — at which point reconsider the hybrid (§6).
- **Exit:** sustained 60fps on target hardware for measured worst-case in-match frames, or
  a documented decision to fall back to hybrid for the over-budget cases.

---

## 6. Risks & open questions

### 6.1 Copyright — ROM code on the client (may be decisive)
The replica ships a **16–26 MB MCSV blob that contains MVC2 code and decoded assets** to
every client. CLAUDE.md is emphatic about ROM-derived material. This is a genuine
licensing/DMCA posture change versus every other approach, all of which keep ROM-derived
execution server-side. **This alone may force the hybrid.** Mitigations are weak (content-
addressing reduces frequency, not the fact of shipping). **Open question — must be resolved
before Phase 3, not after.**

### 6.2 Perf — wasm SH4 interpreter @ 60fps
The load-bearing engineering risk (§5 Phase 5). Plausible but not safe-assumed; bounded
50k–500k ops/frame, 60fps only in the lower half of that range and only with load-time
decode excluded. No wasm dynarec available. **Retire by measuring op-count during Phase 0.**

### 6.3 Snapshot completeness / fragility — the engine pointer cluster
The render reads the engine-owned pointer cluster `+0x154…+0x184` and the slot-table node
wiring — exactly the state whose corruption killed live injection
(`project_state_replica_injection_deadend`). The render-only framing **inherits** this
hazard (the body walker dereferences `+0x160`; the walk consumes `0x8C287DE0`). Safe for
re-projection (Tier 1); pose changes require re-deriving `+0x154/+0x160` and re-running
`loc_8c034e8c`. **Open questions:**
- Is `0x8C26A974`'s `fmac` accumulate truly non-idempotent across a re-run? (inferred —
  confirm with read-before/read-after in Phase 0.)
- Which routine populates `0x8C2895E0`/`0x8C287DE0` each frame (the slot-table rebuild
  owner)? Needed to rebuild from GSTA, or snapshot-and-patch-pointers instead. **Not pinned.**
- Tier-2 pose-change safety beyond a single anim-load call — the open risk that re-enters
  the corruption class.
- bank11/bank12 leaf read-sets (transform/vertex/submit) are **inferred from Oracle/CHARQ,
  not read from disasm** — confirm before relying.

### 6.4 The hybrid fallback (server-emits-geometry) — when to switch
**Option (b): the server (full SH4) runs the render and ships a compact per-frame
TA/PVR-geometry delta (CHRQ quads, ~hundreds × ~64B, ~100–500 KB/s); the client feeds it
straight to `pvr2-renderer`.** No client SH4, no ROM blob, no 16MB snapshot, no 60fps risk —
**90% already built** (it is the geometry-only slice of the existing mirror). **Switch to
hybrid when ANY of:**
- §6.1 copyright posture is unacceptable (ship no ROM to clients) — **likely decisive**;
- Phase 0 Tier 1 fails or Tier 2 is unsafe (read-set not closed) — **NO-GO for replica**;
- Phase 5 perf blows budget on worst-case frames with no mitigation;
- time-to-working matters more than the bandwidth win.
The hybrid sits at single-digit-Mbps (between dead-mirror 36–88 and GSTA 0.05), is
pixel-exact, and has zero client CPU/ROM risk. **It is the recommended pragmatic target if
the replica's two unique costs (client CPU + ROM blob) don't justify the ~10× bandwidth win
over hybrid.** Note a third option already in flight — the **pure-JS emitter** (geometry
0.00px) feeding TA quads → pvr2-renderer at ~48 kbps with no SH4 and no ROM — covers
everything except part *pixels* (supplied by `MAPLECAST_PARTDUMP`) and the effects/supers
the offline atlas misses. **The replica is worth pursuing mainly for exactly those gaps;
for those specific gaps, the hybrid is the lower-risk way to fill them than a full
in-browser SH4.**

---

## 7. Effort / Risk profile (risk-retired-per-effort, no time estimates)

Ordered so each step retires the maximum uncertainty for the least work, and so a kill
decision lands as early and cheaply as possible.

1. **Phase 0 — VALIDATION (do first, always).** Near-zero effort (reuses armed Oracle hook
   + `ta_parse(ctx,true)` + Stop/ResetCache; ~env gate + diff). Retires **the single
   load-bearing unknown** (read-set hermeticity) and **gives the perf op-count for free**.
   A red Tier 1 kills the whole idea here, before any client work. **Highest
   risk-retired-per-effort by a wide margin — this is the gate.**

2. **§6.1 Copyright decision (parallel with Phase 0, before Phase 3).** Zero engineering
   effort; pure policy. Can kill the replica independent of any technical result. Resolve it
   while Phase 0 runs so a green Phase 0 isn't wasted on a non-shippable approach.

3. **Phase 1 — Snapshot export (MCSV content-address).** Low effort (~50 lines, primitives
   exist). Retires the "can we even get the static memory to the client cheaply" question
   and the per-match amortization story. Low risk.

4. **Phase 2 — GSTA field additions.** Low effort, append-only, no parser break. Retires
   wire-completeness. May be skippable if Phase 0 Tier 2 proves client-side re-derivation.

5. **Phase 3 — Client SH4 slice.** **Highest effort, highest residual risk** — but its
   chief uncertainty (hermeticity) is already retired by Phase 0, so it becomes an
   engineering carve-out rather than a research gamble. Still gated by bank11/12 leaf
   confirmation. Do not start before 1–2 are green.

6. **Phase 4 — pvr2-renderer wiring.** Low effort, low risk (back-half is gold-standard and
   the replica output is byte-identical in kind). Cheap once Phase 3 produces a taBuffer.

7. **Phase 5 — Perf hardening.** Effort unknown until Phase 0's op-count lands; risk
   front-loaded into that measurement. If the count is in the bad half of the range, this
   phase may trigger the hybrid fallback (§6.4) rather than a heroic optimization.

**Decision rule:** proceed past each phase only on its exit criteria; treat §6.1 and Phase 0
Tier 1 as hard gates. If either fails, switch to the **hybrid (server-emits-geometry)**,
which delivers the same pixel-exactness at single-digit Mbps with none of the replica's two
unique costs.

---

## 8. Option C — AOT transpile: SCOPE & PATH (CHOSEN executor)

Static analysis of the full render call-tree (BFS over the marvelous2 disasm). **Verdict: MEDIUM
tractability, toward the small end — more tractable than §5/§6.3 assumed.**

**§6.3 CORRECTION:** the "bank11/bank12 leaf bodies ABSENT from the disasm" claim is **FALSE
for this checkout** — every render-tree function (`loc_8c1216c0`, `loc_8c11e2e0/e460/e860`,
`loc_8c1244b0`, …) is present with a full body (earlier greps missed UPPERCASE `loc_8C…`
labels). **Zero functions are referenced-but-missing.** Removes the single largest Option-C risk.

**Scope (CONFIRMED):**
- **114 functions / 842 basic blocks / ~9,850 SH4 instructions** across banks 02/03/11/12/13/17 — ~3% of the game. Bounded. Root = `loc_8c0308c2` (Render_sprites slot-walk); `loc_8c0305d8` is a sibling setup entry (root BFS at both).
- The GFX1 LZSS/part decoder (`loc_8c033d78`) is **NOT in the tree** (load-time only) — Phase-5's mandatory exclusion is automatic.
- **85 distinct opcodes.** Substantial FPU subset: `ftrv`×20 (matrix×vec transform core), `frchg`×12/`fschg`×22 (must model FPSCR.FR/SZ bank+size state), `fsca`×3 (sin/cos table), `fmac`×14, `fmul`/`fdiv`/`fadd`. **Single-precision throughout** (1 isolated `fcnvsd`). **ZERO privileged/MMU/trap/SMC/MMIO** instructions — clean user-mode FP+integer. No `trapa/sleep/ldtlb/movca/ldc/rte/fipr/fmtrx`.
- **Indirect branches ~96% statically resolved:** 253 `jsr/jmp` via ROM pool constants (trivial def-use), 2 ROM jump tables (17+8 enumerable targets: the cell-processor Duff device `loc_8c1295bc` + the render-mode `braf` switch), and **only 1 true RAM vtable** (`loc_8c0305ce`, per-object render-override at node+0x28) — handled by a one-site runtime `switch(pc)` escape hatch (the body-only milestone sidesteps it). **No general indirect-jump problem.**
- **No SMC, no code-as-data** beyond legit `#data` pools + the static jump tables.

**Toolchain (CHOSEN): lift-to-C (N64Recomp-style) → emscripten → wasm.** Each SH4 function → a C
function over a `Sh4Ctx{r[16],fr[16],xf[16],fpscr,fpul,pr,…}` + flat `ram[16MB]+vram[8MB]`
(`translate(a)=a&0x1FFFFFFF`, area-route; **no MMIO routing needed** — tree touches only area-3
RAM+VRAM). **Key leverage: harvest opcode semantics from flycast's interpreter**
(`core/hw/sh4/interpr/sh4_opcodes.cpp`) — it has byte-exact, determinism-validated C for all 85
mnemonics incl `ftrv`/`frchg`/`fsca`/`div1`/FPSCR; the lifter emits the *same* C, inlined +
specialized on the decoded operands, with native C control flow replacing the dispatch loop.
Optionally lower flycast's `shil` IR instead of raw decode. (Rejected: hand-written SH4→wasm
(max FP-bug surface); retargeting the dynarec to wasm (no wasm backend, structured-CF mismatch).)

**Control flow is trivial here** — marvelous2 already labels all 842 BBs (function→C function,
BB→label/switch arm, delay slots emitted before branch effect); the 2 jump tables→`switch(idx)`
over enumerated targets; the 1 vtable→`switch(pc)`. No general indirect resolver needed.

**Riskiest part: FP bit-exactness of the `ftrv`/`fsca`/`frchg` transform chain.** wasm IEEE-754
binary32 matches SH4 single-precision for `+−×÷` (straight C `float` is bit-exact for arithmetic);
care only for `ftrv` accumulation order, `fsca` (table), `ftrc` (truncation), and FPSCR.FR/SZ
routing. **Mitigation already built:** `validate_emitter_geom.py` 0.00px diff vs the Frame Oracle
+ the pure-JS emitter (also 0.00px on the body path) = two independent oracles. Unknown: FPSCR
rounding mode at entry (INFERRED RM=0; confirm with one Oracle FPSCR read).

**SMALLEST FIRST MILESTONE (the C proof-of-concept, independent of Phase 0):** transpile
**`loc_8c0344d4` (body walker, 6 BBs) + its single leaf `bank11.loc_8c11e460` (vertex builder)** —
fully static, NO indirect branches/vtable/jump-table. Feed one snapshot node (known
character_id+sprite_id), diff its emitted quads vs the CHARQ ground truth
(`_ryu_capture/probe_body_uv.json`) via `validate_emitter_geom.py`. **0.00px = the lift-to-C
transpiler + FP model are PROVEN** for the body path. Then expand: transform `loc_8c1216c0/1219b0`
(proves `ftrv`/`frchg`) → submit `loc_8c1244b0/12e6cc` → root `loc_8c0308c2` → effect path +
the `loc_8c0305ce` vtable last. This milestone needs neither Phase 0 nor a browser — it's a
native C diff against existing ground truth, so it de-risks the whole transpiler approach cheaply.
