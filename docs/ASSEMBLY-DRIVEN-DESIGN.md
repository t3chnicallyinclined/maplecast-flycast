# Assembly-Driven Part Renderer — Design & Feasibility

> **Goal.** Ship only compact game STATE per frame (`sprite_id` + screen pos per body
> and per pool object — already in the GSTA). The client reconstructs each frame by
> looking up a precomputed assembly (`sprite_id → parts + offsets`) and drawing PARTS
> from a locally-cached part atlas. This is the most-compact, pixel-exact, zero-
> copyrighted-assets-on-the-wire path.
>
> **Companion docs:** `docs/MARVELOUS2-GFX-NOTES.md` (SH4 render model),
> `docs/PART-ASSEMBLY-PLAN.md` (RE status), `docs/MARVELOUS2-RE-HANDOFF.md` (codec),
> `web/webgpu/pldat-codec.mjs` (offline decoder + the codec wall).
>
> Clean-room: addresses / field offsets / algorithm descriptions in our own words.
> No verbatim disassembly. **Never commit ROM-derived pixels.**

---

## 0. The blocker, and the way around it

Offline static decode of the part pixels is **blocked**. The PLDAT GFX codec (flag-bit
LZSS over u16 LE, `pldat-codec.mjs`) is cracked, but the game decodes each part into a
**single fixed scratch buffer** and large parts back-reference the residue of the
*previously* decoded part sitting in that scratch. Only ~14% of parts are self-contained
and decode offline; the other ~86% need the live scratch state, which the static
`GFX_DATA_00` file does not capture. So we cannot build the atlas purely offline.

**But we run the game.** The feasibility question is therefore: *can the headless server
capture the already-decoded parts at runtime?* The answer — traced end to end in the
marvelous2 disassembly — is **yes, cleanly, from SH4 main RAM the mirror already reads.**

---

## 1. KEY FEASIBILITY FINDING — runtime part capture is feasible; decoded parts live in main RAM

### Where the decoded parts end up (the copy-out destination)

The per-part decode loop is `bank03.asm:loc_8c032696`. Per part it:

1. Loads the compressed source from the GFX offset table (`src = table_base + table[idx]`).
2. Calls the LZSS decoder (`loc_8c03552a`) with the destination **pinned to the constant
   scratch `0x0CE60000`** (the literal `0x0ce60000` at `loc_8c032854`). This is the
   `Texture_Decompress_Buffer` — confirmed by name in `marvelous2/memory/work.asm`
   (`;0ce60000 - Texture_Decompress_Buffer`). Every part decodes into the **same** scratch,
   overwriting the previous one (this is exactly why offline decode underflows).
3. **Copies the decoded result OUT of the scratch** into the part's persistent texture
   slot. Two `mov.l @rN+ ... mov.l rM,@rDest` copy loops move the result word-by-word into
   a destination pointer fetched from a per-part directory: `dest = *(dir_entry + 0x8)`,
   where the directory has **0x10-byte stride** per part and its base is read from the GFX
   work header at `*(0x0CE80008)`.

### The persistent home: `0x0CE80000` — "DM00 Poly"

The directory base and the destination slots live in the region starting at **`0x0CE80000`**,
labelled **`;0ce80000 - DM00 Poly`** in `work.asm` (the neighbouring poly/texture regions
are `0x0CEA0000` Stage Poly, `0x0CED0000` Effect Poly). After the decode loop runs for a
character, every one of its parts sits **decompressed, in plain 4bpp indexed texels, in
main RAM at a stable, directory-addressable address** — not transient.

**Crucial address-space fact:** `0x0C000000`–`0x0CFFFFFF` is the Dreamcast **system RAM**
(the 16 MB SH4 main RAM, `mem_b[]`). `0x0CE60000` and `0x0CE80000` are offsets
`0x00E60000` / `0x00E80000` into that array — i.e. **they are inside `mem_b`, NOT VRAM.**
The mirror already memcpy's all of `mem_b[]` (`maplecast_mirror.cpp:529`,
`memcpy(&mem_b[0], snap+off, 16MB)`) and the gamestate reader already random-access reads
it via `addrspace::read32(0x8C......)` (`maplecast_gamestate.cpp`). So **reading the
decoded parts is the exact same mechanism we already use for `sprite_id`, palettes, and
the EXTRAS list — no new emulator surface.**

### The capture mechanism (precise)

A server-side hook reads, once per character after its DAT loads (`Dat_FilePointer`
player+0x17c becomes non-null and the GFX directory at `0x0CE80000` is populated):

```
dir_base  = read32(0x8CE80008)          ; per-part directory base (DM00 Poly header +8)
nParts    = (GFX1 offset-table[0]) >> 2 ; part count from the offline table (we already parse this)
for idx in 0..nParts-1:
    entry   = dir_base + idx*0x10        ; 0x10-byte stride
    dest    = read32(entry + 0x8)        ; the part's persistent texel slot (in mem_b)
    # part dims come from the 4-byte blob header (w,h,sw,sh in 8px tiles) we already read
    # offline from the GFX offset table; texel count = sw*sh*64 nibbles.
    texels  = read_bytes(dest, sw*sh*64/2)   ; raw 4bpp indexed texels, already de-LZSS'd
    emit part rectangle (de-interleave 8x8 tiles via tileToImage(), index 0 = transparent)
```

i.e. the **directory at `0x0CE80000` enumerates the parts; field +0x8 of each 0x10-byte
entry is the part's decoded address; the blob header (already parsed offline) gives the
dimensions.** No need to reverse the scratch residue — we read the *output* of the copy-out,
which is the clean, fully-decoded part. The offline codec stays useful only as the
**part-count / dimensions / directory cross-check oracle**, not as the pixel source.

> Two small live confirmations to do at the box (read-only, one capture each, per the
> "confirm presence first" rule):
> 1. Confirm the directory header field — that `*(0x0CE80008)` is the part directory base
>    and entry+0x8 is the dest (vs +0x0 / a different stride); single-step
>    `loc_8c032696`'s two copy loops or just dump 0x0CE80000..+0x40 and match against the
>    part dims.
> 2. Confirm the dest slots stay resident (DM00 Poly is per-character-persistent, not
>    reused mid-frame). If they're reused, capture during the load gap instead of in the
>    hot path; the load happens once per character entrance, so timing is generous.

### Why not read parts straight from VRAM?

We could, but it's strictly worse here. The TA's TCW addresses point at uploaded textures
in VRAM (the mirror already reads `vram[]` and decodes TCW formats —
`maplecast_mirror.cpp:1310-1347`). However: (a) by the time a part is in VRAM it is
**twiddled / format-encoded** for the PVR and would need un-twiddling per format; (b) VRAM
textures are paletted/packed at upload granularity, not 1:1 with our part rectangles; (c)
VRAM churns every frame, so isolating one character's parts means tracking uploads. The
**`0x0CE80000` copy-out is the canonical, format-clean, per-part, once-per-load source** —
plain 4bpp indexed texels in the exact tile layout `tileToImage()` already expects. Use
VRAM only as a fallback cross-check.

**Bottom line:** runtime part capture is feasible and clean. Decoded parts live at
`0x0CE80000` (DM00 Poly) in main RAM, enumerated by a 0x10-stride directory at
`*(0x0CE80008)`, each entry's +0x8 pointing at the part's decompressed 4bpp texels —
all reachable by the same `addrspace::read*` / `mem_b[]` path the mirror already uses.

---

## 2. The deliverable design

### 2.1 Part atlas (offline-once-per-char, runtime-captured)

Per character, run a one-time capture pass on the headless server:

- **Trigger:** load the character (training/character-select forces the DAT load); detect
  `Dat_GFX1` (player+0x15c) populated and the `0x0CE80000` directory built.
- **Enumerate:** part count from the GFX1 offset table (`table[0]>>2`); per part, read the
  directory entry (`*(0x0CE80008) + idx*0x10`), follow +0x8 to the decoded texels, read
  `sw*sh*64` nibbles.
- **Emit:** `PL{hex}_parts.png` — a packed atlas of all part rectangles (indexed, with a
  rect table), plus `PL{hex}_asm.json` (§2.2). **ROM-derived → gitignored** (`.gitignore`
  already blocks the dasm/Output/Dev-Files trees; the atlas joins them). Never committed.
- **Validation oracle:** composite an assembly from captured parts and diff against the
  community indexed sprite-sheet rip (`MvC2_Spritesheets_*/PL{hex}.png`) — pixel-for-pixel,
  same oracle the codec used. The runtime capture is *exact* (it is the game's own output),
  so this is a sanity check, not a tuning loop.

This replaces the blocked offline LZSS decode entirely as the atlas source. The decoder in
`pldat-codec.mjs` is retained for part-count / dims / self-contained-part cross-checks.

### 2.2 Assembly table (`sprite_id → parts + offsets`)

Precompute offline from EXTRAS + ANIMATION (per `MARVELOUS2-GFX-NOTES.md` §3–4):

```
PL{hex}_asm.json = {
  parts:  [ {atlas_x, atlas_y, w, h} ... ],            # rects into PL{hex}_parts.png
  palette: [ ...16 ARGB4444 entries... ],              # Dat_Pal, per char
  assemblies: {                                         # keyed by sprite_id
    <sprite_id>: [ {part_idx, dx, dy, flip, z} ... ]    # one record per placement
  }
}
```

- **EXTRAS walk** (`bank10.asm:loc_8C108060/86`): 8-byte records
  `{dx s16, dy s16, part_idx u8, b5 u8, mode u8, flip u8}`; `mode==0xFF` ends an assembly;
  `flip & 0x80` = horizontal mirror. (Already cracked: 42 assemblies for PL00, parts 0–21.)
- **ANIMATION → assembly**: `sprite_id` (player+0x144) selects the cell; the keyframe table
  (player+0x168, 20-byte records, `sprite_id` at bytes 4–6) resolves `sprite_id → cell →
  EXTRAS assembly`. **The GSTA already ships the resolved `sprite_id`, so no timer emulation
  is needed** — map `sprite_id → assembly index` once, offline.
- **Per-object z from the priority byte**: per `MARVELOUS2-RE-HANDOFF.md §3`, the pool has
  no numeric z; order is category (`record+0x03`) + sprite_id range (cape behind body,
  lightning/super in front). Bake a `z` per assembly from (category, sprite_id band) so the
  client can stable-sort. For the body, z = base layer.

### 2.3 Per-frame wire (unchanged)

The GSTA already carries everything: per body, `sprite_id` + `screen_x/y` + facing +
palette id; per pool object, `sprite_id` + `screen_x/y` + category + xflip (the planned
OBJS 9-byte stride). **No wire change.** Client per frame:

```
for each live object (6 bodies + N pool objects):
    asm = atlas[char_id].assemblies[object.sprite_id]
    for rec in asm:                                   # z-ordered across all objects
        part = atlas[char_id].parts[rec.part_idx]
        x = object.screen_x + (rec.flip^facing ? -rec.dx - part.w : rec.dx) * Sx
        y = object.screen_y + rec.dy * Sy
        blit(part, x, y, hflip = rec.flip ^ facing,
             palette = palette_for(char_id, object.pal_id))   # recolor as today
```

Scale `Sx=1.6667 / Sy=2.1428` (`CpsXScale/CpsYScale` from `work.asm`), zoom=1.

### 2.4 Numbers

| Quantity | Value | Basis |
|---|---|---|
| **Per-frame wire (bodies)** | 261 B/frame → **~15.3 KB/s** | `maplecast_gamestate.h` `WIRE_SIZE=261`, 60fps |
| **Per-frame wire (+ pool objs)** | +9 B/obj; ~20–48 objs typical | OBJS block; worst case ~36 KB/s |
| **Steady-state total** | **~15–36 KB/s** | identical to current GSTA — no change |
| **Atlas per char** | **~0.5–1.5 MB** | ~1500 parts × ~small rects, indexed 4bpp + PNG-deflate; PL00 GFX_00 is 425 KB *compressed on disc*, decoded-then-packed-then-PNG lands ~1 MB |
| **Assembly JSON per char** | **~20–80 KB** | ~42–200 assemblies × ~24 records × small ints + part rects |
| **Total client footprint (59 chars)** | **~60–110 MB** | one-time download, cached locally; ~1–1.8 MB/char |

The atlas downloads **once** (cached, like a texture pack). Steady-state bandwidth is
*only* the ~15–36 KB/s GSTA — the same as today.

---

## 3. Honest comparison: assembly-driven vs texture-cache / stripped-TA

The sibling design ("texture-cache"): keep streaming a stripped TA display list, but ship
texture pixels **once** (client texture cache keyed by TCW/hash), so steady-state frames
carry only geometry + cache references.

| Axis | **Assembly-driven (this doc)** | **Texture-cache / stripped-TA** |
|---|---|---|
| **Steady-state bandwidth** | **Lowest.** ~15–36 KB/s (GSTA only). No geometry on the wire — the client *derives* it from `sprite_id`. | Higher. Per-frame TA geometry (vertices/UVs/poly headers per object) even with cached textures — typically 100s of KB/s. |
| **Pixel-exactness** | Exact *if* the assembly+atlas are faithful. We capture the game's own decoded parts and replay its own assembly list, so geometry is the SH4's geometry. Residual risk = our `sprite_id→assembly` map + scale. | Exact by construction (it's the actual TA output) — modulo whatever the strip drops. |
| **Build effort** | **More.** Runtime part-capture hook, atlas baker, assembly extractor, client assembly renderer, per-char validation, cape/pool object handling. | Less. Mostly a TA-strip + client texture cache; reuses the existing mirror/TA path. |
| **Copyrighted assets on the wire** | **Zero.** Only `sprite_id`+pos cross the wire. Atlas is built locally from the operator's own ROM, gitignored, downloaded by clients from the operator (same trust boundary as today's skins). | **Texture pixels cross the wire** (once, but they do). Those are ROM-derived. Higher exposure. |
| **Cape (and crouch) correctness** | **Definitively fixed.** The cape is a *separate pool object* with its **own `sprite_id`/assembly** (`MARVELOUS2-GFX-NOTES.md §7`). The per-object render loop draws it as "just another object," at its own z, with its own assembly — including its **crouch** cell, because crouch is simply a different `sprite_id` selecting a different cape assembly. We never special-case attach; the object table drives it. | Cape "just works" because it's whatever the TA emitted — but at the cost of streaming its geometry every frame. |

### The cape / crouch confirmation (per-object render handles it exactly)

The whole-sprite branch fought cape-attach because it reconstructed placement from body
state. The assembly-driven model **eliminates that class of bug**: each object (body, cape,
projectile, super overlay) is rendered independently from *its own* `sprite_id → assembly →
parts`, z-ordered by category. Crouch is not a special case — it is the cape object's
`sprite_id` changing to its crouch cell, which selects the crouch assembly automatically.
The only requirement is that the **OBJS pool reader enumerates the cape object** (it does:
pool base `0x8C26AA54`, stride `0x1D0`, owner@+0x80, sprite_id@+0x12C —
`MARVELOUS2-RE-HANDOFF.md §3`). One live capture confirms the cape appears as a sibling
object during a cape frame (`MARVELOUS2-GFX-NOTES.md §7`, final bullet).

### Recommendation

**Assembly-driven wins on our stated goals** (lowest bandwidth, pixel-exact cape, zero
copyrighted assets) — it is the only path that ships *zero* ROM-derived pixels on the wire,
holds steady-state at the ~15–36 KB/s GSTA, and fixes cape/crouch structurally rather than
by special-casing. It costs more build than texture-cache, and carries the residual risk in
the `sprite_id→assembly` map and scale constants (mitigated by the sprite-sheet oracle).
Given the project's identity (compact state, recolorable skins, distributed clients that
already download skin data), **pursue assembly-driven as the primary path; keep the existing
whole-sprite path on `feat/rom-asset-probe` as the working fallback** until the assembly
renderer validates per-character.

---

## 4. Phased build plan

Each phase has a concrete `verify:` check.

- **Phase 0 — Live confirmation of the capture point (read-only, 1 capture).**
  At the box, dump `0x0CE80000..+0x80` and a few `*(dir+idx*0x10+0x8)` targets after a
  character loads. *verify:* the dims at the dest match the offline blob headers (w,h,sw,sh)
  and the first self-contained part (PL00 part 326, the clean 8×8 tile) matches the offline
  decode pixel-for-pixel.

- **Phase 1 — Server part-capture hook.**
  Add a `MAPLECAST_DUMP_PARTS` path in the gamestate/mirror layer: on character load, walk
  the directory, read each part's texels, write `PL{hex}_parts.png` + rect table (gitignored).
  *verify:* re-run on PL00; composite assembly 0 from captured parts and diff against
  `MvC2_Spritesheets_*/PL00.png` — zero mismatched non-transparent pixels.

- **Phase 2 — Assembly extractor.**
  Offline tool: EXTRAS grouping + ANIMATION keyframe map → `PL{hex}_asm.json`
  (`sprite_id → [{part_idx,dx,dy,flip,z}]` + part rects + palette).
  *verify:* every `sprite_id` the GSTA emits for PL00 in a recorded match resolves to a
  non-empty assembly; composited frames match the sprite sheet.

- **Phase 3 — Client assembly renderer.**
  In `sprite-client.mjs` / `sprite-gpu.mjs`: per object, `sprite_id → assembly → parts`,
  z-ordered by category, flip = facing XOR record bit, scale `CpsX/CpsY`, palette recolor as
  today. Replaces whole-sprite draw for body **and** each pool object.
  *verify:* side-by-side a recorded match vs the real game frame; body + projectiles align.

- **Phase 4 — Cape / crouch.**
  Ensure the OBJS reader ships cape objects; client renders them as independent objects.
  *verify:* a cape character (Magneto/Storm/Doom) crouches and the cape follows exactly,
  with correct z (behind body); no attach special-case in the code.

- **Phase 5 — Roster bake.**
  Run the Phase-1 hook across all 59 PAKs → atlas + assembly JSON per char (gitignored).
  Client lazy-loads per character.
  *verify:* every roster character renders from state alone; steady-state bandwidth unchanged
  (~15–36 KB/s, `MAPLECAST_DUMP_TA=1` determinism rig clean at phase end).

---

## 5. Citations (into the local, gitignored marvelous2 clone — re-grep to extend)

| Fact | Where |
|---|---|
| Per-part decode loop, scratch dest `0x0CE60000`, copy-out loops | `bank03.asm:loc_8c032696` (~5668), const `loc_8c032854` |
| Per-part fetch wrapper (`src = table_base + table[idx]`) | `bank03.asm:loc_8c0322c0` (5069) |
| File-load stage path (`0x0CC00000` staged GFX) | `bank03.asm:loc_8c0323b2` (5221) |
| Scratch = `Texture_Decompress_Buffer`; `0x0CE80000` = DM00 Poly | `memory/work.asm:36-39` |
| GFX directory base read from work header `*(0x0CE80008)` | `bank03.asm` decode loop (`r8 = @(0x8,r4)`, r4=`0x0ce80000`) |
| LZSS decoder (codec) | `bank03.asm:loc_8c03552a` (12740) |
| EXTRAS list iterator / 8-byte records / `mode==0xFF` | `bank10.asm:loc_8C108060/86`; `MARVELOUS2-GFX-NOTES.md §3` |
| `sprite_id`→cell→assembly; 20-byte keyframes | `bank03.asm:loc_8c034dee` (11567); `MARVELOUS2-GFX-NOTES.md §4` |
| Pool base/stride; cape = separate object | `bank04.asm:loc_8c044dce`; `MARVELOUS2-RE-HANDOFF.md §3`, `GFX-NOTES §7` |
| Scale constants `CpsXScale/CpsYScale` | `memory/work.asm:44-45` |
| Mirror reads `mem_b[]` / `vram[]`; gamestate `addrspace::read*` | `core/network/maplecast_mirror.cpp:529`; `maplecast_gamestate.cpp` |
| GSTA wire size 261 B/frame | `core/network/maplecast_gamestate.h:80` |
