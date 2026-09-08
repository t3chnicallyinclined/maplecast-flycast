-- 194_supergun_select_slot12_pages_rebuild.surql -- why a CHANGE seat sat under the wrong two banners
-- after select re-entry, and the game-owned rebuild (FUN_14062b130 compare) that fixes it. Settled
-- 2026-09-06 on live run cgeom17 + headless gates rp2_p1same / rp2_same400 / rp2_fresh. Forward
-- slashes only in ref/note; no apostrophes.
USE NS re DB kb;

UPSERT source:supergun_slot12_pages_20260906 SET kind='reproduction', strength='reproduction',
  ref='GhidraMCP decompile of FUN_14062b130 (per-side select frame: preview cache compare + outer-state dispatch) and FUN_14062a1d0 (side entry, outer 0); live run rr_play/cgeom17 (P1 SAME auto-locked, P2 CHANGE showed Cable / Marrow / Cyclops banners while its last team was Cable / Blackheart / Juggernaut, cids 23/53/41, REENTER probe hover [44,23,20,22,21,6] with config+0x6B4 [44,23,50,53,16,41]); headless gates scratchpad rp2_p1same (mask 1), rp2_same400 (mask 3, 400 ticks), rp2_fresh; runner supergun/supersoaker/runner/sg_runner/src/main.rs carry_roster_into_select.',
  note='Character ids: 23 Cable, 53 Blackheart, 41 Juggernaut, 22 Marrow, 6 Cyclops, 44 Magneto, 50 Colossus, 16 Tron (RetroReceipts-agent/agent/char_sigs.json).';

UPSERT finding:select_preview_cache_compare_rebuild SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='FUN_14062b130 (per side, every select frame) rebuilds a slot preview + banner only when DAT_142eeea40[i] >= 0 and != fighter_i+0x6C0, for i = side, side+2, side+4; a cache of -1 never rebuilds, so restored anchor pages for un-entered slots stay whatever the capture session built',
  statement='FUN_14062b130(side object): for i in side, side+2, side+4: cid = fighter_i+0x6C0 (blk+0x4478 + i*0x738); if (DAT_142eeea40[i] >= 0 && DAT_142eeea40[i] != cid) { 0x142edf543 = 1; 0x142edf546 |= side+1; FUN_140616730(i); FUN_140616960(i); DAT_142eeea40[i] = cid }. Then, unless blk+0x33a12 & side_bit, it dispatches PTR_FUN_140a6fb70[outer state fighter_side+4]. FUN_14062a1d0 (outer 0, side entry) sets all three slots of a human side to the default table DAT_140a6fad0 (+0x6C0 = +0x001, +0x655 = 0, +0x613 (0x448b) = slot) and does not touch the cache. The vs-01 anchor holds caches [14, 23, -1, -1, -1, -1] and its dcram.bin holds the pages the capture session last built for slots 1/2 (P2: Marrow / Cyclops) -- so after the runner re-entry those two banners came back on a CHANGE seat regardless of config+0x6B4 (live cgeom17), while an AUTO seat was fine because slot entry rebuilt them within 75 ticks each.',
  consequence='To show a team on slots that have not been entered, use the compare: set fighter_i+0x6C0 = +0x001 = cid and +0x655 = assist (the bytes FUN_140629dc0 writes at that slot entry anyway; nothing recomputes them before it) and park DAT_142eeea40[i] at a non-cid value >= 0 (runner uses 0x7F). Gate rp2_p1same: tick 1 hover [44,23,50,53,16,41], cache [44,23,50,53,16,41], 4 preview uploads (0x810/0x811/0x81c/0x81d); rp2_same400 unchanged (match load at t356 with the roster); rp2_fresh caches settle on [19,23,20,22,21,6]. Not yet confirmed live on screen.';

RELATE finding:select_preview_cache_compare_rebuild->cites->source:supergun_slot12_pages_20260906 SET id='e194_cmp_src';
RELATE finding:select_preview_cache_compare_rebuild->about->steam_routine:FUN_14062b130 SET id='e194_cmp_fn';
RELATE finding:select_preview_cache_compare_rebuild->cites->finding:select_highlight_upload_chain SET id='e194_cmp_188';
RELATE finding:select_preview_cache_compare_rebuild->cites->finding:supergun_select_reentry_through_slot0_entry SET id='e194_cmp_193';

UPDATE finding:select_preview_cache_compare_rebuild SET status='confirmed';
