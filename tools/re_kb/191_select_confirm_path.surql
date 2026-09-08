-- 191_select_confirm_path.surql -- CHARACTER SELECT: the input plumbing (edge vs held words), the game's own SAME-TEAM
-- auto-confirm mechanism (config+0x840 == 2 + config+0x6B4/+0x694/+0x6D4 + blk+0x33a5c), the assist sub-state, the
-- word -> costume rule (FUN_14062b7b0), the DONE chain, and why hand-written fighter bytes get clobbered.
-- 2026-09-06, SUPERGUN lane. Decompiles + capstone of mvc_dump.bin; values from the vs-01 anchor.
USE NS re DB kb;

UPSERT source:select_confirm_decompiles_20260906 SET kind='disasm', strength='code',
  ref='Ghidra dumpproj/mvc_dump.bin: FUN_140619840, FUN_140628d60, FUN_140628c70, FUN_14060b790, FUN_140629d10, FUN_14062b7b0, FUN_14073ed60, FUN_14073e970, FUN_14062a1d0, FUN_140629dc0, FUN_14062aa00, FUN_14062c480, FUN_1407404a0; capstone 0x14062a460..0x14062a712 (assist), 0x14062a090..0x14062a196 (wait), 0x14062b000..0x14062b054 (inner 0 / outer 10), 0x14062a1b0 (outer 9), 0x1407406b0..; tables DAT_140a6fae8/DAT_140a6faf4/DAT_140a6fad0 read from mvc_dump.bin; vs-01 game_state.bin (+0x6B4 = [14,23,1,22,20,6], +0x694 = 0 x6, +0x6D4 = 0x40 x6, +0x840 = 2, +0x828 = 0, +0x80C = 1, +0x7C0 = 1) and blk.bin (+0x33a54 = 3600, +0x33a5c/5d = 0, +0x33a08/09 = 0, G+0x49 = 2).',
  note='Edge-vs-held settled by reading FUN_140619840 (block copy of blk+0x3C66.. into fighter+0x67C..) and FUN_140628d60 (fighter+0x680 -> blk+0x33a68, fighter+0x67C -> blk+0x33a6c).';

UPSERT steam_routine:FUN_140619840 SET addr='0x140619840', name='FUN_140619840',
  role='copies each seat pad block (0x14 B at blk+0x3C66 seat 0 / blk+0x3C7A seat 1: +0 current, +2 previous, +4 EDGE, .., +0x10 repeat, +0x12 hold counter) into the three fighters of that side at +0x67C (fighters 0/2/4 <- seat 0, 1/3/5 <- seat 1), then FUN_140634920. So fighter+0x67C = current word, fighter+0x680 = edge word.';
UPSERT steam_routine:FUN_140628d60 SET addr='0x140628d60', name='FUN_140628d60',
  role='CHARACTER SELECT frame (G+2 == 1): blk+0x33a68 = fighter0+0x680 (P1 EDGE), +0x33a6a = fighter1+0x680 (P2 EDGE), +0x33a6c = fighter0+0x67C (P1 HELD), +0x33a6e = fighter1+0x67C (P2 HELD); then PTR_FUN_140a6fb00[G+3](); rand tick when config+0x828 != 0.';
UPSERT steam_routine:FUN_14060b790 SET addr='0x14060b790', name='FUN_14060b790',
  role='pad key-repeat per seat block: +0x10 = 0; if current == previous: ++counter; at 0x24 held frames re-emit current into +0x10 and set counter 0x1E (repeat every 6). Not consumed by the select scene (it reads the EDGE word).';
UPSERT steam_routine:FUN_140629d10 SET addr='0x140629d10', name='FUN_140629d10',
  role='OUTER 6 = inner dispatch PTR_LAB_140a6fbc8[+5](). Then START (edge word < 0, bit 0x8000): if config+0x828 == 0 && config+0x7C0 != 0 -> blk+0x33a5c+side = 1 (the SAME-TEAM AUTO flag); if config+0x828 != 0 -> blk+0x33a54 = 0 (timer expired -> both sides auto-confirm 0x200).';
UPSERT steam_routine:FUN_14062b7b0 SET addr='0x14062b7b0', name='FUN_14062b7b0',
  role='confirm WORD -> COSTUME: DAT_140a6fae8 {0x200 LP,0x100 HP,0x80 A1,0x40 LK,0x20 HK,0x10 A2} -> DAT_140a6faf4 {0,2,4,1,3,5}; if the cid is not in the hidden-costume mask (blk+0x33a44/48 P1, +0x33a4c/50 P2 by G+0x84|G+0x85): A1 -> 0x100, A2 -> 0x20 before the lookup and 4 -> 0, 5 -> 1 after; duplicate rule: costumes of the same cid already confirmed (other side, and this side other slots; mirror table blk+0x33a70+side*3+k / +0x33a76) are skipped for the lowest free 0..5. Unknown word -> 0.';
UPSERT steam_routine:FUN_14073ed60 SET addr='0x14073ed60', name='FUN_14073ed60', role='cursor cell -> cid: DAT_140a98f80[blk+0x33a0c+side + (blk+0x33a0e+side)*8] (8x8 grid; FUN_14073e970 wraps both to 0..7).';

UPSERT finding:select_same_team_auto_confirm SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='The game has its own SAME-TEAM path at character select: config+0x840 == 2 preloads each slot from config+0x6B4/+0x694 and blk+0x33a5c+side replays config+0x6D4 as the confirm word (START sets it in a VS rematch)',
  statement='FUN_140629dc0 (outer 3, every team-slot entry) with config+0x840 == 2: if config+0x6B4+i*4 >= 0 then fighter_i+0x6C0 = fighter_i+0x001 = config+0x6B4+i*4 and fighter_i+0x655 = config+0x694+i*4 (i = side + 2*slot), else the defaults DAT_140a6fad0. FUN_14062aa00 (choose): when blk+0x33a5c+side != 0 the seat word is replaced by config+0x6D4+i*4, fighter+0x001 = fighter+0x6C0 and the confirm runs at once (costume = FUN_14062b7b0(word), config+0x6B4/+0x6D4/+0x578/+0x758 mirrors, FUN_1407412a0 + FUN_140741510, *(u16*)(side+5) = 2, blk+0x33a0a+side |= 1<<slot). Assist state (0x14062a4c3): the same flag (or timer 0) forces word 0x200 and confirms the preset fighter+0x655; UP 0x2000 / DOWN 0x1000 cycle (x+3)%3, any 0x3F0 confirms. Inner 3 (0x14062a090) waits for blk+0x33a08+side == 3 then slot += 1 and outer = 3, or after slot 2: outer = 10 (G+0x49 < 4). Outer 10 (0x14062b040) ORs side_bit into blk+0x33a04; FUN_14062c480 advances G+3 = 4 when blk+0x33a04 == (G+0x84|G+0x85). FUN_140629d10 sets blk+0x33a5c+side = 1 on START (edge word negative) when config+0x828 == 0 && config+0x7C0 != 0.',
  consequence='To lock seat S to its last team through game code: write config+0x6B4/+0x694/+0x6D4 for i in {S, S+2, S+4} (snapshotted BEFORE reenter_select reloads config from exe_image.bin, which restores the CAPTURE session picks [14,23,1,22,20,6]), set blk+0x33a5c+S = 1, and because the anchor is already past slot-0 entry (outer 6 / inner 1) write fighter_S+0x6C0 = cid and +0x655 = assist for slot 0 only (or re-run outer 3 via side+4 = 3, ungated). Feed S no input. For the by-hand seat T set config+0x6B4+(T+2k)*4 = -1 so slot entries fall back to defaults. Gate per rule 4 against a real battle-entry blk.';

UPSERT finding:select_words_are_edge_words SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='The select scene is EDGE-driven: blk+0x33a68/+0x33a6a are the per-seat EDGE words (pressed this frame), blk+0x33a6c/+0x33a6e the HELD words',
  statement='FUN_140048630 translates raw -> seat bits (DAT_140a4f780): UP 0x10->0x2000, DOWN 0x40->0x1000, LEFT 0x80->0x800, RIGHT 0x20->0x400, LP 0x8000->0x200, HP 0x1000->0x100, A1 0x200->0x80, LK 0x4000->0x40, HK 0x2000->0x20, A2 0x800->0x10, START 0x08->0x8000, 0x01->0x4000. The per-seat block blk+0x3C66+seat*0x14 holds {+0 current, +2 previous, +4 edge, +0x10 repeat, +0x12 hold}; FUN_140619840 copies it into fighter+0x67C of the side fighters; FUN_140628d60 copies fighter+0x680 (edge) to blk+0x33a68+side*2 and fighter+0x67C (held) to blk+0x33a6c+side*2 every select frame. Moves (0x3C00) and confirms (0x3F0) act once per press; a new press needs a released tick in between. The held word is read only by the random-select hold (LEFT+RIGHT, counter blk+0x33a5a to 0x3C).',
  consequence='An injected confirm = present the attack bit for one tick, then release. Costume follows the bit (FUN_14062b7b0), so the runner must inject the saved word, not always LP.';

UPSERT finding:select_hand_written_fighter_bytes_are_clobbered SET status='inferred', confidence='high', date='2026-09-06',
  title='Writing fighter+0x6C0/+0x001/+0x655 by hand at select re-entry is overwritten by the scene: outer 0 (defaults), outer 3 (config+0x6B4/+0x694 when config+0x840 == 2), and any cursor move (grid lookup)',
  statement='FUN_14062a1d0 (outer 0) sets +0x6C0 = +0x001 = DAT_140a6fad0 and +0x655 = 0 for all three slots of a human side. FUN_140629dc0 (outer 3, each slot entry) with config+0x840 == 2 preloads from config+0x6B4/+0x694; the runner sets +0x840 = 2 and reloads config from exe_image.bin, so the CAPTURE session picks come back. FUN_14062aa00 recomputes +0x6C0/+0x001 from the cursor cell DAT_140a98f80[x + 8y] on any move.',
  consequence='The observed "P1 came back on the capture-session roster" is FUN_140629dc0 preloading config+0x6B4 from the reloaded exe_image config. Use the config mirrors (and blk+0x33a5c) as the lever; write fighter bytes only for the slot the anchor is already inside.';

RELATE finding:select_same_team_auto_confirm->cites->steam_routine:FUN_140629dc0 SET id='e191_s_slot';
RELATE finding:select_same_team_auto_confirm->cites->steam_routine:FUN_14062aa00 SET id='e191_s_choose';
RELATE finding:select_same_team_auto_confirm->cites->steam_routine:FUN_140629d10 SET id='e191_s_start';
RELATE finding:select_same_team_auto_confirm->cites->steam_routine:FUN_14062b7b0 SET id='e191_s_costume';
RELATE finding:select_same_team_auto_confirm->cites->steam_routine:FUN_14062c480 SET id='e191_s_advance';
RELATE finding:select_same_team_auto_confirm->cites->finding:select_cursor_state_machine SET id='e191_s_prior';
RELATE finding:select_words_are_edge_words->cites->steam_routine:FUN_140619840 SET id='e191_w_copy';
RELATE finding:select_words_are_edge_words->cites->steam_routine:FUN_140628d60 SET id='e191_w_frame';
RELATE finding:select_words_are_edge_words->cites->steam_routine:FUN_140048630 SET id='e191_w_xlat';
RELATE finding:select_words_are_edge_words->cites->steam_routine:FUN_14060b790 SET id='e191_w_repeat';
RELATE finding:select_words_are_edge_words->cites->finding:input_seat_route_confirmed SET id='e191_w_prior';
RELATE finding:select_hand_written_fighter_bytes_are_clobbered->cites->steam_routine:FUN_14062a1d0 SET id='e191_c_outer0';
RELATE finding:select_hand_written_fighter_bytes_are_clobbered->cites->steam_routine:FUN_140629dc0 SET id='e191_c_outer3';
RELATE finding:select_hand_written_fighter_bytes_are_clobbered->cites->steam_routine:FUN_14073ed60 SET id='e191_c_grid';
RELATE finding:select_same_team_auto_confirm->cites->source:select_confirm_decompiles_20260906 SET id='e191s_a0';
RELATE finding:select_words_are_edge_words->cites->source:select_confirm_decompiles_20260906 SET id='e191s_a1';
RELATE finding:select_hand_written_fighter_bytes_are_clobbered->cites->source:select_confirm_decompiles_20260906 SET id='e191s_a2';
UPDATE finding:select_same_team_auto_confirm SET status='confirmed';
UPDATE finding:select_words_are_edge_words SET status='confirmed';
UPDATE finding:select_hand_written_fighter_bytes_are_clobbered SET status='confirmed';
