#!/usr/bin/env python3
"""
decode_stage_pol.py — MapleCast MVC2 STAGE/BACKGROUND decoder (TIER 1, RENDER-MASTER-PLAN-V2 §2.2/T4)

Parses an MVC2 stage POL+TEX pair (Sega NinjaLibrary / libspr "NLOBJPUT" display
objects, pre-relocated to load base 0x0CEA0000) into:
  - a list of textured triangle meshes (verts in WORLD/pre-projected space, UVs,
    vertex colors, texture refs) -> per-stage JSON
  - the PVR TextureList textures, de-twiddled to RGBA8888 PNGs

This is a FAITHFUL PORT of ModNao's NaomiLib parser (vendored at
_vendor/modnao/src/utils/polygons/serialize/*.ts and .../workers/*.ts), which is
already confirmed to decode MVC2 stage POL/TEX. Offsets/sizes mirror
_vendor/modnao/src/constants/StructOffsets.ts + StructSizes.ts.

CONFIRMED (hexdump of all 17 STGxx + RENDER-MASTER-PLAN-V2 §2.2):
  POL header: +0x00 modelTablePtr (abs, &0xffffff00 = ramOffset=0x0CEA0000)
              +0x04 modelCount
              +0x08 pvrStart (abs) ; +0x10 pvrEnd (abs)  -> TextureList region
  TextureList rec (16B): +0x00 u16 width, +0x02 u16 height, +0x04 u8 colorFmt
              (0=ARGB1555,1=RGB565,2=ARGB4444,3=ARGB8888), +0x05 u8 type
              (1=plain-twiddled, 3=VQ), +0x08 u32 baseLocation (VRAM 0x0CC00000 fam)
  Model header 0x18 ; Mesh header 0x50 ; Polygon header 0x08
  Vertex A (direct) 0x20 ; Vertex B (reference) 0x08
  TEX file: RAW TWIDDLED texels, no GBIX/PVRT. realLocation = baseLocation -
            firstTextureBaseLocation. de-twiddle via Z-Morton (encodeZMortonPosition).

Usage:
  python3 tools/decode_stage_pol.py --pol "MVC2 Dev Files/STG00POL.BIN" \
      --tex "MVC2 Dev Files/STG00TEX.BIN" --out _stage_out/STG00 [--no-png]
  python3 tools/decode_stage_pol.py --pol .../STG00POL.BIN   # tex/out auto-derived

Output:
  <out>/STG00.json   meshes + textureList + header summary
  <out>/tex_NN.png   each decoded TextureList entry (unless --no-png)
"""

import argparse
import json
import os
import struct
import sys

RAM_BASE_MASK = 0xFFFFFF00

# StructSizes.ts
MODEL_HEADER = 0x18
MESH = 0x50
POLYGON_HEADER = 0x08
VERTEX_A = 0x20   # direct
VERTEX_B = 0x08   # reference

# StructOffsets.ts (subset we consume)
O_MODEL_POSITION = 0x08
O_MODEL_RADIUS = 0x14

O_MESH_BASE_PARAMS = 0x00
O_MESH_TEXTURE_INSTRUCTIONS = 0x04
O_MESH_TEXTURE_SIZE = 0x08
O_MESH_UV_FLIP = 0x0A
O_MESH_TEXTURE_CONTROL = 0x0C
O_MESH_TEXTURE_COLOR_FORMAT = 0x0F
O_MESH_POSITION = 0x10
O_MESH_TEXTURE_NUMBER = 0x20
O_MESH_VERTEX_COLOR_MODE = 0x24
O_MESH_ALPHA = 0x2C
O_MESH_COLOR = 0x30
O_MESH_SPECULAR_ALPHA = 0x3C
O_MESH_SPECULAR_COLOR = 0x40
O_MESH_VERTEX_DATA_LENGTH = 0x4C

O_POLY_VERTEX_GROUP_TYPE = 0x00
O_POLY_VERTEX_COUNT = 0x04

O_VTX_CONTENT_FLAG = 0x00
O_VTX_OFFSET_VAR = 0x04
O_VTX_POSITION = 0x00
O_VTX_UV = 0x18
O_VTX_NORMALS = 0x0C   # colored verts only
O_VTX_COLORS = 0x10    # colored verts only

O_TEX_WIDTH = 0x00
O_TEX_HEIGHT = 0x02
O_TEX_COLOR_FORMAT = 0x04
O_TEX_TYPE = 0x05
O_TEX_LOCATION = 0x08

VQ_TYPE = 3
COLOR_FORMATS = {0: 'ARGB1555', 1: 'RGB565', 2: 'ARGB4444', 3: 'ARGB8888'}


# ----------------------------------------------------------------------------
# little-endian readers
# ----------------------------------------------------------------------------
def u8(b, o):  return b[o]
def i8(b, o):  return b[o] - 256 if b[o] > 0x7F else b[o]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def i32(b, o): return struct.unpack_from('<i', b, o)[0]
def f32(b, o): return struct.unpack_from('<f', b, o)[0]
def f3(b, o):  return [f32(b, o), f32(b, o + 4), f32(b, o + 8)]


# ----------------------------------------------------------------------------
# texture format helpers (port of getTextureSize / color conversions)
# ----------------------------------------------------------------------------
def get_texture_size(value):
    base_w = (value % 64) // 8
    base_h = value % 8
    return (1 << (3 + base_w), 1 << (3 + base_h))


def encode_z_morton(x, y):
    """Port of encodeZMortonPosition.ts — interleave x/y to twiddled offset."""
    x &= 0xFFFF
    y &= 0xFFFF
    x = (x | (x << 8)) & 0x00FF00FF
    y = (y | (y << 8)) & 0x00FF00FF
    x = (x | (x << 4)) & 0x0F0F0F0F
    y = (y | (y << 4)) & 0x0F0F0F0F
    x = (x | (x << 2)) & 0x33333333
    y = (y | (y << 2)) & 0x33333333
    x = (x | (x << 1)) & 0x55555555
    y = (y | (y << 1)) & 0x55555555
    return x | (y << 1)


def conv_rgb565(v):
    r = (v >> 11) & 0x1F; g = (v >> 5) & 0x3F; b = v & 0x1F
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2)
    return (r, g, b, 255)


def conv_argb4444(v):
    a = ((v >> 12) & 0xF) * 0x11; r = ((v >> 8) & 0xF) * 0x11
    g = ((v >> 4) & 0xF) * 0x11; b = (v & 0xF) * 0x11
    return (r, g, b, a)


def conv_argb1555(v):
    a = ((v >> 15) & 0x01) * 255; r = ((v >> 10) & 0x1F) * 8
    g = ((v >> 5) & 0x1F) * 8; b = (v & 0x1F) * 8
    return (r, g, b, a)


CONV = {'RGB565': conv_rgb565, 'ARGB4444': conv_argb4444, 'ARGB1555': conv_argb1555}


# ----------------------------------------------------------------------------
# POL parsing
# ----------------------------------------------------------------------------
def scan_header(b):
    model_table_ptr = u32(b, 0x00)
    ram_offset = model_table_ptr & RAM_BASE_MASK
    model_count = u32(b, 0x04)
    pvr_start = u32(b, 0x08) - ram_offset
    pvr_end = u32(b, 0x10) - ram_offset
    table_off = model_table_ptr - ram_offset
    model_ptrs = []
    for i in range(model_count):
        ram = u32(b, table_off + 4 * i)
        model_ptrs.append({'ram': ram, 'file': ram - ram_offset})
    return {
        'ramOffset': ram_offset,
        'modelCount': model_count,
        'pvrStart': pvr_start,
        'pvrEnd': pvr_end,
        'modelPtrs': model_ptrs,
    }


def scan_texture_list(b, hdr):
    """Port of scanTextureHeaderData.ts — 16B recs in [pvrStart, pvrEnd)."""
    texs = []
    ram_offset_first = None
    o = hdr['pvrStart']
    while o < hdr['pvrEnd']:
        w = u16(b, o + O_TEX_WIDTH)
        h = u16(b, o + O_TEX_HEIGHT)
        fmt_v = u8(b, o + O_TEX_COLOR_FORMAT)
        typ = u8(b, o + O_TEX_TYPE)
        loc = u32(b, o + O_TEX_LOCATION)
        if ram_offset_first is None:
            ram_offset_first = loc
        if w > 0:  # discard trailing empty
            texs.append({
                'index': len(texs),
                'width': w,
                'height': h,
                'colorFormatValue': fmt_v,
                'colorFormat': COLOR_FORMATS.get(fmt_v, 'RGB555'),
                'type': typ,           # 1=plain twiddled, 3=VQ
                'baseLocation': loc,    # VRAM addr (0x0CC00000 family)
                'ramOffset': ram_offset_first,
                'fileOffset': loc - ram_offset_first,  # offset into TEX file
            })
        o += 16
    return texs


def get_vertex_addressing_mode(value):
    sv = (value & 0xFFFF0000) >> 16
    return 'reference' if 0x5FF0 <= sv <= 0x5FFF else 'direct'


def get_poly_type_flags(v):
    return {
        'cullingType': 'back' if (v >> 0) & 1 else 'front',
        'culling': bool((v >> 1) & 1),
        'spriteQuad': bool((v >> 2) & 1),
        'triangles': bool((v >> 3) & 1),
        'strip': bool((v >> 4) & 1),
        'gouraud': bool((v >> 6) & 1),
        'envMaps': bool((v >> 8) & 1),
    }


# isOpaque deduction (port of nlMeshConversions TEXTURE_INSTRUCTIONS switch)
_OPAQUE_83000000 = {0x8000001C, 0x8000002C, 0x8000003C, 0x8000009C, 0x800000AC, 0x800000BC}
_OPAQUE_83400000 = {0x8000001D, 0x8000002D, 0x8000003D, 0x8000009D, 0x800000AD, 0x800000BD}


def scan_mesh(b, addr):
    base_params = u32(b, addr + O_MESH_BASE_PARAMS)
    tex_instr = u32(b, addr + O_MESH_TEXTURE_INSTRUCTIONS)
    is_opaque = False
    if tex_instr == 0x83000000 and base_params in _OPAQUE_83000000:
        is_opaque = True
    if tex_instr == 0x83400000 and base_params in _OPAQUE_83400000:
        is_opaque = True
    tex_size_v = u8(b, addr + O_MESH_TEXTURE_SIZE)
    vcm = i32(b, addr + O_MESH_VERTEX_COLOR_MODE)
    return {
        'address': addr,
        'baseParams': base_params,
        'textureInstructions': tex_instr,
        'isOpaque': is_opaque,
        'textureSizeValue': tex_size_v,
        'textureSize': list(get_texture_size(tex_size_v)),
        'textureWrappingValue': u8(b, addr + O_MESH_UV_FLIP),
        'textureControlValue': u32(b, addr + O_MESH_TEXTURE_CONTROL),
        'textureColorFormatValue': u8(b, addr + O_MESH_TEXTURE_COLOR_FORMAT),
        'position': f3(b, addr + O_MESH_POSITION),
        'textureIndex': u8(b, addr + O_MESH_TEXTURE_NUMBER),
        'vertexColorModeValue': vcm,
        'hasColoredVertices': vcm == -3,
        'alpha': f32(b, addr + O_MESH_ALPHA),
        'color': f3(b, addr + O_MESH_COLOR),
        'specularAlpha': f32(b, addr + O_MESH_SPECULAR_ALPHA),
        'specularColor': f3(b, addr + O_MESH_SPECULAR_COLOR),
        'polygonDataLength': u32(b, addr + O_MESH_VERTEX_DATA_LENGTH),
        'polygons': [],
    }


def scan_vertex(b, addr, colored):
    content_flag = u32(b, addr + O_VTX_CONTENT_FLAG)
    mode = get_vertex_addressing_mode(content_flag)
    content_addr = addr
    vertex_offset = None
    if mode == 'reference':
        vertex_offset = i32(b, addr + O_VTX_OFFSET_VAR)
        content_addr = addr + vertex_offset + POLYGON_HEADER
    pos = f3(b, content_addr + O_VTX_POSITION)
    uv = [f32(b, content_addr + O_VTX_UV), f32(b, content_addr + O_VTX_UV + 4)]
    v = {
        'addressingMode': mode,
        'position': pos,
        'uv': uv,
    }
    if colored:
        n = [i8(b, content_addr + O_VTX_NORMALS + k) for k in range(3)]
        v['normals'] = [(x - 0x100) / 0x80 if x > 0x7F else x / 0x7F for x in n]
        c = [u8(b, content_addr + O_VTX_COLORS + k) for k in range(4)]
        # ModNao stores [b,g,r,a] order -> (r,g,b,a)
        v['colors'] = [c[2] / 255.0, c[1] / 255.0, c[0] / 255.0, c[3] / 255.0]
    return v, mode


def build_indices(verts, group_mode, culling_type):
    """Port of the strip/triple index builder in scanModel.ts."""
    indices = []
    if group_mode == 'regular':
        for i in range(len(verts)):
            if i > len(verts) - 3:
                break
            if i % 2 == 0:
                if culling_type == 'front':
                    indices += [i + 1, i, i + 2]
                else:
                    indices += [i, i + 1, i + 2]
            else:
                if culling_type == 'front':
                    indices += [i, i + 1, i + 2]
                else:
                    indices += [i + 1, i, i + 2]
    elif group_mode == 'triple':
        for i in range(2, len(verts), 3):
            if culling_type == 'front':
                indices += [i - 1, i - 2, i]
            else:
                indices += [i - 2, i - 1, i]
    return indices


def scan_model(b, addr, ram_addr, index):
    model = {
        'index': index,
        'address': addr,
        'ramAddress': ram_addr,
        'position': f3(b, addr + O_MODEL_POSITION),
        'radius': f32(b, addr + O_MODEL_RADIUS),
        'meshes': [],
        'totalVertexCount': 0,
    }
    detected_end = False
    sa = addr + MODEL_HEADER
    n = len(b)
    while sa < n and not detected_end:
        if u32(b, sa) == 0:
            detected_end = True
            sa += 4
            model['totalVertexCount'] = u32(b, sa)
            sa += 4
            break
        mesh = scan_mesh(b, sa)
        sa += MESH
        mesh_end = sa + mesh['polygonDataLength']
        while sa < mesh_end and sa + VERTEX_B < n and not detected_end:
            poly_addr = sa
            grp_val = u32(b, poly_addr + O_POLY_VERTEX_GROUP_TYPE)
            is_triple = ((grp_val >> 3) & 1) == 1
            group_mode = 'triple' if is_triple else 'regular'
            flags = get_poly_type_flags(grp_val)
            vcount = u32(b, poly_addr + O_POLY_VERTEX_COUNT)
            actual_vcount = vcount * (3 if is_triple else 1)
            poly = {
                'vertexGroupMode': group_mode,
                'vertexGroupModeValue': grp_val,
                'flags': flags,
                'vertexCount': vcount,
                'vertices': [],
            }
            sa = poly_addr + POLYGON_HEADER
            detected_mesh_end = False
            for i in range(actual_vcount):
                if u32(b, sa) == 0:
                    detected_end = True
                    detected_mesh_end = True
                    sa += 4
                    model['totalVertexCount'] = u32(b, sa)
                    sa += 4
                    break
                if detected_mesh_end:
                    break
                if sa + VERTEX_B >= n:
                    break
                v, mode = scan_vertex(b, sa, mesh['hasColoredVertices'])
                poly['vertices'].append(v)
                sa += VERTEX_A if mode == 'direct' else VERTEX_B
                if sa >= mesh_end:
                    detected_mesh_end = True
            poly['indices'] = build_indices(
                poly['vertices'], group_mode, flags['cullingType'])
            mesh['polygons'].append(poly)
        model['meshes'].append(mesh)
    return model


# ----------------------------------------------------------------------------
# TEX decode
# ----------------------------------------------------------------------------
def decode_texture(tex, tex_bytes):
    """De-twiddle one TextureList entry from the TEX file to RGBA8888 bytes."""
    if tex['type'] == VQ_TYPE:
        return None  # VQ not handled in scaffold (no VQ stage textures seen)
    conv = CONV.get(tex['colorFormat'])
    if conv is None:
        return None
    w, h = tex['width'], tex['height']
    base = tex['fileOffset']
    out = bytearray(w * h * 4)
    need = base + w * h * 2
    if need > len(tex_bytes):
        return None  # out of range (points to live VRAM residue)
    for y in range(h):
        row = w * y
        for x in range(w):
            morton = encode_z_morton(x, y)
            ro = base + morton * 2
            v = tex_bytes[ro] | (tex_bytes[ro + 1] << 8)
            r, g, b, a = conv(v)
            co = (row + x) * 4
            out[co] = r; out[co + 1] = g; out[co + 2] = b; out[co + 3] = a
    return bytes(out)


# ----------------------------------------------------------------------------
# main
# ----------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description='MVC2 Ninja stage POL/TEX decoder')
    ap.add_argument('--pol', required=True, help='STGxxPOL.BIN path')
    ap.add_argument('--tex', help='STGxxTEX.BIN path (auto from --pol if omitted)')
    ap.add_argument('--out', help='output dir (default _stage_out/<STGxx>)')
    ap.add_argument('--no-png', action='store_true', help='skip PNG texture export')
    ap.add_argument('--summary', action='store_true', help='print summary, no JSON')
    args = ap.parse_args()

    pol_path = args.pol
    stg_name = os.path.basename(pol_path).replace('POL.BIN', '').replace('POL.bin', '')
    tex_path = args.tex
    if not tex_path:
        tex_path = pol_path.replace('POL.BIN', 'TEX.BIN').replace('POL.bin', 'TEX.bin')
    out_dir = args.out or os.path.join('_stage_out', stg_name)

    with open(pol_path, 'rb') as f:
        pol = f.read()
    tex_bytes = b''
    if os.path.exists(tex_path):
        with open(tex_path, 'rb') as f:
            tex_bytes = f.read()
    else:
        print('WARN: TEX file not found: %s (geometry only)' % tex_path, file=sys.stderr)

    hdr = scan_header(pol)
    texs = scan_texture_list(pol, hdr)
    models = [scan_model(pol, mp['file'], mp['ram'], i)
              for i, mp in enumerate(hdr['modelPtrs'])]

    # stats
    total_meshes = sum(len(m['meshes']) for m in models)
    total_polys = sum(len(me['polygons']) for m in models for me in m['meshes'])
    total_verts = sum(len(p['vertices'])
                      for m in models for me in m['meshes'] for p in me['polygons'])

    print('=== %s ===' % stg_name)
    print('POL %d bytes  TEX %d bytes' % (len(pol), len(tex_bytes)))
    print('ramOffset 0x%08X  models %d  textures %d'
          % (hdr['ramOffset'], hdr['modelCount'], len(texs)))
    print('meshes %d  polygons %d  vertices %d' % (total_meshes, total_polys, total_verts))
    print('texture dims:', ', '.join('%dx%d/%s' % (t['width'], t['height'], t['colorFormat'])
                                      for t in texs))

    if args.summary:
        return

    os.makedirs(out_dir, exist_ok=True)

    # decode textures -> PNG
    if not args.no_png and tex_bytes:
        try:
            from PIL import Image
        except ImportError:
            print('Pillow not available; skipping PNG export', file=sys.stderr)
            Image = None
        if Image:
            for t in texs:
                rgba = decode_texture(t, tex_bytes)
                if rgba is None:
                    print('  tex[%d] %dx%d %s type=%d OUT-OF-RANGE/VQ -> skipped'
                          % (t['index'], t['width'], t['height'], t['colorFormat'], t['type']))
                    continue
                img = Image.frombytes('RGBA', (t['width'], t['height']), rgba)
                png = os.path.join(out_dir, 'tex_%02d.png' % t['index'])
                img.save(png)

    # emit JSON (mesh geometry + texture list)
    doc = {
        'stage': stg_name,
        'ramOffset': hdr['ramOffset'],
        'modelCount': hdr['modelCount'],
        'textureList': texs,
        'models': models,
        'stats': {
            'meshes': total_meshes, 'polygons': total_polys, 'vertices': total_verts,
        },
    }
    json_path = os.path.join(out_dir, '%s.json' % stg_name)
    with open(json_path, 'w') as f:
        json.dump(doc, f)
    print('wrote %s (%d bytes)' % (json_path, os.path.getsize(json_path)))
    if not args.no_png and tex_bytes:
        print('wrote %d texture PNGs to %s' % (len(texs), out_dir))


if __name__ == '__main__':
    main()
