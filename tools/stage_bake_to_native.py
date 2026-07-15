#!/usr/bin/env python3
"""Convert a STGxx_ta.json stage bake into the native client's .mcstg binary.

Faithful port of web/webgpu/stage-client.mjs _buildFromTA rules (all cited):
  - per-vertex RGB comes from the bake's `rgb` (the type-correct base colour;
    the legacy `i` intensity is UNUSED — re_kb/26 deck-colour fix),
  - world-authored discriminator: a mesh re-projects through the live camera
    iff its world minZ < -500 OR X extent > 1000; local-space props (never
    captured placement matrix) render their baked SCREEN pos verbatim,
  - sanity cull: drop a triangle if any FINAL vertex is non-finite or
    |x|/|y| > 100000 (drops the 6 ±14.5M unplaced-prop garbage tris; keeps
    floor/deck/dome — the 2026-07-11 floor fix),
  - listType 0 -> opaque, 2 -> translucent; engine pcw/isp/tsp/tcw verbatim.

Strips: the bake stores independent triangles (expanded from strips). We
re-join consecutive triangles that follow the renderer's exact strip
triangulation rule (render.rs: even i -> (i,i+1,i+2), odd -> (i+1,i,i+2)),
then VERIFY by re-triangulating and asserting byte-identical vertex sequences.
Unmergeable tris become 3-vert strips.

Format (.mcstg, little-endian):
  'MCSG' u32 version=1 u32 meshCount
  per mesh: u32 pcw,isp,tsp,tcw  u8 listType  u8 worldAuthored  u16 pad
            u32 stripCount
    per strip: u32 vertCount, then vertCount x 24B:
            f32 x,y,z   (world if worldAuthored else baked screen sx,sy,invw)
            f32 u,v
            u8  r,g,b,a (a=255)

Usage: python stage_bake_to_native.py [in.json] [out.mcstg]
"""
import json, struct, sys

IN = sys.argv[1] if len(sys.argv) > 1 else \
    r"c:\Users\trist\projects\maplecast-flycast\atlas\stages\STG0B_ta.json"
OUT = sys.argv[2] if len(sys.argv) > 2 else \
    r"c:\Users\trist\projects\maplecast-flycast\atlas\stages\STG0B_ta.mcstg"

SANITY = 100000.0

def world_authored(mesh):
    minz = 1e18; minx = 1e18; maxx = -1e18
    for tri in mesh["tris"]:
        for v in tri:
            w = v.get("world")
            if not w:
                return False
            minz = min(minz, w[2]); minx = min(minx, w[0]); maxx = max(maxx, w[0])
    return minz < -500 or (maxx - minx) > 1000

def tri_ok(tri):
    for v in tri:
        x, y = v["pos"][0], v["pos"][1]
        if not (abs(x) <= SANITY and abs(y) <= SANITY and x == x and y == y):
            return False
    return True

def vkey(v, use_world):
    src = v["world"] if use_world else v["pos"]
    return (tuple(src), tuple(v["uv"]), tuple(v.get("rgb", [255, 255, 255])))

def rejoin_strips(tris, use_world):
    """Greedy strip re-join matching render.rs triangulation exactly."""
    strips = []
    cur = None            # list of vkeys
    for tri in tris:
        k = [vkey(v, use_world) for v in tri]
        if cur is not None:
            n = len(cur)              # next vert index would be n; tri index i = n-2
            i = n - 2
            exp = (cur[i], cur[i + 1]) if i % 2 == 0 else (cur[i + 1], cur[i])
            if (k[0], k[1]) == exp:
                cur.append(k[2]); continue
        cur = list(k)
        strips.append(cur)
    # VERIFY: re-triangulate with the renderer's rule; must equal input exactly
    out = []
    for s in strips:
        for i in range(len(s) - 2):
            out.append((s[i], s[i+1], s[i+2]) if i % 2 == 0 else (s[i+1], s[i], s[i+2]))
    ref = [tuple(vkey(v, use_world) for v in t) for t in tris]
    assert out == ref, "strip rejoin round-trip mismatch"
    return strips

SHADOW_TCW_ADDR = 0x8F700  # character drop-shadow texture: DYNAMIC content that
                           # was baked at capture positions (meshes 4/5 in STG0B,
                           # measured 2026-07-14) — live shadows ride the wire.

def main():
    d = json.load(open(IN))
    meshes_out = []
    kept = dropped = 0
    for m in d["meshes"]:
        if (m["tcw"] & 0x1FFFFF) == SHADOW_TCW_ADDR:
            print(f"  dropping baked-shadow mesh (tcw {m['tcw']:#x}, {len(m['tris'])} tris)")
            continue
        if m["listType"] != 0:
            # keep-rule v4: ALL dynamics (TR props, TIME/LEVEL backings, shadows)
            # ride the wire now — the bake carries ONLY the opaque stage geometry
            # the wire strips, avoiding baked-vs-live double-draw.
            print(f"  dropping TR mesh (tcw {m['tcw']:#x}, {len(m['tris'])} tris) — rides the wire")
            continue
        wa = world_authored(m)
        tris = [t for t in m["tris"] if tri_ok(t)]
        dropped += len(m["tris"]) - len(tris)
        kept += len(tris)
        if not tris:
            continue
        strips = rejoin_strips(tris, wa)
        meshes_out.append((m, wa, strips))
    buf = bytearray()
    buf += b"MCSG" + struct.pack("<II", 1, len(meshes_out))
    for m, wa, strips in meshes_out:
        buf += struct.pack("<IIII", m["pcw"] & 0xFFFFFFFF, m["isp"] & 0xFFFFFFFF,
                           m["tsp"] & 0xFFFFFFFF, m["tcw"] & 0xFFFFFFFF)
        buf += struct.pack("<BBH", m["listType"], 1 if wa else 0, 0)
        buf += struct.pack("<I", len(strips))
        for s in strips:
            buf += struct.pack("<I", len(s))
            for (pos, uv, rgb) in s:
                buf += struct.pack("<5f", pos[0], pos[1], pos[2], uv[0], uv[1])
                buf += struct.pack("<4B", int(rgb[0]) & 255, int(rgb[1]) & 255,
                                   int(rgb[2]) & 255, 255)
    open(OUT, "wb").write(buf)
    tstrips = sum(len(s) for _, _, s in meshes_out)
    tverts = sum(len(st) for _, _, s in meshes_out for st in s)
    print(f"{IN}\n -> {OUT}: {len(meshes_out)} meshes, {tstrips} strips, "
          f"{tverts} verts, tris kept {kept} / dropped {dropped}, {len(buf):,} B")
    print("world-authored:", [(1 if wa else 0) for _, wa, _ in meshes_out])

if __name__ == "__main__":
    main()
