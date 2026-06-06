# MARVELOUS2 GFX Notes — How MVC2 Renders a Character from Sprite PARTS

Clean-room notes from reading the `marvelous2` SH4 disassembly (NTSC-U Dreamcast MVC2).
**Facts only** — addresses, field offsets, and algorithm descriptions in our own words.
No verbatim instruction listings. Cite format: `bankNN.asm` / `loc_XXXX` / `char_prg/...`.

> Goal: replicate the game's per-frame "draw character from PARTS + assembly list" so a
> low-bandwidth client can render pixel-exact characters from `sprite_id` alone.

---

## 0. TL;DR — the answer to the blocker

The on-disc GFX "offset table → blob" indirection that didn't line up with the 22
`part_idx` values is **not indexed by `part_idx` at all** at the level we were guessing.
The real pipeline is:

1. **`sprite_id` (player+0x144) selects an animation cell** via the ANIMATION pointer
   (player+0x168). The cell pointer is cached at **`current_cell_data` (player+0x154)**.
2. **The EXTRAS list (player+0x178) is the per-cell assembly** — a list of 8-byte part
   placements. The game does **not** re-resolve `part_idx` against the raw GFX offset
   table on the SH4 at draw time. Instead, at draw time it reads an **already-resolved
   "draw object"** whose texture/cell pointer field (+0x84) holds a pointer fetched from
   one of the player DAT pointer slots (GFX1 0x15c / GFX2 0x160 / ANIMATION 0x168 /
   EXTRAS 0x178), and whose UV/size come from small **per-sprite-type lookup tables**
   (atlas grids) indexed by a running part counter — not by `part_idx`.
3. **The `part_idx → pixels` map you must build offline is exactly what your existing
   PLDAT decoder already produces** (the GFX offset table, decoded to part rectangles).
   The SH4 confirms the *placement and atlas/UV* semantics; it does **not** add a hidden
   per-`part_idx` GFX indirection beyond the offset table you already have. The
   "indirection layer" mismatch (1533/441 blobs vs 22 part_idx) is because **GFX1/GFX2
   offset tables are character-wide pools of cells; `part_idx` in EXTRAS indexes the
   per-character part set, and the ANIMATION cell selects which EXTRAS assembly runs.**
   See §2 for the precise resolution and the one residual unknown.

**Bottom line for the renderer:** keep doing offline `GFX offset-table → part rects`
(your decoder is the oracle), and `sprite_id → EXTRAS assembly → [{part_idx,dx,dy,flip}]`.
The SH4 does the same thing; it just caches the resolved cell pointer and pulls UVs from
atlas tables. No new on-disc indirection to reverse. (§2 lists the one thing still worth
confirming empirically.)

---

## 1. Player struct pointer slots (the DAT header, in RAM)

From `memory/pl_mem.asm`. At runtime the PLxx_DAT header pointers live in the player
struct (base e.g. P1C1 `0x8C268340`, stride `0x5A4`):

| Player offset | Symbol | DAT header off | Meaning |
|---|---|---|---|
| 0x144 | `sprite_id` (u16) | — | current sprite/cell id (drives animation) |
| 0x154 | `current_cell_data` (ptr) | — | cached pointer to the selected cell |
| 0x158 | `anim_id`/`anim_group` (u8) | — | animation group selector |
| 0x15c | `Dat_GFX1` (ptr) | 0x00 | GFX pool 1 (part pixels, planar 4bpp) |
| 0x160 | `Dat_GFX2` (ptr) | 0x04 | GFX pool 2 (part pixels) |
| 0x164 | `Dat_Pal` (ptr) | 0x08 | palette |
| 0x168 | `animations` (ptr) | 0x14 | animation keyframe table |
| 0x16c | `hitbox_pattern_table` (ptr) | 0x18 | — |
| 0x170 | `hitbox_data` (ptr) | 0x1c | — |
| 0x174 | `attack_data` (ptr) | 0x20 | — |
| 0x178 | `Sprite_Extras` (ptr) | 0x0c | **EXTRAS = the assembly list** |
| 0x17c | `Dat_FilePointer` (ptr) | — | base of the loaded DAT file |
| 0x184 | `FAC_ptr` (ptr) | — | the loaded **FAC** companion file |

> Note the DAT-header order vs player order differs (extras is header 0x0c but lands at
> player+0x178). `Dat_FilePointer` (0x17c) is the DAT base for resolving file-relative
> offsets into RAM pointers.

### Master draw-context globals (RAM)
- `0x8C26A904`, `0x8C26A908` — "current draw object" master pointers. Double-deref reaches
  the **player struct**: `(*(*0x8C26A908))` → player; the setup code then reads
  `@(0x160/0x164/0x168/0x178, player)` from there (`bank0f.asm` `loc_8C0FE3BC`,
  `loc_8C0FE3F6`, `loc_8C0FE458`, `bank10.asm` `loc_8C1075EA`).
- `work.GameGlobalPointer` — global game state base.

---

## 2. THE KEY MAPPING: `part_idx` → pixels (PSEUDOCODE)

### What the SH4 actually does (draw time)
The body draw entry is **`bank10.asm:loc_8C107122`** (the per-character composer). It:
- allocates a 0x84-byte **draw object** (alloc `bank04.asm:loc_8c044F12`),
- installs handler `bank10.asm:loc_8C106F68` (a state machine, byte@0x04 = state),
- reads flip from **player+0x130** (`xflip_copy`) → sets the 0x8000 mirror flag
  (stored at draw-obj+0x44, see §6),
- reads **`Dat_GFX1` (player+0x15c)** through the master-pointer path
  (`bank10.asm:loc_8C1070CC` / `loc_8C1070D4`).

The EXTRAS list is bound to the draw object at **`bank10.asm:loc_8C1075EA`**:
```
extras_ptr   = *(*(*0x8C26A908))            ; = player
asm_list_ptr = *(extras_ptr + 0x178)        ; = Sprite_Extras (EXTRAS base)
draw_obj[0x84] = asm_list_ptr               ; cell/assembly pointer field
```
So **draw_obj+0x84 = the assembly-list pointer**, and the body handler walks it.

The per-record texture/UV resolution (`bank10.asm:loc_8C106FB2`, the "animate" state):
```
# obj.sprite_id at a small offset; two parallel tables selected by it
height_tbl_base = HEIGHT_TABLE                  # u16 entries, row stride = id*0x10
uv_tbl_base     = UVSCALE_TABLE                 # float entries, row stride = id*0x20
part_counter    = obj[0x1C]                      # running index within the cell
obj[0x1E] = u16  height_tbl[id_row + part_counter*2]
obj[0x74] = float uv_tbl[id_row + part_counter*4]   # per-part scale/UV
```
i.e. **size + UV come from per-sprite-type atlas tables indexed by a running part
counter (obj+0x1C), NOT by `part_idx`.** The texture *base* (which VRAM page / blob) is
the +0x84 pointer, advanced by a fixed cell stride per part:
`bank10.asm:loc_8C107602/loc_8C107612` advances draw_obj+0x84 by **0x38e bytes per part**
and indexes a UV float table at `bank13.asm:0x8C13DF9C` by obj+0x1C.

The atlas UV grid itself is a constant table at **`bank13.asm:loc_8c13DE20`**: float
pairs `{0.0, 0.25, 0.5, 0.75, ...}` — i.e. parts live on a fixed UV grid inside a texture
page, selected by sub-position.

### So the resolution, in renderer terms
```
# OFFLINE (your PLDAT decoder already does this — keep it as the oracle):
#   GFX1/GFX2 begin with a u32 offset table → each entry is a part-cell blob
#   (planar 4bpp tiles). Decode each to a rectangle. This is your part atlas.
#   part_idx indexes the per-character PART SET (0..21 for PL00).

# RUNTIME (what we replicate):
def draw_character(player):
    cell = select_cell(player.sprite_id, player.animations)   # §4
    asm  = cell.extras_assembly      # one assembly = list of 8-byte records (§3)
    for rec in asm:                  # rec = {dx, dy, part_idx, b5, mode, flip}
        if rec.mode == 0xFF: break   # assembly separator
        part = part_atlas[rec.part_idx]          # OFFLINE-decoded rectangle
        x = char_screen_x + (rec.dx if not hflip else -rec.dx - part.w)
        y = char_screen_y + rec.dy
        blit(part, x, y, hflip = rec.flip & 0x80, palette = char_palette)  # §5,§6
```

### The ONE residual unknown (and where to look)
Whether GFX1 vs GFX2 (and which offset-table entry) is chosen **per `part_idx`** by a
small per-character index table, or whether `part_idx` indexes one pool directly. The SH4
caches the resolved pointer (draw_obj+0x84) and advances it by a fixed stride, so the
*selection* table is built when the cell loads, not at blit time. **Verify empirically:**
your decoder already emits `*_assembly_layout.png` for PL00 — if those match the in-game
frame, `part_idx → part_atlas[part_idx]` (one pool, offset-table indexed) is correct and
there is no hidden table. If a few parts are wrong, the missing piece is a per-character
"part_idx → (pool, cell)" remap table, which would live in the **FAC file** (player+0x184)
— FAC is the companion file loaded next to each DAT (`pl_A_FACfile` etc. in `pl_mem.asm`)
and is the natural home for cell-geometry metadata. Look there next.

---

## 3. EXTRAS walk (the assembly iterator)

Confirmed 8-byte record layout (matches `docs/PART-ASSEMBLY-PLAN.md`):
```
struct ExtrasRec {           // 8 bytes
    s16 dx;                  // +0  signed X offset
    s16 dy;                  // +2  signed Y offset
    u8  part_idx;            // +4  index into the character's part set
    u8  b5;                  // +5  (sub-state / counter; used as table index too)
    u8  mode;                // +6  0xFF = assembly separator/header
    u8  flip;                // +7  bit 0x80 = horizontal mirror
};
```

The generic list iterator engine is **`bank10.asm:loc_8C108060`** (init) +
**`loc_8C108086`** (step), with public driver **`loc_8C10823E`** (init then loop step
until done). State lives in scratch globals `0x8C28C864..0x8C28C87c` (all bank10-local):

- `loc_8C108060` seeds the walk: `record_ptr = gfx/extras_base + 0x18`,
  `out_slot_ptr = +0x18` (so the first 0x18 bytes are a header, then records/slots).
- `loc_8C108086` advances one record per call:
  - record stride is **0x08** for normal records (`add 0x08`), and there is a
    **0x50-byte** sub-structure stride for nested/instanced parts (`add 0x50`) — i.e. the
    composer also supports a "0x50-byte part instance" form, used by the layered draw list.
  - a **sign test** on the first word distinguishes a real record from a control word;
    a **0x10 bit test** flags a special record class; the **repeat count** lives in a
    separate global and the loop emits one output slot (0x20 bytes) per part.
  - returns 0xFF-equivalent / zero to signal "assembly finished" → matches `mode==0xFF`.

So: **walk 8-byte records from EXTRAS; `mode==0xFF` ends the current assembly; each
non-terminator record is one part placement.** This is the recipe your offline grouping
already cracked (42 assemblies for PL00, parts 0–21).

> The same `loc_8C108060/86` engine is reused all over (bank01/02/03/0f/10/11 — dozens of
> `#data loc_8c108060` vtable refs). It is the **generic display-list composer**, not
> character-specific. That's why it's pointer-table-driven.

---

## 4. ANIMATION → assembly (`sprite_id` → which EXTRAS assembly)

- **`sprite_id` = player+0x144** (u16). The ANIMATION table = player+0x168.
- Animation keyframes are 20-byte records (sprite_id at bytes 4–6, per
  `PART-ASSEMBLY-PLAN.md`). The animation system walks keyframes by `anim_id`
  (player+0x158) + frame timer (`frame_count` player+0x142, `anim_timer`/`anim_flags`
  region) and writes the resolved **cell pointer to `current_cell_data` (player+0x154)**.
- At draw time the body composer reads `sprite_id` and uses it to index the per-sprite
  size/UV tables (§2, `loc_8C106FB2`), and binds the EXTRAS assembly pointer
  (`loc_8C1075EA`). So the chain is:
  ```
  anim_id + timer  →  keyframe  →  sprite_id (0x144)  →  cell (0x154)
                                                      →  EXTRAS assembly (0x178)
                                                      →  size/UV row (sprite_id-indexed)
  ```
- **Practical shortcut for the client:** the GSTA already ships `sprite_id`. Map
  `sprite_id → assembly index` offline (the keyframe/cell table gives this), then render
  that assembly's parts. No need to emulate the timer — `sprite_id` is the resolved state.

Animation flags worth noting (player+0x14a `anim_flags`, from `pl_mem.asm`): 0x20 = no
cancel, 0x40 = recovery, 0x80 = opponent can proximity-block. These are gameplay, not
render, but they ride along on the same animation struct (header byte 0x10 of anim recs).

---

## 5. Palette

- Palette pointer = **`Dat_Pal` (player+0x164)**, the DAT header 0x08 pointer. It is
  bound into the draw object's +0x84 cell-pointer family by the effect-object setup
  (`bank0f.asm:loc_8C0FE3BC` reads `@(0x164, player)`).
- **Per-character, not per-part** — the body uses one palette pointer; parts share it.
  (The skin/PVR-bank work we already do operates at this granularity: one palette bank per
  character slot — see `CLAUDE.md` "PVR palette bank formula".)
- Format (from our existing SKIN-SYSTEM): ARGB4444 LE, 16 colors/palette, 32 bytes/palette,
  index 0 = transparent. The DAT palette pointer feeds the same 16-entry banks.
- `pl_palid_match` (player+0x25) and `pal_id` (player+0x52d) select **which** palette
  (color variant). NOTE: `0x25` is the match-selected color; `0x52d` is a copy — the GSTA
  work already flagged this (`MEMORY.md`: "palette 0x25 not 0x52D").

---

## 6. dx/dy/flip semantics

- **Origin:** `dx`/`dy` are added to the **character's screen position**, i.e.
  `x_pos_screenspace` (player+0xe0) / `y_pos_screenspace` (player+0xe4) — these are the
  on-screen pixel coordinates (CLAUDE.md `screen_x/screen_y`). The composer reads the
  character's transform fields (player+0x34/0x38 world, +0x50/0x54 scale) and adds the
  part's dx/dy as a local offset (`bank0f.asm:loc_8c0332e0` multiplies a per-part byte by
  the +0x50 scale float; `bank10.asm:loc_8C106FB2` does `obj_pos += char_pos` via fadd on
  the 0x34/0x38 and 0x5c/0x60 fields).
- **Scale:** per-character `x/y/z_sprite_scale` (player+0x50/0x54/0x58) multiplies part
  geometry. For our client this is the "size = const 1.75×" we already use; zoom=1
  (no dynamic zoom) per `MEMORY.md`.
- **flip (bit 0x80):** horizontal mirror. The mirror flag (0x8000) is set from
  `xflip_copy` (player+0x130) at composer setup (`loc_8C107122` → `loc_8C10719C`) and
  stored at draw-obj+0x44. Mirroring is applied in the **vertex/UV transform**
  (`bank11.asm` `loc_8C11F870`/`loc_8c11f890` use `xmtrx` ftrv with a sign-flipped basis;
  the 0x44 flag swaps U). **Mirror is about the part's placement frame, not its own
  center:** when flipped, the part's `dx` is reflected across the character pivot and the
  texture U is reversed, so a part at `(+dx, dy)` becomes `(-dx - w, dy)` with U mirrored.
  This is the standard "flip around the character's facing axis" — replicate as
  `x = char_x + (flip ? -dx - part.w : dx)` and draw the part horizontally mirrored.
- `facing` (player+0x110) and `xflip`/`xflip_copy`/`xflip_copy_2` (0x1d2/0x130/0x110)
  all carry the side/facing; the composer uses the 0x130 copy. For the client, use the
  character's facing to drive the per-assembly flip globally, then XOR with each record's
  0x80 bit (some parts are pre-flipped relative to the body).

---

## 7. The cape question — is the cape IN the body assembly?

**Evidence says: capes/separable appendages are drawn as their OWN object (separate
sprite/draw object), NOT as parts inside the body's EXTRAS assembly.**

Reasoning from the disassembly:
- The composer is a **generic display-list engine** (`loc_8C108060/86`) driven by
  per-state vtables (`bank16.asm:loc_8c1658xx`, `bank0f.asm:loc_8c164bec/ca8`). Each
  "draw object" is independent and is queued into a **priority-sorted display list**
  (`bank04.asm:loc_8c0450C0`, sorting by the byte@0x03 layer/priority). Capes have
  their own z-order relative to the body, which requires a separate object.
- The effect/secondary-object setup routines (`bank0f.asm:loc_8C0FE2BA`,
  `loc_8C0FE2DE`, `loc_8C0FE344`, `loc_8C0FE3BC`, `loc_8C0FE3F6`, `loc_8C0FE458`,
  `loc_8C0FE516`, `loc_8C0FE5E8`) each pick a **different GFX/ANIM/PAL pointer**
  (`@0x160/0x164/0x168, player`) and a **different sprite-type** (byte@0x20/0x21 → its own
  size/scale row in `bank16.asm:loc_8c164c18/c30/c78`). That is exactly how a cape would be
  driven: a sibling object with its own animation timer and z-priority, sharing the
  character's transform but with its own assembly.
- The per-character SPL programs (`char_prg/code/S_PLxx.asm`) spawn these secondary
  objects (the `#data bank10.loc_8c108060` refs in bank01/02/03 are the SPL-driven
  spawns).

**Implication for the renderer:** rendering the **body** assembly will **NOT**
automatically include the cape. The cape is a separate object with its own
`sprite_id`/assembly and z-order. To get capes (Magneto, Dr. Doom, Storm, etc.) you must
also render the character's **secondary objects**. The good news: the GSTA object table
(`0x8C26A000`, per `MEMORY.md` pointer-follow notes) already enumerates these sibling
objects — render each object the same way (its own sprite_id → assembly → parts), honoring
its layer/priority byte for z-order. Treat the cape as "just another object," not as part
of the body.

> If you want a quick confirmation per-character: dump the live object table during a cape
> frame and check for a second object sharing the character's transform but with a distinct
> `sprite_id` and a draw-priority that interleaves with the body.

---

## 8. Routine index (citations)

| Routine | Where | Role |
|---|---|---|
| `loc_8C107122` | `bank10.asm` | per-character **body composer** (allocates draw obj, binds GFX1, flip) |
| `loc_8C106F68` | `bank10.asm` | body draw-object **handler dispatch** (vtable by state byte@0x04) |
| `loc_8C106FB2` | `bank10.asm` | **animate state**: size+UV from sprite_id-indexed tables (the part→size/UV map) |
| `loc_8C1075EA` | `bank10.asm` | binds **EXTRAS assembly** ptr (`@0x178, player`) to draw-obj+0x84 |
| `loc_8C107602/612` | `bank10.asm` | advances cell ptr by **0x38e/part**, UV from `0x8C13DF9C` by obj+0x1C |
| `loc_8C108060` | `bank10.asm` | **list iterator init** (record_ptr = base+0x18) |
| `loc_8C108086` | `bank10.asm` | **list iterator step** (8-byte stride, 0x50 sub-stride, 0xFF end) |
| `loc_8C10823E` | `bank10.asm` | public driver: init + loop the iterator |
| `loc_8c0331d8` / `loc_8c0332e0` | `bank03.asm` | per-part **quad geometry** build (scale × part byte) |
| `loc_8c03319e` | `bank03.asm` | EXTRAS/record index advance helper (state at `0x8c1f9d7c`) |
| `loc_8c0450c0` | `bank04.asm` | **display-list insert**, priority-sorted (byte@0x03 = layer) |
| `loc_8c044F12` | `bank04.asm` | draw-object **allocator** |
| `loc_8C0FE2BA..5E8` | `bank0f.asm` | **secondary-object** (effects/cape) setup; each picks own GFX/ANIM/PAL/type |
| `loc_8c1294C8` | `bank12.asm` | fast 0x40-byte **param block copy** (Duff's-device) into draw slot |
| `loc_8C122560` | `bank12.asm` | matrix-driven **vertex transform** + UV emit |
| `loc_8C11F870` / `loc_8c11f890` | `bank11.asm` | **vertex transform** quads (xmtrx ftrv; handles mirror basis) |
| `loc_8c13DE20` | `bank13.asm` | **UV atlas grid** constants (0.0/0.25/0.5/0.75 …) |
| `0x8C13DF9C` | `bank13.asm` | per-part **UV/scale float table** (indexed by obj+0x1C) |
| `loc_8c164c18/c30/c78` | `bank16.asm` | per-sprite-type **size/scale** tables (secondary objects) |
| `loc_8c1658xx` | `bank16.asm` | draw-handler **vtables** |
| `0x8C26A904 / 0x8C26A908` | RAM | "current draw object" master pointers (double-deref → player) |
| `0x8C28C864..87c` | RAM | iterator scratch state |

---

## 9. Net guidance for the client renderer

1. **Keep your offline PLDAT decode** (`GFX offset table → part rects`, planar 4bpp tiles
   via `combine_planes()`). That is the authoritative `part_idx → pixels`. The SH4 does not
   add a hidden per-`part_idx` GFX indirection at blit time — it caches a resolved cell
   pointer and uses atlas UV tables.
2. **Build `sprite_id → assembly` offline** from ANIMATION + EXTRAS (you have the 42
   assemblies for PL00). The GSTA ships `sprite_id`, so no timer emulation needed.
3. **Render each object independently**: body and each secondary object (cape, weapon,
   projectile) are separate draw objects with their own `sprite_id`/assembly and a
   z-priority byte. Iterate the live object table (`0x8C26A000`) and draw each.
4. **Placement:** part screen pos = `char screen_x/y (0xe0/0xe4) + (dx,dy)`, scaled by
   `sprite_scale (0x50/0x54)`; flip = char facing XOR record bit 0x80; reflect dx across
   the pivot and mirror U when flipped.
5. **Palette:** one per character (`Dat_Pal` 0x164), selected by `pal_id`/`pl_palid_match`;
   ARGB4444 LE, 16 colors, index 0 transparent — same banks our skin system uses.
6. **Cape verification (do this):** confirm the cape appears as a sibling object in the
   live object table; if not present there, check the FAC file (player+0x184) for a part
   remap. This is the only step that needs an in-game check to be 100% certain.
