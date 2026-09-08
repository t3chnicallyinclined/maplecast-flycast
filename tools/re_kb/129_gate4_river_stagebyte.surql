-- 129_gate4_river_stagebyte.surql -- GATE 4 addendum: the live stage-0x10 gate, N1c on four stages, the
-- stage-7 vs stage-16 write-set asymmetry, and the mechanism + safety predicate for blk+0x6D04.
-- SUPERGUN ENGINE lane, 2026-09-04 (odd seeds; 128 is the server-architecture lane).
-- Doc: mvc-live-skins-quarters/docs/RECEIPT-RUNNER-GATE4.md addendum A1..A5
-- Idempotent and replayable; explicit edge ids; additive fields only on findings owned by earlier seeds.
USE NS re DB kb;

UPSERT source:gate4_river_dumps SET kind='capture', strength='runtime',
  ref='mvc-live-skins-quarters/d3dcap/ttd/runs/river_a (stage 7 Ice River, clock 7490, blk 0x13fc1000), river_b (stage 16 River Raft, clock 1692, blk 0x166c1000), riverraft_river1 (same boot as river_a, clock 238)',
  note='Captured by Tris 2026-09-04, mid-round, unreadable_pages empty. river_a and river_b differ by 1,680 of 8,192 DC-RAM pages.';

UPSERT steam_routine:FUN_14062a720 SET addr='0x14062a720',
  role='stage-SELECT cursor: reads the pad bitmask at blk+0x33A68 + side*2, bit 0x800 decrements and bit 0x400 increments blk+0x6D04, wraps mod 0x11 (17 stages), then prints the debug string STAGE %02X (s_STAGE____02X_14097e8d0). Reached from the menu dispatch table at 0x140a6fbb0.';

UPSERT finding:stage10_fixup_confirmed_live SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='The stage-0x10 ISP/TSP fix-up is CONFIRMED against live memory: 16 bytes differ without it, 0 with it',
  statement='dcram_build.py --gate on the live River Raft dump (river_b, stage 16) returns UNEXPLAINED NONE and the stage POL does not appear in the residual list at all. Explicitly, over the whole 0x30000-byte stage bank: an arc build WITHOUT the FUN_14060d470 stage-0x10 branch differs from the live image in exactly 16 bytes; WITH it, 0 bytes differ. All 16 fix-up bytes match live individually (0x83 to 0x87 on rec+0x7 where texIndex == 10, 0x82 to 0x80 on rec+0x3 where texIndex == 5, models 1..8). The Ice River dump (river_a, stage 7) also returns UNEXPLAINED NONE. This closes the open item of finding:stage10_isp_tsp_fixup_exercised, which had only ever run offline.';

UPSERT finding:n1c_four_stages SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='N1c arm C passes on four stages: 9, 3 Carnival, 7 Ice River, 16 River Raft',
  statement='Three-arm check at depth 8, 300 ticks, a rollback after every eligible tick. Arm A (blk + GGPO) FAILS only on stage 9 and PASSES on 3, 7 and 16. Arm B (A + the proven 8 B ctx+0x1F8230..0x1F8238) passes on all four. Arm C (A + the shipped 64 KB window ctx[0x1F0000..0x200000)) PASSES on all four, so the netcode lane s9.2 save set still survives and the escalate branch (B fails, C passes) has still never been reached. Arm C cost at depth 8, event p50/p99/max: river_b 0.434/0.937/2.153 ms, river_a 0.441/0.896/1.179 ms; 0 events over the 16.667 ms budget in any arm; heap_wraps 0 throughout.',
  caveat='river_a and river_b were driven with IDLE inputs because no tape matches those boots. Idle exercises the prop/water animation and the whole render path but not fighter action. Stage 9 and Carnival used real recorded inputs.';

UPSERT finding:river_stage_writeset_asymmetry SET status='confirmed', confidence='high', date='2026-09-04',
  title='Stage 7 and stage 16 share a routine table but NOT a per-frame DC-RAM write set',
  statement='RECEIPT-RUNNER-DCRAM s4.2 states that on stage 7 / 0x10 the three water meshes (models 9/11/13) are rewritten every frame by FUN_140750eb0. Measured over 300 ticks from the live anchors: stage 7 (river_a) writes 5 DC-RAM pages per run of which 2 are stage POL (0x0D854000..0x0D856000), and its live stage POL differs from the pristine arc by 753 B in 296 runs. Stage 16 (river_b) writes 2 pages, NONE of them stage POL, and its live stage POL equals the pristine arc bytes EXACTLY (0 B differ). Both anchors have the same list-5 node structure with models 9, 11 and 13 bound and drawn. So the per-frame rewrite claim holds for stage 7 and not for stage 16 from these anchors.',
  open_question='Mechanism UNKNOWN: phase-dependent gating, a different per-stage index list, or the water callback not installed on 16. Consequence for the loader: on stage 16 the arc bytes are the final bytes; on stage 7 they are a starting point the engine overwrites.';

UPSERT finding:stage_byte_6d04_is_a_select_cursor SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='blk+0x6D04 is the stage-SELECT cursor, and it is authoritative exactly when the battle-mode predicate holds',
  statement='FUN_14062a720 is the stage-select cursor: it reads the pad bitmask at blk+0x33A68 + side*2, decrements blk+0x6D04 on bit 0x800 and increments on bit 0x400, wraps the result mod 0x11 (17 stages), and prints the debug string STAGE %02X (s_STAGE____02X_14097e8d0); it is reached from the menu dispatch table at 0x140a6fbb0. Other writers are non-battle: FUN_14060e190 and FUN_14060e250 (demo/attract, blk+0x6D04 = G+0x2C & 7, from caseD_4 and caseD_1) and the fixed writes in FUN_14060af70 (0), FUN_14062bc80 (0x11) and FUN_14073f200 (0xB). The match loader CONSUMES the field: caseD_5 reads it at 0x14060ed0a, FUN_14060c370 case 1 indexes DAT_140a6aa80[id] and DAT_140a6aa30[id] to pick the stage POL and TEX AFS entries, and FUN_14060d470 re-reads it for the stage-0x10 fix-up. So once a stage bank exists in DC-RAM the byte is necessarily the stage that bank belongs to. The comment const per match on harvest.rs:1183 is therefore wrong in letter; that line also names blk+0x6D3C in prose while the constant STG_OFF is 0x6d04.',
  gate='Across all 16 dumps on disk: 13 frames satisfy the battle-mode predicate blk+0x3CB8[0..2] == (2,1,2), and in 13 of 13 blk+0x6D04 matches the resident stage bank, identified by byte-exact comparison of the resident TEX at DC 0x0D85D000 against AFS 802+2*id (all 17 stage TEX files are byte-unique). Zero mismatches.';

UPSERT finding:stage_byte_hazard_is_the_select_window SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='The blk+0x6D04 hazard window is CHARACTER SELECT, where health already reads full -- not the pre-load window',
  statement='Two distinct non-battle states were found in the dumps on disk. (1) PRE-LOAD: riverraft_river1 and riverraft at clock 238 have blk+0x6D04 = 6 while NO stage bank is loaded (POL header at 0x0D82D000 all zeros, TEX at 0x0D85D000 all 0xCD) and all six fighters at HP 0; mode bytes (2,1,1). A health-gated sampler cannot fire here. Note also that blk is allocated once per BOOT (FUN_140607b50), so two dumps sharing a blk address share a boot and not a match; equal allocation is not evidence of one continuous match. (2) CHARACTER SELECT: receipt-20260903-165128 has blk+0x6D04 = 1, all six fighters at HP 144, and the resident stage bank is stage 0; mode bytes (1,1,1). THIS is the real hazard: the cursor has advanced to the next selection while the previous match bank is still resident, and health already reads full, so a health-only start gate can fire on a frame whose stage byte is wrong. The agent primary gate is gs_match_load with a fallback of all real characters at hp >= 144 for 1.5 s; the fallback is the exposed path.',
  consequence='The safe predicate is the battle-mode triple blk+0x3CB8[0..2] == (2,1,2), which already exists as the runner C1 self-check (RECEIPT-RUNNER-GATE1 s1 step 5) and separates all 16 dumps perfectly, where health does not. A free second confirmation: the resident stage TEX at DC 0x0D85D000 matches AFS 802+2*id byte-exactly, and a zero dword at DC 0x0D82D000 means no bank is loaded. NOT IMPLEMENTED HERE -- the agent belongs to the coordinator lane; this is mechanism, predicate and counter-example only.';

UPSERT finding:stage10_isp_tsp_fixup_exercised SET
  live_gate_note='2026-09-04: closed against live memory by finding:stage10_fixup_confirmed_live -- 16 bytes differ without the fix-up, 0 with it, on the River Raft dump river_b.';
UPSERT finding:n1c_shipped_window_survives SET
  four_stage_note='2026-09-04: extended to four stages (9, 3, 7, 16) by finding:n1c_four_stages. Arm C still unbroken.';

RELATE finding:stage10_fixup_confirmed_live->cites:g4r_s10_run->source:gate4_river_dumps;
RELATE finding:stage10_fixup_confirmed_live->cites:g4r_s10_tool->source:tool_dcram_poison;
RELATE finding:stage10_fixup_confirmed_live->about:g4r_s10_fn->steam_routine:FUN_14060d470;
RELATE finding:n1c_four_stages->cites:g4r_n1c_run->source:gate4_river_dumps;
RELATE finding:n1c_four_stages->cites:g4r_n1c_tool->source:tool_gate_n1c;
RELATE finding:river_stage_writeset_asymmetry->cites:g4r_ws_run->source:gate4_river_dumps;
RELATE finding:river_stage_writeset_asymmetry->cites:g4r_ws_tool->source:tool_dcram_poison;
RELATE finding:stage_byte_6d04_is_a_select_cursor->cites:g4r_sb_run->source:gate4_river_dumps;
RELATE finding:stage_byte_6d04_is_a_select_cursor->cites:g4r_sb_tool->source:tool_dcram_poison;
RELATE finding:stage_byte_6d04_is_a_select_cursor->about:g4r_sb_fn->steam_routine:FUN_14062a720;
RELATE finding:stage_byte_hazard_is_the_select_window->cites:g4r_hz_run->source:gate4_river_dumps;
RELATE finding:stage_byte_hazard_is_the_select_window->cites:g4r_hz_tool->source:tool_dcram_poison;
RELATE finding:stage_byte_hazard_is_the_select_window->about:g4r_hz_fn->steam_routine:FUN_14062a720;
