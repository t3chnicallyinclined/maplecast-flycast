-- ============================================================================
-- 92: curate the ADDRESS SPINE -- the memory map everything else hangs off
--
-- Measured 2026-09-01: the `address` table was 77% doc-derived (369 of 481
-- rows were markdown scrapes), and NOT ONE of the six addresses this project
-- uses most had a curated row:
--
--   0x8C268340  P1C1 struct base        0 curated rows
--   0x8C289624  in_match                0
--   0x8C2895E0 / 0x8C287DE0  slot table 0
--   0x8C3496B0  frame_counter           0
--   0x8C2681DC  Input_DEC               0
--
-- The foundational layer was never entered as knowledge -- it was inherited
-- from doc scraping, and nobody noticed because the doc rows made the table
-- LOOK populated. That is why the PreToolUse hook answered a lookup for the
-- slot table with "(doc) CLAUDE.md#Global game state" instead of a name.
--
-- An `address` row is an ARTIFACT, not a claim: a label for a location. It
-- carries no status and is not gated by 88. The CLAIMS about these addresses
-- are separate `finding` rows, which is why each row here also gets an
-- `about` edge from the finding that establishes it, where one exists.
--
-- Ids are (what it is), not (address, whichever doc mentioned it) -- the doc
-- rows are keyed `doc_0x8c268340_claude_md`, which is why 481 rows covered far
-- fewer distinct addresses.
--
-- Apply: PYTHONIOENCODING=utf-8 python tools/re_kb/apply_seed.py \
--            tools/re_kb/92_address_spine.surql
-- ============================================================================
USE NS re DB kb;

-- ---- the six character structs (page 616, stride 0x5A4) --------------------
UPSERT address:char_p1c1 SET addr='0x8C268340', name='P1C1 char struct base',
  owner='char_struct', kind='ram_abs',
  note='Player 1 character 1. The six fighter structs are contiguous at stride 0x5A4: P1C1 0x8C268340, P2C1 0x8C2688E4, P1C2 0x8C268E88, P2C2 0x8C26942C, P1C3 0x8C2699D0, P2C3 0x8C269F74. Field offsets hang off struct:char_struct.';
UPSERT address:char_p2c1 SET addr='0x8C2688E4', name='P2C1 char struct base', owner='char_struct', kind='ram_abs', note='stride 0x5A4 from P1C1.';
UPSERT address:char_p1c2 SET addr='0x8C268E88', name='P1C2 char struct base', owner='char_struct', kind='ram_abs', note='stride 0x5A4.';
UPSERT address:char_p2c2 SET addr='0x8C26942C', name='P2C2 char struct base', owner='char_struct', kind='ram_abs', note='stride 0x5A4.';
UPSERT address:char_p1c3 SET addr='0x8C2699D0', name='P1C3 char struct base', owner='char_struct', kind='ram_abs', note='stride 0x5A4.';
UPSERT address:char_p2c3 SET addr='0x8C269F74', name='P2C3 char struct base', owner='char_struct', kind='ram_abs', note='stride 0x5A4.';

-- ---- the draw list (the one that keeps getting re-derived) -----------------
UPSERT address:slot_count_table SET addr='0x8C2895E0', name='slot/layer COUNT table',
  owner='render', kind='ram_abs',
  note='u8[layer] = node count for that layer, 16 layers. Walked by Render_sprites loc_8c0308c2 together with the pointer table at 0x8C287DE0 (u32 node ptrs, row stride 0x180). THIS IS THE DRAW LIST: the engine renders only the char-base nodes PRESENT in it -- it does not iterate the six char structs. Layers ascend 0->15, so a lower layer draws first (behind).';
UPSERT address:slot_ptr_table SET addr='0x8C287DE0', name='slot/layer NODE-POINTER table',
  owner='render', kind='ram_abs',
  note='u32 node pointers, row stride 0x180, indexed [layer][i]. Paired with the count table at 0x8C2895E0. 0xFF is the sentinel.';

-- ---- global game state (page 649) -----------------------------------------
UPSERT address:in_match SET addr='0x8C289624', name='in_match flag', owner='global', kind='ram_abs',
  note='Non-zero during a real match. The dataset exporter gates on this so it never records idle frames; the DUMP_RAM trigger checks it too.';
UPSERT address:match_sub_state SET addr='0x8C289621', name='match_sub_state', owner='global', kind='ram_abs';
UPSERT address:round_counter SET addr='0x8C28962B', name='round_counter', owner='global', kind='ram_abs';
UPSERT address:game_timer SET addr='0x8C289630', name='game timer', owner='global', kind='ram_abs';
UPSERT address:stage_id SET addr='0x8C289638', name='stage_id', owner='global', kind='ram_abs';
UPSERT address:p1_meter_fill SET addr='0x8C289646', name='p1_meter_fill (u16)', owner='global', kind='ram_abs';
UPSERT address:p1_combo SET addr='0x8C289670', name='p1_combo (u16)', owner='global', kind='ram_abs';
UPSERT address:frame_counter SET addr='0x8C3496B0', name='frame_counter (u32)', owner='global', kind='ram_abs',
  note='The engine frame number. Used to align captures across the mirror wire, ASMTRACE and the replica -- if two artifacts disagree about a frame, this is the arbiter.';

-- ---- input ----------------------------------------------------------------
UPSERT address:input_dec_p1 SET addr='0x8C2681DC', name='Input_DEC P1 (latched input)',
  owner='input', kind='ram_abs',
  note='The CLEAN latched controller value, stride 0x14 per player (P2 at 0x8C2681F0). MVC2 latches input EXACTLY ONCE per frame inside the vblank chain, so this -- not a raw port read -- is what every gameplay consumer sees. The .mcrec input log writes a neutral duplicate per frame, which is why per-frame input must be the UNION of presses there; Input_DEC has no such problem.';
UPSERT address:input_dec_p2 SET addr='0x8C2681F0', name='Input_DEC P2 (latched input)', owner='input', kind='ram_abs', note='stride 0x14 from P1.';

-- ---- object pool / arena --------------------------------------------------
UPSERT address:objpool_budget SET addr='0x8C287AE8', name='pool poly/record BUDGET (u16)',
  owner='objpool', kind='ram_abs',
  note='The allocator loc_8c044f12 gates on this with cmp/pl and returns 0 when it is <= 0. A spawn that silently does not happen usually means this hit zero.';
UPSERT address:objpool_list_heads SET addr='0x8C287A5C', name='pool list-head table (base + cat*4)', owner='objpool', kind='ram_abs';
UPSERT address:submit_arena SET addr='0x8C1F9D94', name='per-frame submit arena', owner='render', kind='ram_abs';
UPSERT address:effect_poly_bank SET addr='0x0CED0000', name='Effect Poly bank', owner='render', kind='ram_abs',
  note='Shared hitspark / electric / energy effect models + cell records. Named in work.asm; loaded once at match load by the bank02 loader family.';

-- ---- link the spine to the claims that establish it ------------------------
RELATE finding:slot_table_is_drawlist_not_active->about->address:slot_count_table;
RELATE finding:slot_table_is_drawlist_not_active->about->address:slot_ptr_table;
RELATE finding:gsta_draw_layer_per_slot->about->address:slot_count_table;
RELATE finding:input_latch_once_per_vblank->about->address:input_dec_p1;
RELATE finding:object_pool_spawn_chain->about->address:objpool_budget;
RELATE finding:object_pool_spawn_chain->about->address:objpool_list_heads;
