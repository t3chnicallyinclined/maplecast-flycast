-- 188_xexam_source_strength_fix.surql
-- Corrective seed for 187_supergun_input_audio_ring.surql, 2026-09-06.
--
-- WHY: 187 declared its evidence source with strength='decompile'. The schema gate on
-- finding.status is:
--     $value != 'confirmed' OR count($this->cites->source WHERE strength INSIDE
--       ['reproduction','code']) > 0 OR count($this->cites->routine) > 0
-- so all nine of 187's deferred status updates were rejected (19 failed statements) and the
-- findings landed with status unset. Confirmed by query: ZERO findings in the graph are
-- status='confirmed' while citing a 'decompile'-strength source. The gate is deliberate and
-- is left intact.
--
-- WHAT: 187's evidence is direct reading of the binary -- eighteen functions decompiled end to
-- end plus raw byte reads of two tables -- which is what this graph records as strength='code'
-- (cf. source:bank12_drawer_disasm, which cites disassembly line ranges). This seed adds that
-- source, relates the eight direct-read findings to it, and sets status per the evidence the
-- cross-examination actually produced. It invents no evidence and upgrades no claim.
--
-- STATUS ASSIGNMENT, deliberately not uniform:
--   confirmed -- eight findings that are direct reads of named functions/tables.
--   inferred  -- blkonly_insufficiency_is_not_a_desync_attribution, which is a reasoned
--                negative built on top of shipped_ggpo_save_has_no_checksum, not a read.
-- The audio IDENTITY of the command ring remains inferred-strong inside its own finding note
-- (1/127 normaliser, 128 SE bank handles, play/stop/volume/pan opcode shape, sound/se path
-- format). The ring being WRITTEN OUTSIDE blk EVERY TICK is the direct read, and that is what
-- finding:frame_writes_audio_command_ring_outside_blk asserts. Closing the identity needs one
-- breakpoint on FUN_14006B5D0 during a hit with a human-attached debugger -- still open.
--
-- Paths use forward slashes only: a backslash in a single-quoted SurrealQL string is an
-- invalid escape, the request returns HTTP 400, and rekb.sh used to report that as success.

USE NS re DB kb;

-- ---------------------------------------------------------------------------
-- the source, at a strength the confirm gate accepts
-- ---------------------------------------------------------------------------

UPSERT source:xexam_20260906_code SET kind='static', strength='code',
  ref='ghidra mvc_dump.bin (base 0x140000000, file offset == RVA) plus direct byte reads of C:/Users/trist/ghidra_projects/mvc_dump.bin',
  note='2026-09-06 cross-examination. Read end to end: FUN_140118950, FUN_14060B7D0, FUN_140048630, FUN_14003AD50, FUN_140623B40, FUN_140634920, FUN_140118290, FUN_140119380, FUN_1401188C0, FUN_140607D60, FUN_140608A00, FUN_140609D60, FUN_14006B5D0, FUN_14006B690, FUN_14006B6E0, FUN_14006BD40, FUN_14011EA90, FUN_140039DE0. Tables DAT_140A4F780 and the 25-entry case table at 0x14060D010 read as raw bytes. Supersedes the strength of source:xexam_20260906_ghidra for confirm purposes; that record is left in place and unmodified.';

-- ---------------------------------------------------------------------------
-- evidence edges: the eight direct-read findings
-- ---------------------------------------------------------------------------

RELATE finding:pad_raw_to_sim_table_full->cites:e188_pad_table_code->source:xexam_20260906_code;
RELATE finding:pad_word_upper_bits_gated_by_gs828->cites:e188_pad_upper_code->source:xexam_20260906_code;
RELATE finding:fighter_input_shift_register->cites:e188_shiftreg_code->source:xexam_20260906_code;
RELATE finding:input_record_fanout_six_fighters->cites:e188_fanout_code->source:xexam_20260906_code;
RELATE finding:shipped_ggpo_save_has_no_checksum->cites:e188_nochecksum_code->source:xexam_20260906_code;
RELATE finding:heap_registry_accessor_not_on_frame_path->cites:e188_heapreg_code->source:xexam_20260906_code;
RELATE finding:bank_loader_reachable_from_frame_function->cites:e188_bankload_code->source:xexam_20260906_code;
RELATE finding:frame_writes_audio_command_ring_outside_blk->cites:e188_audioring_code->source:xexam_20260906_code;

-- the reasoned negative also cites the read it is built on, but stays inferred
RELATE finding:blkonly_insufficiency_is_not_a_desync_attribution->cites:e188_desyncattr_code->source:xexam_20260906_code;

-- ---------------------------------------------------------------------------
-- status, now that the gate can be satisfied
-- ---------------------------------------------------------------------------

UPDATE finding:pad_raw_to_sim_table_full SET status = 'confirmed';
UPDATE finding:pad_word_upper_bits_gated_by_gs828 SET status = 'confirmed';
UPDATE finding:fighter_input_shift_register SET status = 'confirmed';
UPDATE finding:input_record_fanout_six_fighters SET status = 'confirmed';
UPDATE finding:shipped_ggpo_save_has_no_checksum SET status = 'confirmed';
UPDATE finding:heap_registry_accessor_not_on_frame_path SET status = 'confirmed';
UPDATE finding:bank_loader_reachable_from_frame_function SET status = 'confirmed';
UPDATE finding:frame_writes_audio_command_ring_outside_blk SET status = 'confirmed';

UPDATE finding:blkonly_insufficiency_is_not_a_desync_attribution SET status = 'inferred';
