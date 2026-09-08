-- 189_game_speed_turbo.surql -- GAME SPEED / TURBO: host config field, in-block variables, the
-- frame-doubling mechanism, the VS-screen label gate, and the SUPERGUN runner write set.
-- 2026-09-06. SH4 annotation transfer by function matching (seed constant work.asm:11-12
-- Frameskip Timer Value 6/4/2 -> Steam byte-pattern hits -> call graph -> block map).
-- Forward slashes only in ref/note; no apostrophes.
USE NS re DB kb;

UPSERT source:game_speed_re_20260906 SET kind='disasm', strength='code',
  ref='Ghidra dumpproj / mvc_dump.bin: FUN_140608690, FUN_14060b440, FUN_140628020, FUN_140626bb0, FUN_14060f450, FUN_14060E170 caseD_7 (0x14060ede0..0x14060efc3), PTR_FUN_140a6fa10[1] = 0x140627310 (capstone, no Ghidra fn), select machine 0x14062a0fc-0x14062a112 and 0x14062a8e0-0x14062a9f3, FUN_14003fa60, FUN_14008e620, FUN_1407d5cb0, FUN_1407fc7d0 caseD_8 0x1407fc980, 0x1407edc54; SH4 refs/marvelous2 work.asm:11-12, bank04.asm:14180-14210 (loc_8c045ef6), 14411-14428 (loc_8c04608c), 15856-15880 (loc_8c0469c8), 15926-15975 (loc_8c046a2a), bank03.asm:33567-33701 (loc_8c03dfa8), build/bank01.asm:1452, build/bank13.asm:8037-8043; anchors d3dcap/ttd/runs/*/pre/blk.bin + exe_image.bin (13 captures).',
  note='Scratchpad answer turbo.md (session 193726f3). All Steam addresses are live addresses (no ASLR).';

UPSERT finding:game_speed_frame_doubling_mechanism SET status='inferred', confidence='high', date='2026-09-06',
  title='Game speed = frame-doubling period N in the battle block; the battle machine runs the sim body TWICE every N-th displayed frame',
  statement='Battle block *(0x142edf628) = blk+0x324E0 == DC 0x8c2895f0 (delta 0x8C257110); DC base+0x30/0x31/0x32 (work.asm 0x8c289620/21/22 Frameskip Timer Value / Counter / flag) == Steam +0x50/+0x51/+0x52 = blk+0x32530/0x32531/0x32532 (+0x20 because the six fighter pointers at +0x20 grew 4->8 bytes; FUN_140628020 == loc_8c045ef6). CONSUMER: FUN_14060E170 caseD_7 (table 0x140a6abd0[G+3]) at 0x14060eea4-0x14060ef59 == loc_8c03dfa8/loc_8c03dfe2 bank03.asm:33567-33701: +0x52=0; if(+0x50) +0x51--; loop { G+0x90++; FUN_140627c30; +0x5b nibble; FUN_1406283a0; FUN_140621690; FUN_140628320; FUN_140619970; if(+0x50 && +0x51==0){ +0x51=+0x50; +0x52=0xff; goto loop } } then ONE display-list build (FUN_14061e690, FUN_140620190, FUN_14061e560 x4, FUN_140620960). Input is latched once per frame by FUN_14060b7d0 so the doubled step re-uses the same pad word. N=6 -> 70 sim steps/s, N=4 -> 75, N=2 -> 90, N=0 -> 60. Not a render skip, not a timer scale.',
  consequence='A tape row per displayed frame (FUN_140118950 call) still drives the frame; on doubled frames it drives two sim steps. Readers of the +0x52 flag on Steam: UNKNOWN.';

UPSERT finding:game_speed_writer_state1 SET status='inferred', confidence='high', date='2026-09-06',
  title='Round-start battle state 1 (0x140627310) computes the period from G+0x89 and G+0x49; Steam added level 0 = no doubling',
  statement='PTR_FUN_140a6fa10[1] = code at 0x140627310 (no Ghidra function; capstone) == SH4 loc_8c04608c + loc_8c046a2a (bank04.asm:14411-14428, 15926-15975): dec u16 +0x1a, if >0 ret; +0x50=4,+0x51=1,+0x52=0; idx=(s8)G+0x89; if G+0x49>4 idx += G+0x49-4; switch idx: 0->+0x50=0, 1->6, 2->4, 3->2, else 4; +0x1a = (+0x65!=0)?1:0x78; G+0x19=0; state=2. DC loc_8c046a2a mapped option 0->6, 1->4, 2->2 (G[0x89] only). Round init FUN_140628020 writes +0x50=0 (DC wrote 6); KO/round-end FUN_140626bb0 == loc_8c0469c8 zeroes +0x50..+0x52 (no doubling during KO/win poses).',
  consequence='Encoding of the resolved index: 0 = plain 60 (new in Steam), 1 = period 6 (DC NORMAL), 2 = period 4 (DC TURBO 1), 3 = period 2 (DC TURBO 2). A value changed mid-round takes effect at the next round state 1.';

UPSERT finding:game_speed_config_and_block_fields SET status='inferred', confidence='high', date='2026-09-06',
  title='Host writes config+0x298 (u32 byte-valued); FUN_140608690 copies it to G+0x49 = blk+0x3D01; the select confirm resolves G+0x89 = blk+0x3D41',
  statement='config = *(0x140ACD3A0) = 0x140ac6d40. config+0x298 written by FUN_14003fa60 @0x14003fb44 from the persisted settings blob *(param_1+8)+0x5a2fb (same routine fills config+0x284..0x2c8 from blob+0x5a2f5..0x5a307) and by FUN_14008e620 @0x14008e810 from the lobby match-settings blob *(param_1+0x350)+0xe (blob +0xb..+0x1a -> config+0x28c..0x2c8); 0x1400678aa/0x140067ffb/0x1400687b7/0x14008fdf4 are the same store shape (INFERRED same source). FUN_140037B50 does NOT write it. FUN_140608690 (blk init at mode entry): G+0x49 = config[0x298] (also G+0x1b<-config[0x290], G+0x46<-config[0x294], G+0x4f<-config[0x4dc], G+0x28<-config[0x2a4]); FUN_14060b440: G+0x49 = DAT_142edf22c (boot default; DC 0x8c212cc0, Set Turbo to 0, build/bank01.asm:1452). G+0x89 (DC 0x8c2682c9) writers: select confirm 0x14062a0fc-0x14062a112: if G+0x49<4 { G+0x89=G+0x49; blk+0x33a3a=1 } else free-select path; free-select toggle 0x14062a8e0-0x14062a9f3: G+0x89 = (G+0x89+1)&1, fighter+0x656 = G+0x89, config+0x690 = G+0x89, entry G+0x89=1 or config[0x690] when config+0x840==2; battle case 0 FUN_14060f450 @0x14060f8bb-0x14060f8d0: if G+0x49>3 G+0x89 = human point fighter+0x656 (fighter0 unless G+0x84==1 -> fighter1; skipped when G+0x84==3).',
  consequence='config+0x298 / G+0x49 encoding: 0 = no doubling, 1 = period 6, 2 = period 4, 3 = period 2, >=4 = FREE SELECT at character select (pick p in {0,1}, idx = p + (value-4)). Collection menu label names: UNKNOWN (no UI string in the exe). Captured default in all 13 anchors: config+0x298 = 2, G+0x49 = 2, config+0x690 = 1, fighter+0x656 = 1 x6; battle anchors G+0x89 = 2 and triple [4, n, 0|0xFF]; select anchors G+0x89 = 0.';

UPSERT finding:vs_screen_speed_label_gate SET status='inferred', confidence='high', date='2026-09-06',
  title='The VS-screen NORMAL/TURBO label is a sprite object drawn iff obj+0x35 == G+0x89 (blk+0x3D41); no text string in the exe',
  statement='list_strings has no TURBO/NORMAL game text (the game text block ends at STAGE   : %02X 0x14097e8d0). FUN_1407d5cb0 (called from object states caseD_23/24/2e at 0x140729467/0x1407294c3/0x140729660) spawns three label objects with obj+0x35 = 0,1,2 (update fn LAB_1407d5950). Draw gate: 0x1407fc980 (FUN_1407fc7d0 caseD_8): obj+0x170 = (obj+0x35 == (s8)G+0x89); 0x1407edc54 (switch at 0x1407ed72f): obj+6 = (G+0x89 != obj+0x35)+1. So G+0x89 = 0 shows NORMAL, 1/2 show the TURBO art. DC kept ASCII NORMAL 0 / TURBO  1 at bank13 loc_8c138260/loc_8c13826c (table bank14 loc_8c14DA14) for the options/free-select text.',
  consequence='Which screen the 0x140729xxx object belongs to (VS/loading vs select) is INFERRED from the runner symptom, not read live.';

UPSERT finding:supergun_runner_speed_write_set SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='SUPERGUN runner: BUILD A MATCH bypasses the select confirm, so G+0x89 stays 0 -> period 0 (no doubling) and the NORMAL label; write G+0x89 = G+0x49 (default 2) before the first tick',
  statement='sg_runner 5b BUILD A MATCH writes G+2=2, G+3..7=0 and skips the select-confirm code 0x14062a112 (G+0x89 = G+0x49). From a select anchor (G+0x89 = 0) the runner reaches battle state 1 with idx 0 -> blk+0x32530 = 0 -> the sim never doubles, while the real game at the captured setting (2) doubles every 4th frame. Gates 1/2 passed on battle anchors, which already carry G+0x89 = 2 and the triple, so they could not see it. Write set, after the anchor blk is restored and before the first tick (startup 5b and at the end of reenter_select after blk.bin/exe_image.bin reload): wr8(blk+0x3D01, speed); wr8(blk+0x3D41, speed<4 ? speed : pick); wr32(config+0x298, speed); for i in 0..6 wr8(fighter_i+0x656, pick); wr32(config+0x690, pick). Default speed=2, pick=2 = the captured real-game configuration. Do NOT hand-write blk+0x32530..2 (state 1 recomputes it every round; the anchor countdown is part of blk determinism). Carried in the anchor: G+0x49 (blk.bin) and config+0x298 (exe_image.bin game_state page, reloaded by reenter_select); G+0x89 only in battle anchors.',
  consequence='Gate unchanged: battle-entry blk vs a real-game blk for the same roster/stage (Gate 1/N1) must show blk+0x3D41 = 2 and blk+0x32530 = 4 at state 2, as every battle anchor does. Any other speed is a CHANGE TO THE SIM and must be gated against a real capture at that setting.';

UPSERT finding:sh4_steam_pairs_game_speed SET status='inferred', confidence='high', date='2026-09-06',
  title='SH4<->Steam function pairs for the game-speed subsystem',
  statement='loc_8c045ef6 (bank04.asm:14180) <-> FUN_140628020 (round block init, six fighter ptrs + speed triple); loc_8c0469c8 (bank04.asm:15856) <-> FUN_140626bb0 (KO state, zero triple, timer 0x1e/0x186 on G+0x88); loc_8c04608c+loc_8c046a2a (bank04.asm:14411, 15926) <-> 0x140627310 = PTR_FUN_140a6fa10[1] (inlined); loc_8c03dfa8/loc_8c03dfe2 (bank03.asm:33567) <-> FUN_14060E170 caseD_7 0x14060ede0; init_default_settings 8c01099c turbo byte 0x8c212cc0 <-> DAT_142edf22c via FUN_14060b440; GameGlobal+0x89 0x8c2682c9 <-> blk+0x3D41; GameGlobal+0x49 0x8c268289 <-> blk+0x3D01; battle base 0x8c2895f0 <-> blk+0x324E0 (*(0x142edf628)); DC base+0x30/31/32 <-> Steam +0x50/51/52 (blk+0x32530/1/2); DC strings NORMAL 0 / TURBO  1 bank13 loc_8c138260/26c have no exe twin (sprite labels instead).',
  consequence='Fingerprints used: constants 6/4/2 and 0x78, the 0x0e10 frame-counter test, the &0xf nibble, the 0xff flag, the (+0x65 ? 1 : 0x78) timer -- never instruction bytes.';

RELATE finding:game_speed_frame_doubling_mechanism->cites->source:game_speed_re_20260906 SET id='e189_mech_src';
RELATE finding:game_speed_writer_state1->cites->source:game_speed_re_20260906 SET id='e189_writer_src';
RELATE finding:game_speed_config_and_block_fields->cites->source:game_speed_re_20260906 SET id='e189_fields_src';
RELATE finding:vs_screen_speed_label_gate->cites->source:game_speed_re_20260906 SET id='e189_label_src';
RELATE finding:supergun_runner_speed_write_set->cites->source:game_speed_re_20260906 SET id='e189_runner_src';
RELATE finding:sh4_steam_pairs_game_speed->cites->source:game_speed_re_20260906 SET id='e189_pairs_src';
RELATE finding:supergun_runner_speed_write_set->cites->finding:game_speed_writer_state1 SET id='e189_runner_writer';
RELATE finding:supergun_runner_speed_write_set->cites->finding:game_speed_config_and_block_fields SET id='e189_runner_fields';
RELATE finding:game_speed_writer_state1->cites->finding:game_speed_frame_doubling_mechanism SET id='e189_writer_mech';
RELATE finding:vs_screen_speed_label_gate->cites->finding:game_speed_config_and_block_fields SET id='e189_label_fields';
RELATE finding:sh4_steam_pairs_game_speed->cites->finding:game_speed_frame_doubling_mechanism SET id='e189_pairs_mech';
RELATE finding:supergun_runner_speed_write_set->cites->finding:select_load_state_differs_per_mode SET id='e189_runner_select';

-- Promotion AFTER the cites edges exist (finding.status ASSERT: confirmed requires a cited source
-- of strength code/reproduction or a cited routine). Same order as seed 179.
UPDATE finding:game_speed_frame_doubling_mechanism SET status='confirmed';
UPDATE finding:game_speed_writer_state1 SET status='confirmed';
UPDATE finding:game_speed_config_and_block_fields SET status='confirmed';
UPDATE finding:vs_screen_speed_label_gate SET status='confirmed';
UPDATE finding:sh4_steam_pairs_game_speed SET status='confirmed';
