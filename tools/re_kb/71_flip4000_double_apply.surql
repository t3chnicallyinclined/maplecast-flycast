USE NS re DB kb;

-- ============================================================================
-- 2026-07-10 — Storm Lightning-Strike "flipped/mirrored/wrong-spot" body =
-- the per-record 0x4000 flip applied TWICE (draw-time texU mirror + a spurious
-- storage-column reversal in the native carve). Confirms re_kb/24.
-- ============================================================================

UPSERT source:storm_native_gate SET kind='measurement',
  ref='tools/render-replica-poc/_storm_native_gate.mjs (runs the REAL production ensureBodyTextures w/ quadMirror+quadSrcDesc, per-tile byte-compare vs engine verbatim-DMA VRAM); torture.mcrr Storm cid42 + rocket.mcrr; _storm_ls_dump.mjs per-tile structure',
  note='Lightning Strike (re_kb/63 Lightning Attack) plays as pre-drawn sprite frames sid 0x1ea->0x1eb->0x1f0->0x1f1 at BOTH facings — ALL quads axis-aligned (0 rotated/sheared), so the perceived ~90deg upright->flying "rotation" is pre-drawn art, NOT a transform. Flying-pose sid0x1f1 uses native part sel0x35d 64x64 (2x2) with per-record 0x4000 SET (srcdesc fl=3). Pre-fix: sel0x35d fac1 52/52 WRONG, fac0 48/48 WRONG. Post-fix: 52/52 + 48/48 EXACT vs engine VRAM. No regression: 100+ non-flip native parts unchanged; every flip4000 native part across the roster (sel0xdcc/dcf/dd7/ddc-4x4/de0/de6/df2/dfc) CUR-BAD->fix-ok, 0 cur-ok->FIX-BAD.';

UPSERT finding:flip4000_double_apply_carve SET
  statement='CONFIRMED + FIXED 2026-07-10. The per-record GFX2 flag 0x4000 is a DRAW-TIME texU mirror ONLY (loc_8c0346c4 neg-r8 texture-U flip; facing XOR 0x4000), NEVER a storage re-store — per re_kb/24 (per_side_storage_col_reverses) storage is facing-INDEPENDENT. The bodytex=local native-chunk carve (body_decoder.mjs ensureBodyTextures) applied 0x4000 TWICE: once correctly as render_frame_quad_mirror (the texU flip), and again WRONGLY as a storage-column reversal col=(dfl&2)?(pCols-1-cc):cc feeding twTileYFirst(col,...). For flip4000 native parts this swapped the tile columns [2,3,0,1] -> tiles land mirrored/wrong-spot. VISIBLE on Storm Lightning-Strike flying pose (sel0x35d 64x64 flip4000); the geometry gate (_tx_detect, positions only) was BLIND to it because quad positions were engine-clean — only the per-tile CONTENT/orientation was wrong. FIX: on the native (m=32 twiddle-chunk) path use the RAW storage column (ncol = dcx % pCols; no 0x4000 reversal); the reversed col stays for the LINEAR (sub-32 satellite) path (untouched — TEXFIX2 reversal there is unvalidated, re_kb/24 says likely spurious too but not gated). DEPLOYED browser body_decoder.mjs ?v=scene10. C++ twins maplecast_mirror.cpp gstaDecodeBodies + texel_gate.cpp source-only (need native rebuild). SEPARATE pre-existing gap (NOT this fix): sid0x61 sel0x316 128x256 4x8 flip4000=n multi-instance = 32/64 WRONG before AND after (distinct non-flip 4x8/multi-instance carve gap, OPEN).',
  status='confirmed', confidence='high', date='2026-07-10';

RELATE finding:flip4000_double_apply_carve->cites->source:storm_native_gate;

UPSERT finding:multiinstance_4x8_carve_gap SET
  statement='OPEN (2026-07-10, surfaced by _storm_native_gate). sid0x61 sel0x316 128x256 (4x8, flip4000=n, MULTI-INSTANCE) decodes 32/64 tiles WRONG identically before AND after the flip4000 fix — a DISTINCT native-chunk carve gap for non-flip 4x8/multi-instance parts (2+ satellite nodes drawing the same (gfx1,sel) collapse the rank grid / the twTileYFirst chunk key needs the per-instance disambiguation). Unrelated to the flip4000 double-apply. Needs its own capture + gate.',
  status='open', confidence='medium', date='2026-07-10';

RELATE finding:multiinstance_4x8_carve_gap->cites->source:storm_native_gate;
