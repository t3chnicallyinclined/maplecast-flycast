-- 201_battle_entry_chain.surql -- THE BATTLE-ENTRY CHAIN on the Steam MvC2 recompile: what actually runs
-- between the character-select commit (G+2 = 2, G+3 = 0) and the first fighting frame, why it costs
-- exactly 304 ticks, why the VS screen cannot be skipped by input in this build, why the whole game has
-- EXACTLY ONE code path that loads character data, and why a battle-state snapshot CANNOT have its
-- roster swapped by writing bytes. Answers the SUPERGUN question "can the runner start directly at the
-- first battle frame with an arbitrary chosen roster". 2026-09-07.
-- RE METHOD: step 1-2 the battle machine FUN_14060e170 and its jump tables were matched from the already
-- ported select machine (seed 175) and seeded on the constants 300 / 0x3c / 0x96 / 0x360 / 0xfc9f and on
-- the per-slot PL address tables; step 3 every G offset is given through the block map (G = blk+0x3CB8);
-- step 4 CONFIRMED = read in the image AND matched by an independent measurement, INFERRED = read only.
-- Forward slashes only in ref/note; no apostrophes.
USE NS re DB kb;

-- ---------------------------------------------------------------------------------------------------
-- SOURCES
-- ---------------------------------------------------------------------------------------------------
UPSERT source:battle_entry_chain_re_20260907 SET kind='disasm', strength='code',
  ref='Ghidra dumpproj / mvc_dump.bin (base 0x140000000, file offset == RVA): FUN_140611a20 (mode machine, dispatch table PTR_caseD_2_140a6bfd8), FUN_14060e170 (battle machine, dispatch on G+3, sub-dispatch on G+4), its jump-table blocks caseD_5 (the LOAD tick, calls at 0x14060ed63 and 0x14060ed91), code_r0x00014060ebf0 (round init), caseD_7 (the fighting step), caseD_9 (0x14060f201 / 0x14060f21b), FUN_140628570 + PTR_FUN_140a6fa98 (VS intro), FUN_1406285f0 / FUN_1406288b0 / FUN_1406289c0 / FUN_140628ac0 (VS intro sub-states, FUN_1406288b0 read instruction by instruction), FUN_14060e0e0, FUN_14060d100 (PL slot loader), FUN_14060c370 (bank loader), FUN_1406233b0 (six-fighter spawn), FUN_140623680 (per-cid fighter init), FUN_14060efd0 (round re-init), FUN_140607da0 (training reset entry), PTR_FUN_140a6fc60 (G+3 == 1 sub-machine), PTR_LAB_140a6eeb0 (per-character program table), PTR_LAB_142eed3b0 (per-character state table).',
  note='Pointer tables read straight out of mvc_dump.bin with python struct at file offset = VA - 0x140000000. PTR_caseD_2_140a6bfd8 = [0x140611d90, 0x140611e80, 0x140611b50, 0x140611a60, 0x140611a70, 0x140611b60, 0x140611a80] indexed by G+2; index 2 = 0x140611b50 = the block that calls FUN_14060e170 (matches the single xref to FUN_14060e170). PTR_FUN_140a6fa98 = [0x1406285f0, 0x1406288b0, 0x1406289c0, 0x140628ac0] indexed by G+4 during G+3 == 2. PTR_FUN_140a6fc60 = [0x140629890, 0x140629ab0, 0x1406295e0, 0x140629690, 0x1406296c0, 0x140629700, 0x140629740, 0x1406297d0, 0x1406297f0] indexed by G+4 during G+3 == 1. Whole-image xref queries: FUN_14060d100 has exactly ONE caller; FUN_1406233b0 has exactly TWO; FUN_140623680 has exactly THREE.';

UPSERT source:start_match_measurement_20260907 SET kind='reproduction', strength='reproduction',
  ref='supergun/docs/TEST-CHANGELOG.md:276-280 (build sgbuild8); supergun/supersoaker/runner/sg_runner/src/main.rs:3802-3820 (the START_MATCH fast-forward loop), :3553-3579 (rematch), :5588-5619 (the --roster write set).',
  note='Measured: --start-match reaches G+2 == 2 and G+3 >= 3 in 304 load ticks in 4 ms, run back to back with no pacing and no present, instead of 5.1 s at 60 Hz; 787 tile slots live; the first drawn frame is already battle (G+1..4 = [1,2,3,1]); default pixel gate IDENTICAL 4435/4435. The 304 is the independent measurement that the static derivation 1 + 1 + (1 + 240 + 60 + 1) = 304 reproduces exactly.';

-- ---------------------------------------------------------------------------------------------------
-- STEAM ROUTINES
-- ---------------------------------------------------------------------------------------------------
UPSERT steam_routine:FUN_14060e170 SET addr=5375500656, role='BATTLE machine, reached as PTR_caseD_2_140a6bfd8[G+2] with G+2 == 2 (the block 0x140611b50 inside FUN_140611a20). Dispatches on G+3; G+3 == 3 sub-dispatches on G+4 into the SAME jump-table blocks, which is why Ghidra shows one merged switch. G+3 == 0 battle-order commit; == 1 gate (advance iff G+0x9a or G+0x8b, else the order-select sub-machine PTR_FUN_140a6fc60[G+4]); == 2 FUN_140628570 VS intro; == 3 the match itself (G+4 == 0 LOAD caseD_5, == 1 round init, == 2 the fighting step caseD_7); == 4 FUN_140614b70 win screen.';
UPSERT steam_routine:LAB_14060eca0 SET addr=5375502496, role='caseD_5 = THE LOAD TICK (G+3 == 3, G+4 == 0), one tick, everything synchronous, no file I/O (the arc is resident at ctx[0]): G+4++, G+0x19 = 1, G+0x90 = 0, FUN_140609df0(blk+0x6D04 stage), FUN_14060a270, FUN_14060b230, FUN_14060c370(1) stage bank, FUN_14060c370(2) effects bank, [if G+0x14 != 0x40] FUN_14060c370(4) HUD bank, then for slot 0..5 FUN_14060d100(blk+0x3DB8+slot*0x738, slot) (the 11 AFS files per character) and FUN_140612180 (the body-sheet decoder into base+0x145000), then FUN_1406274f0 round block, FUN_14061e170, FUN_1406233b0 SIX-FIGHTER SPAWN, FUN_140621960, FUN_14061c6e0, FUN_140610710, FUN_1406104d0, and [if G+0x14 != 0x40] FUN_140620200 + FUN_140620670.';
UPSERT steam_routine:FUN_1406285f0 SET addr=5375305200, role='VS intro sub-state 0 (PTR_FUN_140a6fa98[0]). Gated on FUN_14060e0e0() (four cursor pairs at 0x142ec68b6/b7, 0x142ec68f6/f7, 0x142ec6892/93, 0x142ec68d2/d3 must be equal). Then: G+4++, G+8 = 300 (the ONLY VS-screen timer), G+0xE = 0, G+0x19 = (G+0x84 == 0), the three-Servbot cid 0x18 fixup, FUN_14060c370(6) = the VS-intro bank AND the per-character 32 KB tail (AFS 3+cid) and fighter+0x1F8, camera blk+0x6914/0x695c reset, the six FUN_14073e540 / FUN_14073e8a0 VS panels, fade FUN_1406105d0(0xffffffff, 0x1e), banner blk+0x6a88..0x6a94.';
UPSERT steam_routine:FUN_1406288b0 SET addr=5375305392, role='VS intro sub-state 1: DEC word [G+8]; AND word [blk+0x3c66], 0xfc9f; AND word [blk+0x3c7a], 0xfc9f; then TEST word [blk+0x3c66], 0x360 / TEST word [blk+0x3c7a], 0x360 -- the two masks are complements, so the input skip is provably always false. Advance therefore happens only on G+8 == 0x3c, i.e. after 240 ticks. G+8 == 0x96 sets G+0xE = 1 (the second banner beat).';
UPSERT steam_routine:FUN_1406289c0 SET addr=5375305664, role='VS intro sub-state 2: DEC G+8 from 0x3c to 0, advance at 0 = 60 ticks; below 0x1f FUN_140609bb0(1, 0x1e); at 10 G+0xE = 3; while G+0xE > 1 fades the banner blk+0x6a94 by DAT_14097e89c and blk+0x6a93 += 2, and on completion FUN_1406104b0(0xffffffff) and hostdev+0x280 = -1.';
UPSERT steam_routine:FUN_140628ac0 SET addr=5375305920, role='VS intro sub-state 3, one tick: G+3++ (to 3 = the match), G+4..7 = 0, G+0x19 = 1, then the VS-screen ORDER SWAP: per human side, the held word blk+0x4420 / blk+0x4b58 masked with 0x90 swaps the team-position bytes +0x6D3 (blk+0x448b / 0x52fb / 0x616b and 0x4bc3 / 0x5a33 / 0x68a3). FUN_1406104b0(0xffffffff), hostdev+0x280 = 0.';
UPSERT steam_routine:FUN_1406233b0 SET addr=5375272880, role='SIX-FIGHTER SPAWN. For side 0..1, slot 0..2 takes the entity pointer from *(blk+0x324E0 + 0x20 + i*8), sets about 40 fields (obj+0 point flag, +0x170 visible, +0x38, +0x50/+0x54/+0x58 from LAB_140a6d8b0 + side*0xc, +0x282/+0x174 facing, +0x558/+0x560 the shared meter cells, +0x130/+0x134/+0x14c), sets the PER-CHARACTER state table pointer obj+0x580 = &PTR_LAB_142eed3b0 + obj[0x230]*0x1e, then calls FUN_1406627e0 and FUN_140623680. Callers: caseD_5 at 0x14060ed91 and FUN_14060efd0 at 0x14060f009 -- nothing else in the image.';
UPSERT steam_routine:FUN_140623680 SET addr=5375272576, role='PER-CID FIGHTER INIT. memset fighter+0x280 .. +0x557 (0x2d8 B), fighter+1 = fighter+0x6C0 (cid), +0x39 = +0x6C1 (costume), then (*(code *)(&PTR_LAB_140a6eeb0)[cid])(fighter) = the per-character program (the DC S_PLxx overlay, compiled into the exe on Steam), FUN_140654890, +0x2b7 = DAT_140a97ec0[cid], FUN_140639c10, +0x180..+0x183 = DAT_140a97fd0[cid*4], +0x46d = DAT_140a98328[cid]*2, the team bitmask 1 << (cid AND 0x1f) into +0x56c (cid >= 0x20) or +0x570 (cid < 0x20), the cid 0x18/0x19/0x1a scale specials (+0x6c/+0x70), memset +0x36c..+0x46b, and a 20-case per-cid switch (cids 0, 3, 4, 6, 10, 0xb, 0xc, 0x10, 0x14, 0x15, 0x18, 0x19, 0x1c, 0x1e, 0x1f, 0x27, 0x2a, 0x2c, 0x38) calling FUN_1406551e0 / FUN_1406555a0 / FUN_140657b50 / FUN_140657e20 / FUN_1406580d0 / FUN_1406582c0 / FUN_140659d50 / FUN_14065a0b0 / FUN_14065c250 / FUN_14065ccb0 / FUN_14065cfe0 / FUN_14065d390 / FUN_14065d560 / FUN_14065da20 / FUN_14065da60 / FUN_14065e630 / FUN_14065e850 / FUN_14065f110 / FUN_14065f5e0 / FUN_14065f860 / FUN_14065fb30 / FUN_14065fd30 / FUN_14065ff80.';

-- ---------------------------------------------------------------------------------------------------
-- FINDINGS
-- ---------------------------------------------------------------------------------------------------
UPSERT finding:battle_entry_chain_is_304_ticks SET
  title='The select-commit to first fighting frame costs EXACTLY 304 ticks, and 302 of them are one 16-bit VS-screen countdown at G+8',
  statement='Static derivation from FUN_14060e170 and PTR_FUN_140a6fa98, tick by tick. G+3 = 0 (battle-order commit: moves cid/assist/costume/team-position from the select fields into battle order using source slot (f+0x6B0 AND 1) + f+0x6D3*2, sets G+0x99 = G+0x84 and G+0x8b = (G+0x84 == 3), clears the six +0x6B1 CPU flags per the human mask, FUN_140624d90 / FUN_14061e170 / FUN_140609b40 x2) = 1 tick. G+3 = 1 advances immediately iff G+0x9a or G+0x8b is nonzero, i.e. immediately whenever both sides are human (G+0x84 == 3) or the mode was just entered = 1 tick; otherwise it runs the order-select sub-machine PTR_FUN_140a6fc60[G+4], which is INPUT-DRIVEN. G+3 = 2 VS intro = 1 tick (sub 0, sets G+8 = 300) + 240 ticks (sub 1, decrements G+8 to 0x3c) + 60 ticks (sub 2, decrements 0x3c to 0) + 1 tick (sub 3, G+3++) = 302. G+3 = 3, G+4 = 0 = the LOAD, ONE tick, all synchronous memcpy out of the resident arc at ctx[0]. G+4 = 1 round init = 1 tick. G+4 = 2 is the first fighting frame. Total 1 + 1 + 302 = 304 ticks before the load tick is entered. There is NO other timer: no frame counter, no animation clock, no I/O wait, nothing asynchronous.',
  consequence='The measured 304 load ticks in 4 ms of the SUPERGUN runner --start-match (supergun/docs/TEST-CHANGELOG.md:280) is the whole chain, and the static derivation reproduces the number exactly. Loading a match is not slow because the game is doing work; it is slow at 60 Hz because the game is showing a 5 s VS screen. Ticked back to back with no pacing and no present the entire thing is under one frame of wall time, so the answer to instant match with a chosen roster is RUN THE GAME LOADER FAST, not skip it.',
  status='confirmed', confidence='high', date='2026-09-07', contribution_candidate=true;

UPSERT finding:vs_screen_input_skip_is_dead_code SET
  title='The VS screen cannot be skipped by input in this build: FUN_1406288b0 clears the very bits it then tests',
  statement='0x1406288bb MOV ECX, 0xfc9f; 0x1406288c0 MOV R8D, 0x360; 0x1406288d1 AND word ptr [RAX + 0x3c66], CX; 0x1406288df AND word ptr [RAX + 0x3c7a], CX; then 0x140628900 TEST word ptr [RCX + 0x3c66], R8W and 0x14062890f TEST word ptr [RCX + 0x3c7a], R8W. 0xfc9f is the exact complement of 0x360 over 16 bits, so both TESTs are provably zero on every path. The only surviving advance condition in sub-state 1 is CMP word ptr [RAX + 8], 0x3c.',
  consequence='Feeding seat words during the VS screen changes nothing; a shell that wants a shorter VS screen has to write the countdown G+8 (blk+0x3CC0) itself, which is NOT a path the real game can reach and therefore needs a Gate 1 style comparison against a real battle-entry blk before it is trusted. Given the whole chain is 4 ms unpaced, do not.',
  status='confirmed', confidence='high', date='2026-09-07', contribution_candidate=true;

UPSERT finding:one_and_only_character_load_path SET
  title='FUN_14060d100 (the PL slot loader) has EXACTLY ONE caller in the entire image -- there is no second, shorter way to load a character',
  statement='Whole-image xref query on mvc_dump.bin: FUN_14060d100 is called only from caseD_5 at 0x14060ed63 (the LOAD tick of the battle machine). FUN_1406233b0 (the six-fighter spawn) is called only from caseD_5 at 0x14060ed91 and from FUN_14060efd0 at 0x14060f009 (the training reset). FUN_140623680 (the per-cid init) is called only from FUN_1406233b0 at 0x140623576 and twice from caseD_9 at 0x14060f201 / 0x14060f21b. There is therefore no arcade path, no attract or demo path and no quick-match path that reaches battle by a different loader; every mode funnels through the same caseD_5.',
  consequence='Falsification test PASSED for the hypothesis that some other mode jumps closer to battle. Any SUPERGUN entry to a match with a chosen roster must run caseD_5, and the only free variable is how fast the ticks before it are run.',
  status='confirmed', confidence='high', date='2026-09-07', contribution_candidate=true;

UPSERT finding:battle_roster_cannot_be_swapped_by_bytes SET
  title='A battle-state snapshot CANNOT have its roster swapped by writing bytes: the per-fighter blk state is produced by per-cid CODE, not by any table',
  statement='Half the character state IS bytes-derivable and is already proven so: the DC-RAM half is rebuilt from the user own game_50.arc byte-exact (Gate 3, 66/66 files over six slots, UNEXPLAINED NONE on two dumps, supergun/docs/RECEIPT-RUNNER-GATE3.md and RECEIPT-RUNNER-DCRAM.md section 2.1), and the fighter pointer fields +0x1A8/+0x1B0/+0x1B8/+0x1E0/+0x1E8/+0x1F0/+0x1F8/+0x200/+0x208/+0x210/+0x218/+0x220/+0x1C0/+0x1C8/+0x1D0/+0x1D8/+0x6B8 are pure arithmetic on the per-slot base and the PL header dwords. The OTHER half is not. FUN_1406233b0 then FUN_140623680 build every fighter entity in blk by CALLING the per-character program (*(&PTR_LAB_140a6eeb0)[cid])(fighter), by installing the per-character state table &PTR_LAB_142eed3b0 + slot*0x1e, by indexing DAT_140a97ec0[cid] / DAT_140a97fd0[cid*4] / DAT_140a98328[cid], by setting the team bitmask 1 << (cid AND 0x1f) into +0x56c or +0x570 depending on cid < 0x20, by the cid 0x18-0x1a scale specials, and by a 20-case per-cid switch of further setup routines -- and the palette rows blk+0x1040 + bank*0x38 come from FUN_140612930(DatPal + costume*0x100). None of that is a lookup a shell can copy; it is arbitrary code output.',
  consequence='DECIDES the architecture question. Load the match state directly can only mean (a) restore a battle anchor for the roster that anchor was captured with -- proven and byte-exact (finding:determinism_battle_anchor_carry, Gates 1 and 2) -- or (b) write the select write set and RUN the game loader, also proven (the runner --roster / --start-match paths). It can NEVER mean take one battle anchor and retype the cids: that would require reimplementing PTR_LAB_140a6eeb0 for 56 characters, the exact failure mode the SUPERGUN thesis exists to avoid. And an anchor per roster is not a product: the roster space is over 3e10 teams.',
  consequence_2='UNKNOWN and worth one experiment if a shorter path is ever needed: swap the six DC-RAM PL slots offline (dcram_build.py, bytes only) plus the fighter file-pointer fields, then let the game own FUN_14060efd0 respawn, which would have game code produce the blk half from the new cid in ONE tick. Untested, and it skips FUN_14060c370(4) and (6) so the HUD portrait pages and the 32 KB per-character tails would still be the old roster.',
  status='confirmed', confidence='high', date='2026-09-07', contribution_candidate=true;

UPSERT finding:training_reset_is_a_one_tick_round_restart SET
  title='FUN_14060efd0 restarts a round in ONE tick without running any loader -- it can restart the loaded roster, never change it',
  statement='FUN_14060efd0 sets G+4 = 1, G+0x19 = 1, G+0x90 = 0 and then re-runs exactly the TAIL of caseD_5 -- FUN_1406274f0, FUN_14061e170, FUN_1406233b0 (the six-fighter spawn), FUN_140621960, FUN_1406146d0(fighter_i, 0) for i = 0..5, FUN_14061c6e0, FUN_14061ca70, FUN_140610710, FUN_1406104d0, FUN_140620200, FUN_140620670 -- then battle+0x65 = 99 and inlines the round-init block caseD_6 and the fighting step caseD_7 TWICE, so it returns with the match already running. It does NOT call FUN_140609df0 (stage), FUN_14060c370 (any bank) or FUN_14060d100 (the PL slots). Its only caller is FUN_140607da0, the training reset entry stored at config+0x28 and gated on config+0x828 != 0.',
  consequence='The game already owns a one-tick match restart, and SUPERGUN can call it between ticks. It is the right primitive for RESET / RESET LEFT / RESET RIGHT and for a same-roster run-it-back that must not cost 304 ticks -- but because it runs no loader it cannot change the roster, the costumes read from DatPal or the stage. A roster change always costs the full chain.',
  status='confirmed', confidence='high', date='2026-09-07', contribution_candidate=true;

UPSERT finding:supergun_instant_match_recipe SET
  title='SUPERGUN instant match with an arbitrary roster: the recipe that exists today, and the two gaps in it',
  statement='EXISTS AND MEASURED: sg_runner --start-match writes the roster onto the fighters through the game own rematch path (main.rs:3553 rematch: fighter+0x6C0 cid, +0x6C1 costume, +0x655 assist, +0x6B0 slot, +0x6D3 team position, blk+0x6D04 stage, G+0x14 = 0x60, G+2 = 2, G+3..7 = 0, plus the restored human masks blk+0x3D3C and blk+0x3CD0) and then runs the game own load ticks back to back with no pacing and no present until G+2 == 2 and G+3 >= 3 (main.rs:3802), walking the tile ring on every tick so the first battle frame has textures. Measured 304 ticks in 4 ms. GAP 1: --start-match takes only the six cids -- it fakes LAST_MATCH with costume 0, assist 0, stage 5 (main.rs:5662) -- while the separate --roster path (main.rs:5588) accepts --costume / --assist / --stage but is not wired to the fast-forward loop and does not restore the human masks, so from a select anchor whose G+0x84 is 0 it can enter the match with all six fighters CPU (finding:supergun_rematch_both_cpu_fix). GAP 2: G+3 == 1 only advances in one tick when G+0x84 == 3 or G+0x9a != 0; in a one-human mode it drops into the input-driven order-select sub-machine PTR_FUN_140a6fc60[G+4], which no shell currently answers.',
  consequence='The work is to MERGE the two runner paths -- one entry that takes cids, costumes, assists, stage and the human mask, writes the select write set, and runs the fast-forward -- not to invent a new way into battle. The gate is unchanged and is not it reached battle: the battle-entry blk the shell produces must equal one the real game produced for the same roster and stage.',
  status='inferred', confidence='high', date='2026-09-07',
  gate='NOT YET RUN: capture a real same-roster same-stage battle-entry blk and compare it byte for byte against the one --start-match produces, at minimum blk+0x3D3C, the six fighter+0x6B1 CPU flags, the six +0x6C0/+0x6C1/+0x655/+0x6D3 in battle order, blk+0x6D04 and the palette rows blk+0x1040 + bank*0x38.',
  ref='supergun/supersoaker/runner/sg_runner/src/main.rs:3553-3579 rematch, :3802-3820 START_MATCH, :5588-5619 --roster; supergun/docs/TEST-CHANGELOG.md:276-280',
  contribution_candidate=true;

UPSERT finding:mode_byte_G2_is_a_dispatch_index SET
  title='G+2 is an index into PTR_caseD_2_140a6bfd8: [1] = character select, [2] = the battle machine FUN_14060e170',
  statement='FUN_140611a20 dispatches on G+1; its case 1 calls (*(code *)(&PTR_caseD_2_140a6bfd8)[(char)G+2])(). The table at 0x140a6bfd8 reads [0x140611d90, 0x140611e80, 0x140611b50, 0x140611a60, 0x140611a70, 0x140611b60, 0x140611a80]; entry 2 is 0x140611b50, which is the only xref to FUN_14060e170 in the image. Ghidra shows these as merged cases of FUN_140611a20 because they are jump-table blocks inside it, not separate functions.',
  consequence='The runner single byte G+2 = 2 that hands character select over to the battle machine is literally a dispatch index, which is why it is safe to write and why G+3..G+7 must be zeroed with it (FUN_14062BB30 does exactly that when the game commits the select).',
  status='confirmed', confidence='high', date='2026-09-07';

-- ---------------------------------------------------------------------------------------------------
-- EDGES
-- ---------------------------------------------------------------------------------------------------
RELATE finding:battle_entry_chain_is_304_ticks->cites->source:battle_entry_chain_re_20260907 SET id='e201_304_src';
RELATE finding:battle_entry_chain_is_304_ticks->cites->source:start_match_measurement_20260907 SET id='e201_304_meas';
RELATE finding:battle_entry_chain_is_304_ticks->cites->steam_routine:FUN_14060e170 SET id='e201_304_r1';
RELATE finding:battle_entry_chain_is_304_ticks->cites->steam_routine:FUN_1406285f0 SET id='e201_304_r2';
RELATE finding:battle_entry_chain_is_304_ticks->cites->steam_routine:FUN_1406288b0 SET id='e201_304_r3';
RELATE finding:battle_entry_chain_is_304_ticks->cites->steam_routine:FUN_1406289c0 SET id='e201_304_r4';
RELATE finding:battle_entry_chain_is_304_ticks->cites->steam_routine:LAB_14060eca0 SET id='e201_304_r5';
RELATE finding:battle_entry_chain_is_304_ticks->cites->finding:live_charselect_committer SET id='e201_304_f1';
RELATE finding:vs_screen_input_skip_is_dead_code->cites->source:battle_entry_chain_re_20260907 SET id='e201_skip_src';
RELATE finding:vs_screen_input_skip_is_dead_code->cites->steam_routine:FUN_1406288b0 SET id='e201_skip_r1';
RELATE finding:one_and_only_character_load_path->cites->source:battle_entry_chain_re_20260907 SET id='e201_one_src';
RELATE finding:one_and_only_character_load_path->cites->steam_routine:LAB_14060eca0 SET id='e201_one_r1';
RELATE finding:one_and_only_character_load_path->cites->steam_routine:FUN_1406233b0 SET id='e201_one_r2';
RELATE finding:battle_roster_cannot_be_swapped_by_bytes->cites->source:battle_entry_chain_re_20260907 SET id='e201_swap_src';
RELATE finding:battle_roster_cannot_be_swapped_by_bytes->cites->steam_routine:FUN_140623680 SET id='e201_swap_r1';
RELATE finding:battle_roster_cannot_be_swapped_by_bytes->cites->steam_routine:FUN_1406233b0 SET id='e201_swap_r2';
RELATE finding:battle_roster_cannot_be_swapped_by_bytes->cites->finding:determinism_battle_anchor_carry SET id='e201_swap_f1';
RELATE finding:battle_roster_cannot_be_swapped_by_bytes->cites->finding:anchor_phase_validity SET id='e201_swap_f2';
RELATE finding:training_reset_is_a_one_tick_round_restart->cites->source:battle_entry_chain_re_20260907 SET id='e201_rst_src';
RELATE finding:training_reset_is_a_one_tick_round_restart->cites->steam_routine:FUN_14060efd0 SET id='e201_rst_r1';
RELATE finding:training_reset_is_a_one_tick_round_restart->cites->finding:steam_training_reset_entry_config_0x28 SET id='e201_rst_f1';
RELATE finding:supergun_instant_match_recipe->cites->source:start_match_measurement_20260907 SET id='e201_rec_src';
RELATE finding:supergun_instant_match_recipe->cites->finding:battle_entry_chain_is_304_ticks SET id='e201_rec_f1';
RELATE finding:supergun_instant_match_recipe->cites->finding:battle_roster_cannot_be_swapped_by_bytes SET id='e201_rec_f2';
RELATE finding:supergun_instant_match_recipe->cites->finding:supergun_rematch_both_cpu_fix SET id='e201_rec_f3';
RELATE finding:supergun_instant_match_recipe->cites->finding:supergun_rematch_roster_slot_entry SET id='e201_rec_f4';
RELATE finding:mode_byte_G2_is_a_dispatch_index->cites->source:battle_entry_chain_re_20260907 SET id='e201_g2_src';
RELATE finding:mode_byte_G2_is_a_dispatch_index->cites->finding:live_charselect_committer SET id='e201_g2_f1';

-- Promotion AFTER the cites edges exist. supergun_instant_match_recipe stays INFERRED: its gate has not run.
UPDATE finding:battle_entry_chain_is_304_ticks SET status='confirmed';
UPDATE finding:vs_screen_input_skip_is_dead_code SET status='confirmed';
UPDATE finding:one_and_only_character_load_path SET status='confirmed';
UPDATE finding:battle_roster_cannot_be_swapped_by_bytes SET status='confirmed';
UPDATE finding:training_reset_is_a_one_tick_round_restart SET status='confirmed';
UPDATE finding:mode_byte_G2_is_a_dispatch_index SET status='confirmed';

-- ---------------------------------------------------------------------------------------------------
-- ADDENDUM 2026-09-07, same session: both gaps named above were CLOSED in the runner working tree
-- while this seed was being written (the edit cites seed 201 in its own comment). Re-read of
-- supergun/supersoaker/runner/sg_runner/src/main.rs after the edit:
--   :4714  "--start-match" => { roster = next(); START_MATCH = true }   -- now literally --roster plus
--          the fast-forward, so --costume / --assist / --stage apply to it (GAP 1 closed).
--   :5629-5631  the --roster block now writes blk+0x3D3C and blk+0x3CD0 from LAST_HUMAN_MASK
--          (default (3,3)) and logs them (GAP 2 closed).
-- Independent measurement widened at the same time: three runner logs under %TEMP%/rr_play/{sm1,sm2,sm3}
-- all report "battle reached in 304 ticks" (4, 5 and 4 ms) on roster [44,44,42,42,52,8] -- sm1 through
-- rematch() with humanMask 3/3, sm2 and sm3 through the --roster write set. 304 is therefore three
-- reproductions, not one doc line. Roster-INDEPENDENCE of the 304 remains INFERRED: no timer on the
-- chain reads a cid, but only one roster has been measured.
-- ---------------------------------------------------------------------------------------------------
UPSERT source:start_match_runner_logs_20260907 SET kind='reproduction', strength='reproduction',
  ref='%TEMP%/rr_play/sm1/runner.log:290, %TEMP%/rr_play/sm2/runner.log:281, %TEMP%/rr_play/sm3/runner.log:282',
  note='Three runs, same roster [44, 44, 42, 42, 52, 8]: START MATCH battle reached in 304 ticks (4 ms / 5 ms / 4 ms of load, run back to back instead of 5.1 s at 60 Hz), 787 tile slots live in each. sm1 took the rematch path (REMATCH roster ... humanMask G+0x84=0x03 G+0x18=0x03, stage 5); sm2 and sm3 took the --roster write set (BUILT A MATCH, costume [1]x6, assist [1,1,0,0,2,2] and [0]x6, stage 14 unchanged) and sm3 also logged human masks G+0x84 = 3 G+0x18 = 3. Same 304 on both paths and on both assist sets.';

UPSERT finding:supergun_instant_match_recipe SET
  title='SUPERGUN instant match with an arbitrary roster: --start-match == --roster plus the load fast-forward; 304 ticks, 4 ms, reproduced three times',
  statement='sg_runner --start-match CIDS (main.rs:4714) is now exactly --roster CIDS plus the fast-forward, so --costume / --assist / --stage apply. The write set (main.rs:5588-5637) is the character-select write set and nothing else: per fighter i at blk+0x3DB8+i*0x738 write +0x6C0 cid, +0x6C1 costume, +0x655 assist, +0x6B0 slot, +0x6D3 team position (i >> 1); blk+0x6D04 stage; G+0x14 = 0x60 (else FUN_14060C370(4) never runs and the HUD keeps the previous roster pages); the human masks blk+0x3D3C and blk+0x3CD0 from LAST_HUMAN_MASK, default (3,3) (main.rs:5629-5631, added 2026-09-07 citing this seed); then G+2 = 2 and G+3..G+7 = 0, which is exactly what FUN_14062BB30 does when the game commits the select. The runner then ticks the game own frame function back to back with no pacing and no present until G+2 == 2 and G+3 >= 3 (main.rs:3810), walking the tile ring every tick so the first battle frame has textures. Measured 304 ticks in 4-5 ms in three independent runs. NOTHING is synthesised: every one of those ticks is the game own loader.',
  consequence='This is the answer to instant match with a chosen roster and it is already built. Do NOT add a battle anchor per roster (impossible, see finding:battle_roster_cannot_be_swapped_by_bytes) and do NOT poke the VS countdown G+8 (see finding:vs_screen_input_skip_is_dead_code). The only remaining known hole is a one-human mode: G+3 == 1 advances in one tick only when G+0x84 == 3 or G+0x9a != 0, otherwise it drops into the input-driven order-select sub-machine PTR_FUN_140a6fc60[G+4] that no shell answers, so an arcade-style --start-match with mask 1 would stall to the 900-tick cap. Untested.',
  status='inferred', confidence='high', date='2026-09-07',
  gate='STILL NOT RUN: capture a real same-roster same-stage battle-entry blk and compare it byte for byte against the one --start-match produces, at minimum blk+0x3D3C, the six fighter+0x6B1 CPU flags, the six +0x6C0/+0x6C1/+0x655/+0x6D3 in battle order, blk+0x6D04 and the palette rows blk+0x1040 + bank*0x38. Reaching battle is not the gate.',
  ref='supergun/supersoaker/runner/sg_runner/src/main.rs:3810 START_MATCH loop, :4711-4714 the flags, :5588-5637 the write set, :3553-3579 rematch; supergun/docs/TEST-CHANGELOG.md:276-280',
  contribution_candidate=true;

RELATE finding:supergun_instant_match_recipe->cites->source:start_match_runner_logs_20260907 SET id='e201_rec_logs';
RELATE finding:battle_entry_chain_is_304_ticks->cites->source:start_match_runner_logs_20260907 SET id='e201_304_logs';
