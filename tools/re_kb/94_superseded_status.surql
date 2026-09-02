-- ============================================================================
-- 94: a claim that has been superseded must SAY it has been superseded
--
-- The moment 93 made the supersession graph queryable, it answered a question
-- nobody could ask before -- and the answer was bad:
--
--   SELECT id FROM finding WHERE count(<-supersedes) > 0 AND status='confirmed'
--   -> 10 rows
--
-- Ten findings had a newer claim explicitly superseding them and were still
-- reading as `confirmed`. Traversal returned them, the PreCompact digest
-- carried them as settled fact, and the hook offered them as current
-- knowledge. This is the precise failure the status vocabulary exists to
-- prevent, and it was invisible for as long as the supersession lived in
-- prose.
--
-- 77: "superseded -- we believed this, we were wrong, here is what it actually
-- was." The row STAYS, with its statement and its date. A superseded claim is
-- a lead, not garbage: it records what was believed and why it was replaced.
--
-- CORRECTS DOES NOT DEMOTE. Only `supersedes` does. `corrects` is the narrower
-- relation -- one detail was wrong, the rest stands -- so a corrected finding
-- stays confirmed. Demoting on `corrects` would retire a mostly-true claim
-- over a footnote.
--
-- Apply: PYTHONIOENCODING=utf-8 python tools/re_kb/apply_seed.py \
--            tools/re_kb/94_superseded_status.surql
-- ============================================================================
USE NS re DB kb;

UPDATE finding:hud_name_garble_is_texture_decode_not_order SET status='superseded',
  superseded_why='Superseded by finding:hud_pal4_decode_is_correct_residual_is_vram_timing (2026-09-01 status sync: the edge existed, the status did not follow it).';
UPDATE finding:hud_sprite_client_fix_shipped SET status='superseded',
  superseded_why='Superseded by finding:hud_sprite_engine_order_replaces_heuristic.';
UPDATE finding:hud_sprite_engine_order_replaces_heuristic SET status='superseded',
  superseded_why='Superseded by finding:hud_name_garble_is_texture_decode_not_order.';
UPDATE finding:hudq_empty_root_cause SET status='superseded',
  superseded_why='Superseded by finding:hudq_now_live_supersedes_empty.';
UPDATE finding:replica_live_hud SET status='superseded',
  superseded_why='Superseded by finding:replica_live_hud_angled_layout -- the first step of the HUD chain.';
UPDATE finding:replica_live_hud_angled_layout SET status='superseded',
  superseded_why='Superseded by finding:replica_live_hud_real_art.';
UPDATE finding:replica_live_hud_garble_fix SET status='superseded',
  superseded_why='Superseded by finding:replica_live_hud_real_ta.';
UPDATE finding:replica_live_hud_real_art SET status='superseded',
  superseded_why='Superseded by finding:replica_live_hud_garble_fix.';
UPDATE finding:wide_part_tile_storage_order SET status='superseded',
  superseded_why='Superseded by finding:faithful_texture_decode_transpile (2026-06-14) -- the edge was modelled in 93 from the row\'s own prose.';
UPDATE finding:wide_part_tile_storage_order_v2 SET status='superseded',
  superseded_why='Superseded by finding:faithful_texture_decode_transpile (2026-06-14).';

-- The HUD chain above is worth reading as a chain: replica_live_hud ->
-- angled_layout -> real_art -> garble_fix -> real_ta is five generations of
-- the same claim, and until now every one of them read as `confirmed`
-- simultaneously. Anyone asking the graph "how does the HUD work?" got five
-- answers with no way to tell which was current.
