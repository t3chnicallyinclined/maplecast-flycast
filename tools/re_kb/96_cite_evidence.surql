-- ==========================================================================
-- 96: evidence ON the cites->routine edge (GENERATED, re-runnable)
--
-- `finding:x -> cites -> routine:loc_8c0344d4` asserted only that the
-- claim MENTIONS that PC. A LOCATOR IS NOT A CITATION. This lifts the
-- bank:line and the quoted instruction the finding ALREADY names next to
-- that PC, verbatim, onto the edge.
--
-- Nothing is invented. An edge whose finding does not supply a bank:line
-- or an instruction near the PC is LEFT ALONE and listed at the bottom --
-- writing the routine's own bank_lines there would be the same locator
-- wearing a different field name.
-- ==========================================================================
USE NS re DB kb;

UPDATE cites SET shows = 'mov.l r8,@r6' WHERE record::id(in) = '3d_draw_emit_map' AND record::id(out) = 'loc_8c129cc0';
UPDATE cites SET at = 'bank03:10218' WHERE record::id(in) = 'body_walker_tiling' AND record::id(out) = 'loc_8c0344d4';
UPDATE cites SET at = 'bank12:7677' WHERE record::id(in) = 'effect_poly_structure' AND record::id(out) = 'loc_8c129cc0';
UPDATE cites SET at = 'bank03:12661' WHERE record::id(in) = 'faithful_lzss_transpile' AND record::id(out) = 'loc_8c0354c0';
UPDATE cites SET at = 'bank03:10232-10243' WHERE record::id(in) = 'gsta_sprite_id_sampling_phase' AND record::id(out) = 'loc_8c0344d4';
UPDATE cites SET at = 'bank03:11567' WHERE record::id(in) = 'gsta_sprite_id_sampling_phase' AND record::id(out) = 'loc_8c034dee';
UPDATE cites SET at = 'bank03:1200' WHERE record::id(in) = 'gsta_wire_ext_completeness_2026_06_11' AND record::id(out) = 'loc_8c0308c2';
UPDATE cites SET at = 'bank03:11567' WHERE record::id(in) = 'gsta_wire_field_audit' AND record::id(out) = 'loc_8c0344d4';
UPDATE cites SET at = 'bank03:11567' WHERE record::id(in) = 'gsta_wire_field_audit' AND record::id(out) = 'loc_8c034dee';
UPDATE cites SET at = 'bank03:10357-10361' WHERE record::id(in) = 'inner_tile_loop_descriptor' AND record::id(out) = 'loc_8c0344d4';
UPDATE cites SET at = 'bank04:12166', shows = 'mov 4,r3' WHERE record::id(in) = 'objpool_list_system_complete' AND record::id(out) = 'loc_8c04515e';
UPDATE cites SET at = 'bank03:191-226' WHERE record::id(in) = 'objs_effect_blend_gap' AND record::id(out) = 'loc_8c0301f6';
UPDATE cites SET at = 'bank03:191-226' WHERE record::id(in) = 'objs_effect_blend_gap' AND record::id(out) = 'loc_8c0305d8';
UPDATE cites SET at = 'bank12:23318-23337' WHERE record::id(in) = 'p3d_emit_slot_is_listtype' AND record::id(out) = 'loc_8c129cc0';
UPDATE cites SET at = 'bank03:10477' WHERE record::id(in) = 'sprite_id_bit15_dispatch' AND record::id(out) = 'loc_8c0d97ee';
UPDATE cites SET at = 'bank03:10247' WHERE record::id(in) = 'storm_tilecount_authoritative' AND record::id(out) = 'loc_8c0344d4';
UPDATE cites SET at = 'bank03:10661-10668' WHERE record::id(in) = 'tile_grid_is_per_pose_static' AND record::id(out) = 'loc_8c03478e';

-- ---- 94 edges whose finding supplies no bank:line or instruction ----------
--   finding:body_walker_y_anchor                         -> routine:loc_8c0344d4
--   finding:bug_rocket_emitter_far_heuristic             -> routine:loc_8c030af8
--   finding:carve_tile_pitch_m                           -> routine:loc_8c0344d4
--   finding:carve_wide_square_twiddle_fix                -> routine:loc_8c0344d4
--   finding:client_missing_facing_texu_mirror            -> routine:loc_8c03453a
--   finding:client_missing_facing_texu_mirror            -> routine:loc_8c034548
--   finding:client_missing_facing_texu_mirror            -> routine:loc_8c0346c4
--   finding:descriptor_table_runtime_built               -> routine:loc_8c0344d4
--   finding:effect_poly_structure                        -> routine:loc_8c027d2c
--   finding:emitter_audit_2026_06_10                     -> routine:loc_8c035162
--   finding:faithful_texture_decode_transpile            -> routine:loc_8c033d78
--   finding:faithful_texture_decode_transpile            -> routine:loc_8c0354c0
--   finding:faithful_verbatim_vram_write                 -> routine:loc_8c033d78
--   finding:flip4000_double_apply_carve                  -> routine:loc_8c0346c4
--   finding:gfx1_decode_equals_vram                      -> routine:loc_8c033d78
--   finding:gfx1_decode_equals_vram                      -> routine:loc_8c0354c0
--   finding:gsta_draw_layer_per_slot                     -> routine:loc_8c0308c2
--   finding:gsta_hud_bar_magenta_correct                 -> routine:loc_8c15ffb0
--   finding:gsta_super_effect_overtile                   -> routine:loc_8c033b0a
--   finding:gsta_super_effect_overtile                   -> routine:loc_8c0344d4
--   finding:gsta_wire_ext_completeness_2026_06_11        -> routine:loc_8c0344d4
--   finding:gsta_wire_ext_completeness_2026_06_11        -> routine:loc_8c034606
--   finding:gsta_wire_ext_completeness_2026_06_11        -> routine:loc_8c0347bc
--   finding:gsta_wire_ext_completeness_2026_06_11        -> routine:loc_8c0348c8
--   finding:gsta_wire_ext_completeness_2026_06_11        -> routine:loc_8c034bea
--   finding:gsta_wire_ext_completeness_2026_06_11        -> routine:loc_8c034dee
--   finding:gsta_wire_ext_completeness_2026_06_11        -> routine:loc_8c035000
--   finding:hitspark_class_resolved                      -> routine:loc_8c100968
--   finding:hud_portraits_real_vs_procedural             -> routine:loc_8c161fec
--   finding:object_pool_spawn_chain                      -> routine:loc_8c044f12
--   finding:objpool_list_system_complete                 -> routine:loc_8c0308c2
--   finding:overlays_are_reconstructable_zero_bw         -> routine:loc_8c15ffb0
--   finding:per_side_facing_fix                          -> routine:loc_8c0346c4
--   finding:per_side_storage_col_reverses                -> routine:loc_8c0344d4
--   finding:pure_state_textures                          -> routine:loc_8c0344d4
--   finding:pure_state_textures                          -> routine:loc_8c0354c0
--   finding:render_calltree_scope                        -> routine:loc_8c0308c2
--   finding:render_extra                                 -> routine:loc_8c034dee
--   finding:render_replica_option_c_validated            -> routine:loc_8c0344d4
--   finding:render_replica_option_c_validated            -> routine:loc_8c11e460
--   finding:render_replica_scramble_fix                  -> routine:loc_8c033d78
--   finding:render_replica_scramble_fix                  -> routine:loc_8c0344d4
--   finding:render_replica_scramble_fix                  -> routine:loc_8c0354c0
--   finding:render_replica_sh4_little_endian             -> routine:loc_8c03093c
--   finding:render_replica_ta_emit                       -> routine:loc_8c0344d4
--   finding:render_replica_ta_emit                       -> routine:loc_8c1216c0
--   finding:render_replica_ta_emit                       -> routine:loc_8c1246da
--   finding:render_replica_ta_emit                       -> routine:loc_8c1248ea
--   finding:replica_effect_overemit_sel_ff               -> routine:loc_8c0344d4
--   finding:replica_effect_walker_faithful               -> routine:loc_8c030af8
--   finding:replica_effect_walker_faithful               -> routine:loc_8c0344d4
--   finding:replica_emitter_effects_fx_atlas             -> routine:loc_8c030af8
--   finding:replica_emitter_sat_garble_flicker           -> routine:loc_8c030af8
--   finding:replica_emitter_sat_garble_flicker           -> routine:loc_8c0344d4
--   finding:sprite_id_bit15_dispatch                     -> routine:loc_8c0344d4
--   finding:sprite_id_bit15_dispatch                     -> routine:loc_8c0348c8
--   finding:sprite_id_bit15_dispatch                     -> routine:loc_8c034bea
--   finding:storm_move_spawn_catalog                     -> routine:loc_8c04f478
--   finding:storm_move_spawn_catalog                     -> routine:loc_8c05115a
--   finding:storm_move_spawn_catalog                     -> routine:loc_8c09b988
--   ... +34 more
