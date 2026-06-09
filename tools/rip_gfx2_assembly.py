#!/usr/bin/env python3
"""
rip_gfx2_assembly.py — CRACKED-structure offline assembly extractor (per character).

Replaces the old EXTRAS-based grouping (the "6 stacked poses / scramble") with the
GFX2 cell table indexed by sprite_id, per the 2026-06-08/09 cell-data RE:

  current-pose part list = GFX2[sprite_id & 0x7FFF]   (GFX2 = *(node+0x160))

  cell_rec = GFX2_base + *(u32)(GFX2_base + (sid & 0x7FFF)*4)
    first u16 of cell_rec = group/part COUNT
    then COUNT * 8-byte records:
       [dx s16 @+0] [dy s16 @+2] [palette u16 @+4] [GFX-selector u16 @+6]
    (the OLD emitter read the selector from +4 = the constant palette; the REAL
     texture selector is +6.)

  part texture for a selector:
    gfx1 = *(node+0x15c)  (= GFX_DATA_00.BIN)
    blob = gfx1 + *(u32)(gfx1 + selector*4)
    w = blob[2]<<3, h = blob[3]<<3 ; texels at blob+4, LZSS-decoded (loc_8c0354c0),
    4bpp / PAL4.
    palette row = (rec[+4] & 0x3ff) >> 4

GFX2 = the gfx2 segment (header slot +0x04 = GFX_DATA_01.BIN).
GFX1 = the gfx1 segment (header slot +0x00 = GFX_DATA_00.BIN).

Output (operator-local, ROM-derived, gitignored):
  <out>/PL{hex}_asm.json    { char, atlas, atlas_w, atlas_h, parts, assemblies, _note }
                              assemblies keyed by CELL INDEX == sprite_id (full table).
                              each rec = {dx, dy, part(=+6 selector), pal(row), flip:0}
  <out>/PL{hex}_parts.json  { <selector>: {x,y,w,h} }   (rect in the atlas)
  <out>/PL{hex}_parts.png   packed part rectangles (only selectors actually referenced)

Usage:
  python3 tools/rip_gfx2_assembly.py --gfx1 dasm_PLDAT/Output/PL00_DAT/PL00_DAT_GFX_DATA_00.BIN \
                                     --gfx2 dasm_PLDAT/Output/PL00_DAT/PL00_DAT_GFX_DATA_01.BIN \
                                     --pal  dasm_PLDAT/Output/PL00_DAT/PL00_DAT_PALETTE_DATA.BIN \
                                     --char PL00 --out web/test-atlas/chars
"""
import argparse, json, os, struct, bisect
from PIL import Image

TILE = 8
OPERAND_MASK = 0x07ff
MAGENTA = (0xFF, 0x00, 0xFF)   # MAPLECAST_PARTDUMP PPM transparent key (P6 has no alpha)


# ----- REAL part pixels from a live MAPLECAST_PARTDUMP capture ----------------
# The offline GFX1 LZSS decode (build_part_pixels) is a CONFIRMED DEAD END: the
# texture LZSS back-references the live 0x0CE60000 scratch residue, absent from the
# static GFX_DATA_00 file (see web/webgpu/pldat-codec.mjs "DECOMPRESSION ARCHITECTURE").
# The ONLY correct pixel source is the running emulator's decode buffer, captured by
# the MAPLECAST_PARTDUMP probe (core/network/maplecast_gamestate.cpp) as PPM previews
# keyed by the +6 GFX-selector, with the live per-part palette already baked in. This
# loader sources those PPMs so the emitter geometry (cumulative pen + selector->rect,
# both disasm-confirmed) is paired with REAL pixels. Returns { sel: (rgba, W, H) }.
def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        return None
    idx = 2
    toks = []
    while len(toks) < 3:
        while idx < len(data) and data[idx] in b" \t\n\r":
            idx += 1
        if idx < len(data) and data[idx:idx + 1] == b"#":
            while idx < len(data) and data[idx] not in b"\n":
                idx += 1
            continue
        st = idx
        while idx < len(data) and data[idx] not in b" \t\n\r":
            idx += 1
        toks.append(int(data[st:idx]))
    idx += 1
    w, h, _mx = toks
    return w, h, data[idx:idx + w * h * 3]


def load_real_parts(realdir, hexname, selectors):
    parts = {}
    miss = []
    for sel in selectors:
        ppm = os.path.join(realdir, f"{hexname}_part_{sel:03d}.ppm")
        if not os.path.exists(ppm):
            miss.append(sel)
            continue
        got = read_ppm(ppm)
        if not got:
            miss.append(sel)
            continue
        w, h, px = got
        rgba = bytearray(w * h * 4)
        for i in range(w * h):
            r, g, b = px[i * 3], px[i * 3 + 1], px[i * 3 + 2]
            if (r, g, b) == MAGENTA:
                continue  # transparent
            rgba[i * 4 + 0] = r
            rgba[i * 4 + 1] = g
            rgba[i * 4 + 2] = b
            rgba[i * 4 + 3] = 255
        parts[sel] = (bytes(rgba), w, h)
    return parts, miss


# ----- GFX2 cell table (the cracked assembly structure) ---------------------
def read_cells(gfx2):
    """Return { cell_index: [ {dx,dy,pal_raw,sel}, ... ] } for every valid cell."""
    n = struct.unpack_from("<I", gfx2, 0)[0] >> 2
    tbl = [struct.unpack_from("<I", gfx2, i * 4)[0] for i in range(n)]
    cells = {}
    for idx in range(n):
        off = tbl[idx]
        if off + 2 > len(gfx2):
            continue
        cnt = struct.unpack_from("<H", gfx2, off)[0]
        if cnt == 0 or cnt > 64 or off + 2 + cnt * 8 > len(gfx2):
            continue
        recs = []
        p = off + 2
        # CUMULATIVE RUNNING PEN (bank03.asm loc_8c0344d4 / loc_8c0345c4):
        # the geometry emitter does NOT place each record absolutely from the
        # object origin. It keeps a running pen (r10 = X acc, @(0x14,r15) = Y acc)
        # initialized to the cell hotspot (node+0x134/0x136, ~0 for the body) and
        # advances it by each record's (dx,dy) BEFORE emitting that record's part:
        #     X_acc += dx ;  Y_acc += dy   (facing-neutral; global flip is applied
        #     downstream in the emitter via owner.facing).  Each part is then drawn
        #     at  screen_xy(node+0xE0/E4) + (X_acc,Y_acc)*scale.
        # Per-record +4 bit 0x10 = the part's own X-mirror (tile flip); we carry it
        # as `flip` so the emitter can XOR it with global facing.
        # See docs/MARVELOUS2-GFX-NOTES.md §3a (cumulative pen).
        px = py = 0
        for _ in range(cnt):
            dx, dy, pal, sel = struct.unpack_from("<hhHH", gfx2, p)
            p += 8
            px += dx
            py += dy
            recs.append({"dx": px, "dy": py, "ddx": dx, "ddy": dy,
                         "pal_raw": pal, "sel": sel})
        cells[idx] = recs
    return cells, n


# ----- GFX1 part LZSS decode (shared scratch window, loc_8c0354c0) ----------
def decode_into_capped(stream, out, cap):
    n = len(stream)

    def rd(p):
        if p + 1 < n:
            return stream[p] | (stream[p + 1] << 8)
        return stream[p] if p < n else 0

    start = len(out)
    pos = 0
    mask = 0
    flag = 0
    while pos + 1 < n and (len(out) - start) < cap:
        if mask == 0:
            flag = rd(pos)
            pos += 2
            mask = 0x8000
            continue
        bit = flag & mask
        mask >>= 1
        if bit == 0:
            out.append(stream[pos])
            out.append(stream[pos + 1])
            pos += 2
        else:
            tok = rd(pos)
            pos += 2
            count = tok >> 11
            operand = tok & OPERAND_MASK
            if count == 0:
                count = rd(pos)
                pos += 2
            if operand == 0:
                for _ in range(count):
                    if (len(out) - start) >= cap:
                        break
                    out.append(0)
                    out.append(0)
            else:
                src = len(out) - (operand << 1)
                for _ in range(count):
                    if (len(out) - start) >= cap:
                        break
                    if 0 <= src and src + 1 < len(out):
                        out.append(out[src])
                        out.append(out[src + 1])
                    else:
                        out.append(0)
                        out.append(0)
                    src += 2
    while (len(out) - start) < cap:
        out.append(0)


def build_part_pixels(gfx1, selectors):
    """Decode every referenced selector into the shared scratch (file order), then
    de-twiddle each into a 4bpp index image. Returns { sel: (idx_bytes, w, h, palrow_hint) }."""
    n_parts = struct.unpack_from("<I", gfx1, 0)[0] >> 2
    offs = [struct.unpack_from("<I", gfx1, i * 4)[0] for i in range(n_parts)]
    srt = sorted(set(offs) | {len(gfx1)})

    def blob_end(off):
        i = bisect.bisect_right(srt, off)
        return srt[i] if i < len(srt) else len(gfx1)

    # Decode ALL parts in file order into one growing buffer so back-refs resolve
    # against earlier parts' decoded output (the accumulated scratch window).
    order = sorted(range(n_parts), key=lambda i: offs[i])
    buf = bytearray()
    starts = {}
    dims = {}
    for idx in order:
        o = offs[idx]
        if o + 4 > len(gfx1):
            starts[idx] = len(buf)
            continue
        sw, sh = gfx1[o + 2], gfx1[o + 3]
        lw, lh = gfx1[o], gfx1[o + 1]
        cap = (sw * TILE * sh * TILE) >> 1
        if cap <= 0 or cap > (1024 * 1024) // 2:
            starts[idx] = len(buf)
            dims[idx] = (sw, sh, lw, lh)
            continue
        starts[idx] = len(buf)
        dims[idx] = (sw, sh, lw, lh)
        decode_into_capped(gfx1[o + 4:blob_end(o)], buf, cap)

    parts = {}
    for sel in selectors:
        if sel >= n_parts or sel not in dims:
            continue
        sw, sh, lw, lh = dims[sel]
        W, H = sw * TILE, sh * TILE
        if W <= 0 or H <= 0 or W > 1024 or H > 1024:
            continue
        need = (W * H) >> 1
        st = starts[sel]
        texels = bytes(buf[st:st + need])
        if len(texels) < need:
            texels = texels + bytes(need - len(texels))
        idxbuf = tile_to_indices(texels, sw, sh)
        cw = (lw * TILE) if 0 < lw <= sw else W
        ch = (lh * TILE) if 0 < lh <= sh else H
        parts[sel] = (idxbuf, W, H, cw, ch)
    return parts


def tile_to_indices(texels, sw, sh):
    """8x8 storage-row-major 4bpp -> linear index buffer (W*H bytes)."""
    W = sw * TILE
    H = sh * TILE
    idx = bytearray(W * H)
    for ty in range(sh):
        for tx in range(sw):
            base = (ty * sw + tx) * 64
            for py in range(TILE):
                row = base + py * TILE
                ob = (ty * TILE + py) * W + tx * TILE
                for px in range(TILE):
                    t = row + px
                    b = texels[t >> 1] if (t >> 1) < len(texels) else 0
                    idx[ob + px] = (b >> 4) & 0xf if (t & 1) else b & 0xf
    return idx


# ----- palette --------------------------------------------------------------
def palette_rgba(pal, bank):
    out = []
    base = bank * 32
    for i in range(16):
        if base + i * 2 + 1 >= len(pal):
            out.append((0, 0, 0, 0))
            continue
        v = pal[base + i * 2] | (pal[base + i * 2 + 1] << 8)
        a = (v >> 12) & 0xf
        r = (v >> 8) & 0xf
        g = (v >> 4) & 0xf
        b = v & 0xf
        out.append((0, 0, 0, 0) if i == 0 else (r * 17, g * 17, b * 17, (a * 17) if a else 255))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gfx1", required=True)
    ap.add_argument("--gfx2", required=True)
    ap.add_argument("--pal", required=True)
    ap.add_argument("--char", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--palbank", type=int, default=0, help="palette bank for the atlas PNG (def 0 = PL00 body bank; was 1)")
    ap.add_argument("--realparts", default=None,
                    help="dir of MAPLECAST_PARTDUMP PPMs (PLxx_part_NNN.ppm, keyed by +6 selector). "
                         "When set, part PIXELS come from the live emulator decode (the ONLY correct "
                         "source) instead of the dead offline LZSS. Geometry/selectors are unchanged.")
    args = ap.parse_args()

    gfx1 = open(args.gfx1, "rb").read()
    gfx2 = open(args.gfx2, "rb").read()
    pal = open(args.pal, "rb").read()

    hexname = args.char.upper()
    cells, ncells = read_cells(gfx2)
    selectors = sorted({r["sel"] for recs in cells.values() for r in recs})

    ATLAS_W = 1024
    rects = {}

    if args.realparts:
        # REAL pixels from the live decode (correct). Pack RGBA PPMs by selector.
        real, miss = load_real_parts(args.realparts, hexname, selectors)
        print(f"realparts: {len(real)}/{len(selectors)} selectors resolved from "
              f"{args.realparts} ; missing {len(miss)}" + (f" e.g. {miss[:12]}" if miss else ""))
        items = sorted(real.items(), key=lambda kv: -kv[1][2])  # tallest first
        x = y = rowh = 0
        placed = []
        for sel, (rgba, W, H) in items:
            if x + W > ATLAS_W:
                x = 0
                y += rowh
                rowh = 0
            rects[sel] = {"x": x, "y": y, "w": W, "h": H}
            placed.append((sel, rgba, W, H, x, y))
            x += W + 1
            rowh = max(rowh, H + 1)
        atlas_h = y + rowh
        atlas = Image.new("RGBA", (ATLAS_W, max(atlas_h, 1)), (0, 0, 0, 0))
        for sel, rgba, W, H, ax, ay in placed:
            atlas.paste(Image.frombytes("RGBA", (W, H), rgba), (ax, ay))
    else:
        # OFFLINE LZSS decode — CONFIRMED DEAD (scratch-residue back-refs). Kept for the
        # GEOMETRY-only regen (assemblies/selectors are correct); pixels will be noise.
        parts_px = build_part_pixels(gfx1, selectors)
        palrgba = palette_rgba(pal, args.palbank)
        items = sorted(parts_px.items(), key=lambda kv: -kv[1][2])  # tallest first
        x = y = rowh = 0
        placed = []
        for sel, (idxbuf, W, H, cw, ch) in items:
            if x + cw > ATLAS_W:
                x = 0
                y += rowh
                rowh = 0
            rects[sel] = {"x": x, "y": y, "w": cw, "h": ch}
            placed.append((sel, idxbuf, W, H, cw, ch, x, y))
            x += cw + 1
            rowh = max(rowh, ch + 1)
        atlas_h = y + rowh
        atlas = Image.new("RGBA", (ATLAS_W, max(atlas_h, 1)), (0, 0, 0, 0))
        apx = atlas.load()
        for sel, idxbuf, W, H, cw, ch, ax, ay in placed:
            for yy in range(ch):
                row = yy * W
                for xx in range(cw):
                    pi = idxbuf[row + xx]
                    if pi == 0:
                        continue
                    apx[ax + xx, ay + yy] = palrgba[pi]

    os.makedirs(args.out, exist_ok=True)
    png_path = os.path.join(args.out, f"{hexname}_parts.png")
    atlas.save(png_path)

    # Assembly table keyed by cell index == sprite_id. pal row = (pal_raw & 0x3ff)>>4.
    assemblies = {}
    for idx, recs in cells.items():
        out_recs = []
        for r in recs:
            if r["sel"] not in rects:
                continue
            out_recs.append({
                "dx": r["dx"], "dy": r["dy"],          # CUMULATIVE pen (absolute in cell space)
                "part": r["sel"],
                "pal": (r["pal_raw"] & 0x3ff) >> 4,
                "flip": 1 if (r["pal_raw"] & 0x10) else 0,   # +4 bit 0x10 = part X-mirror
            })
        if out_recs:
            assemblies[str(idx)] = out_recs

    parts_json = {str(sel): rect for sel, rect in rects.items()}
    asm_json = {
        "char": hexname,
        "atlas": f"{hexname}_parts.png",
        "atlas_w": ATLAS_W, "atlas_h": max(atlas_h, 1),
        "screenW": 640, "screenH": 480,
        "parts": parts_json,
        "assemblies": assemblies,
        "_note": "CRACKED GFX2 cell table: assemblies keyed by cell-index==sprite_id; "
                 "rec part=+6 selector (GFX1 offset-table idx), pal=(rec+4 & 0x3ff)>>4, "
                 "flip=(rec+4 & 0x10). dx/dy are CUMULATIVE (running pen): the geometry "
                 "emitter loc_8c0344d4/loc_8c0345c4 advances a pen by each record's raw "
                 "(dx,dy) and draws the part at screen_xy+(pen)*scale, so the JSON stores "
                 "the accumulated absolute pen per record. rip_gfx2_assembly.py.",
    }
    with open(os.path.join(args.out, f"{hexname}_asm.json"), "w") as f:
        json.dump(asm_json, f)
    with open(os.path.join(args.out, f"{hexname}_parts.json"), "w") as f:
        json.dump({"parts": parts_json}, f)

    print(f"cells: {ncells}  with-parts: {len(assemblies)}")
    print(f"selectors referenced: {len(selectors)}  packed: {len(rects)}")
    print(f"atlas: {ATLAS_W}x{max(atlas_h,1)} -> {png_path}")
    import collections
    cc = collections.Counter(len(v) for v in assemblies.values())
    print("assembly record-count dist:", dict(sorted(cc.items())))


if __name__ == "__main__":
    main()
