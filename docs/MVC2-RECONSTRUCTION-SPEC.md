# MVC2 Per-Frame Render Pipeline — Reconstruction Spec

> **What this is.** The exact per-frame render algorithm MVC2 (NTSC-U Dreamcast / SH4) runs, traced
> from the `marvelous2` disassembly, written as an implementable spec for a from-state "replay
> engine": reconstruct each frame from game state + ripped ROM assets, with the SH4 OFF. It is NOT
> shipping pixels (TA mirror) and NOT the hand-rolled atlas guesswork that garbled.
>
> **Sourcing.** Every routine cited is a `loc_8cXXXXXX:` label whose name **is** the SH4 PC, in the
> local `marvelous2/build/bankNN.asm` clone (gitignored). Struct offsets are `marvelous2/memory/pl_mem.asm`;
> globals `marvelous2/memory/work.asm`; data semantics cross-checked against anotak
> (`docs/MVC2-FRAMEDATA-FIELDS.md`). Where the disasm does not settle a point I write **UNKNOWN**.
> CONFIRMED = read directly off the instruction in this pass; INFERRED = derived from layout + a
> companion doc, not pinned to an instruction here.
>
> Companion docs: `docs/MARVELOUS2-GFX-NOTES.md` (part/EXTRAS decode), `docs/MVC2-FRAMEDATA-FIELDS.md`
> (cell/attack fields), `docs/MARVELOUS2-RE-HANDOFF.md` (codec + pool). Wire: `core/network/maplecast_gamestate.h`.

---

## 0. The frame in one diagram

```
vblank (after SH4 has computed state):
  loc_8c030858  "Render Characters?"  (bank03)  ── top-level frame builder
    ├─ loc_8c0308c2  Render_sprites    walks the SLOT TABLE (16 layers) ─ STAGE+CHARS+EFFECTS+HUD objects
    │     per node, gate on category byte node+0x03:
    │       != 0 → loc_8c03093c  Render Main Sprite   (transform → screen_x/y)
    │       == 0 → loc_8c030af8  (alt/effect composer, same transform)
    │     both feed → loc_8c033e90  QUAD EMITTER   (EXTRAS → 16-byte quads)
    ├─ loc_8c030cc0 / d12 / d24 / d36 / dcc  ── sub-passes (lifebars/overlays, see §5)
  Stage geometry: loc_8c027ba6 state machine → loc_8c027e32 stage draw (0x0CEA0000 Stage Poly)
  HUD objects: pooled draw-objects, per-object handler loc_8C0F0160 (bank0f), vtable bank15
```

The **slot table is the single draw list** for every per-object visual (stage objects, the 6 bodies,
capes, projectiles, super overlays, HUD elements that are pooled). Stage *background* polygons are a
separate Ninja/NaomiLib pass (loc_8c027e32). Both run each vblank; z-order = (layer index, then a
depth byte). This matches `reference_mvc2_slot_table_drawlist` and is already implemented as
`readAllDrawn()` in `core/network/maplecast_gamestate.cpp`.

---

## Stage 1 — Top-level render dispatch (the draw-list walker)

### Algorithm (CONFIRMED)

**Entry: `loc_8c030858` "Render Characters?" (bank03:1131).** Reads a mode selector
(`loc_8c0310f2`) and in the normal in-match path calls, in order:
1. `loc_8c0308c2` **Render_sprites** — the slot-table walk (the bulk of the frame).
2. `loc_8c030cc0`, `loc_8c030d12`, `loc_8c030d24`, `loc_8c030d36`, `loc_8c030dcc` — sub-passes
   (overlays / lifebar / post effects; the in-match branch gates one block on `GameGlobalPointer+0x98`).
3. Tail: if `GameGlobalPointer+0x2E == 1`, jumps to `loc_8c031470`.

**`loc_8c0308c2` Render_sprites (bank03:1200) — the walker.** CONFIRMED structure:
```
SLOT_COUNT_BASE = 0x8C2895E0        ; u8[layer]   = node count for that layer
SLOT_PTR_BASE   = 0x8C287DE0        ; u32 node-ptr array, row stride 0x180
NLAYERS = 16    (outer ends when ptr >= 0x8C2895E0 + 0x10)
for layer in 0..15:
    count = u8 @ (0x8C2895E0 + layer)          ; mov.b @r13 ; cmp/ge
    row   = 0x8C287DE0 + layer*0x180           ; r12 += 0x180 per layer
    for i in 0..count-1:
        node = u32 @ (row + i*4)               ; shll2 ; mov.l @(r0,r10)
        cat  = u8 @ (node + 0x03)              ; mov.b @(0x3,r4)
        if cat != 0:  loc_8c03093c(node)       ; Render Main Sprite
        else:         loc_8c030af8(node)       ; alt/effect composer
```
- The **+0x03 category byte gates which composer** runs (CONFIRMED: `mov.b @(0x3,r4); tst r0,r0`).
- Order is **layer-major** (outer loop = z-bands), node order within a layer second. So **z-order =
  (layer, then in-layer index)**; the depth byte `node+0x31` noted in the memory file is a
  finer in-object sort, not exercised in this loop (INFERRED).

### State fields it reads
- `0x8C2895E0[16]` per-layer counts (u8). — *not on the wire; the slot walk is server-side.*
- `0x8C287DE0` node-pointer array (16×0x180). — *server-side enumeration.*
- per node: `+0x03` category, then everything Stage 2/3 reads.

### Asset inputs
None at this stage — it only routes. (Geometry/texture come from the composer + emitter.)

### Cite
`bank03.asm:1131` loc_8c030858; `bank03.asm:1200` loc_8c0308c2; literals `loc_8c030934=0x8C287DE0`,
`loc_8c030938=0x8C2895E0`, `loc_8c030924=0x0180` stride, `loc_8c030922=0x98` (the sub-pass gate).

---

## Stage 2 — Character (and per-object) assembly → quads

### Algorithm (CONFIRMED)

**`loc_8c03093c` Render Main Sprite (bank03:1281) — per-object transform.** For a node it:
1. Early-out if `node+0x12c != 0`-gate (`unk_012c`, the visibility/draw gate; `tst` then branch).
   (NOTE: code branches *to the draw* when the gate passes; `unk_012c` is documented "≈1 always".)
2. Runs the world→screen transform: reads world pos `+0x34/+0x38/+0x3c` (x,y,z f32), calls the
   transform (`bank12` matrix path), writes **screen_x `+0xe0`, screen_y `+0xe4`** (f32). CONFIRMED:
   `fmov @(0x34..0x3c,r14)` in, `fmov fr3,@(0xe0/0xe4,r14)` out.
3. Applies sprite scale: multiplies stored geometry by `x_sprite_scale +0x50`, `y_sprite_scale +0x54`
   (CONFIRMED `fmul` against `@(0x50/0x54,r14)`), and a per-character correction read at `+0x24`×scale.
4. Copies `+0x48`→`+0x104`; grabs **`xflip_copy +0x130`** and writes the xflip-copy-2 at `+0x134`
   (CONFIRMED: `add 0x2C,r0 ;0x104+0x2c=0x130 ; grab xflip_copy ... write xflip_copy2`).
5. Reaches the quad emitter (falls into / branches to `loc_8c033e90`).

**`loc_8c030af8` (bank03:1526) — the alternate composer** for nodes with category `+0x03 == 0`
(it also requires `0 < node+0x03 < 5` for its own sub-objects). It performs the **same** world→screen
transform (`+0x34/0x38/0x3c` → `+0xe0/0xe4`, scale `+0x50/0x54`, `+0x24`) — i.e. effects/secondary
objects are placed identically to bodies. CONFIRMED.

**`loc_8c033e90` QUAD EMITTER "reading sprite data" (bank03:9258) — the geometry.** This is the one
routine the replay engine must reimplement. CONFIRMED record-walk:
```
gfx_base = u32 @ (node + 0x15c)                  ; Dat_GFX1   (loc_8c033f72 = 0x015c)
; per EXTRAS record (r13 walks the assembly list, stride 0x08):
for each record:
    part_idx = u16 @ (rec + 0x06)                 ; mov.w @(0x6,r13)
    off      = u32 @ (gfx_base + part_idx*4)       ; offset-table lookup
    blob     = gfx_base + off
    blob    += 0x02                                ; skip 2-byte piece header
    w_dim    = u8 @ blob++ ;  quad_w = w_dim << 3   ; (×8)  -> quad[+0x0] u16
    h_dim    = u8 @ blob++ ;  quad_h = h_dim << 3   ; (×8)  -> quad[+0x2] u16
    quad[+0x4] = attr (u32)        ; attr literal 0x0502 (loc_8c033f74) merged w/ palette-select
    quad[+0x8] = texptr  = running r8 (advances by w*h>>1 each part = 4bpp byte size)
    quad[+0xC] = palptr  = r12 (the palette base for this object, see Stage 3)
    emit 16-byte quad; r14 += 0x10 ; r13 += 0x08
; terminate with a 0x00FF/0x00FF sentinel quad (loc_8c033f14 path)
```
- The EXTRAS record is **8 bytes** `[dx:s16][dy:s16][part_idx:u16][attr:u16]`, `attr=0x00FF` =
  frame terminator (`docs/MARVELOUS2-GFX-NOTES.md §3`; r13 += 0x08 here confirms the stride).
- **`part_idx` indexes the GFX1 offset table** (`@(gfx_base + part_idx*4)`), exactly the
  offline-decoded part atlas — CONFIRMED no hidden per-part_idx indirection at blit time.
- The running `texptr` (r8) and **the palette select** are woven in at `loc_8c033e3e`/`loc_8c033e76`,
  which read **`node+0x12d` (loc_8c033f6c=0x012d)** and **`node+0x12e` (loc_8c033f70=0x012e)** and the
  EXTRAS `rec+0x04` low byte to compute the per-part palette index. So **the per-part palette
  row = f(rec+0x04 palette-row byte, node+0x12d, node+0x12e)**. CONFIRMED these two char bytes feed
  the palette pointer; their exact arithmetic is the masked combine at lines 9232-9256
  (`& 0x03ff`, `shad` by -4).

**Resolved blocker (carry-over):** the part→pixels map you build offline (PLDAT GFX offset-table →
part rect, then the LZSS decode of each blob) **is** the oracle. The SH4 caches a resolved cell
pointer and pulls per-part size/UV from atlas tables (`bank10.loc_8C106FB2`,
`bank13.loc_8c13DE20` UV grid) — see `docs/MARVELOUS2-GFX-NOTES.md §2`. For a from-`sprite_id`
client you skip the anim/cell walk entirely: `sprite_id (+0x144)` already names the cell, so
`sprite_id → EXTRAS assembly → [{dx,dy,part_idx,flip}]` offline, then this emitter loop.

**dx/dy/flip (CONFIRMED in GFX-NOTES §6):** part screen pos = `screen_x/y (+0xe0/+0xe4) + (dx,dy)`,
scaled by `+0x50/+0x54`. Flip = character facing XOR record bit 0x80; when flipped, reflect dx across
the pivot (`x = char_x + (flip ? -dx - part.w : dx)`) and mirror U. The composer's mirror flag is set
from `xflip_copy +0x130`.

### State fields it reads (per object)
| field | offset | width | role | on wire? |
|---|---|---|---|---|
| world x / y / z | +0x34/+0x38/+0x3c | f32 | transform input | pos_x/pos_y yes; **z NO** |
| screen x / y | +0xe0 / +0xe4 | f32 | output (server reads these directly) | YES |
| x/y/z sprite scale | +0x50/+0x54/+0x58 | f32 | per-object scale | **NO** (client uses const 1.6667/2.1428) |
| `+0x24` | +0x24 | u8 | scale-correction index | **NO** |
| sprite_id | +0x144 | u16 | names the EXTRAS assembly | YES |
| visibility gate | +0x12c | u8 | draw / skip | partly (`active`) |
| xflip_copy | +0x130 | u8 | mirror | YES (facing_right) — but disasm-authoritative is +0x1d2 |
| palette-select lo/hi | +0x12d / +0x12e | u8 | per-part palette row select | **NO — GAP** (see Stage 3) |
| EXTRAS rec | (asset) | 8B×N | dx,dy,part_idx,attr | derived from sprite_id offline |

### Asset inputs
- **GFX1/GFX2 part pixels** (`Dat_GFX1 +0x15c`): the LZSS-compressed 4bpp part blobs, offset-table
  indexed by `part_idx`. Decoder `loc_8c03552a` (bank03:12740) — flag-bit LZSS, CONFIRMED cracked
  (`docs/MARVELOUS2-RE-HANDOFF.md §2`). Offline → `atlas/chars/PLxx.png` keyed by sprite_id.
- **EXTRAS assembly** (`Sprite_Extras +0x178`): per-cell 8-byte part list. Offline → `PLxx.json`.

### Cite
`bank03.asm:1281` loc_8c03093c; `bank03.asm:1526` loc_8c030af8; `bank03.asm:9258` loc_8c033e90
(literals 0x015c texptr, 0x0502 attr, 0x012d/0x012e palette bytes, 0x00ff sentinel). Part/UV detail:
`bank10.asm:loc_8C106FB2`, `bank13.asm:loc_8c13DE20`. Codec: `bank03.asm:12740` loc_8c03552a.

---

## Stage 3 — Palette application (bank, hit-flash, super/aura override)

### Algorithm (CONFIRMED)

**Base palette pointer: `loc_8c035000` (bank03:11891).** Computes the object's palette base:
```
pal_base  = u32 @ (char + 0x164)          ; Dat_Pal     (loc_8c035074 = 0x0164)
pal_id    = u8  @ (char + 0x25)            ; pl_palid_match (the chosen color variant)
sel       = pal_id << 7                    ; ×0x80  (shll2 x2, shll x2 ... = <<7)
palptr    = pal_base + sel
PAL_OFFSET (hurt/overlay region) = 0x0300  ; (loc_8c035076)
; per-character special cases, branch on char+0x01 (character_id):
;   0x01 Zangief : if Buff_HyperArmor(+0x202)!=0 use bank base +0x08 rows  (loc_8c035034)
;   0x22 Sakura  : (loc_8c035080) +0x14 rows
;   0x37 Jin     : (loc_8c0350b0) reads +0x08 of the cell ptr, special bank
;   0x14 SonSon  : (loc_8c0350e6)
;   else         : (loc_8c035114) plain base
```
So **which palette = f(Dat_Pal +0x164 base, pl_palid_match +0x25, character_id +0x01)**, with a hurt
region at `Dat_Pal + 0x300`. CONFIRMED. (This matches CLAUDE.md PVR bank formula
`16×(char_pair+1)+8×side` at the VRAM-bank granularity; here it's the source-pointer granularity.)

**Per-frame hit-flash / super / aura overlay: `loc_8c035162` (bank03:12114).** This is a
**draw-object handler** (installed as a `#data loc_8c035162` vtable entry from bank04 — 9 sites) that
builds a 16-color palette-extras override each frame:
```
target_field = char + 0x12e                 ; (loc_8c035250 = 0x012e) the live palette-effect word
HURT_OFFSET  = 0x0300                        ; (loc_8c035252 "Hurt Effect offset")
selector     = r5 (caller arg, 0x00..0x0A)  ; the RenderExtra/effect class
; dispatch on selector:
;   0x00 → read RenderExtra index char+0x1a4 (loc_8c035254=0x01a4), map via table
;          bank13.loc_8c1355d4, then loc_8c035000, copy 0x10 words (32B = 16-color pal)
;          into the palette-extras buffer; advance per character part (stride 0x30)
;   0x01..0x0A → variant overlays (white-flash 0x0000ff00, additive aura, etc.)
; writes into the palette-extras buffer at 0x8C26AA34 region (loc_8c033f8c)
```
- **The live palette-effect target is `char+0x12e`** (CONFIRMED by both the quad emitter reading it
  and this handler writing it). This corroborates `reference_mvc2_wire_gaps` (real hit-flash field =
  +0x12e, not +0x40). `char+0x40` (`char_pal_effect`) is a *separate, higher-level* flag the project
  already ships as `PALF`; it gates whether the hurt bank is used, but the renderer reads +0x12d/+0x12e.
- **The RenderExtra/overlay selector source is `char+0x1a4`** in this handler (CONFIRMED literal
  0x01a4) — NOT `char+0x151` as earlier docs assumed. `char+0x151` is the per-cell RenderExtra byte
  copied from the keyframe (`docs/MVC2-FRAMEDATA-FIELDS.md §3`); +0x1a4 is the runtime expansion the
  handler dispatches on. **Both are GAPS on the wire (see §6).**

### State fields it reads
| field | offset | role | on wire? |
|---|---|---|---|
| Dat_Pal | +0x164 | palette base pointer | NO (asset-resolved client-side from sprite pack) |
| pl_palid_match | +0x25 | chosen color variant | NO — **wire has palette_id from +0x52d, a COPY** |
| character_id | +0x01 | per-char special bank | YES |
| Buff_HyperArmor | +0x202 | Zangief/SonSon special bank | NO |
| palette-effect lo/hi | +0x12d / +0x12e | per-part palette row + hit-flash | **NO — GAP** |
| RenderExtra (cell) | +0x151 | overlay class (per-cell) | **NO — GAP** |
| RenderExtra (runtime) | +0x1a4 | overlay class (handler dispatch) | **NO — GAP** |
| char_pal_effect | +0x40 | hurt-bank-active flag | YES (PALF packet) |

### Asset inputs
- **PALETTE_DATA** (`Dat_Pal +0x164`): 16-color ARGB4444-LE palettes, idx 0 transparent, 32B each;
  hurt/overlay variants at base+0x300. From PLDAT (`reference_pldat_sprite_format`, 57 palettes).
- The skin system already overrides these via PVR banks (CLAUDE.md). Client preloads per
  `(char_id, pal_id)`.

### Cite
`bank03.asm:11891` loc_8c035000 (literals 0x0164 Dat_Pal, 0x25 pal_id, 0x300 offset, char_id branches);
`bank03.asm:12114` loc_8c035162 (literals 0x012e target, 0x0300 hurt, 0x01a4 RenderExtra-runtime,
0x0164, bank13.loc_8c1355d4 map table); vtable installs `bank04.asm:9959,16515,18662,21425,...`.

---

## Stage 4 — Effects (hitsparks / supers / auras)

### Algorithm (CONFIRMED + carry-over)

Effects are **pool objects on the same slot table**, drawn by the same `loc_8c030af8`/`loc_8c033e90`
path. They are **not a separate sprite_id namespace** — texture resolves from the object's own GFX
base pointer `node+0x15c` (Dat_GFX1), indexed by `part_idx`. So shared-effect sprite_ids COLLIDE with
per-character part indices; **you must route by the GFX base pointer, not by sprite_id**
(`reference_mvc2_effects_bank`, CONFIRMED).

- **Shared effect graphics live in `0x0CED0000` "Effect Poly"** (`work.asm:39`). Loaded per-stage by
  the category-indexed loader **`loc_8c032be0` (bank03:6485)**: reads `STG_ID 0x8C26A95C`
  (loc_8c032c38), dispatches by a count to load into `0x0CED0000` (Effect Poly), `0x0CE80000`
  (DM00 Poly), via decompress staging `0x0CC00000` / `0x0CE60000`. Load-time, not per-frame. CONFIRMED.
- **Effect spawn** copies the shared GFX/Pal/anim ptr into a fresh pool node. Trigger on hit:
  `loc_8c035000` region + the cell EffectTrigger at `char+0x14c` (`bank04.loc_8c042014`); attack
  Hitspark type at `record+0x0b` selects the effect class (`bank05.loc_8c0578c0` →
  `bank0f` spawn) — `docs/MVC2-FRAMEDATA-FIELDS.md §2,§4`.
- **Effect type byte** is at the pool node `+0x03` category / a type byte (the OBJS `type` field the
  project already ships). Size/scale tables for effect types live in **bank16**
  (`loc_8c164c18` size, `loc_8c164c30` scale — per `reference_mvc2_effects_bank`).

### State fields it reads (per effect node)
| field | offset | role | on wire? |
|---|---|---|---|
| GFX base | +0x15c | which texture bank (→ Effect Poly vs PLxx) | NO — server must TAG (effects-bank check) |
| screen x/y | +0xc8/+0xcc (pool) or +0xe0/+0xe4 | placement | YES (OBJS) |
| owner ptr | +0x80 | which char → palette bank | YES (owner_cid) |
| sprite_id / type | +0x12c / +0x03 | effect variant | YES (OBJS sprite_id + type) |
| xflip | +0x130 | mirror | YES (OBJS) |

### Asset inputs
- **Effect Poly bank `0x0CED0000`** — shared hitspark/super/aura textures + their palettes (loaded by
  `loc_8c032be0` per stage). Rip → `effects.json`/`.png` keyed by **effect_type** (~21 types), via
  runtime capture of 0x0CED0000 (same as the 0x0CE80000 part capture) or static LZSS decode.
- bank16 size/scale tables (`loc_8c164c18/c30`) for per-type quad dimensions.

### Cite
`bank03.asm:6485` loc_8c032be0 (literals 0x0ced0000, 0x0ce80000, 0x0cc00000, STG_ID 0x8c26a95c);
effect routing `reference_mvc2_effects_bank` (bank0f spawn, bank16 tables); on-hit
`bank05.asm:loc_8c0578c0`, `bank04.asm:loc_8c042014`.

> **Honest note:** the *dynamic per-frame geometry* of complex supers/auras is input/RNG-driven
> multi-strip additive geometry that cannot be rebuilt from a few state bytes. The project decision
> (`project_render_pipeline_state`, agent doc "bandwidth ladder") is to **STREAM** these as real
> quads via `PVR2Renderer`, not reconstruct them. The reconstruction spec covers bodies + simple
> typed effects; complex supers stay streamed. This is a deliberate boundary, not a gap to close.

---

## Stage 5 — Stage background + HUD

### Stage background (CONFIRMED structure, format port pending)

**Dispatcher `loc_8c027ba6` (bank02) → state `loc_8c027e32` (bank02:19124) stage draw.**
```
state = u5 of a state byte (and 0x1F)
jt    = bank14.loc_8c14cfdc          ; per-state jump table  (loc_8c027f18)
poly_base = 0x0CEA0000               ; "Stage Poly"          (loc_8c027f14)
; the draw states walk a per-stage NLOBJPUT object list:
;   obj-table = bank14.loc_8c1491f4  (loc_8c027f24), indexed by obj+0x55c / obj+0x52c
;   per object: poly ptr (r6) + texture ptr (r5) → loc_8c0279e4 (Ninja/NaomiLib draw)
; reads STG_ID 0x8c26a95c, stage state byte 0x8c1f978c/8d, camera 0x8c1f9cd8/cdc
```
- Stage = **NaomiLib/Ninja "NLOBJPUT" objects** drawn from `0x0CEA0000`, selected by `stage_id`, an
  **animation/phase state byte** at `0x8C1F978C/8D`, and the **camera at `0x8C1F9CD8/CDC`**. CONFIRMED
  addresses; the polygon/display-list decode itself is the one from-scratch port (ModNao NaomiLib
  parser — see agent doc / `docs/RENDER-MASTER-PLAN-V2.md`).
- The project already ships `stage_anim_timer` (`0x8C1F9D80`) + `camera_x/y` (`0x8C1F9CD8/CDC`) on the
  wire; `stage_id` is on the wire. So the **state inputs for the stage are already carried**; the
  missing piece is the asset (STGTEX/STGPOL rip + NaomiLib renderer), not the state.

**State inputs:** `stage_id` (YES), `stage_anim_timer 0x8C1F9D80` (YES), `camera_x/y 0x8C1F9CD8/CDC`
(YES). **Asset inputs:** STGTEX/STGPOL per-stage NaomiLib model+texture (rip from disc; 17 stages).

**Cite:** `bank02.asm:19124` loc_8c027e32 (literals 0x0cea0000, STG_ID 0x8c26a95c, 0x8c1f978c,
jt bank14.loc_8c14cfdc, obj-table bank14.loc_8c1491f4); dispatcher `bank02.asm:18700` loc_8c027ba6.

### HUD (CONFIRMED structure)

**HUD is a pool of draw-objects; per-object handler `loc_8C0F0160` (bank0f:175).**
```
loc_8C0F0160(obj):
    state = u8 @ (obj + 0x04)
    vtable = bank15.loc_8c15FF78       ; (loc_8C0F0194)
    jmp vtable[state]                  ; per-state handler (init/animate/idle/insert)
; handlers read: health 0x0420 (loc_8c0f0174), 0x0411, battle-state 0x8C2895F0,
;   0x8C2896B0, 0x8C26A518; insert into draw list via bank04.loc_8c0450c0
```
- Each HUD element (lifebar, super meter, timer digits, hit counter, portraits) is a pooled object
  with a state-machine handler; it reads the gameplay globals and inserts a textured quad into the
  shared display list (`bank04.loc_8c0450c0`). CONFIRMED dispatcher; per-element draw rects/fill math
  are in the individual state handlers (vtable `bank15.loc_8c15FF78`) — **per-element fill geometry
  not yet traced (partial).**

**State inputs:** `health +0x420` / `red_health +0x424` (YES), `p1/p2 meter_fill` `0x8C289646/...`
(YES), `meter_level` (YES), `game_timer 0x8C289630` (YES), `p1/p2 combo 0x8C289670/...` (YES),
battle-state `0x8C2895F0` (NO, but derivable from in_match), round_counter `0x8C28962B` (NO).
**Asset inputs:** HUD texture atlas (lifebar WHITE texture, meter, digit font, portraits) — fixed disc
assets; the bar is the white texture drawn at `width = health/max`, MODULATED by green/yellow vertex
color (agent doc).

**Cite:** `bank0f.asm:175` loc_8C0F0160 (literals 0x0420 health, 0x8C2895F0 battle-state,
bank15.loc_8c15FF78 vtable, bank04.loc_8c0450c0 list-insert).

---

## 6. Complete state-input set vs the 262-byte GSTA wire

The wire is `core/network/maplecast_gamestate.h` (`WIRE_SIZE = 262`): per-char (×6) `active,
character_id, facing_right, health, red_health, special_move_id, assist_type, palette_id, pos_x,
pos_y, screen_x, screen_y, vel_x, vel_y, sprite_id, animation_state, anim_timer, anim_pointer`; plus
global `in_match, game_timer, stage_id, p1/p2 meter_level, p1/p2 combo, p1/p2 meter_fill, camera_x,
camera_y, frame_counter, p1/p2 buttons+triggers, stage_anim_timer`; plus side channels `OBJS`/`OBJF`
(per-object owner_cid, sprite_id, screen_x/y, type, category, xflip, owner_slot) and `PALF`
(per-slot char+0x40 hit-flash flag).

### What the pipeline needs and where it stands

| input | offset / addr | stage | on wire today | gap? |
|---|---|---|---|---|
| sprite_id | char+0x144 | 2 | YES | — |
| screen_x / screen_y | char+0xe0/0xe4 | 2 | YES | — |
| world pos x/y | char+0x34/0x38 | 2 | YES (pos_x/y) | — (transform done server-side; screen_x/y shipped) |
| **world z (+0x3c)** | char+0x3c | 2 | NO | minor — only matters if client re-does transform; it doesn't |
| **sprite scale x/y/z** | char+0x50/0x54/0x58 | 2 | NO | **GAP** — client uses const CpsX/CpsY; per-char zoom (e.g. Sentinel, Abyss, juggernaut grow) WRONG |
| facing / xflip | char+0x1d2 (auth) / 0x130 | 2 | YES (facing_right, from 0x110 copy) | minor — read 0x1d2 not 0x110 (RE-HANDOFF item #1) |
| character_id | char+0x01 | 2,3 | YES | — |
| **palette select pl_palid_match** | char+0x25 | 3 | wire uses 0x52d (a COPY) | low — usually equal; confirm 0x25==0x52d holds |
| **per-part palette / hit-flash** | char+0x12d / +0x12e | 2,3 | NO (PALF ships +0x40 flag only) | **GAP** — hit-flash row + per-part palette select |
| **RenderExtra (cell / runtime)** | char+0x151 / +0x1a4 | 3 | NO | **GAP** — super/aura overlay class (which overlay palette) |
| Buff_HyperArmor | char+0x202 | 3 | NO | low — only Zangief/SonSon special bank |
| EXTRAS assembly | asset (from sprite_id) | 2 | derived offline | — |
| GFX part pixels | asset (Dat_GFX1) | 2 | ripped → atlas | — |
| palette data | asset (Dat_Pal) | 3 | ripped → atlas | — |
| effect GFX base (route flag) | node+0x15c | 4 | NO (server must tag) | **GAP** — tag OBJS as Effect-Poly vs PLxx by 0x15c range |
| effect type | node+0x03 / type | 4 | YES (OBJS type) | — |
| Effect Poly textures | asset (0x0CED0000) | 4 | not ripped yet | asset TODO (effects.json) |
| stage_id | 0x8C26A95C / 0x8C289638 | 5 | YES | — |
| stage anim/phase | 0x8C1F9D80 (+ 0x8C1F978C state) | 5 | YES (stage_anim_timer) | minor — confirm 0x8C1F978C phase covered |
| camera x/y | 0x8C1F9CD8/CDC | 5 | YES | — |
| stage assets (NaomiLib) | disc STGTEX/STGPOL | 5 | not ripped yet | asset TODO (ModNao port) |
| health / red_health | char+0x420/0x424 | 5 | YES | — |
| meter fill / level | 0x8C289646.. | 5 | YES | — |
| game timer | 0x8C289630 | 5 | YES | — |
| combo | 0x8C289670.. | 5 | YES | — |
| battle-state / round | 0x8C2895F0 / 0x8C28962B | 5 | NO | low — derivable / cosmetic |
| HUD assets | disc HUD atlas | 5 | not ripped yet | asset TODO |

### The real GSTA gaps (state, not assets), in priority order
1. **Per-char sprite scale `+0x50/+0x54` (and which chars use it).** Without it, dynamic-zoom
   characters and grow/shrink supers are mis-sized. The const CpsX/CpsY is right for the *global*
   aspect, not per-char zoom. **Highest-impact state gap.**
2. **Palette/hit-flash bytes `+0x12d/+0x12e`** (per-part palette row + live hit-flash). PALF ships
   the coarse +0x40 flag; the renderer actually keys off +0x12d/+0x12e.
3. **RenderExtra overlay class `+0x151` (and runtime `+0x1a4`)** — selects which super/aura palette
   overlay; needed for correct super tinting without streaming the overlay quads.
4. **Effect routing flag** — server must tag each OBJS node by whether `node+0x15c` points into the
   Effect Poly bank (0x0CED0000), so the client routes to the effects atlas vs the PLxx atlas.
5. Minor: read facing from `+0x1d2` not the `+0x110` copy; confirm `pal_id +0x25` vs the shipped
   `+0x52d`.

(Items 1-3 are the "GSTA enrich" wire bump already scoped in `docs/MARVELOUS2-RE-HANDOFF.md §5 #4`;
they touch all four parsers per CLAUDE.md.)

---

## 7. CONFIRMED vs UNKNOWN — honest assessment

### CONFIRMED in the disassembly (read off instructions this pass)
- Top-level dispatch and the **slot-table walk** (16 layers, count@0x8C2895E0, ptr@0x8C287DE0
  stride 0x180, category gate @node+0x03) — `loc_8c0308c2`.
- The world→screen **transform** and where screen_x/y / scale / xflip_copy are read/written —
  `loc_8c03093c`, `loc_8c030af8`.
- The **quad emitter**: EXTRAS 8-byte walk, `part_idx → GFX1 offset table → blob (+2 hdr) → w/h×8`,
  16-byte quad layout, 0x00FF sentinel, palette bytes +0x12d/+0x12e woven into palptr — `loc_8c033e90`.
- The **palette pointer** computation (Dat_Pal +0x164, pal_id +0x25 ×0x80, char_id special banks,
  hurt offset +0x300) — `loc_8c035000`; and the **overlay handler** target +0x12e / selector +0x1a4
  — `loc_8c035162`.
- The **effect loader** (Effect Poly 0x0CED0000, by STG_ID) — `loc_8c032be0`; effects share the
  emitter and route by GFX base +0x15c (cross-doc CONFIRMED).
- The **stage dispatcher** (state machine, Stage Poly 0x0CEA0000, by stage_id + phase byte
  0x8C1F978C + camera 0x8C1F9CD8) — `loc_8c027e32`.
- The **HUD handler dispatcher** (per-object state vtable, reads health/meter globals, inserts to
  the shared list) — `loc_8C0F0160`.

### Partial / INFERRED (layout-derived, not pinned to an instruction here)
- The exact **palette-index arithmetic** combining rec+0x04 with +0x12d/+0x12e (masks `&0x03ff`,
  `shad -4` seen, full formula not unit-verified).
- The **depth/in-layer sort byte** `node+0x31` (memory note; the walk loop is layer-major only).
- **Per-HUD-element fill geometry** (lifebar width = health/max, digit placement) — the dispatcher is
  confirmed; the individual state handlers (vtable bank15) are not traced field-by-field.
- **RenderExtra +0x151 vs +0x1a4** relationship — both feed overlay selection; the per-cell→runtime
  expansion table (`bank13.loc_8c1355d4`) is named but its contents not dumped.

### UNKNOWN / risk (needs a live capture or a from-scratch port)
- **NaomiLib stage polygon/display-list decode** — the addresses are confirmed; the binary format
  itself is the one genuine port (mitigated: ModNao already parses MVC2 NaomiLib + PVR; port not RE).
- **Complex super/aura per-frame geometry** — explicitly NOT reconstructable from a few state bytes;
  the chosen architecture STREAMS these (not a gap to close, a boundary).
- **Per-character scale usage** — which of the 56 chars actually drive `+0x50/+0x54` dynamically (vs
  constant) needs an empirical sweep; the field is confirmed, the population is not characterized.
- **Cape / secondary-object completeness** — confirmed capes are sibling pool objects (GFX-NOTES §7);
  the FAC file (`+0x184`) may hold a per-char part remap — flagged "verify in-game," not confirmed.

---

## 8. Bottom line for the replay engine

The five stages decompose cleanly into: **(walk slot table) → (per node: transform → emit quads from
EXTRAS+GFX) → (resolve palette) → (route effects by GFX bank) → (stage + HUD as their own object
passes)**. Four of the five are fully traced to instructions; the renderer reimplements exactly one
routine — the `loc_8c033e90` quad emitter — over offline-decoded part atlases. The state the pipeline
consumes is mostly already on the 262-byte wire; the **actionable state gaps are per-char scale
(+0x50/54), the palette/hit-flash bytes (+0x12d/12e), the RenderExtra overlay class (+0x151/1a4), and
an effect-routing tag** — all small, all in the already-scoped GSTA-enrich wire bump. The remaining
risk is asset-side (NaomiLib stage decode, Effect Poly rip) and the deliberate decision to keep
streaming complex supers, not reconstruct them.
