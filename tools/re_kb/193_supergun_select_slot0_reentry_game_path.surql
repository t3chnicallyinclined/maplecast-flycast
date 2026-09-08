-- 193_supergun_select_slot0_reentry_game_path.surql -- the select re-entry roster carry-over done
-- through the game's own slot-entry path, settled 2026-09-06 with a headless gate (400 ticks, both
-- sides AUTO): cursor placement from the cid table, the per-side AUTO flag, the by-hand fighter
-- bytes retired, and the fact that the select anchor itself carries the capture session's team in
-- config+0x6B4. Forward slashes only in ref/note; no apostrophes.
USE NS re DB kb;

UPSERT source:supergun_slot0_reentry_gate_20260906 SET kind='reproduction', strength='reproduction',
  ref='GhidraMCP decompile of FUN_140629dc0 (team-slot entry) and FUN_14073ed90 (cursor cell from cid); vs-01 anchor bytes read from d3dcap/ttd/runs/vs-01/pre exe_image.bin + blk.bin; runner supergun/supersoaker/runner/sg_runner/src/main.rs carry_roster_into_select / reenter_probe_tick / --rematch-probe; headless runs scratchpad rp_same300 (400 ticks, mask 3), rp_p1same (mask 1), rp_fresh (no roster) with per-tick blk dumps.',
  note='Gate numbers below are tick indexes after the re-entry in rp_same300 read out of blk_tNNN.bin: G+1..4, side outer state fighter+4, picked bits blk+0x33a0a/0b, hovered ids fighter+0x6C0.';

UPSERT finding:select_slot_entry_places_cursor_from_cid SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='Team-slot entry FUN_140629dc0 places the cursor from the hovered cid through FUN_14073ed90 and the cid-to-cell table DAT_14097fb80 (x at cid*2, y at cid*2+1) into blk+0x33a0c+side / blk+0x33a0e+side',
  statement='FUN_140629dc0(side object): +4 = 4; +0x28 = fighter pointer for i = side + 2*slot; if (blk+0x33a0a+side & 7) == 7 then outer 9 with slot 2 and all three fighters respawned (all characters already picked). Otherwise fighter_i+0x001 = fighter_i+0x6C0 = DAT_140a6fad0[(side*3 + k)*4] (k = the first slot whose DAT_140a6fab8 mask hits blk+0x33a28+side*8 when blk+0x33a15 == 0, else 0), fighter_i+0x655 = 0; then when config+0x840 == 2: a rematch id equal to an already-placed same-side slot is cleared to -1, and if config+0x6B4+i*4 >= 0 it overrides +0x6C0/+0x001 with that cid and +0x655 with config+0x694+i*4. FUN_14073ed90 then writes blk+0x33a0c+side = DAT_14097fb80[cid*2] and blk+0x33a0e+side = DAT_14097fb80[cid*2+1] (vs-01: Venom 14 -> (5,5), Magneto 44 -> (5,1), Ruby Heart 19 -> (3,3), Cable 23 -> (4,3)), the deferred preview upload is requested (0x142edf543 = 1, 0x142edf546 |= side+1), FUN_140616730/FUN_140616960 build the pages, DAT_142eeea40[i] = cid, blk+0x33a3c+side = slot+1, FUN_14073ec70(side, slot).',
  consequence='Re-running slot entry is the complete, game-owned way to put a seat on a given character: cursor, hovered id, assist preset, preview cache and upload all come from one call. Writing fighter+0x6C0 by hand is not (the hovered id is derived from the cursor cell every frame, re_kb 191).';

UPSERT finding:supergun_select_anchor_carries_capture_roster SET status='confirmed', confidence='high', date='2026-09-06',
  title='The vs-01 select anchor was captured on a rematch re-entry: its exe_image.bin holds config+0x840 = 2 and config+0x6B4 = [14, 23, 1, 22, 20, 6] (P1 Venom/Zangief/SonSon, P2 Cable/Marrow/Cyclops) -- the cursor trail every fresh start showed',
  statement='Read straight out of d3dcap/ttd/runs/vs-01/pre: exe_image.bin at 0x140ac6d40 + 0x840 = 2, + 0x6B4.. = [14, 23, 1, 22, 20, 6], + 0x6D4.. = 0x40 x6 (LK words), + 0x694.. = 0 x6, + 0x690 = 1, + 0x7D0 = 1, + 0x7E0 = 0, + 0x828 = 0, + 0x80C = 1, + 0x7C0 = 1; blk.bin: fighter+0x6C0 = [14, 23, 20, 22, 21, 6] (slot 0 already preloaded from config, slots 1/2 still on the default table because their entry has not run), side outer/inner = (6, 1) both, slot 0, cursor P1 (5,5) P2 (4,3), blk+0x33a5c = 0/0, blk+0x33a0a = 0/0, blk+0x33a68 = 0/0, timer blk+0x33a54 = 3600. The previous team therefore lives in the exe .data config block (game_state), not in blk and not in the arc; the arc holds no team state (re_kb 192).',
  consequence='A shell that wants a fresh select from this anchor writes config+0x6B4 = -1 x6, +0x694 = 0, +0x6D4 = 0x200 and re-runs slot-0 entry for both sides; slot entry then takes the default table DAT_140a6fad0 = {19 Ruby Heart, 20 SonSon, 21 Amingo / 23 Cable, 22 Marrow, 6 Cyclops} (rp_fresh: hover [19, 23, 20, 22, 21, 6], cursors (3,3) / (4,3)).';

UPSERT finding:supergun_select_reentry_through_slot0_entry SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='Select re-entry recipe that locks a SAME-TEAM seat and carries a CHANGE seat, all through game code: roster mirrors + per-side AUTO flag blk+0x33a5c + replay of the inner-3 transition for slot 0 (*(u16*)(side+5) = 0, side+4 = 3); gated headless 400 ticks',
  statement='After the anchor restore and the config reload (which brings the capture roster back): config+0x6B4+i*4 = cid_i, config+0x694+i*4 = assist_i, config+0x6D4+i*4 = word_i (the confirm words snapshotted from config+0x6D4 at match start; FUN_14062b7b0 maps word to costume), blk+0x33a5c+side = 1 for a SAME seat and 0 for a CHANGE seat, then for both sides *(u16*)(fighter_side+5) = 0 and fighter_side+4 = 3 (the game own inner-3 -> next-slot transition, replayed for slot 0). Gate rp_same300 (both AUTO, roster [44, 23, 50, 22, 16, 6], words 0x200): t1 outer 4 (FUN_140629dc0 ran: hover [44, 23, ..], cursors (5,1)/(4,3)), t2 outer 5, t3 outer 6, t5 picked 0x01 (choose auto-confirmed), t23 picked 0x11 (assist auto-confirmed), t74 outer 3 (slot 1 entry), t75 hover slot 1 = 50 cursor (7,2), t97 picked 0x33, t148 slot 2 entry, t149 hover slot 2 = 16, t171 picked 0x77, t232 outer 10 both (DONE), t233 G+3 = 4, t234..t355 select outro G+4 = 1..4 then G+3 = 6, t356 G+2 = 2 (match load) with fighter+0x6C0 = [44, 23, 50, 22, 16, 6] and +0x6C1 = 0 x6 (word 0x200 = LP = costume 0). Gate rp_p1same (mask 1): P1 runs the same chain; P2 sits in choose (6/1) on Cable with cursor (4,3), auto 0, picked 0, waiting for input. Gate rp_fresh: default table, both seats in choose.',
  consequence='Runner: carry_roster_into_select(same) replaces the by-hand fighter+0x6C0/+0x001/+0x655 writes of re_kb 192 (those were clobbered by choose recomputing the hovered id from the cursor cell -- the operator saw P1 back on Venom/SonSon/Amingo). Timing is the game own: about 75 ticks per slot, 60 ticks from all-picked to DONE, 123 ticks of outro. Not yet gated live with a human on the CHANGE seat.';

RELATE finding:select_slot_entry_places_cursor_from_cid->cites->source:supergun_slot0_reentry_gate_20260906 SET id='e193_cursor_src';
RELATE finding:select_slot_entry_places_cursor_from_cid->about->steam_routine:FUN_140629dc0 SET id='e193_cursor_fn';
RELATE finding:select_slot_entry_places_cursor_from_cid->cites->finding:supergun_rematch_roster_slot_entry SET id='e193_cursor_192';
RELATE finding:supergun_select_anchor_carries_capture_roster->cites->source:supergun_slot0_reentry_gate_20260906 SET id='e193_anchor_src';
RELATE finding:supergun_select_anchor_carries_capture_roster->cites->finding:supergun_rematch_roster_slot_entry SET id='e193_anchor_192';
RELATE finding:supergun_select_reentry_through_slot0_entry->cites->source:supergun_slot0_reentry_gate_20260906 SET id='e193_recipe_src';
RELATE finding:supergun_select_reentry_through_slot0_entry->cites->finding:select_same_team_auto_confirm SET id='e193_recipe_191a';
RELATE finding:supergun_select_reentry_through_slot0_entry->cites->finding:select_hand_written_fighter_bytes_are_clobbered SET id='e193_recipe_191b';
RELATE finding:supergun_select_reentry_through_slot0_entry->cites->finding:select_slot_entry_places_cursor_from_cid SET id='e193_recipe_cursor';
RELATE finding:supergun_select_reentry_through_slot0_entry->about->steam_routine:FUN_140629dc0 SET id='e193_recipe_fn';

UPDATE finding:select_slot_entry_places_cursor_from_cid SET status='confirmed';
UPDATE finding:supergun_select_anchor_carries_capture_roster SET status='confirmed';
UPDATE finding:supergun_select_reentry_through_slot0_entry SET status='confirmed';
