-- 175_select_state_machine.surql -- CHARACTER SELECT: dispatch tables, mode byte, turn token, per-side
-- cursor machine, input routing, and the headless two-player select-to-battle run.
-- 2026-09-05. Every routine claim below is a Ghidra decompile of mvc_dump.bin (project dumpproj, GhidraMCP);
-- every value claim is a read of the select-01 anchor (d3dcap/ttd/runs/select-01/pre) or a per-tick blk dump
-- from the SUPERGUN runner driven by a scripted inputs file (sg_sim3..sg_sim7). No live probe, no prediction.
USE NS re DB kb;

-- ── sources ───────────────────────────────────────────────────────────────────────────────────────
UPSERT source:select_decompiles_20260905 SET kind='disasm', strength='code',
  ref='Ghidra project dumpproj over mvc_dump.bin (base 0x140000000) via GhidraMCP decompile_function_by_address: FUN_14062bc80, FUN_14062c480, FUN_14062b130, FUN_14062a1d0, FUN_140629d10, FUN_14062aa00, FUN_140629490, FUN_140118950, FUN_140048630, FUN_140270490, FUN_14011ea90, FUN_140037b50, switchD_14060bd43::caseD_3; plus the three dispatch tables read out of select-01/pre/exe_image.bin at 0x140a6fb00 / 0x140a6fb70 / 0x140a6fbc8 and the pad table at DAT_140a4f780.',
  note='Static read only. Mode-byte writers/comparisons were found by regex over d3dcap/replay/re_map/cache/steam_disasm.jsonl (29,678 functions) matching a load of DAT_142edf580 followed by a byte access at +0x4c.';

UPSERT source:supergun_scripted_select_20260905 SET kind='capture', strength='reproduction',
  ref='SUPERGUN runner sg_runner.exe --pre d3dcap/ttd/runs/select-01/pre --arc game_50.arc --no-flush --two-local --inputs <file>, per-tick blk dumps in C:/Users/trist/AppData/Local/Temp/sg_sim{3,4,5,6,7}. Inputs are hex word pairs (seat0 seat1) per tick.',
  note='sg_sim6 = both sides scripted through three characters and three assists (420 ticks) and reaching G+2 = 2. sg_sim7 = the same padded to 1100 ticks; it faults at tick 423 in FUN_140270490. Anchor values were read directly out of select-01/pre/{blk.bin,ctx.bin,dcram.bin,exe_image.bin,host_device.bin}.';

UPSERT source:capgate_raster_state_20260905 SET kind='capture', strength='reproduction',
  ref='d3dcap/replay/capgate/frame_{4445,12033,15323,11943}.pack manifests (RRPK), draws[].raster and draws[].depth counted directly.',
  note='2,511 draws total. Adds FrontCounterClockwise and DepthFunc, which docs/TSP-RENDER-STATE-GHIDRA.md did not record.';

-- ── routines ──────────────────────────────────────────────────────────────────────────────────────
UPSERT steam_routine:FUN_14062bc80 SET addr='0x14062bc80', name='FUN_14062bc80',
  role='CHARACTER SELECT ENTRY = PTR_FUN_140a6fb00[0]. Guarded init on config+0x840 == 0. Decides the turn token blk+0x33a00: if (G+0x4c == 1 || DAT_142edf2fc == 2) { blk+0x33a00 = 0xFF; G+0x8c = 0 } else { 0; bit0 of G+0x84 -> 2; bit1 -> 1; G+0x8c != 0 -> G+0x8c (then cleared); G+0x84 == 0 && G+0x8c == 0 -> 0xFF; if (config+0x828 != 0) blk+0x33a00 = config+0x82c + 1; config+0x7c0 compares +0x5b8/+0x5bc }. Then G+3 += 1, G+4..7 = 0, FUN_140628df0(0)/(1) per human side (G+0x84 & 3), roster masks blk+0x33a18/20 OR-shared, camera set A for G+0x4c == 1 else B, FUN_14060ba00(FUN_140620ad0).';
UPSERT steam_routine:FUN_14062c480 SET addr='0x14062c480', name='FUN_14062c480',
  role='CHARACTER SELECT PER-FRAME = PTR_FUN_140a6fb00[3] (G+3 == 3). Calls FUN_14062b130 for side 0 (blk+0x3db8) and side 1 (+0x738). ADVANCE TEST: if (u32 blk+0x33a04 == (G+0x84 | G+0x85)) { G+3 = ((mask) != 3) + 4; zero +0x3dbd/be/bf of both sides; G+0x8d = G+0x8e = 0 }. Also uses config+0x828/+0x80c/+0x82c/+0x844 for the single-local-player overlay.';
UPSERT steam_routine:FUN_14062b130 SET addr='0x14062b130', name='FUN_14062b130',
  role='PER-SIDE CURSOR TICK. param_1 = side struct blk+0x3db8 + side*0x738 (+0x6b0 = side index). Refreshes DAT_142eeea40[side, side+2, side+4] from each team slot +0x6c0 (hovered id) via FUN_140616730/960. Then if ((blk+0x33a12 & param_1+0x654) == 0) dispatch PTR_FUN_140a6fb70[*(u8*)(param_1+4)](param_1) -- the OUTER cursor state at blk+0x3dbc.';
UPSERT steam_routine:FUN_14062a1d0 SET addr='0x14062a1d0', name='FUN_14062a1d0',
  role='PER-SIDE CURSOR ENTRY = PTR_FUN_140a6fb70[0]. side_bit = param_1+0x654 (1 = P1, 2 = P2). Runs only if (side_bit & (G+0x85 | G+0x84)). Sets +4 = 1, +0x30 = 1. TOKEN HAND-OVER: if (-1 < (s8)blk+0x33a00 && side_bit == G+0x8c && other side +0x3dbc not in 7..8) blk+0x33a00 = side_bit. 0xFF is signed negative so the hand-over is SKIPPED: both sides stay active at once. Then, for a human side, initialises the three team slots from DAT_140a6fad0 (+0x4478 hovered, +0x3db9 picked, +0x440d = 0, +0x448b = slot k).';
UPSERT steam_routine:FUN_140629d10 SET addr='0x140629d10', name='FUN_140629d10',
  role='OUTER CURSOR STATE 6 = PTR_FUN_140a6fb70[6]. Sets param_1+0x28 = blk+0x3db8 + (side + slot*2)*0x738 (the team-slot fighter struct, slot = s16 param_1+0x32) and dispatches the INNER state PTR_LAB_140a6fbc8[*(u8*)(param_1+5)]() -- labels inside FUN_1407f3570, not Ghidra functions. Timer blk+0x33a68+side*2 < 0 -> blk+0x33a5c+side = 1 (config+0x828 == 0 && +0x7c0) or blk+0x33a54 = 0 (config+0x828 != 0).';
UPSERT steam_routine:FUN_14062aa00 SET addr='0x14062aa00', name='FUN_14062aa00',
  role='INNER CURSOR STATE 1 = CHOOSE CHARACTER = PTR_LAB_140a6fbc8[1]. Input word = u16 blk+0x33a68 + side*2 (copied from the pad; anchor 0x33a54 != 0 path). Moves on & 0x3c00: 0x800 x-1, 0x400 x+1 (blk+0x33a0c+side), 0x2000 y-1, 0x1000 y+1 (blk+0x33a0e+side); FUN_14073e970/ed60 recompute the hovered id into slotstruct+0x6c0, looping while the roster mask bit (blk+0x33a28 + side*8, +4 for id > 0x1f) is clear. CONFIRM on & 0x3f0: slotstruct+1 = hovered; config+0x6d4/+0x6b4/+0x578/+0x758 mirrors; param_1+5 = 2; blk+0x33a0a+side |= 1 << slot; roster bit of the picked id CLEARED (blk+0x33a15 == 0); param_1+0x30 += 1; FUN_14062b7b0 -> slotstruct+0x6c1.';
UPSERT steam_routine:FUN_140118950 SET addr='0x140118950', name='FUN_140118950',
  role='SIM TICK ENTRY. *param_1 += 1; config+0x798 = 1; config+0x228..0x234 = previous config+0x218..0x224; config+0x218..0x224 = 0; for k in 0..4: if (s32 config+0x258+k*4 >= 0) config[0x218 + seat_k*4] = inputs[k] & 0xFFFFFF. Then tail-calls config+0x10 (the frame function). The SEAT MAP is config+0x258..0x264.';
UPSERT steam_routine:FUN_140048630 SET addr='0x140048630', name='FUN_140048630',
  role='PAD TRANSLATE(i): raw = u32 config+0x218 + i*4; 12 (mask, bit) pairs at DAT_140a4f780: raw&0x08->0x8000, 0x01->0x4000, 0x10->0x2000, 0x40->0x1000, 0x80->0x800, 0x20->0x400, 0x8000->0x200, 0x1000->0x100, 0x200->0x80, 0x4000->0x40, 0x2000->0x20, 0x800->0x10. No history buffer. The caller stores the result at blk+0x3C66 + i*0x14 (edge/held word at +4).';
UPSERT steam_routine:FUN_140270490 SET addr='0x140270490', name='FUN_140270490',
  role='SERVICE-LOCATOR THUNK. RAX = FUN_14011ea90(&PTR_PTR_142ebce38); R10 = [RAX]; JMP [R10 + 0x38]. In the SUPERGUN shell [RAX] is 0 -> EXCEPTION 0xc0000005 READ 0x38 at RIP 0x1402704c8 (RAX 0x50cbb20, outside every mapped image). Reached from character-select ENTRY (FUN_14062bc80, when G+3 is forced back to 0) and from the MATCH LOAD path 3 ticks after G+2 becomes 2 (sg_sim7 tick 423).';

-- ── findings ──────────────────────────────────────────────────────────────────────────────────────
UPSERT finding:select_dispatch_tables SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='Character select is three nested function tables at 0x140a6fb00 / 0x140a6fb70 / 0x140a6fbc8',
  statement='Read out of the select-01 exe_image.bin. PTR_FUN_140a6fb00 (indexed by G+3, the select sub-state): [0] FUN_14062bc80 entry, [1] FUN_14062c440, [2] FUN_14062baf0, [3] FUN_14062c480 per-frame, [4] FUN_14062c710, [5] FUN_14062c640, [6] FUN_14062bb30 commit, [7..12] FUN_140629370/490/4d0/2b0/310/280, [13] 0. PTR_FUN_140a6fb70 = the same array from +0x70 (indexed by the OUTER per-side state blk+0x3dbc + side*0x738): [0] FUN_14062a1d0 entry, [1] FUN_14062b060, [2] FUN_14062b110, [3] FUN_140629dc0, [4] FUN_14062a1a0, [5] FUN_14062a1c0, [6] FUN_140629d10, [7] FUN_14062a810, [8] FUN_14062a720, [9] FUN_14062a1b0. PTR_LAB_140a6fbc8 (indexed by the INNER state blk+0x3dbd): [0] 0x14062b000, [1] 0x14062aa00 choose character, [2] 0x14062a460, [3] 0x14062a090, [4] 0x14062cb70, [5..11] 0x14062cc10/cc50/cc90/cd20/c780/c7f0/c830 -- labels inside FUN_1407f3570 (0x1406260a0..0x1407f475e), not functions.',
  consequence='G+3 is the select phase, blk+0x3dbc the per-side outer state, blk+0x3dbd the per-side inner state. Every observation below is stated in those three bytes.';

UPSERT finding:select_mode_byte SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='G+0x4c (blk+0x3d04) is the MODE byte; 0 = 1P arcade, 1 = two human players; 2, 5, 7 also tested',
  statement='Writers with an immediate: FUN_14060e170, FUN_14060e250, caseD_3 (all write 0). Compared against 1 in FUN_140618ab0, FUN_140629490, FUN_14062bc80; against 2 in FUN_14060e170, FUN_140627c50, FUN_14062e090, caseD_7; against 5 in FUN_14060fb90; against 7 in FUN_140611a20, FUN_140614c30, FUN_140615cf0, FUN_140627c50, FUN_14062e090, caseD_8, caseD_d. FUN_14062bc80 takes its first branch for mode 1: blk+0x33a00 = 0xFF, G+0x8c = 0, and camera set A (0x6910=4, 0x6918=0x433e0000, 0x691c=0x444b16de, 0x6960=0x433e0000, G+8=0, FUN_14061ca70). The select-01 anchor holds G+0x4c = 0, DAT_142edf2fc = 0, config+0x828 = 1, config+0x82c = 0, so it computed blk+0x33a00 = 1.',
  consequence='Mode 0 lets ONE side pick by design (the other team is CPU); there is no P2 cursor to unlock in a mode-0 anchor. A two-player select needs mode 1.';

UPSERT finding:select_turn_token SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='blk+0x33a00 is the whose-turn token; 0xFF (signed negative) means both sides active at once',
  statement='FUN_14062a1d0: if (-1 < (s8)blk+0x33a00 && side_bit == G+0x8c && other side outer state not in 7..8) blk+0x33a00 = side_bit. FUN_14062a1d0 reads it with CMP byte ptr [RAX+0x33a00],DIL; JL (0x14062a224). With the anchor values (token 1, G+0x8c 0) side_bit == G+0x8c is false for both sides (bits 1 and 2), so the token never moves: P2 never activates, blk+0x33a04 never reaches G+0x84|G+0x85 (= 3 in the anchor, G+0x85 = 3), FUN_14062c480 never advances G+3 past 3. That is the observed hang after P1 third character.',
  consequence='Runner flag --two-local writes G+0x4c = 1, blk+0x33a00 = 0xFF, G+0x8c = 0 (the three values the mode-1 branch of FUN_14062bc80 writes) and leaves G+3 at 3. This is a CHANGE TO ANCHOR STATE (mode-0 capture restarted as mode 1) and is not gated against a real VS-mode select capture.';

UPSERT finding:config_single_local_player_override SET status='inferred', confidence='high', date='2026-09-05',
  title='config+0x828 / +0x82c force a single local player index into the select token',
  statement='config = *(0x140acd3a0) = 0x140ac6d40. FUN_14062bc80 (else branch, i.e. mode != 1): if (config+0x828 != 0) blk+0x33a00 = config+0x82c + 1. Writers: FUN_140037b50 at 0x140037e1c and 0x140037e4e. Readers include FUN_14062c480 (three sites), FUN_14062bc80, FUN_140629d10, FUN_14061eb00, FUN_140616050, FUN_140634920, FUN_140624910, caseD_0/caseD_3 of 0x14060b..., and the input/pad subsystem FUN_14003ad50 / FUN_140036ce0. Anchor: +0x828 = 1, +0x82c = 0, +0x7c0 = 1, +0x840 = 1.',
  consequence='Not the lever for two local players: in mode 1 this branch is never reached.';

UPSERT finding:select_cursor_state_machine SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='Per-side cursor: inner 1 choose -> 2 assist -> 3 wait 51 frames -> outer 3,4,5,6 (4 frames) -> next slot; bits in blk+0x33a0a/0b',
  statement='Measured on per-tick blk dumps (sg_sim6, both sides driven): LP edge in inner state 1 -> inner 2 on the same tick, blk+0x33a0a|=1<<slot, param_1+0x30 += 1. LP edge in inner 2 -> inner 3, blk+0x33a0a |= 0x10<<slot (assist type confirmed: the ASSIST TYPE SELECT panels). 51 frames later (t051->t102, t143->t194, t235->t296 for slots 0/1/2) the OUTER state runs 3,4,5,6 on consecutive ticks and inner returns to 1 with param_1+0x32 (slot) += 1 and +0x28 pointing at the next slot struct (blk+0x3db8 + (side+slot*2)*0x738). Direction words are honoured ONLY in inner state 1 (RIGHT 0x0400 at t112 moved hovered 0x14->0x16; the same word during inner 2/3 changed nothing). After the third assist the side goes to outer 10 (t296, slot = 3). Duplicate character: the confirm clears the picked id bit in blk+0x33a28+side*8, so a later confirm without movement lands on the next available id (slot 1 got 0x14 after 0x13 was taken).',
  consequence='A scripted select needs, per slot: confirm, confirm, wait >= 55 frames, move, and moves must not overlap inner states 2/3. Three characters = 6 confirms per side.';

UPSERT finding:select_advance_and_commit_measured SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='Both sides at outer 10 -> blk+0x33a04 = 3 -> G+3 = 4 next tick -> G+3 = 6 -> G+2 = 2 (battle) 123 ticks later',
  statement='sg_sim6/sg_sim7 (select-01 anchor, --two-local, both seats scripted, 420 ticks): t296 both sides outer 10 slot 3; t297 blk+0x33a04 = 3 and G+3 = 4 (FUN_14062c480 advance, mask G+0x84|G+0x85 = 3); t300 G+4 = 1 and stage blk+0x6d04 6 -> 11; t320/370/400 G+4 = 2/3/4; t419 G+3 = 6; t420 G+2 = 2, G+3 = 0 (FUN_14062bb30 commit). The select-to-battle transition therefore runs to completion in the shell from a scripted input stream with no host renderer.',
  consequence='Character select is fully input-drivable in SUPERGUN for two local players once seat 1 is fed. The live symptom P2 cannot select is an input-delivery problem in the window path, not a game-state problem.';

UPSERT finding:input_seat_route_confirmed SET status='inferred', confidence='high', date='2026-09-05',
  title='inputs[k] -> config+0x218 + seat_k*4 -> FUN_140048630(i) -> blk+0x3c66 + i*0x14; select reads blk+0x33a68 + side*2',
  statement='FUN_140118950 writes config[0x218 + config[0x258 + k*4]*4] = inputs[k] & 0xFFFFFF for each k whose seat is >= 0. The runner forces the seat map {0,1,-1,-1}, so inputs[1] lands at config+0x21c. FUN_140048630(1) reads config+0x218 + 4 and translates through DAT_140a4f780. Measured: runner word 0x8000 (LP) arrives as 0x0200 at blk+0x3c66 with the edge word blk+0x3c6a = 0x0200 for one tick; 0x20 (RIGHT) arrives as 0x0400; the same values appear in the select cursor word blk+0x33a68 + side*2. Seat 1 words drive P2 identically (blk+0x3c7a, blk+0x33a6a; sg_sim6 P2 picked three characters).',
  consequence='Seat routing in the runner is correct end to end. P2 gets nothing only when inputs[1] is zero.';

UPSERT finding:select_entry_reentry_faults_in_shell SET status='inferred', confidence='high', date='2026-09-05',
  title='Forcing G+3 = 0 to re-run FUN_14062bc80 faults in FUN_140270490 (null vtable via FUN_14011ea90); so does the match load path',
  statement='Runner run with G+0x4c = 1 and G+3 = 0: EXCEPTION 0xc0000005 READ 0x38 at RIP 0x1402704c8 on tick 1 (RAX 0x50cbb20 outside every mapped image, RBX/RDI/RSI 0). The same RIP, registers and RAX recur at tick 423 of sg_sim7, three ticks after G+2 became 2. FUN_140270490 is RAX = FUN_14011ea90(&PTR_PTR_142ebce38); JMP [[RAX]+0x38]. This is the fault finding:default_allocator_is_in_image already located at vtable slot 7 (+0x38) and for which it proposed repointing the allocator registry entries that lie in the dead heap at the in-image Default allocator 0x142E12EF0.',
  consequence='Select entry cannot be re-run in the shell; the two-player state has to be WRITTEN (finding:select_turn_token). The match LOAD path reaches the same host subsystem, so reaching a playable round from a select anchor now depends on resolving that locator, and finding:default_allocator_is_in_image is the recorded next step.';

UPSERT finding:world_raster_state_measured SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='Captured D3D rasterizer/depth state for world draws: FrontCounterClockwise = 1 on every culling draw; DepthFunc LESS_EQUAL; two write/stencil pairs',
  statement='Counted over capgate/frame_{4445,12033,15323,11943}.pack manifests: (cull, ccw) = (FRONT,1) x1782, (NONE,1) x647, (BACK,1) x70, (NONE,0) x12. (DepthFunc, DepthWriteMask, StencilEnable) = (LESS_EQUAL,1,1) x1798, (LESS_EQUAL,0,0) x701, (ALWAYS,1,1) x8, (LESS_EQUAL,1,0) x4. The blend/sampler/cull/ps derivation in docs/TSP-RENDER-STATE-GHIDRA.md is unchanged; this adds the two fields it did not record.',
  consequence='A port that leaves FrontCounterClockwise at the D3D default 0 culls the wrong faces on 1,852 of 2,511 draws (mirrored lettering, surfaces painting over their occluders).';

-- ── edges ─────────────────────────────────────────────────────────────────────────────────────────
RELATE finding:select_dispatch_tables->cites->steam_routine:FUN_14062bc80 SET id='e175_tab_entry';
RELATE finding:select_dispatch_tables->cites->steam_routine:FUN_14062c480 SET id='e175_tab_frame';
RELATE finding:select_dispatch_tables->cites->steam_routine:FUN_140629d10 SET id='e175_tab_outer6';
RELATE finding:select_mode_byte->cites->steam_routine:FUN_14062bc80 SET id='e175_mode_entry';
RELATE finding:select_turn_token->cites->steam_routine:FUN_14062a1d0 SET id='e175_token_entry';
RELATE finding:select_turn_token->cites->steam_routine:FUN_14062c480 SET id='e175_token_advance';
RELATE finding:config_single_local_player_override->cites->steam_routine:FUN_14062bc80 SET id='e175_cfg_entry';
RELATE finding:select_cursor_state_machine->cites->steam_routine:FUN_14062aa00 SET id='e175_cursor_choose';
RELATE finding:select_cursor_state_machine->cites->steam_routine:FUN_14062b130 SET id='e175_cursor_tick';
RELATE finding:select_advance_and_commit_measured->cites->steam_routine:FUN_14062c480 SET id='e175_adv_frame';
RELATE finding:select_advance_and_commit_measured->cites->steam_routine:FUN_14062bb30 SET id='e175_adv_commit';
RELATE finding:input_seat_route_confirmed->cites->steam_routine:FUN_140118950 SET id='e175_in_tick';
RELATE finding:input_seat_route_confirmed->cites->steam_routine:FUN_140048630 SET id='e175_in_xlat';
RELATE finding:select_entry_reentry_faults_in_shell->cites->steam_routine:FUN_140270490 SET id='e175_fault_thunk';
RELATE finding:select_entry_reentry_faults_in_shell->cites->finding:default_allocator_is_in_image SET id='e175_fault_prior';
RELATE finding:select_turn_token->supersedes->finding:select_entry_reentry_faults_in_shell SET id='e175_token_vs_reentry', note='written state is the workable path because re-entry faults';

-- ── every finding cites its evidence, then is promoted to confirmed ──────────────────────────────
RELATE finding:select_dispatch_tables->cites->source:select_decompiles_20260905 SET id='e175s_a0';
RELATE finding:select_dispatch_tables->cites->source:supergun_scripted_select_20260905 SET id='e175s_b0';
RELATE finding:select_mode_byte->cites->source:select_decompiles_20260905 SET id='e175s_a1';
RELATE finding:select_mode_byte->cites->source:supergun_scripted_select_20260905 SET id='e175s_b1';
RELATE finding:select_turn_token->cites->source:select_decompiles_20260905 SET id='e175s_a2';
RELATE finding:select_turn_token->cites->source:supergun_scripted_select_20260905 SET id='e175s_b2';
RELATE finding:config_single_local_player_override->cites->source:select_decompiles_20260905 SET id='e175s_a3';
RELATE finding:config_single_local_player_override->cites->source:supergun_scripted_select_20260905 SET id='e175s_b3';
RELATE finding:select_cursor_state_machine->cites->source:select_decompiles_20260905 SET id='e175s_a4';
RELATE finding:select_cursor_state_machine->cites->source:supergun_scripted_select_20260905 SET id='e175s_b4';
RELATE finding:select_advance_and_commit_measured->cites->source:select_decompiles_20260905 SET id='e175s_a5';
RELATE finding:select_advance_and_commit_measured->cites->source:supergun_scripted_select_20260905 SET id='e175s_b5';
RELATE finding:input_seat_route_confirmed->cites->source:select_decompiles_20260905 SET id='e175s_a6';
RELATE finding:input_seat_route_confirmed->cites->source:supergun_scripted_select_20260905 SET id='e175s_b6';
RELATE finding:select_entry_reentry_faults_in_shell->cites->source:select_decompiles_20260905 SET id='e175s_a7';
RELATE finding:select_entry_reentry_faults_in_shell->cites->source:supergun_scripted_select_20260905 SET id='e175s_b7';
RELATE finding:world_raster_state_measured->cites->source:select_decompiles_20260905 SET id='e175s_a8';
RELATE finding:world_raster_state_measured->cites->source:supergun_scripted_select_20260905 SET id='e175s_b8';
RELATE finding:world_raster_state_measured->cites->source:capgate_raster_state_20260905 SET id='e175s_cap';

UPDATE finding:select_dispatch_tables SET status='confirmed';
UPDATE finding:select_mode_byte SET status='confirmed';
UPDATE finding:select_turn_token SET status='confirmed';
UPDATE finding:config_single_local_player_override SET status='confirmed';
UPDATE finding:select_cursor_state_machine SET status='confirmed';
UPDATE finding:select_advance_and_commit_measured SET status='confirmed';
UPDATE finding:input_seat_route_confirmed SET status='confirmed';
UPDATE finding:select_entry_reentry_faults_in_shell SET status='confirmed';
UPDATE finding:world_raster_state_measured SET status='confirmed';
