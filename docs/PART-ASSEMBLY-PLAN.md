# Part-Assembly Rendering — Plan (branch `feat/part-assembly`)

> **Why:** the whole-sprite approach (branch `feat/rom-asset-probe`) places *pre-baked
> whole sprites* and *reconstructs* placement from state — which keeps fighting us on
> cape-attach, alive, z-order, because those are runtime/assembly facts we throw away.
> **MVC2 doesn't draw whole sprites — it assembles each frame from reusable PARTS** at
> per-frame offsets. If we render the same way (parts + the assembly list), it's
> pixel-exact and the whole class of bugs dissolves. Still low-bandwidth: the assembly
> is deterministic from `sprite_id`, which the GSTA already ships.

## What we confirmed (2026-06-06)
- **The codec is already cracked / decoded.** `dasm_PLDAT/` (the `dasm_PLDAT_v005a.py`
  tool + decoded `Output/PL00_DAT/`) gives us, per character DAT:
  - `GFX_DATA_00/01.BIN` — the **part pixels** (planar 4bpp tiles; `combine_planes()` decodes)
  - `EXTRAS_DATA.BIN` — the **assembly lists**: 8-byte records `dx(s16) dy(s16) part_idx(u8) b5 mode flip` (rec with `mode=0xFF` = separator/header; `flip` bit 0x80 = hflip)
  - `ANIMATION_DATA.BIN` — keyframes (the 20-byte records; → sprite_id → assembly)
  - `PALETTE_DATA.BIN` — the palette
  - `*_assembly_layout.png` — proves it: each frame = part rectangles at their offsets
- PLDAT header pointers: gfx1@0x00, gfx2@0x04, palette@0x08, extras@0x0C, animations@0x14,
  hitbox_pattern@0x18, hitbox@0x1C, attack@0x20, AI@0x24+.
- Only **PL00** is decoded locally; the rest extract from the operator's ROM (GDI → PLxx_DAT.BIN → the tool).

## Architecture: precompute offline, render by sprite_id at runtime
1. **Offline, per character:** parse GFX (parts) + EXTRAS (assemblies) + ANIMATION → emit
   a **part atlas** (`PL{hex}_parts.png`) + an **assembly table**
   (`PL{hex}_asm.json`: `sprite_id → [{part_idx, dx, dy, flip}], + part rects + pal`).
   Operator-local, gitignored (ROM-derived).
2. **Runtime (client, low-bandwidth):** the GSTA already ships `sprite_id` per char + pool
   object. The client looks up the assembly for that sprite_id and **draws the parts at
   their offsets** from the part atlas — exactly the SH4 render loop. No new wire data;
   the assembly is deterministic.
3. **Result:** cape/crouch/jump, effects, projectiles, supers — all exact, because we
   replay the game's own assembly instead of guessing placement. Same ~30 KB/s.

## Build steps
- [ ] **GFX part format** — map `part_idx → (tile offset, width, height, pixels)` in
  `GFX_DATA_00`. (Layout PNG shows the rects; need the part table / dims source.)
- [ ] **EXTRAS grouping** — split the 8-byte records into per-assembly groups (separator = `mode=0xFF`?).
- [ ] **ANIMATION → assembly** — map `sprite_id` (0x144 / pool +0x12C) → the assembly index.
- [ ] **Prototype on PL00** — render one sprite's assembly, validate against the layout PNG / a live TA frame.
- [ ] **Baker** — emit `PL{hex}_parts.png` + `PL{hex}_asm.json` per char (extend `bake.mjs`).
- [ ] **Client render** — new `assembly-render` path: sprite_id → parts; replaces the
  whole-sprite draw for bodies + pool objects. Flip/palette per the records.
- [ ] **Roster** — extract all PLxx_DAT from the GDI, decode, bake.

## Open questions to resolve from the decoder/data (not guesses)
- Part dimensions: per-part header in GFX, or derived from the EXTRAS rect? (layout PNG drew rects — find the source)
- Does a body assembly include its cape parts, or is the cape a separate `sprite_id` assembly (pool object)? Determines whether cape "just works."
- Palette: per-part or per-character bank? (ties into the existing skin/pal128 work)

## Keep
- The GSTA pipeline, prediction, HUD, palette-recolor — all unchanged, all reused.
- The whole-sprite path stays on `feat/rom-asset-probe` as the working fallback.
