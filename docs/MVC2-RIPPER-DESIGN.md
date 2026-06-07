# MVC2 Asset Ripper Design — `tools/rip_mvc2.py`

**Goal.** A *pure-offline* ripper that turns the local extracted disc (`MVC2 Dev Files/`)
into an organized, cross-referenced asset DB keyed by **player + type**, with no running
emulator. Output:

```
atlas/chars/PLxx.{png,json}        # 59 character sprite atlases (native res)
atlas/effects/effects.{png,json}   # the SHARED hitspark/effect atlas, keyed by effect type
catalog.json                       # player+type index → sprite_id/effect/attack → anotak + marvelous2 offsets
```

Everything here is grounded in the actual local bytes (hexdumps of `MVC2 Dev Files/*`),
the working decoder (`dasm_PLDAT/dasm_PLDAT_v005a.py`), and `marvelous2/` bank:line
citations. **CONFIRMED** = read from the bytes / disassembly in this pass; **INFERRED**
= derived from layout + dictionary, not yet pinned to an instruction.

> ROM-derived rule: every PNG/raw this tool emits is gitignored (ROM-derived). Source files
> in `MVC2 Dev Files/` are copyrighted disc data — never commit them or their pixel rips.

---

## 0. TL;DR findings (the loadbearing ones)

1. **`PLxxPAK.BIN` is NOT the file the PLDAT decoder wants.** The on-disc per-character
   container is `PLxxPAK.BIN` (59 of them). The game's file table
   (`marvelous2/build/data/filepnt.asm`) only knows **`PLxx_DAT.BIN`** (ids `0x0d1` PL00 …
   `0x0e8` PL17 …) — there is **no `PAK` entry in `filedic.asm` at all**. `PLxxPAK.BIN` is the
   *packed* on-disc form; `PLxx_DAT.BIN` (what lives in `dasm_PLDAT/PLDATs/`, and what the
   decoder ingests) is the *unpacked* form. Sizes prove it: `PL00PAK.BIN` = 518 976 B,
   `dasm_PLDAT/PLDATs/PL00_DAT.BIN` = 557 408 B (DAT larger ⇒ decompressed). The headers
   differ too: PAK's pointer block begins at file +0x20, DAT's at +0x40, and DAT has the
   AI-script pointers (+0x14…+0x30) populated while PAK does not.
   **→ The ripper must feed `PLxx_DAT.BIN` to the decoder. We already have all 59 unpacked
   under `dasm_PLDAT/PLDATs/` (the PAK→DAT unpack is a solved upstream step).**

2. **The shared effects GFX lives in `EFKYTEX.BIN` (texels) + `EFKYPOL.BIN` (poly/part
   descriptors).** Traced end-to-end: the effect loader `bank0e:loc_8C0EEFCC` is a 21-entry
   jump table (`add 0xFF / cmp/hs 0x15` bounds = effect types `0x00..0x14`). Its case for
   the generic hitspark loads texture IDs **`0x026e`/`0x026f`**, which the file table
   `filepnt.loc_8c148688` resolves to **`EFKYPOL.BIN` (0x26e)** and **`EFKYTEX.BIN`
   (0x26f)** (`filepnt.asm:625-626`). Other effect-type cases load `0x0292/0x0293`
   (`DM00POL/DM00TEX`), `0x029e/0x029f` (`DM07POL/DM07TEX`), `0x02a0..0x02a3`
   (`DM08/DM09 POL/TEX`). So: **most shared hitsparks decode from `EFKYTEX.BIN`; a handful
   of effect types pull from specific `DMxxTEX.BIN`.** This is the missing piece.

3. **The numbered `HIT_xx`/`HIT_FMxx`/`HIT_DTxx` are per-character data, NOT effect GFX.**
   Byte-for-byte: `HIT_00.BIN` ≡ decoded `PL00_DAT_HITBOX_PATTERN_TABLE.BIN` (identical),
   `HIT_FM00.BIN` ≡ decoded `PL00_DAT_HITBOX_DATA.BIN` (identical), `HIT_DT00.BIN` ≡ decoded
   `PL00_DAT_ATTACK_DATA.BIN` (the 0x1C-stride attack records). `ATCK00.BIN` is the attack
   *index/dispatch* table (3 pointers + a per-move ramp). None are loaded by name through
   `filepnt` — they are the **source tables the build merges into `PLxx_DAT.BIN`**. The
   PLDAT decoder already re-extracts them, so we have two equivalent sources.

4. **Effect size/scale tables are constants in `bank16`.** `loc_8c164c18` = per-type size
   (u16 pairs), `loc_8c164c30`/`loc_8c164c78` = per-type float32 scale. Indexed by the
   effect object's type byte (`obj+0x20`). This gives `effect-type → {w,h,scale}`.

---

## 1. Character sprites (by player) — CONFIRMED decodable offline

### 1.1 The container format (PLDAT)

`PLxx_DAT.BIN` is a 16-entry u32-LE pointer header followed by concatenated data blocks.
The decoder (`dasm_PLDAT_v005a.py`, lines 341-444) reads the header and slices the file at
each consecutive non-zero pointer. Header layout (file offsets, all u32 LE):

| hdr off | pointer | block emitted | player-struct slot |
|--------:|---------|---------------|--------------------|
| 0x00 | gfx_ptr1 | `GFX_DATA_00` (part-cell pool 1) | +0x15c `Dat_GFX1` |
| 0x04 | gfx_ptr2 | `GFX_DATA_01` (part-cell pool 2) | +0x160 `Dat_GFX2` |
| 0x08 | palette_ptr | `PALETTE_DATA` (ARGB4444 LE) | +0x164 `Dat_Pal` |
| 0x0c | extras_ptr | `EXTRAS_DATA` (8-byte assembly recs) | +0x178 `Sprite_Extras` |
| 0x10 | (blank) | — | — |
| 0x14 | animations | `ANIMATION_DATA` (20-byte cells) | +0x168 `animations` |
| 0x18 | hitbox_pattern_tbl | `HITBOX_PATTERN_TABLE` | +0x16c |
| 0x1c | hitbox_data | `HITBOX_DATA` | +0x170 |
| 0x20 | attack_data | `ATTACK_DATA` (0x1C-stride) | +0x174 `attack_data` |
| 0x24..0x34 | AI scripts 0..4 | `AI_SCRIPT_DATA_00..04` | — |
| 0x38,0x3c | (blank) | — | — |

Verified against `dasm_PLDAT/PLDATs/PL00_DAT.BIN` header
(`40 00 00 00  40 7f 06 00  40 e4 06 00  60 eb 06 00 …`) and the emitted block sizes
(`GFX_DATA_00` = 425 728 B, `PALETTE_DATA` = 1 824 B, `EXTRAS_DATA` = 65 536 B).

### 1.2 GFX block → part rectangles

`GFX_DATA_00` begins with a **u32-LE offset table** (`PL00`: `f4 17 00 00, 4d 18 00 00,
ef 1a 00 00 …` = 0x17f4, 0x184d, 0x1aef …). Each entry points (file-relative) to a
**part-cell blob**: a 2-byte header (skipped) then LZSS-compressed **planar 4bpp tiles**
(`combine_planes()` in the decoder is the planar→indexed unpacker; the LZSS stage matches
`bank03:loc_8c03552a`, staging buffer `0x0CE60000`). Width/height per part come from the
dim byte ×8 convention (`docs/MARVELOUS2-GFX-NOTES.md §2`, quad emitter
`bank03:loc_8c033e90`).

`PALETTE_DATA` is ARGB4444 LE, 16 colors / 32 bytes per bank, index 0 transparent
(`PL00`: `00 00, 11 f1, c9 ff, …`). Decode each indexed part against its palette bank.

### 1.3 sprite_id → assembly → parts

- `ANIMATION_DATA` = 20-byte cells; **Sprite @ cell+0x04 (u16) = sprite_id**
  (`docs/MVC2-FRAMEDATA-FIELDS.md §3`, anim tick `bank03:loc_8c034dee`).
- `EXTRAS_DATA` = 8-byte assembly records `{dx:s16, dy:s16, part_idx:u8, b5:u8, mode:u8,
  flip:u8}`; `mode==0xFF` terminates an assembly (`docs/MARVELOUS2-GFX-NOTES.md §3`).
- Chain: `sprite_id → cell → EXTRAS assembly → [{part_idx,dx,dy,flip}]`. The renderer keys
  the atlas by `sprite_id`; per-part placement uses dx/dy + flip (§6 of GFX-NOTES).

### 1.4 The Cable (PL17) upscale bug — and the fix

The committed `atlas/chars/PL17.png` is **2048×3864** vs `PL00.png` 2048×1413, and its
`PL17.json` placement offsets are **fractional** (`dx: -62.5, dy: -174`, `wG:125`). That
is the signature of a 2× upscaled rip (the current atlases came from the live PARTDUMP
probe + the `tools/sprite-upscaler`, not from clean disc source). **Re-ripping PL17 from
`PL17_DAT.BIN` at native resolution produces integer-pixel parts and fixes it.** The same
re-rip regenerates all 59 atlases consistently from one offline source.

### 1.5 Exact flow / command

```bash
# (already done upstream) PLxxPAK.BIN  →  PLxx_DAT.BIN  (unpack) → dasm_PLDAT/PLDATs/
# Step A: slice every PLxx_DAT.BIN into its 13 blocks (the existing decoder)
cd dasm_PLDAT && python dasm_PLDAT_v005a.py          # reads PLDATs/*_DAT.BIN → Output/PLxx_DAT/

# Step B (NEW, in rip_mvc2.py): GFX offset-table → LZSS → planar 4bpp → palette → PNG,
#         pack to atlas/chars/PLxx.png, emit PLxx.json keyed by sprite_id.
python tools/rip_mvc2.py chars --all --src "MVC2 Dev Files" --pldat dasm_PLDAT/PLDATs \
    --out atlas/chars
```

`PLxx.json` schema (matches the renderer's existing consumer, agent doc §"decoded data"):
```json
{ "screenW":640, "screenH":480, "name":"<char>", "image":"PLxx.png",
  "sprites": { "<sprite_id>": {"x":,"y":,"w":,"h":,"dx":,"dy":,"wG":,"hG":,"facing":} } }
```

---

## 2. THE EFFECTS ATLAS (priority) — shared hitspark/effect GFX located

### 2.1 The loader trace (CONFIRMED, bank:line)

`bank0e:loc_8C0EEFCC` (`marvelous2/build/bank0e.asm:36135`) is the **shared-effect loader**.
Register/constant setup at entry:
- `r10 = 0x0CED0000` (Effect Poly base — the shared effect texture page;
  `loc_8c0ef008`), `r12 = 0x0CC00000`, `r13 = 0x0CE80000` (staging buffers),
  `r14 = bank02.loc_8c027366` (the **by-name file loader**, called as `jsr @r14`).
- Bounds check: `mov r4,r0 / add 0xFF,r0 / mov 0x15,r1 / cmp/hs r1,r0` → valid effect
  types **0x00..0x14 (21 entries)**, out-of-range → default case `loc_8c0ef268`.
- 21-entry jump table at `loc_8c0ef018` (`bank0e.asm:36185-36206`).

Each case loads one or two **texture IDs** (the `mov.w @(loc_…),r4` constants), passing the
ID in `r4` and the destination buffer in `r5` to `jsr @r14`. The IDs, from
`bank0e.asm:36388-36411`:

| const label | tex ID | resolves to (filepnt) |
|-------------|-------:|-----------------------|
| loc_8c0ef15e | 0x026e | **EFKYPOL.BIN** |
| loc_8c0ef160 | 0x026f | **EFKYTEX.BIN** |
| loc_8c0ef164 | 0x0292 | DM00POL.BIN |
| loc_8c0ef166 | 0x0293 | DM00TEX.BIN |
| loc_8c0ef168 | 0x0d10 | (RAM/inline arg, not a file id) |
| loc_8c0ef16a | 0x029e | DM07POL.BIN |
| loc_8c0ef16c | 0x029f | DM07TEX.BIN |
| loc_8c0ef16e | 0x02a0 | DM08POL.BIN |
| loc_8c0ef170 | 0x02a1 | DM08TEX.BIN |
| loc_8c0ef172 | 0x02a2 | DM09POL.BIN |
| loc_8c0ef174 | 0x02a3 | DM09TEX.BIN |

### 2.2 texture-ID → disc file (CONFIRMED)

`bank02:loc_8c027366` (`bank02.asm:17432`) resolves an item ID by indexing the **File Table
`filepnt.loc_8c148688`** (`bank02.asm:17517-17518`, "File Table"). `filepnt.asm` is a flat
u32 array of pointers to filename strings in `filedic.asm`, one per file ID. The relevant
rows (`filepnt.asm:625-626, 661-678`):

```
0x26e EFKYPOL.BIN   0x26f EFKYTEX.BIN
0x292 DM00POL.BIN   0x293 DM00TEX.BIN
0x29e DM07POL.BIN   0x29f DM07TEX.BIN
0x2a0 DM08POL.BIN   0x2a1 DM08TEX.BIN   0x2a2 DM09POL.BIN  0x2a3 DM09TEX.BIN
```

So the loader pulls the **whole file** by name; the texture ID is just a file-table index.
**`EFKYTEX.BIN` (374 784 B) is the shared effects texel blob; `EFKYPOL.BIN` (155 344 B) is
its part/poly descriptor.** (`EFKY` = effekt; present once on the disc, not per-character —
exactly the character-INDEPENDENT shared bank.)

### 2.3 EFKYPOL/EFKYTEX format (CONFIRMED from the bytes)

`*POL`/`*TEX` is the same pairing as stages (`STGxxPOL/TEX`) and demos (`DMxxPOL/TEX`).

**`EFKYPOL.BIN` header** (hexdump):
```
+0x00  u32  0x0CED0010   base load address (Effect Poly 0x0CED0000 + 0x10 header)
+0x04  u32  0x000000F1   = 241 entries (part/sprite descriptors)
+0x08  u32[241]          absolute pointers (0x0CEDxxxx). file_off = ptr - 0x0CED0000.
```
The first entry → 0x0CED03D8 ⇒ file offset 0x3D8. **Part descriptor (16-byte stride):**
```
+0x00 u16  w           (e.g. 0x0080 = 128)
+0x02 u16  h           (e.g. 0x0080 = 128)
+0x04 u32  desc        byte1 = pixel-format selector, byte0 = present flag
                       (0x00000302 → fmt sel 0x03 = PAL8; matches decode_raw_part.E4_SELECTOR)
+0x08 u32  texptr      pointer into the VRAM upload buffer (0x0CC0xxxx = r12 staging)
+0x0C u32  0
```
A `0xFFFFFFFF / 0x0003` word terminates a sub-list. The format byte1 selector is the same
one `tools/decode_raw_part.py` already documents (`E4_SELECTOR`): `0x00 argb1555, 0x01
rgb565, 0x02 argb4444, 0x03 pal8, 0x04 pal4`, all twiddled.

**`EFKYTEX.BIN`** is the raw texel source (head: `00 00 00 00 00 00 00 00 ff 0d ff 0d …`
— ARGB4444/PAL data uploaded to the staging buffer, then referenced by the POL texptrs).

> Decode model for the ripper: walk `EFKYPOL` entries → for each, read `(w,h,fmt)`; the
> texels are `EFKYTEX` content laid out per the POL's load order (the POL pointers are the
> VRAM destinations; the source order is the POL entry order). Decode each with the existing
> twiddle/format machinery in `decode_raw_part.py` (`decode()` for 16-bit, `decode_paletted()`
> for PAL4/PAL8 using a palette from `EFKYTEX`/`EFKYPOL`'s palette region).

### 2.4 effect-type → {w,h,scale} (CONFIRMED constants)

The spawned effect object's **type byte (`obj+0x20`)** indexes constant tables in `bank16`:
- **Size:** `loc_8c164c18` (`bank16.asm:4051`) — u16 pairs
  `{0x0305,0x0360,0x0360,0x016C,0x02D8,0x02D8,…}` (per-type raw w/h).
- **Scale:** `loc_8c164c30` (`bank16.asm:4065`) — float32 LE pairs (`CCCD 3D4C` = `0x3D4CCCCD`
  ≈ 0.05; `CCCD 3CCC` = `0x3CCCCCCD` ≈ 0.025) = per-type X/Y scale.
- **Scale/offset 2:** `loc_8c164c78` (`bank16.asm:4103`) — float32 LE (`3333 3EB3` =
  `0x3EB33333` ≈ 0.35, interleaved with zeros).

### 2.5 effect-type ↔ anotak Hitspark categories (cross-ref)

The attack record's **Hitspark byte @ attack+0x0b** (`docs/MVC2-FRAMEDATA-FIELDS.md §2`,
read at `bank05:loc_8c0578c0`) carries the effect type that drives `loc_8C0EEFCC`. anotak's
`Hitspark` enum (`refs/anotak/fields/attack/Hitspark.json`) maps the type values:

| hex | name | chars | entries |
|----:|------|------:|--------:|
| 0x00 | Light | 57 | 557 |
| 0x01 | Medium | 57 | 576 |
| 0x02 | Heavy | 55 | 500 |
| 0x03 | Special | 59 | 1425 |
| 0x04 | LaserLight | 10 | 57 |
| 0x05 | LaserMedium | 2 | 4 |
| 0x06 | LaserHeavy | 4 | 15 |
| 0x08 | RegularMaybe | 2 | 20 |
| 0x09 | SlashLightF | 3 | 9 |
| 0x0A | SlashLightDF | 3 | 28 |
| 0x0B | SlashLightU | 7 | 17 |
| 0x0C | SlashLightUF | 5 | 29 |
| 0x0D | SlashMediumF | 5 | 51 |
| 0x0E | SlashMediumDF | 6 | 56 |
| 0x0F | SlashMediumD | 3 | 7 |
| 0x10 | SlashMediumDB | 1 | 5 |
| 0xFF | (none) | 5 | 28 |

These 0x00..0x14 values are exactly the index domain of the `loc_8C0EEFCC` jump table, so
**effect type (Hitspark value) → loader case → texture IDs → disc file + POL entry range →
{w,h,scale}**. The ripper keys the effects atlas by this type.

> NOTE on the cell-side trigger: the *cell*-driven effect spawn (`EffectTrigger`) reads
> `cell+0x0c` (player+0x14c) in `bank04:loc_8c042014` (FRAMEDATA-FIELDS §3); the *attack*-side
> Hitspark @0x0b is the on-hit impact effect. Both route into the shared Effect Poly
> `0x0CED0000`. Catalog both as effect-type producers.

---

## 3. Per-character hit / attack data (CONFIRMED identities)

| disc file | PLDAT block (decoder) | content | stride / format | player slot |
|-----------|----------------------|---------|-----------------|-------------|
| `HIT_xx.BIN` | `HITBOX_PATTERN_TABLE` (≡ byte-identical) | hitbox group → pattern indices | u16 entries (`f3 80, 01 00, 02 00 …`) | +0x16c |
| `HIT_FMxx.BIN` | `HITBOX_DATA` (≡ byte-identical) | hitbox geometry | s16 x/y/w/h boxes (`fc ff 14 00, ac ff 0a 00 …`) | +0x170 |
| `HIT_DTxx.BIN` | `ATTACK_DATA` (≈, region-trimmed) | attack records | **0x1C (28) bytes** each | +0x174 |
| `ATCKxx.BIN` | (dispatch table; not a PLDAT block) | move → attack-record index | 3 header ptrs + per-move ramp | feeds +0x1a1 `attack_data_index` |
| `PLxx_TBL.BIN` | (per-char tables; e.g. health/throw) | misc per-char params | u16 fields (`e8 03 e8 03 …` = 1000/1000) | — |
| `PLxx_FAC.BIN` | — | companion FAC (cell-geom metadata) | — | +0x184 `FAC_ptr` |

Cross-references (already mapped in repo):
- **Attack record 0x1C-stride** with fields `Damage@0x00, HitReaction@0x01, BlockFlags@0x02,
  …, Hitspark@0x0b, flags@0x12` — full table in `docs/MVC2-FRAMEDATA-FIELDS.md §2`, anotak
  dicts `refs/anotak/fields/attack/*.json`, per-char values `refs/anotak/attacks/PLxx.json`.
- **Animation cell 0x14-stride** with `Sprite@0x04 (=sprite_id), Duration@0x02,
  HitboxGroup@0x12, EffectTrigger@0x0c` — `FRAMEDATA-FIELDS §3`, `refs/anotak/animations/PLxx.json`.
- These let the ripper attach **frame-data + hit/attack metadata per sprite_id** without
  any new RE — both the disc `HIT_*`/`ATCK*` files and the PLDAT-decoded blocks are available.

---

## 4. `catalog.json` schema (keyed by player + type)

```json
{
  "version": 1,
  "source": "MVC2 Dev Files (local disc extract)",
  "chars": {
    "PL00": {
      "name": "Ryu", "char_id": 0,
      "atlas": "atlas/chars/PL00.png",
      "files": { "pak":"PL00PAK.BIN", "dat":"PL00_DAT.BIN",
                 "hitbox_pattern":"HIT_00.BIN", "hitbox_data":"HIT_FM00.BIN",
                 "attack_data":"HIT_DT00.BIN", "attack_index":"ATCK00.BIN",
                 "tbl":"PL00_TBL.BIN", "fac":"PL00_FAC.BIN" },
      "sprites": {
        "<sprite_id>": {
          "atlas_rect": {"x":,"y":,"w":,"h":,"dx":,"dy":},
          "assembly":  [ {"part_idx":,"dx":,"dy":,"flip":} ],
          "anim": { "duration":, "hitbox_group":, "effect_trigger":,
                    "anotak": "refs/anotak/animations/PL00.json#<cell>" },
          "attack": { "record_index":, "damage":, "hitspark":,
                      "marvelous2": "bank05:loc_8c0578c0",
                      "anotak": "refs/anotak/attacks/PL00.json#<rec>" }
        }
      }
    }
  },
  "effects": {
    "<type_hex>": {
      "name": "Light|Medium|Heavy|Special|LaserLight|Slash...",   // anotak Hitspark name
      "atlas_rect": {"x":,"y":,"w":,"h":},
      "source": { "file":"EFKYTEX.BIN", "pol":"EFKYPOL.BIN",
                  "pol_entry_first":, "pol_entry_count":,
                  "tex_id_pol":"0x026e", "tex_id_tex":"0x026f" },
      "geometry": { "w":, "h":, "scale_x":, "scale_y":,
                    "size_tbl":"bank16:loc_8c164c18", "scale_tbl":"bank16:loc_8c164c30" },
      "loader": "bank0e:loc_8C0EEFCC[case]",
      "anotak": "refs/anotak/fields/attack/Hitspark.json#<type>"
    }
  }
}
```

`type` keys are the anotak Hitspark hex values (0x00..0x10, 0xFF). Effect types that load
`DMxxTEX` instead of `EFKYTEX` carry their own `source.file`.

---

## 5. Ripper plan — `tools/rip_mvc2.py`

### Decodable NOW (existing tooling, just orchestrate offline)
- **Char blocks:** `dasm_PLDAT_v005a.py` already slices `PLxx_DAT.BIN` → 13 blocks
  (GFX/PAL/EXTRAS/ANIM/HITBOX/ATTACK/AI).
- **Part pixels:** the format/twiddle/palette decoders in `tools/decode_raw_part.py`
  (`decode`, `decode_paletted`, twiddle tables ported from flycast `texconv.cpp`) and the
  planar 4bpp `combine_planes()` in the decoder.
- **Atlas packing + JSON:** `tools/pack_part_atlas.py` (rect packer + assembly JSON) — adapt
  its packer to consume offline-decoded parts instead of the live PARTDUMP manifest.
- **Frame-data:** read `HIT_DTxx`/`HIT_FMxx`/`HIT_xx`/`ATCKxx` directly (or the PLDAT blocks)
  and the anotak JSON in `refs/anotak/`.

### Needs NEW decode (the missing piece — the shared effects GFX)
- **EFKYPOL/EFKYTEX parser:** header (base@0x00, count@0x04, ptr table@0x08), 16-byte part
  descriptors (`w,h,fmt,texptr`), texel decode via the existing format/twiddle path. No live
  probe required — it is a static `*POL`+`*TEX` pair like the stages.
- **Effect geometry table reader:** parse the `bank16` constant tables
  (`loc_8c164c18`/`c30`/`c78`) for per-type `{w,h,scale}` (or hardcode the extracted values —
  they are small constant arrays).

### Function layout
```python
# --- containers ---
read_pldat_header(path)            -> dict of 16 pointers           # mirrors dasm_PLDAT
slice_pldat(path)                  -> {GFX0,GFX1,PAL,EXTRAS,ANIM,HBP,HBD,ATK,...}

# --- char sprites ---
decode_gfx_pool(gfx_blob, pal)     -> [Part(idx,img,w,h)]            # offset tbl→LZSS→planar4bpp→palette
parse_anim(anim_blob)              -> {sprite_id: cell}              # 20B cells, Sprite@+4
parse_extras(extras_blob)          -> {assembly_id: [rec8]}         # reuse pack_part_atlas.parse_extras
pack_char_atlas(parts, anim, extras, name) -> (png, json)           # atlas/chars/PLxx.*

# --- effects (NEW) ---
parse_efkypol(pol)                 -> [PartDesc(off,w,h,fmt,texptr)] # base@0,count@4,ptrs@8; 16B descs
decode_effect_parts(pol, tex)      -> [Effect(img,w,h)]             # format/twiddle from decode_raw_part
EFFECT_GEOM = parse_bank16_tables()                                  # type→{w,h,scale}
EFFECT_TYPE_SOURCE = {0x00..:('EFKYTEX',pol_range), 0x..:('DM07TEX',..)}  # from loc_8C0EEFCC trace
pack_effect_atlas(effects, geom)   -> (png, json)                   # atlas/effects/effects.* keyed by type

# --- frame data + catalog ---
parse_attack_records(hit_dt)       -> [rec28]                        # 0x1C stride
parse_hitboxes(hit_fm, hit)        -> geometry                       # HITBOX_DATA + PATTERN_TABLE
attach_anotak(char, refs/anotak)   -> metadata                       # per-char attack/anim JSON
build_catalog(chars, effects)      -> catalog.json                   # §4 schema

# --- CLI ---
#   rip_mvc2.py chars   --all  --src "MVC2 Dev Files" --pldat dasm_PLDAT/PLDATs --out atlas/chars
#   rip_mvc2.py effects        --src "MVC2 Dev Files"                            --out atlas/effects
#   rip_mvc2.py catalog        --out catalog.json
#   rip_mvc2.py all     --all  ...   # full pipeline
```

### Output layout
```
atlas/chars/PLxx.png      atlas/chars/PLxx.json        # 59 chars, native res (fixes PL17 upscale)
atlas/effects/effects.png atlas/effects/effects.json   # shared hitsparks/effects, keyed by type
catalog.json                                           # player+type → sprite_id/effect/attack → anotak + marvelous2
```

### Risk / ordering
1. **Char re-rip first** (lowest risk — pure orchestration of the proven PLDAT+part decoders;
   immediately fixes the PL17 native-res regression and regenerates all 59 from one source).
2. **Effects atlas second** (the new decode). EFKYPOL/EFKYTEX are static `*POL/*TEX` like the
   stages, so the only unknowns are (a) the exact POL-entry → effect-type grouping and (b) the
   palette region for PAL8 effect parts. Resolve (a) from the `loc_8C0EEFCC` per-case texptr
   ranges; resolve (b) by trying the EFKY palette region (and A/B against the live
   `effects-capture/efx_*.png` oracle already captured from VRAM — those are the ground truth
   to validate the offline decode against).
3. **Catalog last** (joins the two atlases to anotak + marvelous2; no new decode).

### Validation oracle
- Char parts: diff a packed assembly against `MvC2_Spritesheets_20260516/PLxx.png`
  (`pack_part_atlas.py --validate`).
- Effects: diff offline-decoded EFKY parts against `effects-capture/efx_NNN.png` (the live
  VRAM capture of `0x0CED0000`) — if they match, the EFKYTEX→type decode is correct and the
  effects atlas is built from clean disc source instead of a runtime VRAM scrape.

---

## Appendix — citation index

| claim | source |
|-------|--------|
| PLDAT 16-ptr header / block slicing | `dasm_PLDAT/dasm_PLDAT_v005a.py:341-444` |
| PAK≠DAT; game loads `PLxx_DAT.BIN` | `marvelous2/build/data/filepnt.asm:212,235`; no PAK in `filedic.asm` |
| effect loader, 21-type jump table | `marvelous2/build/bank0e.asm:36135-36206` |
| effect texture-ID constants | `marvelous2/build/bank0e.asm:36388-36411` |
| texture-ID → file (File Table) | `marvelous2/build/bank02.asm:17432,17517-17518` |
| 0x26e EFKYPOL / 0x26f EFKYTEX | `marvelous2/build/data/filepnt.asm:625-626` |
| DM00/DM07/DM08/DM09 effect files | `marvelous2/build/data/filepnt.asm:661-678` |
| EFKYPOL header + 16B part descriptor | hexdump `MVC2 Dev Files/EFKYPOL.BIN` (this pass) |
| part format selector byte | `tools/decode_raw_part.py:91-97` (`E4_SELECTOR`) |
| effect size table | `marvelous2/build/bank16.asm:4051` (`loc_8c164c18`) |
| effect scale tables | `marvelous2/build/bank16.asm:4065,4103` |
| HIT_00 ≡ HITBOX_PATTERN_TABLE, HIT_FM00 ≡ HITBOX_DATA | byte-cmp vs `dasm_PLDAT/Output/PL00_DAT/*` |
| attack record 0x1C-stride + fields | `docs/MVC2-FRAMEDATA-FIELDS.md §2`; `bank05:loc_8c059384` |
| anim cell 0x14-stride + Sprite@0x04 | `docs/MVC2-FRAMEDATA-FIELDS.md §3`; `bank03:loc_8c034dee` |
| Hitspark @attack+0x0b → effect spawn | `docs/MVC2-FRAMEDATA-FIELDS.md §2`; `bank05:loc_8c0578c0` |
| Hitspark type enum | `refs/anotak/fields/attack/Hitspark.json` |
| PL17 upscale signature | `atlas/chars/PL17.{png(2048×3864),json(fractional dx/dy)}` |
| existing atlas/effect tooling | `tools/pack_part_atlas.py`, `tools/decode_raw_part.py`, `tools/decode_effects.py` |
