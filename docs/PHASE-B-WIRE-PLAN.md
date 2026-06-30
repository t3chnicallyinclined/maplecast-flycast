# Phase B — Client-Side Engine-Table Setup (GSTA-scale wire)

Goal: stop shipping the engine's render tables; have the client COMPUTE them from the
char structs + local-ROM GFX each frame. Target wire: ~hundreds of bytes/frame (from ~7 KB).

## Key reframing (from the scoping pass, 2026-06-13)
Phase B is NOT one giant "transpile the setup." It's three separable wins by table:

| Region | Writer (loc) | Per-frame? | Class | Action |
|---|---|---|---|---|
| **rectab** `*0x8C2DAD4C` (0x10000) | loc_8c123e00 | **match-stable** | pure (load-time) | **demote to ship-once/on-change — NO transpile** |
| **idxtab** `*0x8C2DAD3C` (0x2000) | loc_8c123e00 | **match-stable** | pure (load-time) | **demote to ship-once/on-change — NO transpile** |
| **tiledesc** `0x8C1F9F9C` (0x1800) | loc_8c033b0a | YES | pure(char+GFX) | **transpile into slot-walk (the real work)** |
| **cam_mat/camZ** `0x8C2D6AD8` (0x100) | loc_8C1216C0 | per-frame | MIXED/NEEDS-RE | keep on wire (tiny) until camera-source REd |
| **arena** `0x8C1F9D80` (0x20) | loc_8c033950 | 1-bit parity | pure | compute client-side |

The decisive insight: **rectab + idxtab are constant for the whole match** (built at char-LOAD
time by `loc_8c123e00`, not per-frame) — so they just need ship-once-then-on-change (reuse the
existing `collectFreshGfx`/`pvrPalSig` change-only pattern). That removes ~72 KB/frame raw with
zero transpile. The only genuinely per-frame table is the tiledesc.

## Implementation order (safest → hardest)
- **B1 — arena:** compute `arena_base = parity ? 400 : 16` + reset cursor in `render_frame_reset()`
  (render_frame.c already models G_ARENA_BASE/G_OBJ_CURSOR). ~0 risk. Validates dropping `arena`.
- **B3 — rectab/idxtab demotion:** move them out of `_dynRegs` (per-frame) into a change-only tail
  in `maplecast_replica_live.cpp` (sig over the table; re-ship only on change/tag-in). No transpile.
  **Biggest wire win.**
- **B2 — tiledesc transpile (the core):** new `gen_desc_build.{py,c}` lifting `loc_8c033b0a`
  (bank03:8720) — per node: snapshot cursor→`+0xDC`, read sprite_id(+0x144)/GFX2(+0x160)/GFX1(+0x15C)/
  facing, walk cell records, write m/count/pitch into a client DESC_TABLE before the walker reads it.
  Reuses render_frame.c's already-proven cursor prefix-sum. NEEDS-RE: the m/count/pitch arithmetic
  in loc_8c033ba8..loc_8c033d44.
- **B4 — camera (optional/defer):** lift `loc_8C1216C0` + matrix leaves (ftrv/fsca tree; opcodes
  already in codegen). Only after REing whether `0x8C2D6900/0x8C2D690C` are char-struct-derivable.
  Until then cam_mat stays on wire (256 B — negligible).

## Minimal wire after Phase B
Char structs (8.6 KB) + slot tables (3 KB) + narrowed objpool (few KB) + globals (<1 KB) ≈ ~15 KB
raw → hundreds of bytes/frame zstd (char structs compress hard frame-to-frame). Dropped: rectab +
idxtab + tiledesc (~78 KB raw, the bulk).

## Validation (truth-mandate, incremental, one table at a time)
The shipped tables + the ASMTRACE are ground truth. Per table: byte-diff client-computed vs the
wire's shipped bytes → 0 mismatches → drop from wire → re-run `_diff_vs_asm.mjs` 0.00px gate.
Negative control each (zero the computed table → diff must break; proves non-circular).
- B2 tiledesc is the decisive diff: computed DESC_TABLE m/count/pitch == shipped `0x8C1F9F9C` bytes,
  cross-checked by `diag_tile_pos.c` (the proven 86/86 m-match gate).

## Interactions / sequencing
- Independent of the carve-vs-Oracle decode work (carve is downstream of the walker; unchanged if
  B2's DESC_TABLE diff is 0 → same m → same colrow).
- **B2 must build descriptors for satellite nodes too** (the cursor/descriptor prefix-sum spans all
  cats) → sequence B2 AFTER the cat 1-4 satellite dispatch lands, or build for every walked node
  (loc_8c033b0a is cat-agnostic). Same per-object loop the satellite work touches.
- objpool wire-narrowing (ship only active nodes) is a follow-on, depends on satellite enumeration.

## Critical files
- `tools/render-replica-poc/render_frame.c` — slot-walk + cursor (B1/B2 insertion)
- `core/network/maplecast_replica_live.cpp` `buildTables()` — drop tiledesc/rectab/idxtab; add change-only tail (B3)
- `_marv_re/build/bank03.asm` — loc_8c033b0a @8720 (B2), loc_8c033950 @8466 (B1)
- `_marv_re/build/bank12.asm` — loc_8C1216C0 @3122 (B4 camera), loc_8c123e00 @8842 (rectab/idxtab ref)
- `tools/render-replica-poc/build_wasm_frame.sh` — register new gen_desc_build.{py,c}
