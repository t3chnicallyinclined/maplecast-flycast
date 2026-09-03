#!/usr/bin/env python3
"""
rip_stage.py — decode an MVC2 stage (STGxxPOL.BIN + STGxxTEX.BIN) into a
client-loadable geometry JSON + texture PNGs, by porting the ModNao
(github.com/rob2d/modnao) NaomiLib / libspr "NLOBJPUT" decoder.

This is the offline rip step for RENDER-MASTER-PLAN-V2 §2.2 "Stage". MVC2 stage
data is a pre-relocated Sega NinjaLibrary object tree loaded at base 0x0cea0000.
We decode it OFFLINE (no SH4) and emit geometry projected exactly as the disc
art is, which is why it feeds PVR2Renderer (which consumes ta_parse-shape output)
as the OP (background) layer.

PORTED FROM MODNAO (verbatim logic, cited):
  - scanForModelPointers.ts  -> header: modelRamOffset = u32@0 & 0xffffff00;
                                 modelTablePtr = u32@0 - ramOff; count = u32@4
  - scanTextureHeaderData.ts  -> texture list: u32@8 (pvrStart) .. u32@0x10 (pvrEnd),
                                 16-byte recs {u16 w, u16 h, u8 fmt, u8 type, ...,
                                 u32 baseLocation@+8}; ramOffset = first baseLocation
  - scanModel.ts              -> MODEL_HEADER 0x18, then meshes (MESH 0x50) each with
                                 polygonDataLength@+0x4c; within each mesh, polygons
                                 (POLYGON_HEADER 0x08) {vertexGroupType@0, vertexCount@4};
                                 vertices VERTEX_A 0x20 (direct) or VERTEX_B 0x08 (reference)
  - NLPropConversionDefs.ts   -> mesh fields: textureSize@+8, uvFlip@+0xa,
                                 textureColorFormat@+0xf, textureNumber@+0x20,
                                 vertexColorMode@+0x24 (-3 => colored verts),
                                 alpha@+0x2c; vertex: pos@+0, normals@+0xc,
                                 colors@+0x10 (BGRA u8), uv@+0x18
  - getVertexAddressingMode.ts -> reference vert if (u32@+0 >> 16) in [0x5ff0,0x5fff]
  - getPolyTypeFlags.ts       -> cullingType = bit0 (1=back); triple = bit3
  - loadTextureFileWorker.ts  -> realLocation = baseLocation - ramOffset; per-texel
                                 morton (encodeZMortonPosition) twiddle; fmt decode
  - color-conversions/*.ts    -> ARGB1555 / RGB565 / ARGB4444 -> RGBA8888

OUTPUT (gitignored atlas/stages/):
  STGxx.json  — { stageId, ramOffset, textures:[{index,w,h,fmt,type,vq,file}],
                  meshes:[{texIndex, uvFlip, wrap:{hFlip,vFlip,hRepeat,vRepeat,
                  hStretch}, texCtrl, hasColor, alpha, color:[r,g,b], specular:
                  {a,rgb}, isOpaque, tris:[{pos,uv,col}...] }] }
  STGxx_tNN.png — decoded RGBA texture NN
  NOTE (2026-07-09, all-17 measured): raw uvFlip bytes with bit4/bit7 set occur
  on 267 meshes (0x10..0x89). ModNao's table is 0x00-0x0F and unmasked lookup
  would clamp all of them; we scope the table to the LOW NIBBLE (see wrap_flags
  docstring) and keep the raw byte in "uvFlip". OPEN: live A/B for the high bits.

The JSON is consumed by web/webgpu/stage-client.mjs which converts it to the
PVR2Renderer parsed-object shape (28B VBL strip + PolyParam) — see that file.

Usage:
  python3 tools/rip_stage.py 00            # rip one stage (hex id)
  python3 tools/rip_stage.py all           # rip all 17

── 2026-07-09 residual-facts port (FORMAT FACTS ONLY — fresh implementation) ──
ModNao relicensed 2026-05-25 (non-commercial, no-verbatim). Everything below is
extracted FORMAT FACTS (byte offsets / bit meanings / algorithm parameters, not
copyrightable), re-implemented in our own style. Attribution: github.com/rob2d/modnao.

FACTS (source file -> fact):
  - StructOffsets.ts / NLPropConversionDefs.ts -> additional mesh fields:
      textureControlValue = u32 @ mesh+0x0C (ModNao flags "confirm 0x10 or 0x0C";
      we record it as-is), color (diffuse) = 3 x f32 RGB @ mesh+0x30,
      specularAlpha = f32 @ mesh+0x3C, specularColor = 3 x f32 RGB @ mesh+0x40.
      (alpha = f32 @ +0x2C was already ported.)
  - getTextureWrappingFlags.ts -> the mesh byte @ +0x0A ("uvFlip") decodes by VALUE
      (low nibble, no clean single-bit formula — ModNao itself uses value sets):
        hFlip    for v in {04,06,0C,0E}
        vFlip    for v in {02,03,06,07,0A,0B}
        hRepeat  for v in {00,02,08,0A}
        vRepeat  for v in {00,01,04,05,08,09,0C,0D}
        hStretch for v odd (bit0 set)
      A value outside the tables => all flags false (= clamp both axes).
  - VqFormatConstants.ts / getTextureDefDataLength.ts / decompressVqBuffer.ts ->
      texture TYPE byte 3 = VQ. Container: 256-entry codebook, 4 u16 texels per
      entry (2048 bytes), then w*h/4 index bytes (1 byte per 2x2 texel block).
      AUTHORITY for the block layout is OUR core/rend/texconv.cpp texture_VQ
      (texconv.cpp:408): index byte for block (x,y) sits at twiddled-texel-idx/4;
      codebook entry words map y-fastest within the 2x2. Under this tool's PROVEN
      x-LSB morton (image-validated non-VQ path), that is EXACTLY a linear
      expansion: output twiddled word[4*i + j] = entry(index[i])[j]. (Verified
      algebraically: with x-LSB morton the within-block morton position equals
      flycast's transpose-consistent entry word index.) MEASURED 2026-07-09: all
      183 textures across the 17 MVC2 STGxxTEX files are type=1 (never VQ) — the
      branch is defensive completeness for other NinjaLib content.
  - decompressLzssBuffer.ts -> TEX-container LZSS (16-bit-WORD based — NOT the
      byte-based GFX1 sprite LZSS of body_decoder.mjs decodeA, which has a 4-bit
      distance/4-bit length byte code; the two are NOT interchangeable):
        stream of u16 LE words; a bitmask word precedes each group of 16 chunks,
        flags consumed MSB-first (0x8000 >> chunk).
        flag CLEAR -> literal word, copied out.
        flag SET   -> word 0x0000 terminates; else if the word fits in the 11 LSBs
          it is the LONG form: back-distance(words) = word, next u16 = copy count;
          otherwise SHORT form: copy count = top 5 bits, back-distance = 11 LSBs.
          Copies read from the already-produced output (distance in words); when
          count > distance the source sequence repeats (ring copy).
      MVC2 stage TEX files are never LZSS (ModNao attribs: hasLzssTextureFile
      false for all 17; measured: every texture decodes in-bounds). Fallback only
      triggers when a texture's data range exceeds the TEX file size.
  - mvc2StageAttribMappings.ts -> the 17 stage names (tools/stage_id_map.json
      "names" + web/webgpu/stage-client.mjs STAGE_NAMES).
"""
import os, sys, json, struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEVDIR = os.path.join(REPO, "MVC2 Dev Files")
OUTDIR = os.path.join(REPO, "atlas", "stages")
# Mirror copy served to web/stage-test.html (web root is web/). Same gitignored
# disc-derived data, just reachable by the static server.
WEBOUT = os.path.join(REPO, "web", "test-atlas", "stages")

# ---- StructSizes.ts ----
MODEL_HEADER = 0x18
MESH = 0x50
POLYGON_HEADER = 0x08
VERTEX_A = 0x20
VERTEX_B = 0x08

# ---- StructOffsets.ts (Mesh / Vertex) ----
M_BASE_PARAMS = 0x00
M_TEX_INSTR = 0x04
M_TEX_SIZE = 0x08
M_UV_FLIP = 0x0a
M_TEX_CTRL = 0x0c    # textureControlValue (u32; ModNao notes "confirm 0x10 or 0x0C")
M_TEX_FMT = 0x0f
M_POSITION = 0x10
M_TEX_NUM = 0x20
M_VCOLOR_MODE = 0x24
M_ALPHA = 0x2c
M_COLOR = 0x30       # mesh diffuse color, 3 x f32 RGB
M_SPEC_ALPHA = 0x3c  # specular alpha, f32
M_SPEC_COLOR = 0x40  # specular color, 3 x f32 RGB
M_VDATA_LEN = 0x4c
V_UV = 0x18
V_COLORS = 0x10

# ---- getTextureWrappingFlags.ts (fact table; see header notes) ----
# The mesh byte @ +0x0A decodes by VALUE membership — ModNao itself keys on value
# sets, there is no clean per-bit formula (e.g. hFlip = {04,06,0C,0E} is bit2 set
# AND bit0 clear). Values outside the tables => all False = clamp both axes.
_WRAP_HFLIP = frozenset((0x04, 0x06, 0x0c, 0x0e))
_WRAP_VFLIP = frozenset((0x02, 0x03, 0x06, 0x07, 0x0a, 0x0b))
_WRAP_HREP  = frozenset((0x00, 0x02, 0x08, 0x0a))
_WRAP_VREP  = frozenset((0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x0c, 0x0d))


def wrap_flags(v):
    """Decode the mesh wrapping byte (+0x0A) into sampler intent booleans.

    MEASURED (2026-07-09, all 17 MVC2 stages): 267 meshes carry raw bytes with
    bit4/bit7 set (0x10,0x11,0x18,0x19,0x80,0x81,0x88,0x89) — outside ModNao's
    0x00-0x0F table, which does NOT mask (those would decode all-false = clamp
    both). The low nibble of every such byte IS a valid table value, and the
    unmasked reading would flip 267 previously-repeat meshes to clamp with no
    ground truth behind it, so we scope the table to the LOW NIBBLE (bit4/bit7 =
    unknown non-wrap flags, preserved via the raw "uvFlip" field). OPEN: settle
    the high bits with a live A/B."""
    n = v & 0x0F
    return {
        "hFlip": n in _WRAP_HFLIP,
        "vFlip": n in _WRAP_VFLIP,
        "hRepeat": n in _WRAP_HREP,
        "vRepeat": n in _WRAP_VREP,
        "hStretch": (n & 1) == 1,     # odd values; Ninja stretch hint, no PVR TSP bit
    }


def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u8(b, o):  return b[o]
def f32(b, o): return struct.unpack_from("<f", b, o)[0]


# ---- encodeZMortonPosition.ts (Z-order / twiddle) ----
def morton(x, y):
    x &= 0xffff; y &= 0xffff
    x = (x | (x << 8)) & 0x00ff00ff
    y = (y | (y << 8)) & 0x00ff00ff
    x = (x | (x << 4)) & 0x0f0f0f0f
    y = (y | (y << 4)) & 0x0f0f0f0f
    x = (x | (x << 2)) & 0x33333333
    y = (y | (y << 2)) & 0x33333333
    x = (x | (x << 1)) & 0x55555555
    y = (y | (y << 1)) & 0x55555555
    return x | (y << 1)


# ---- color-conversions/*.ts ----
def argb1555(c):
    a = ((c >> 15) & 1) * 255
    r = ((c >> 10) & 0x1f) * 8
    g = ((c >> 5) & 0x1f) * 8
    b = (c & 0x1f) * 8
    return (r, g, b, a)

def rgb565(c):
    r = (c >> 11) & 0x1f; g = (c >> 5) & 0x3f; b = c & 0x1f
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2)
    return (r, g, b, 255)

def argb4444(c):
    a = ((c >> 12) & 0xf) * 0x11
    r = ((c >> 8) & 0xf) * 0x11
    g = ((c >> 4) & 0xf) * 0x11
    b = (c & 0xf) * 0x11
    return (r, g, b, a)

FMT_CONV = {0: argb1555, 1: rgb565, 2: argb4444}
FMT_NAME = {0: "ARGB1555", 1: "RGB565", 2: "ARGB4444"}


def get_texture_size(value):
    base_w = (value % 64) // 8
    base_h = value % 8
    return (1 << (3 + base_w), 1 << (3 + base_h))


def scan_texture_headers(pol, ram_off):
    """Port of scanTextureHeaderData.ts. Returns list of texture defs."""
    pvr_start = u32(pol, 0x08) - ram_off
    pvr_end = u32(pol, 0x10) - ram_off
    texs = []
    ramoffset = None
    a = pvr_start
    while a < pvr_end:
        w = u16(pol, a); h = u16(pol, a + 2)
        fmt = u8(pol, a + 4); typ = u8(pol, a + 5)
        loc = u32(pol, a + 8)
        if ramoffset is None:
            ramoffset = loc
        if w > 0:
            texs.append({"w": w, "h": h, "fmt": fmt, "type": typ,
                         "baseLocation": loc, "ramOffset": ramoffset})
        a += 16
    return texs


# ---- VQ container (facts in header; block layout per core/rend/texconv.cpp) ----
VQ_TEXTURE_TYPE = 3                 # texture-def TYPE byte value meaning VQ
VQ_CODEBOOK_BYTES = 256 * 4 * 2     # 256 entries x 4 u16 texels


def tex_data_len(t):
    """Byte length of a texture's data in the TEX container."""
    if t["type"] == VQ_TEXTURE_TYPE:
        return VQ_CODEBOOK_BYTES + (t["w"] * t["h"]) // 4
    return t["w"] * t["h"] * 2


def expand_vq(blob, w, h):
    """Expand a VQ texture (codebook + index plane) to a plain twiddled-16bpp
    buffer in this tool's morton convention, so the normal per-texel detwiddle
    path decodes it. Each index byte covers one 2x2 texel block; under the
    x-LSB morton the 4 codebook words of an entry land at consecutive twiddled
    word positions (see header notes for the flycast texture_VQ equivalence)."""
    nidx = (w * h) // 4
    out = bytearray(w * h * 2)
    have = max(0, len(blob) - VQ_CODEBOOK_BYTES)
    for i in range(min(nidx, have)):
        e = blob[VQ_CODEBOOK_BYTES + i]
        src = e * 8
        out[i * 8:i * 8 + 8] = blob[src:src + 8]
    return bytes(out)


def lzss_decompress_tex(buf):
    """NinjaLib TEX-container LZSS (16-bit-word based; parameter facts in header,
    fresh implementation). Returns the decompressed bytes. NOT the GFX1 sprite
    LZSS (body_decoder.mjs decodeA) — different code layout, do not swap them."""
    out = []                               # decompressed u16 words
    n = len(buf) // 2
    i = 0
    bitmask = 0
    chunk = 16                             # forces a bitmask load on entry
    while i < n:
        w = struct.unpack_from("<H", buf, i * 2)[0]; i += 1
        if chunk == 16:
            bitmask = w; chunk = 0
            continue
        if not (bitmask & (0x8000 >> chunk)):
            out.append(w)                  # literal word
        elif w == 0:
            break                          # terminator
        else:
            if (w & 0x7FF) == w:           # long form: dist=word, count=next u16
                dist = w
                if i >= n:
                    break
                cnt = struct.unpack_from("<H", buf, i * 2)[0]; i += 1
            else:                          # short form: count=top 5 bits, dist=11 LSBs
                cnt = (w >> 11) & 0x1F
                dist = w & 0x7FF
            start = len(out) - dist
            seq = out[start:start + min(cnt, dist)]
            if not seq:
                seq = [0]
            for j in range(cnt):           # ring copy when cnt > dist
                out.append(seq[j % len(seq)])
        chunk += 1
    packed = bytearray(len(out) * 2)
    for k, word in enumerate(out):
        struct.pack_into("<H", packed, k * 2, word)
    return bytes(packed)


def decode_texture(tex_file, t):
    """Port of loadTextureFileWorker.createTexturePixelBuffers, all container
    types: plain twiddled 16bpp AND VQ (type 3, expanded via expand_vq).
    Returns (w, h, bytes RGBA8888)."""
    w, h, fmt = t["w"], t["h"], t["fmt"]
    conv = FMT_CONV.get(fmt)
    if conv is None:
        return w, h, None
    real_loc = t["baseLocation"] - t["ramOffset"]
    if t.get("type") == VQ_TEXTURE_TYPE:
        src = expand_vq(tex_file[real_loc:real_loc + tex_data_len(t)], w, h)
        base = 0
    else:
        src = tex_file
        base = real_loc
    out = bytearray(w * h * 4)
    for y in range(h):
        row = w * y
        for x in range(w):
            off_drawn = morton(x, y)
            ro = base + off_drawn * 2
            if ro + 2 > len(src):
                continue
            color = struct.unpack_from("<H", src, ro)[0]
            r, g, b, a = conv(color)
            ci = (row + x) * 4
            out[ci] = r; out[ci + 1] = g; out[ci + 2] = b; out[ci + 3] = a
    return w, h, bytes(out)


def vertex_addressing_mode(value):
    sv = (value & 0xffff0000) >> 16
    return "reference" if 0x5ff0 <= sv <= 0x5fff else "direct"


def scan_model(pol, address):
    """Port of scanModel.ts. Returns list of meshes; each mesh has triangles
    (already strip-expanded with winding from ModNao) carrying pos/uv/col."""
    meshes = []
    detected_end = False
    sa = address + MODEL_HEADER
    n = len(pol)
    while sa < n and not detected_end:
        if u32(pol, sa) == 0:
            break
        m_base = sa
        base_params = u32(pol, m_base + M_BASE_PARAMS)
        tex_instr = u32(pol, m_base + M_TEX_INSTR)
        # The three words @+0/+4/+8 are the DC TA polygon header verbatim: PCW (baseParams), ISP
        # (texInstr), TSP. Steam's consumer FUN_1408482a0 derives blend/sampler/ignore-tex-alpha from
        # the FULL TSP word (bits 13-31); texSizeVal keeps only its low byte, so emit the word too
        # (mvc-live-skins-quarters/docs/TSP-RENDER-STATE-GHIDRA.md).
        tsp_word = u32(pol, m_base + M_TEX_SIZE)
        tex_size_val = u8(pol, m_base + M_TEX_SIZE)
        uv_flip = u8(pol, m_base + M_UV_FLIP)
        tex_ctrl = u32(pol, m_base + M_TEX_CTRL)
        tex_fmt = u8(pol, m_base + M_TEX_FMT)
        tex_num = u8(pol, m_base + M_TEX_NUM)
        vcolor_mode = struct.unpack_from("<i", pol, m_base + M_VCOLOR_MODE)[0]
        has_colored = (vcolor_mode == -3)
        alpha = f32(pol, m_base + M_ALPHA)
        mesh_color = [round(f32(pol, m_base + M_COLOR + 4 * k), 6) for k in range(3)]
        spec_alpha = round(f32(pol, m_base + M_SPEC_ALPHA), 6)
        spec_color = [round(f32(pol, m_base + M_SPEC_COLOR + 4 * k), 6) for k in range(3)]
        poly_data_len = u32(pol, m_base + M_VDATA_LEN)

        sa += MESH
        mesh_end = sa + poly_data_len
        tris = []

        while sa < mesh_end and sa + VERTEX_B < n and not detected_end:
            poly_addr = sa
            vgroup_val = u32(pol, poly_addr + 0x00)
            is_triple = ((vgroup_val >> 3) & 1) == 1
            vgroup_mode = "triple" if is_triple else "regular"
            culling_back = (vgroup_val & 1) == 1
            vcount = u32(pol, poly_addr + 0x04)
            actual_vc = vcount * (3 if is_triple else 1)

            sa = poly_addr + POLYGON_HEADER

            verts = []
            detected_mesh_end = False
            for i in range(actual_vc):
                if u32(pol, sa) == 0:
                    detected_end = True; detected_mesh_end = True
                    sa += 8
                    break
                if detected_mesh_end:
                    break
                if sa + VERTEX_B >= n:
                    break
                cmv = u32(pol, sa)
                amode = vertex_addressing_mode(cmv)
                content_addr = sa
                if amode == "reference":
                    voff = struct.unpack_from("<i", pol, sa + 0x04)[0]
                    content_addr = sa + voff + POLYGON_HEADER
                # position @ content+0x00
                if content_addr + 0x20 <= n:
                    px = f32(pol, content_addr + 0x00)
                    py = f32(pol, content_addr + 0x04)
                    pz = f32(pol, content_addr + 0x08)
                    uu = f32(pol, content_addr + V_UV)
                    vv = f32(pol, content_addr + V_UV + 4)
                    if has_colored:
                        b_ = u8(pol, content_addr + V_COLORS + 0)
                        g_ = u8(pol, content_addr + V_COLORS + 1)
                        r_ = u8(pol, content_addr + V_COLORS + 2)
                        a_ = u8(pol, content_addr + V_COLORS + 3)
                        col = [r_, g_, b_, a_]
                    else:
                        col = [255, 255, 255, 255]
                    verts.append({"pos": [px, py, pz], "uv": [uu, vv], "col": col})
                else:
                    verts.append({"pos": [0, 0, 0], "uv": [0, 0],
                                  "col": [255, 255, 255, 255]})
                sa += VERTEX_A if amode == "direct" else VERTEX_B
                if sa >= mesh_end:
                    detected_mesh_end = True

            # ---- strip -> triangle indices (scanModel.ts winding) ----
            indices = []
            if vgroup_mode == "regular":
                for i in range(max(0, len(verts) - 2)):
                    if i % 2 == 0:
                        if not culling_back:
                            indices += [i + 1, i, i + 2]
                        else:
                            indices += [i, i + 1, i + 2]
                    else:
                        if not culling_back:
                            indices += [i, i + 1, i + 2]
                        else:
                            indices += [i + 1, i, i + 2]
            else:  # triple
                for i in range(2, len(verts), 3):
                    if not culling_back:
                        indices += [i - 1, i - 2, i]
                    else:
                        indices += [i - 2, i - 1, i]

            for k in range(0, len(indices) - 2, 3):
                a_, b_, c_ = indices[k], indices[k + 1], indices[k + 2]
                if a_ < len(verts) and b_ < len(verts) and c_ < len(verts):
                    tris.append([verts[a_], verts[b_], verts[c_]])

        # blend deduction: textureInstructions/baseParams (isOpaque) per NLPropConversionDefs
        is_opaque = False
        if tex_instr in (0x83000000, 0x83400000):
            is_opaque = base_params in (
                0x8000001c, 0x8000002c, 0x8000003c, 0x8000009c, 0x800000ac, 0x800000bc,
                0x8000001d, 0x8000002d, 0x8000003d, 0x8000009d, 0x800000ad, 0x800000bd)
        meshes.append({
            "texIndex": tex_num,
            "texSizeVal": tex_size_val,
            "uvFlip": uv_flip,
            "wrap": wrap_flags(uv_flip),        # decoded +0x0A wrapping byte
            "texCtrl": tex_ctrl,                # textureControlValue u32 @ +0x0C
            "hasColor": has_colored,
            "alpha": alpha,
            "color": mesh_color,                # mesh diffuse RGB (f32) @ +0x30
            "specular": {"a": spec_alpha, "rgb": spec_color},  # @ +0x3C/+0x40
            "isOpaque": is_opaque,
            "baseParams": base_params,
            "texInstr": tex_instr,
            "tsp": tsp_word,                    # full TSP u32 @ +0x08 (see tsp_word above)
            "tris": tris,
        })
    return meshes


# ── WORLD-SPACE ASSEMBLY (re_kb 26: finding:stage_real_bug_pernode_matrices) ──
# The disc POL stores each model in its OWN space. In MVC2 the per-model WORLD
# placement is RUNTIME state the engine pushes onto the NaomiLib matrix stack
# (@0x8C2D6900) during the tree-walk (bank12 loc_8c122fd0/loc_8c122d00) — it is
# NOT in the POL node header (+0x0C/+0x10/+0x14 = NaomiLib bounding-sphere
# center+radius, NOT a transform; confirmed by deref + by the "applying it makes
# placement worse" test). So we cannot derive props purely from the POL header.
#
# GROUND-TRUTH-VALIDATED FACT (this tool, _stage_gt/engine_ta.bin un-projected
# through the live camera M1·M2): MODEL 0 (the deck/skybox, world ±2500) is
# authored DIRECTLY in world space — its raw rip verts project through M1·M2 to
# the engine's screen output at 0.000px residual (median/mean/p90/max all 0.0 over
# 2346 verts of STG0B). So model 0 = IDENTITY world matrix; it needs no transform.
#
# Models 1..N-1 are small LOCAL-space props (verts ±18: cannons/chains/details).
# Their world matrices are loaded from an optional captured sidecar (Oracle: the
# matrix-stack top @0x8C2D6900 per model, camera-removed offline → node-local
# 4x3 world matrix). When the sidecar is absent the prop is emitted with
# placed=false so the CLIENT SKIPS it (the old behavior collapsed every prop to a
# single screen dot at ~320,432 — the "green blob"; skipping is strictly better
# until the matrices are captured). Model 0 always renders.
#
# Sidecar format (atlas/stages/STGxx_matrices.json, optional, gitignored):
#   { "matrices": { "<modelIndex>": [m00,m01,m02,m03, m10,..,m13, m20,..,m23] } }
# i.e. a 3x4 row-major world matrix (last row implicitly 0,0,0,1). Model 0 may be
# omitted (identity). A model present here is placed=true and its verts are
# pre-multiplied into WORLD space here so the client projects them with M1·M2.

# A model whose max |vertex coord| >= this is authored in WORLD space (deck/scenery,
# 100s..1000s of units) and renders with an identity matrix; below it is a LOCAL-space
# prop needing a runtime world matrix. STG0B props peak at ~18u; the deck spans ~2500u.
WORLD_EXTENT_THRESH = 100.0


def _model_extent(meshes):
    """Max absolute vertex coordinate across all of a model's meshes (0 if empty)."""
    mx = 0.0
    for m in meshes:
        for tri in m["tris"]:
            for v in tri:
                p = v["pos"]
                a = abs(p[0]); b = abs(p[1]); c = abs(p[2])
                if a > mx: mx = a
                if b > mx: mx = b
                if c > mx: mx = c
    return mx


def _apply_mat34(mat, p):
    x, y, z = p
    return [
        mat[0]*x + mat[1]*y + mat[2]*z  + mat[3],
        mat[4]*x + mat[5]*y + mat[6]*z  + mat[7],
        mat[8]*x + mat[9]*y + mat[10]*z + mat[11],
    ]


def _load_matrices(sid):
    """Optional per-model world matrices captured live. Returns {modelIdx:mat34}."""
    p = os.path.join(OUTDIR, f"STG{sid}_matrices.json")
    if not os.path.exists(p):
        return {}
    try:
        j = json.load(open(p))
        return {int(k): v for k, v in (j.get("matrices") or {}).items()}
    except Exception as e:
        print(f"  WARN STG{sid}: bad matrices sidecar: {e}")
        return {}


def rip_stage(stage_id_hex):
    sid = stage_id_hex.upper()
    pol_path = os.path.join(DEVDIR, f"STG{sid}POL.BIN")
    tex_path = os.path.join(DEVDIR, f"STG{sid}TEX.BIN")
    if not os.path.exists(pol_path):
        print(f"  SKIP STG{sid}: no POL file"); return
    pol = open(pol_path, "rb").read()
    texf = open(tex_path, "rb").read()

    ram_off = u32(pol, 0x00) & 0xffffff00
    model_table = u32(pol, 0x00) - ram_off
    model_count = u32(pol, 0x04)
    world_mats = _load_matrices(sid)

    textures = scan_texture_headers(pol, ram_off)

    # LZSS container fallback (never triggers for MVC2 stages — see header notes):
    # if any texture's data range exceeds the TEX file, the container is
    # word-LZSS compressed (CvS2-style NinjaLib content); decompress ONCE.
    if textures:
        need = max(t["baseLocation"] - t["ramOffset"] + tex_data_len(t)
                   for t in textures)
        if need > len(texf):
            print(f"  STG{sid}: TEX needs 0x{need:x} > file 0x{len(texf):x} "
                  f"-> LZSS container decompress")
            texf = lzss_decompress_tex(texf)

    from PIL import Image
    os.makedirs(OUTDIR, exist_ok=True)
    tex_meta = []
    for ti, t in enumerate(textures):
        w, h, rgba = decode_texture(texf, t)
        fn = f"STG{sid}_t{ti:02d}.png"
        if rgba is not None:
            Image.frombytes("RGBA", (w, h), rgba).save(os.path.join(OUTDIR, fn))
        tex_meta.append({"index": ti, "w": w, "h": h, "fmt": t["fmt"],
                         "fmtName": FMT_NAME.get(t["fmt"], "?"),
                         "type": t["type"], "vq": t["type"] == VQ_TEXTURE_TYPE,
                         "baseLocation": t["baseLocation"], "file": fn})

    all_meshes = []
    placed_count = 0
    skipped_count = 0
    for mi in range(model_count):
        ram_addr = u32(pol, model_table + 4 * mi)
        addr = ram_addr - ram_off
        if addr < 0 or addr >= len(pol):
            continue
        meshes = scan_model(pol, addr)
        # Decide world placement by VERTEX EXTENT (re_kb 26, validated 0.000px vs
        # engine_ta for STG0B model 0). A model authored DIRECTLY in world space has
        # a large extent (deck/skybox/scenery span 100s..1000s of units) and renders
        # with an IDENTITY matrix — its raw rip verts ARE world coords. Most stages
        # author the WHOLE scene this way (STG00/04/07/09/0D = 0 local models). Only
        # small LOCAL-space props (extent <= WORLD_EXTENT_THRESH, e.g. STG0B's 66
        # cannons/chains ±18u) need a runtime world matrix; without a captured matrix
        # they collapse to one screen dot, so we skip them (client honors placed=false).
        ext = _model_extent(meshes)
        if ext >= WORLD_EXTENT_THRESH:
            mat = None            # world-space: raw verts ARE world coords (identity)
            placed = True
        elif mi in world_mats:
            mat = world_mats[mi]  # captured node-local world matrix -> world space
            placed = True
        else:
            mat = None
            placed = False        # local-space prop, no matrix -> client skips it
        if placed:
            placed_count += 1
        else:
            skipped_count += 1
        for m in meshes:
            m["model"] = mi
            m["placed"] = placed
            if placed and mat is not None:
                for tri in m["tris"]:
                    for v in tri:
                        v["pos"] = _apply_mat34(mat, v["pos"])
        all_meshes.extend(meshes)

    out = {
        "stageId": int(sid, 16),
        "ramOffset": ram_off,
        "modelCount": model_count,
        # world-space assembly status (re_kb 26): model 0 always placed (identity),
        # props placed only when STGxx_matrices.json supplies their world matrix.
        "worldAssembled": True,
        "placedModels": placed_count,
        "unplacedModels": skipped_count,
        "hasMatrixSidecar": bool(world_mats),
        "textures": tex_meta,
        "meshes": all_meshes,
    }
    jpath = os.path.join(OUTDIR, f"STG{sid}.json")
    with open(jpath, "w") as f:
        json.dump(out, f)
    # mirror JSON + PNGs into the web-served dir
    os.makedirs(WEBOUT, exist_ok=True)
    with open(os.path.join(WEBOUT, f"STG{sid}.json"), "w") as f:
        json.dump(out, f)
    import shutil
    for t in tex_meta:
        src = os.path.join(OUTDIR, t["file"])
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(WEBOUT, t["file"]))
    ntris = sum(len(m["tris"]) for m in all_meshes if m.get("placed", True))
    print(f"  STG{sid}: {model_count} models ({placed_count} placed, "
          f"{skipped_count} unplaced/skipped), {len(all_meshes)} meshes, "
          f"{ntris} placed-tris, {len(tex_meta)} textures -> {os.path.basename(jpath)}")


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    arg = sys.argv[1].lower()
    if arg == "all":
        ids = [f"{i:02X}" for i in range(0x11)]  # STG00..STG10 (0..16)
    else:
        ids = [arg.upper().zfill(2)]
    print(f"Ripping to {OUTDIR}")
    for sid in ids:
        rip_stage(sid)


if __name__ == "__main__":
    main()
