USE NS re DB kb;

-- ============================================================================
-- 2026-07-10 — the TEXEL twin of re_kb/68: the native-chunk carve for dense
-- wide parts must be COLUMN-PAIR-MAJOR (bodytex=local Sentinel swap CLOSED).
-- ============================================================================

UPSERT source:zz_realblob_gate SET kind='measurement',
  ref='tools/render-replica-poc/_zz_realblob_gate.mjs (node _zz_realblob_gate.mjs band4.mcrr 34 60); real Sentinel gfx1 blob, production Y-first detwiddlePal4 readback',
  note='Byte-gate of the body_decoder native-chunk carve vs the engine VRAM chunk order for a DENSE 4x4 part (Sentinel sel124/sel112 128x128, all 16 tiles nonzero): row-band-major k=by*Tw+col*bh+(row-by) = 232/464 bad (exactly 50% = the off-diagonal 2x2-block swap); column-pair-major (colPairChunk == rebuild_tile_grid emit order) = 0/464 bad. Also 8x4: row-band 8/32 bad, col-pair 32/32 clean. Prior _live4 sel 0xD61 "row-band validated vs engine" had only 9/16 nonzero tiles -> the swapped off-diagonal (zero) blocks could not distinguish the two orders; the dense part is the discriminating test.';

UPSERT finding:texel_carve_colpair_major SET
  statement='CONFIRMED + FIXED 2026-07-10. The native-chunk texel carve (m==32 && cols>1 && rows>1: copy the engine''s full-WxH twiddle blob''s 32x32 chunks to per-tile VRAM addrs) must index the blob COLUMN-PAIR-MAJOR — the same order render_frame rebuild_tile_grid (widecarve1, re_kb/68) emits geometry/TCW slots. widecarve1 fixed the GEOMETRY twin (quad positions) but left the TEXEL carve at row-band-major, breaking geometry<->texel lockstep: on bodytex=local every dense >2x>2 part (Sentinel sel124/112) had its off-diagonal 2x2 tile-blocks swapped (re_kb/68 "swapped diagonally" on the texel side). The default TCW->TextureManager path was immune (each quad textures from its own TCW->engine VRAM). FIX: body_decoder.mjs colPairChunk(col,row,Tw,Th) replaces k=by*Tw+col*bh+(row-by). Byte-gate 0/464 (was 232/464). No regression: identical to row-band for Tw<=2 OR Th<=2 (all satellites + normal bodies, verified over 1x1..8x8). DEPLOYED browser: body_decoder.mjs ?v=scene8. C++ TWINS updated source-only (need native rebuild): maplecast_mirror.cpp gstaDecodeBodies + texel_gate.cpp decodeBodies. Supersedes the row-band-major claim in those files'' comments (the _live4 witness was sparse). re_kb/68 geometry twin: finding:wide_part_colpair_order.',
  status='confirmed', confidence='high', date='2026-07-10';

RELATE finding:texel_carve_colpair_major->cites->source:zz_realblob_gate;
RELATE finding:texel_carve_colpair_major->about->finding:wide_part_colpair_order;

-- Storm cape: texel carve is byte-CLEAN in every captured pose — the residual is NOT here.
UPSERT finding:storm_cape_texel_clean SET
  statement='OPEN (not a texel-carve bug). On bodytex=local the ensureBodyTextures carve for Storm''s cape is byte-CLEAN: the cape = 3 satellite objects (nodes 26afc4/26adf4 cat=4 + a cat=3) all sharing Storm''s own gfx1 c420040 (resolves fine), decomposing to ALL LINEAR-path parts (32x64 1x2, 64x16 4x1, ...) — ZERO 4x4+ native parts, so the re_kb/70 fix does not touch it. MEASURED (band/band2.mcrr, actual-carve Y-first readback, live-path srcdesc/mask): 0 quads skipped (all mask=1), 232/232 + 304/304 tiles byte-clean vs whole-part detwiddle truth. So the "cape renders wrong" symptom is NON-texel: render_frame satellite geometry/anchor, palette selection, draw-time texU mirror, or z-order/blend — OR an uncaptured pose (typhoon multi-instance sel 0xDEC=3564; band/band2 top out at sel 3017). Needs a Storm-cape-with-engine-VRAM capture (capture_break + re-arm ASMTRACE) to isolate; NOT a fabricated texel fix.',
  status='open', confidence='high', date='2026-07-10';

RELATE finding:storm_cape_texel_clean->cites->source:zz_realblob_gate;
