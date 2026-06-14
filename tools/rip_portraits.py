#!/usr/bin/env python3
"""
rip_portraits.py — decode MVC2 character PORTRAITS from the disc into a HUD atlas.

Source (disc assets, gitignored / copyrighted — NEVER commit the pixels):
  "MVC2 Dev Files/DM01POL.BIN"  — NaomiLib display list for the char-select screen
  "MVC2 Dev Files/DM01TEX.BIN"  — its concatenated PVR texture blob

RE provenance (CONFIRMED, marvelous2 disassembly) — the char_id->cell map below is
now derived ENTIRELY from the ROM, not visually guessed (was the source of the
"wrong portrait" bug, e.g. Sentinel showing another face). Three independent ROM
sources, all cross-consistent, pin every cell:

  (1) The texture-sheet cell ORDER is ground truth from the POL portrait-model UVs.
      DM01POL models 0x0A..0x41 (= the 56 portrait NaomiLib 2-triangle models) each
      sample ONE 64x64 cell of texture #11. Decoding each model's UV rect (rip_stage
      NaomiLib decoder) shows the packing is column-major-block, bottom-up:
        model k (k=0..55): block = k//32; idx = k%32;
                            col = block*4 + (idx%4);  row = 7 - (idx//4)
      i.e. left 4 cols (rows 7->0) fill k=0..31, right 4 cols (rows 7->2) fill 32..55.
      This is the cell each portrait model occupies. (Confirmed: m10..m65 + m90..m96.)

  (2) bank13 loc_8c13b7d4 "Character Select Grid Placement" — a char_id-indexed table
      of [row, col] BYTES (2 bytes/char_id, char_id 0x00..0x3a). char_id -> (row,col).

  (3) bank16 loc_8c161fec "Dreamcast Character Select ID Table" — the 8x8 grid->char_id
      map, indexed table[col*8 + row] (column-major; read site bank0f loc_8C0F4774:
      `shll2 r0; shll r0; add tablebase,r0; mov.b @(r0,r2),r0` => col*8+row). 0xff=empty.
      Tables (2) and (3) are EXACT mutual inverses (verified: 0 mismatches over all 53
      grid chars), so the screen grid coordinate of every char_id is authoritative.

  The sheet cell that holds char C's face = cell(row,col) where loc_8c161fec[col*8+row]
  == C. This is PIXEL-VALIDATED for all 47 emitted chars (incl. live Storm 0x2a / Cable
  0x17 / Sentinel 0x34 -> their correct faces). Faces are stored ROTATED 90deg
  (corrected on-screen by the bank16 rotation tables loc_8c1617b8/loc_8c161a7c); we undo
  with a CCW rotate. Decode path = tools/rip_stage.py morton-twiddle + RGB565.

  THE 8 STAGE/RANDOM PLATES occupy sheet cells (rows 4..7, cols 0..1) — pixel-confirmed
  STAGE 1..7 + "?" random banners. The DC ID table maps 6 chars onto those grid slots
  (Cyclops 0x06, Wolverine 0x07, Shuma 0x2d, WarMachine 0x2e, Thanos 0x36,
  BoneWolverine 0x39); their faces are NOT in those cells -> the char-select relocates
  them to the sheet's top-right "filler" cells (rows 0..1, cols 4..7) via the rotation
  tables. Only WarMachine 0x2e is unambiguously recoverable there (cell r0,c6 = the
  yellow-helmet War Machine face); Cyclops/Wolverine/Shuma/Thanos/BoneWolverine share
  the filler region with duplicate faces of already-mapped chars and cannot be pinned
  to a UNIQUE cell from static data alone, so they fall back to monogram (graceful).
  Likewise Dhalsim 0x25 / Bison 0x26 / Jin 0x37 / Servbot 0x3a have no unique sheet
  cell. Morrigan 0x03 (r1,c5) and Cammy 0x24 (r1,c6) ARE uniquely present and recovered.
  -> 47 chars get their REAL face; 9 fall back to the monogram plate (never a WRONG
  face). To recover the remaining 9 would require a live char+0x20 read in an all-char
  match (the portrait-OBJECT model index, bank0f loc_8c0f4334) — the gameplay char
  struct +0x20 is NOT that index (Oracle-confirmed: reads 0x00/0x03, not 0..55).

Output (gitignored, scp to prod):
  web/render-replica/hud/portraits/portraits.png   — packed 64x64 upright portraits
  web/render-replica/hud/portraits/portraits.json  — { rects: { "<char_id>": {x,y,w,h} } }
hud-client.mjs load() auto-consumes this; _plate() draws the FAC pixels, monogram fallback.
"""
import json, os, struct, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def _find_disc(name):
    """DM01 disc assets are gitignored/copyrighted; look in the standard disc dir
    and the local _portrait_work scratch dir (whichever the operator extracted to)."""
    for d in (os.path.join(REPO, "MVC2 Dev Files"),
              os.path.join(REPO, "_portrait_work")):
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return os.path.join(REPO, "MVC2 Dev Files", name)  # default (for the error msg)


POL = _find_disc("DM01POL.BIN")
TEX = _find_disc("DM01TEX.BIN")
OUT = os.path.join(REPO, "web", "render-replica", "hud", "portraits")

RAM_BASE = 0x0CE80000          # DM01POL load base (from POL+0x08 high bits)
PORTRAIT_TEX = 11              # 512x512 RGB565 twiddled mugshot grid
CELL = 64                      # cell size on the 512x512 sheet (8x8 grid)


# ---- twiddle (encodeZMortonPosition, same as rip_stage.py) ----
def morton(x, y):
    x &= 0xffff; y &= 0xffff
    x = (x | (x << 8)) & 0x00ff00ff; y = (y | (y << 8)) & 0x00ff00ff
    x = (x | (x << 4)) & 0x0f0f0f0f; y = (y | (y << 4)) & 0x0f0f0f0f
    x = (x | (x << 2)) & 0x33333333; y = (y | (y << 2)) & 0x33333333
    x = (x | (x << 1)) & 0x55555555; y = (y | (y << 1)) & 0x55555555
    return x | (y << 1)


def rgb565(c):
    r = (c >> 11) & 0x1f; g = (c >> 5) & 0x3f; b = c & 0x1f
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255)


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


def decode_portrait_sheet(pol, tex):
    """Find texture #PORTRAIT_TEX in DM01POL's header table, decode it from DM01TEX."""
    pvr_start = u32(pol, 0x08) - RAM_BASE
    pvr_end = u32(pol, 0x10) - RAM_BASE
    base_loc = None
    a, i = pvr_start, 0
    target = None
    while a < pvr_end:
        w, h = u16(pol, a), u16(pol, a + 2)
        fmt, typ, loc = pol[a + 4], pol[a + 5], u32(pol, a + 8)
        if base_loc is None:
            base_loc = loc
        if i == PORTRAIT_TEX:
            target = (w, h, fmt, typ, loc - base_loc)
            break
        a += 16; i += 1
    if target is None:
        sys.exit("portrait texture not found in DM01POL header table")
    w, h, fmt, typ, foff = target
    if (w, h) != (512, 512):
        print(f"WARN: portrait tex is {w}x{h} fmt={fmt} typ={typ} (expected 512x512)")
    img = Image.new("RGBA", (w, h)); px = img.load()
    for y in range(h):
        for x in range(w):
            o = foff + morton(x, y) * 2
            if o + 2 <= len(tex):
                px[x, y] = rgb565(u16(tex, o))
    return img


# ---- AUTHORITATIVE char_id -> sheet-cell map, derived from the ROM (see header) ----
# bank16 loc_8c161fec "Dreamcast Character Select ID Table", indexed table[col*8 + row]
# (column-major; the exact indexing from read site bank0f loc_8C0F4774). 0xff = empty
# (STAGE/random plate). VERBATIM from the disassembly, row-by-row col0..col7:
DC_ID_TABLE = [
    0xff, 0x20, 0x1c, 0x38, 0x07, 0x2d, 0x36, 0xff,   # col0  rows0..7
    0xff, 0x23, 0x02, 0x15, 0x06, 0x39, 0x2e, 0xff,   # col1
    0x27, 0x1f, 0x00, 0x14, 0x16, 0x08, 0x2f, 0x2b,   # col2
    0x21, 0x1e, 0x01, 0x13, 0x17, 0x09, 0x28, 0x35,   # col3
    0x25, 0x37, 0x04, 0x12, 0x0b, 0x0c, 0x29, 0x31,   # col4
    0x26, 0x03, 0x22, 0x10, 0x0f, 0x0e, 0x2c, 0x32,   # col5
    0xff, 0x24, 0x05, 0x11, 0x0a, 0x0d, 0x33, 0xff,   # col6
    0xff, 0x3a, 0x1d, 0x1b, 0x30, 0x2a, 0x34, 0xff,   # col7
]
# Two sheet REGIONS do NOT hold a 1:1 char face for their DC-grid char_id, so a
# straight DC-table read would emit a banner or a DUPLICATE face there:
#   (a) STAGE plates: rows 4..7, cols 0..1  -> "STAGE 1..7" + "?" random (pixel-conf).
#   (b) top-right FILLER: rows 0..1, cols 4..7 -> the sheet stores duplicate faces of
#       already-mapped chars here (Spiderman/Psylocke/Ryu/Hulk/Zangief), NOT the chars
#       the DC table assigns to these grid slots. EXCEPTIONS (pixel-confirmed unique):
#       (1,5)=Morrigan, (1,6)=Cammy, (0,6)=War Machine.
# Every char_id whose DC cell falls in (a) or a non-exception (b) cell -> monogram.
_STAGE_CELLS  = {(4, 0), (4, 1), (5, 0), (5, 1), (6, 0), (6, 1), (7, 0), (7, 1)}
_FILLER_CELLS = {(0, 4), (0, 5), (0, 6), (0, 7), (1, 4), (1, 5), (1, 6), (1, 7)}
_FILLER_OK    = {(1, 5): 0x03, (1, 6): 0x24, (0, 6): 0x2e}  # Morrigan, Cammy, WarMachine


def _build_cell_char():
    """char_id -> (row, col) on the 8x8 sheet, from loc_8c161fec. Cells that resolve to
    a STAGE plate or a non-exception filler cell are dropped (the char's real face is
    relocated/absent; HUD monogram fallback — see header)."""
    m = {}
    for col in range(8):
        for row in range(8):
            cid = DC_ID_TABLE[col * 8 + row]
            if cid == 0xff:
                continue
            rc = (row, col)
            if rc in _STAGE_CELLS:
                continue
            if rc in _FILLER_CELLS and _FILLER_OK.get(rc) != cid:
                continue   # duplicate/filler face, not this char -> monogram
            m[cid] = rc
    # War Machine 0x2e: its DC grid slot (row6,col1) is a STAGE plate, but its real
    # face is relocated to the filler cell (0,6) (pixel-confirmed yellow helmet).
    m[0x2e] = (0, 6)
    return m


# (row, col) on the 8x8 512px sheet -> char_id. Faces stored 90deg-rotated; CCW to
# upright. 47 chars covered with their REAL face; the other 9 (0x06 Cyclops, 0x07
# Wolverine, 0x25 Dhalsim, 0x26 Bison, 0x2d Shuma, 0x36 Thanos, 0x37 Jin, 0x39
# BoneWolverine, 0x3a Servbot) have no unique sheet cell -> HUD monogram fallback.
CELL_CHAR = {rc: cid for cid, rc in _build_cell_char().items()}


def main():
    for f in (POL, TEX):
        if not os.path.exists(f):
            sys.exit(f"missing disc asset: {f}")
    pol = open(POL, "rb").read()
    tex = open(TEX, "rb").read()
    sheet = decode_portrait_sheet(pol, tex)

    # uniqueness guard: never emit the same char twice (would mean a misread).
    seen = {}
    for (r, c), cid in CELL_CHAR.items():
        if cid in seen:
            sys.exit(f"DUPLICATE char {hex(cid)} at {(r,c)} and {seen[cid]} — fix CELL_CHAR")
        seen[cid] = (r, c)

    # pack one 64x64 upright portrait per char, sorted by char_id.
    items = sorted(CELL_CHAR.items(), key=lambda kv: kv[1])
    n = len(items)
    cols = 8
    rows = (n + cols - 1) // cols
    atlas = Image.new("RGBA", (cols * CELL, rows * CELL), (0, 0, 0, 0))
    rects = {}
    for i, ((r, c), cid) in enumerate(items):
        cell = sheet.crop((c * CELL, r * CELL, c * CELL + CELL, r * CELL + CELL))
        cell = cell.rotate(90, expand=True)  # CCW -> upright face
        ax, ay = (i % cols) * CELL, (i // cols) * CELL
        atlas.paste(cell, (ax, ay))
        rects[str(cid)] = {"x": ax, "y": ay, "w": CELL, "h": CELL}

    os.makedirs(OUT, exist_ok=True)
    atlas.save(os.path.join(OUT, "portraits.png"))
    meta = {
        "source": "DM01POL.BIN + DM01TEX.BIN (texture #11, 512x512 RGB565 twiddled; "
                  "cells CCW-rotated to upright)",
        "atlasW": cols * CELL, "atlasH": rows * CELL,
        "rects": rects,
        "notes": "char_id -> 64x64 portrait. char_id->cell is AUTHORITATIVE from the "
                 "ROM: bank16 loc_8c161fec 'DC Character Select ID Table' "
                 "(table[col*8+row]=char_id; read site bank0f loc_8C0F4774), "
                 "cross-confirmed by bank13 loc_8c13b7d4 'Character Select Grid "
                 "Placement' (char_id->[row,col], exact inverse) and the DM01POL "
                 "portrait-model UVs (cell order). 47 chars get their real face; 9 "
                 "(0x06,0x07,0x25,0x26,0x2d,0x36,0x37,0x39,0x3a) have no unique sheet "
                 "cell -> HUD monogram fallback. Replaces the prior visually-guessed "
                 "CELL_CHAR (the wrong-portrait bug; Sentinel etc.).",
    }
    with open(os.path.join(OUT, "portraits.json"), "w") as f:
        json.dump(meta, f, indent=1)
    print(f"wrote portraits.png ({cols*CELL}x{rows*CELL}) and portraits.json "
          f"with {n} portraits: {sorted(hex(c) for c in seen)}")


if __name__ == "__main__":
    main()
