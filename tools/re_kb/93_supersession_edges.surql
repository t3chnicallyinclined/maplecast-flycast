-- ============================================================================
-- 93: model the supersessions that existed only in prose
--
-- The process review's ONE no-go was that "which claim is current?" could not
-- be answered: roughly 60% of the supersession graph lived in sentences. 91
-- fixed the query side (about was overloaded); this fixes the data side.
--
-- Detected by scanning every finding for a supersede/correct verb followed by
-- a `finding:` id, then filtering:
--
--   * CASE-SENSITIVE verbs. The seeds write the relationship in CAPS. Lowercase
--     "correct" is nearly always the ADJECTIVE -- "the correct client recipe is
--     the one ALREADY shipped (finding:x)" is a compliment, not a correction.
--     Three of nine candidates were exactly that.
--   * DIRECTION from the passive voice. "SUPERSEDED 2026-06-14 by finding:x"
--     means x supersedes THIS row, and the "by" is separated from the verb by a
--     date -- so it has to be looked for in the span BETWEEN verb and id.
--     Missing that produced one pair pointing both ways at once.
--
-- SUPERSEDES vs CORRECTS is not cosmetic. 77 defines corrects as the NARROWER
-- case: one detail was wrong, the rest stands. Two rows below say they
-- supersede "the parenthetical in" and "the engine-TA-pinned texture-binding
-- of" another finding -- that is a detail, not the claim, so they are recorded
-- as `corrects`. Marking them `supersedes` would retire a finding that is
-- still largely true.
--
-- Apply: PYTHONIOENCODING=utf-8 python tools/re_kb/apply_seed.py \
--            tools/re_kb/93_supersession_edges.surql
-- ============================================================================
USE NS re DB kb;

-- ---- full supersessions ----------------------------------------------------
-- "SUPERSEDES finding:wide_part_tile_storage_order", and the v2 row's own text
-- says "SUPERSEDED 2026-06-14 by finding:faithful_texture_decode_transpile".
RELATE finding:faithful_texture_decode_transpile->supersedes->finding:wide_part_tile_storage_order;
RELATE finding:faithful_texture_decode_transpile->supersedes->finding:wide_part_tile_storage_order_v2;

-- ---- corrections (a detail was wrong; the claim stands) --------------------
-- Was a SCALAR FIELD: corrects='finding:carve_nonsquare_linear_fix'. A
-- supersession chain stored in an untraversable string -- the graph could not
-- walk it, so nothing could answer "what replaced the linear carve?".
RELATE finding:carve_nonsquare_yfirst_twiddle->corrects->finding:carve_nonsquare_linear_fix;

-- "CORRECTS the finding:hud_meter_segments_vram_gap"
RELATE finding:hud_name_garble_is_paratype5_sprite->corrects->finding:hud_meter_segments_vram_gap;

-- "SUPERSEDES the PARENTHETICAL in finding:hud_garble_one_truth_clientgrounded"
-- -- a detail, so `corrects`.
RELATE finding:hud_name_garble_is_paratype5_sprite->corrects->finding:hud_garble_one_truth_clientgrounded;

-- "SUPERSEDES the engine-TA-pinned TEXTURE-BINDING of
-- finding:render_replica_byte_exact_proven" -- again a detail of an otherwise
-- standing capstone finding, so `corrects`.
RELATE finding:render_replica_phase1_codederived->corrects->finding:render_replica_byte_exact_proven;
