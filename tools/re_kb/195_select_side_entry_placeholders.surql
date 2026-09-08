-- 195_select_side_entry_placeholders.surql -- the select side-entry states 1 and 2 (placeholder pages,
-- panel object, wait for the panel) and the fresh-start replay the SUPERGUN runner uses. Settled
-- 2026-09-06 from GhidraMCP decompiles + capstone of the exe image + headless gate rp3_fresh.
-- Forward slashes only in ref/note; no apostrophes.
USE NS re DB kb;

UPSERT source:supergun_side_entry_placeholders_20260906 SET kind='reproduction', strength='reproduction',
  ref='GhidraMCP decompile of FUN_14062b060 (outer 1), FUN_140616610 (placeholder pages), FUN_1407404a0 (panel object spawn); capstone disassembly of 0x14062b110 (outer 2, not a Ghidra function) out of vs-01 exe_image.bin; live run rr_play/cgeom18 (first select drew default-table banners on P2 slots 1/2 when the runner parked the caches); headless gate scratchpad rp3_fresh; runner supergun/supersoaker/runner/sg_runner/src/main.rs carry_roster_into_select.',
  note='Outer-state table PTR_FUN_140a6fb70 per re_kb 175: [0] FUN_14062a1d0 entry, [1] FUN_14062b060, [2] 0x14062b110, [3] FUN_140629dc0 slot entry, [6] FUN_140629d10 choose.';

UPSERT finding:select_side_entry_placeholder_pages SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='Select side entry outer 1 (FUN_14062b060) zeroes the panel state, spawns the panel object, builds the three PLACEHOLDER slot pages with FUN_140616610(side) and requests the upload; outer 2 (0x14062b110) waits for panel == 1 then goes to slot entry -- the caches stay -1 so un-entered slots show placeholders, i.e. the empty first select',
  statement='FUN_14062b060(side object): inner 0: +5 = 1, blk+0x33a08+side = 0, FUN_1407404a0(side, 0) (pool object via FUN_14061dbe0(0,5,1): +0x170 = 0, handler LAB_140740410, +0x34 = side, +0x35 = 2, +0xf8 = 0), FUN_140616610(side); inner 1: +5 = 2, 0x142edf543 = 1, 0x142edf546 |= side+1; inner 2 (and 1): when blk+0x33a08+side == 0 -> *(u16*)(+4) = 2. 0x14062b110: if blk+0x33a08+side == 1 then +4 = 3. FUN_140616610(side): for k in 0..3 with sel = DAT_140a6c220[side*3 + k], copies 4 tiles of 0x400 x 8 bytes from the atlas at G+0x94 (source ((k+0x39 & 0xfc) - 0x1800) * 0x8000 + G+0x94 + (DAT_140a6c070[..] << 13)) into the DC-RAM page ((sel & 0xfc) + 0x1b6) * 0x8000 + (DAT_140a6c070[sel & 3] << 13) relative to *(DAT_142ef0ab0 + 8) -- a pure memcpy, no game state. The preview cache DAT_142eeea40 is -1 x6 from FUN_14062bc80, so FUN_14062b130 never rebuilds an un-entered slot (re_kb 194) and the placeholders stand until that slot entry.',
  consequence='A shell that restores a mid-select anchor and wants the EMPTY first select replays this step: FUN_140616610(side) for both sides, request the upload, caches -1 for slots 1/2, then slot-0 re-entry (re_kb 193). The panel object already exists in the anchor and is not respawned. Gate rp3_fresh: cache [19, 23, -1, -1, -1, -1], uploads 0x810/0x81c/0x811/0x81d, hover [19, 23, 20, 22, 21, 6], cursors (3,3)/(4,3). Parking the caches instead (previous runner) drew default-table banners on slots 1/2 (live cgeom18). On-screen confirmation of the placeholders pending.';

RELATE finding:select_side_entry_placeholder_pages->cites->source:supergun_side_entry_placeholders_20260906 SET id='e195_ph_src';
RELATE finding:select_side_entry_placeholder_pages->about->steam_routine:FUN_14062b060 SET id='e195_ph_fn';
RELATE finding:select_side_entry_placeholder_pages->cites->finding:select_preview_cache_compare_rebuild SET id='e195_ph_194';
RELATE finding:select_side_entry_placeholder_pages->cites->finding:supergun_select_reentry_through_slot0_entry SET id='e195_ph_193';
RELATE finding:select_side_entry_placeholder_pages->cites->finding:select_highlight_upload_chain SET id='e195_ph_188';

UPDATE finding:select_side_entry_placeholder_pages SET status='confirmed';
