# Pack the 25 decoded Effect Poly sprites (efx_NNN.png, produced by decode_effects.py)
# into the fx atlas the client's loadFxAtlas() consumes:
#     <charBase>/../effects/fx_atlas.json  +  fx_atlas.png
# (sprite-client.mjs:699-722 — note the file stem is **fx_atlas**, not "effects").
#
# SCHEMA (matches bake.mjs downloadCharAtlases + loadFxAtlas):
#   { screenW:640, screenH:480, name:"effects", image:"fx_atlas.png",
#     sprites: { "<key>": {x,y,w,h, dx,dy, wG,hG, facing} },
#     effects: [ {idx, key, w, h, fmt, e8, dirEntry, effect_key_guess} ],   # metadata
#     keymap:  { ... binding doc ... } }
#
# buildDrawList() resolves an effect object via  c.sprites[o.sid]  (o.sid = the OBJS
# sprite_id, 0x8000 hflip stripped). It does NOT yet key on effect_key. So we key the
# sprites map by the DIRECTORY INDEX (0..24) AND, when the binding can be derived,
# also alias each rect under the candidate effect_key — the client/parent can pick.
#
# ANCHOR: shared radial effects (hitsparks/flashes) have no body-foot anchor; the
# client draws effect objects at the object's OWN node origin (o.x/o.y) + own-origin
# offset (o.hotDx/hotDy, else sp.dx/sp.dy). We bake a CENTERED own-origin offset
# (dx=-wG/2, dy=-hG/2) so an effect with no hotspot lands centered on its node — the
# correct default for a radial burst. facing=0 (effects are symmetric / use o.xflip).
import json, os
from PIL import Image
import importlib.util

D = "_effects_capture"
OUT = "_effects_capture/atlas"        # tools-only output dir (ROM-derived, gitignored)

# Pull the directory metadata (idx,w,h,fmt,e8) straight from the decoder so we stay
# in lockstep with the decode (single source of truth for the entry list).
spec = importlib.util.spec_from_file_location("decode_effects", "tools/decode_effects.py")
de = importlib.util.module_from_spec(spec); spec.loader.exec_module(de)

# dirBase (from mc_effects.log header) — entry i lives at dirBase + i*0x10.
DIR_BASE = 0x0CED03D8

def main():
    os.makedirs(OUT, exist_ok=True)
    entries = de.parse_dir()          # [(idx,w,h,e4,e8), ...]
    imgs = []
    for idx, w, h, e4, e8 in entries:
        p = f"{D}/efx_{idx:03d}.png"
        if not os.path.exists(p):
            continue
        imgs.append((idx, w, h, e4 & 0xff, e8, Image.open(p).convert("RGBA")))

    # Pack into a grid (row-packed by descending height keeps it tight enough; these
    # are only 25 tiles, no need for a real bin-packer).
    pad = 2
    cols = 5
    rows = (len(imgs) + cols - 1) // cols
    cw = max(im.width for *_, im in imgs) + pad
    ch = max(im.height for *_, im in imgs) + pad
    atlas_img = Image.new("RGBA", (cols * cw, rows * ch), (0, 0, 0, 0))

    sprites = {}
    effects = []
    for k, (idx, w, h, fmt, e8, im) in enumerate(imgs):
        col, row = k % cols, k // cols
        px, py = col * cw, row * ch
        atlas_img.paste(im, (px, py))
        rect = {
            "x": px, "y": py, "w": w, "h": h,
            "dx": round(-w / 2.0, 2), "dy": round(-h / 2.0, 2),   # centered own-origin
            "wG": float(w), "hG": float(h), "facing": 0,
        }
        # Primary key = directory index (what the parent can map o.sid->idx to).
        sprites[str(idx)] = rect
        # Candidate effect_key binding: IF node+0x15c points at the directory ENTRY,
        # then effect_key = (DIR_BASE + idx*0x10) & 0xffff. Alias the rect under it too
        # so that, if the client switches to keying on effect_key, the lookup resolves.
        ek_guess = (DIR_BASE + idx * 0x10) & 0xFFFF
        sprites[str(ek_guess)] = rect
        effects.append({
            "idx": idx, "w": w, "h": h, "fmt": fmt,
            "e8": f"0x{e8:08x}",
            "dirEntry": f"0x{(DIR_BASE + idx*0x10):08x}",
            "effect_key_guess": ek_guess,
            "fmt_name": {0: "ARGB1555", 1: "RGB565", 2: "ARGB4444"}[fmt],
        })

    atlas = {
        "screenW": 640, "screenH": 480, "name": "effects",
        "image": "fx_atlas.png",
        "sprites": sprites,
        "effects": effects,
        "keymap": {
            "note": ("sprites keyed by BOTH directory idx (0..24) AND the candidate "
                     "effect_key = (dirBase + idx*0x10)&0xffff. dirBase=0x0CED03D8 was "
                     "READ from mc_effects.log this capture, but is DYNAMIC (it is "
                     "*(0x0CED0008), re-laid-out per effect-bank load) and UNVERIFIED as "
                     "the actual node+0x15c target. The client currently looks up "
                     "c.sprites[o.sid]; o.sid (node+0x144) is NOT proven to equal the "
                     "directory idx. BINDING IS NOT DERIVABLE FROM THIS CAPTURE ALONE."),
            "dirBase": "0x0CED03D8",
            "effect_key_formula": "(0x0CED03D8 + idx*0x10) & 0xFFFF",
            "capture_needed": ("Log, for each live effect node: node+0x15c (effect_key), "
                               "node+0x144 (sprite_id/o.sid), and resolve which directory "
                               "entry (by e8 OR by entry address) it selects — i.e. add to "
                               "effectsDump a per-node pass that prints {sprite_id, gfxBase "
                               "node+0x15c, matched dir idx}. That row IS the binding table."),
        },
    }
    atlas_img.save(f"{OUT}/fx_atlas.png")
    json.dump(atlas, open(f"{OUT}/fx_atlas.json", "w"), indent=1)
    print(f"packed {len(imgs)} effects -> {OUT}/fx_atlas.png ({atlas_img.size}), "
          f"{OUT}/fx_atlas.json ({len(sprites)} sprite keys: idx + effect_key aliases)")

if __name__ == "__main__":
    main()
