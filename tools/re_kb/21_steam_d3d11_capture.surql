-- 21: PATH B — the Steam D3D11 capture, and what it establishes about MvC2.
--
-- WHY THIS SEED EXISTS
-- --------------------
-- A second, INDEPENDENT window on the same game. Everything in this graph until now came from the
-- DC/NAOMI side: the marvelous2 SH4 disassembly, live flycast captures, the offline atlas bake.
-- `d3dcap` reads the Steam x86-64 recompile's actual D3D11 draw stream, so a claim that agrees
-- across both is corroborated by two different builds of the same game rather than restated.
--
-- ⚠ THE EVIDENCE SPLIT MATTERS MORE THAN USUAL HERE, because the capture is very persuasive and
-- most of it is NOT code-grade:
--   * CODE-grade  = read out of the game's own bytes — shader bytecode via `fxc /dumpbin`, a
--                   D3D11_INPUT_ELEMENT_DESC, a captured blend/depth/sampler struct.
--   * INFERRED    = derived from our capture instrument, however many frames it holds up over.
--                   The instrument has a KNOWN blind spot (it only sees texture writes that go
--                   through Map/Unmap or UpdateSubresource), and a statistic computed from an
--                   instrument can never be evidence about that instrument's blind spot.
--   * A PIXEL DIFF IS NEITHER. It is `record_attempt(outcome='masks_only')` — see 77_epistemics.
--
-- Written 2026-09-02. Repo: mvc-live-skins-quarters/d3dcap, SSOT docs/RENDER-ACCURACY-PROGRAM.md.

USE NS re DB kb;

-- ---- provenance -------------------------------------------------------------
-- TWO sources, deliberately, because one would launder the weaker evidence through the stronger.
-- The shader bytecode and the input-layout descriptors ARE the game bytes and are code-grade. The
-- capture instrument that collected them is not: it has a demonstrated blind spot (it does not see
-- texture writes outside Map/UpdateSubresource) and anything derived from its output is at best a
-- metric. The graph enforces this — only a code- or reproduction-grade source can confirm — and
-- splitting here is what keeps that enforcement meaningful rather than something to work around.
UPSERT source:steam_shader_disasm SET
  kind = 'steam_d3d11',
  strength = 'code',
  ref = 'Steam MvC2 shader bytecode and pipeline descriptors, captured at creation and disassembled '
     || 'with fxc /dumpbin: vs_0000000064057B38 (world VS), ps_00000000642DB9F8 (indexed PS), '
     || 'ps_00000000643D33F8 (texalpha PS); D3D11_INPUT_ELEMENT_DESC[] in il_*.json; per-draw '
     || 'D3D11 blend / depth-stencil / rasterizer / sampler descriptors.',
  note = 'The game own bytes. Code-grade: a claim read directly out of these is not an inference.';

UPSERT source:d3dcap_steam_capture SET
  kind = 'steam_d3d11',
  strength = 'metric',
  ref = 'mvc-live-skins-quarters/d3dcap/ — MinHook capture of the Steam MvC2 D3D11 draw stream, '
     || 'packed offline and replayed on WebGPU, diffed against the game own pre-bloom scene RT.',
  note = 'The INSTRUMENT, not the game. Deliberately NOT confirming-grade: it has a demonstrated '
      || 'blind spot (texture writes outside Map/UpdateSubresource went unseen for a whole session) '
      || 'and a statistic computed from an instrument can never be evidence about that instrument '
      || 'blind spot. Everything derived from it is inferred until something else confirms it.';

-- ---- CONFIRMED: read straight out of Steam's own bytes ----------------------
UPSERT finding:steam_char_draws_are_indexed SET
  statement = 'Steam MvC2 renders CHARACTERS through a palette path: t0 = R8_UNORM index tile, '
           || 't1 = 256x1 palette, POINT sampled on BOTH slots. The direct-RGBA draws '
           || '(64x64 / 128x128 / 256x256 pages) are the 3D STAGE, not the fighters. '
           || 'Consequence: a SKIN SWAP IS A PALETTE WRITE, matching sprite-gpu.mjs setSkin().',
  status = 'confirmed', confidence = 0.98, about = 'field:sprite_palette',
  reasoning = 'ps_5_0 disassembly of ps_00000000642DB9F8 shows sample(t0) -> mov r0.y,l(0) -> '
           || 'sample(t1, r0.xy) — a palette lookup keyed by the sampled index. The captured '
           || 'D3D11_SAMPLER_DESC is filter 0 (MIN_MAG_MIP_POINT) on both slots for all 152 '
           || 'character draws. Draw counts mislead here; reason from where pixels land.';
RELATE finding:steam_char_draws_are_indexed->cites->source:steam_shader_disasm;

UPSERT finding:steam_fragment_maths SET
  statement = 'Steam sprite fragment maths: rgb = texel.rgb * colour0.rgb + colour1.rgb, '
           || 'a = texel.a * colour0.a (or colour0.a alone under pp_IgnoreTexA). Followed by an '
           || 'ALPHA TEST — `ge fAlphaRef, a` then `discard_nz` — with fAlphaRef = 0 on every draw '
           || 'that binds cb0, so alpha-0 texels are DISCARDED and never reach the blender.',
  status = 'confirmed', confidence = 0.98, about = 'routine:pixel_shader',
  reasoning = 'Identical instruction tail in ps_00000000642DB9F8 (indexed) and '
           || 'ps_00000000643D33F8 (texalpha). fAlphaRef read from the shadowed cb0 payload.';
RELATE finding:steam_fragment_maths->cites->source:steam_shader_disasm;

UPSERT finding:steam_vertex_layout_stride40 SET
  statement = 'Steam sprite vertex layout, stride 40: POSITION float4 @0 (.xyz read), '
           || 'NORMAL float2 @16 NEVER READ (uninitialised — real captures hold NaN), '
           || 'TANGENT R8G8B8A8_UNORM @24 = colour0, BINORMAL R8G8B8A8_UNORM @28 = colour1, '
           || 'TEXCOORD float2 @32. Format 28 is RGBA, NOT BGRA.',
  status = 'confirmed', confidence = 0.97, about = 'field:vertex_format',
  reasoning = 'Captured D3D11_INPUT_ELEMENT_DESC[] (il_*.json). The vertex shader input '
           || 'signature confirms NORMAL is declared and unused.';
RELATE finding:steam_vertex_layout_stride40->cites->source:steam_shader_disasm;

UPSERT finding:steam_char_vs_is_passthrough SET
  statement = 'Character and HUD draws use a PASS-THROUGH vertex shader — positions are ALREADY '
           || 'IN NDC. The stage uses a world 3x4 dotted with (pos,1), then accumulated against a '
           || 'view-projection 4x4: clip = w.x*vp[0] + w.y*vp[1] + w.z*vp[2] + vp[3].',
  status = 'confirmed', confidence = 0.97, about = 'routine:vertex_shader',
  reasoning = 'vs_5_0 disassembly. The flat VS declares no constant buffer at all; '
           || 'vs_0000000064057B38 declares CBWorld (row_major float3x4) and CBViewProjection.';
RELATE finding:steam_char_vs_is_passthrough->cites->source:steam_shader_disasm;

-- ---- INFERRED: strong, reproducible, but derived from our own instrument -----
UPSERT finding:steam_char_sprite_plane_384x224 SET
  statement = 'Character quads sit on a 384x224 pixel plane and are NEVER scaled. NDC-per-texel is '
           || 'exactly 2/384 in x and 2/224 in y. This AGREES with the emitter constants rather '
           || 'than contradicting them: 384 * 5/3 = 640 and 224 * 15/7 = 480. The Gate-1 mapping is '
           || 'therefore gx = (ndc.x+1)*192, gy = (1-ndc.y)*112 — NOT *320/*240.',
  status = 'inferred', confidence = 0.85, about = 'field:sprite_geometry',
  reasoning = 'Measured over 22,308 character vertices spanning a 246-frame burst: one distinct '
           || 'NDC-per-texel value on each axis, no spread; (1-y_ndc)*112 minus a single constant '
           || 'bias of 0.0666515 is an exact integer for EVERY vertex, zero exceptions.';
RELATE finding:steam_char_sprite_plane_384x224->cites->source:d3dcap_steam_capture;

UPSERT finding:steam_rewrites_texture_pool_in_place SET
  statement = 'The game REWRITES its sprite-tile texture objects IN PLACE, through a path that is '
           || 'neither Map/Unmap nor UpdateSubresource. Any capture that snapshots a texture only '
           || 'when it believes it changed will serve stale tiles to every frame after the first.',
  status = 'inferred', confidence = 0.9, about = 'field:texture_upload',
  reasoning = 'Same scene, two capture strategies: dirty-flag snapshotting gives 303 distinct '
           || 'tiles over 246 frames with 0.6% consecutive-frame overlap; re-snapshotting every '
           || 'frame gives 10,514 over 300 frames with 56.9% overlap. Random selection from a '
           || 'resident 303-pool would give ~20%. The 0.6% is a ring being cycled and rewritten. '
           || '⚠ An expert review concluded the opposite from "every texture object has exactly '
           || 'ONE content generation" — but a generation only existed when our dirty flag fired, '
           || 'so that statistic was derived from the very blind spot in question.';
RELATE finding:steam_rewrites_texture_pool_in_place->cites->source:d3dcap_steam_capture;

UPSERT finding:steam_assembly_record_is_many_quads SET
  statement = 'ONE assembly record maps to N engine quads, not one. Steam character quads are '
           || 'SQUARE in native units (16x16, 8x8, 32x32) and abut in grids, while PLxx_parts '
           || 'entries are frequently non-square. Any comparison against buildEmitterDrawList must '
           || 'therefore be "the emitter part rect equals the UNION of the Steam quads that tile '
           || 'it", never a quad-size multiset — the multiset form fails by construction.',
  status = 'inferred', confidence = 0.8, about = 'field:tile_assembly',
  reasoning = 'Draws 552-555 of frame 2574 form a 2x2 block of 32x32; draws 568-571 a 1x4 row of '
           || '16x16. A search of all 60 PL*_asm.json for a part-size multiset matching a captured '
           || '54-quad body found no convergence (best 0.685). Consistent with re_kb/66-68: blank '
           || 'sel==0xFF records consuming tiledesc slots, column-pair-major order for wide parts.';
RELATE finding:steam_assembly_record_is_many_quads->cites->source:d3dcap_steam_capture;
RELATE finding:steam_assembly_record_is_many_quads->relates_to->finding:steam_char_sprite_plane_384x224;
