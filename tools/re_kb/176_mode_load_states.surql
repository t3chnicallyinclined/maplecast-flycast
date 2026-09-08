-- 176_mode_load_states.surql -- CHARACTER SELECT IS A DIFFERENT LOAD STATE PER MODE, and the
-- discriminator is the HOST DEVICE session block, not blk.
-- 2026-09-05. Values are LIVE reads of the running Steam game (probe_mode.py, read-only, no suspend)
-- compared against the select-01 arcade anchor. Corrects finding:select_mode_byte from seed 175.
USE NS re DB kb;

UPSERT source:live_mode_probe_20260905 SET kind='capture', strength='reproduction',
  ref='d3dcap/ttd/probe_mode.py against the live MarvelVsCapcomFightingCollection.exe, read-only via ReadProcessMemory, 1 Hz, printing on change. Compared against d3dcap/ttd/runs/select-01/pre/{blk.bin,exe_image.bin,host_device.bin}.',
  note='TWO processes share the image name: a launcher whose *(base+0xACD3A0) is NULL and the game. Measured pids [37020, 7376]; only 7376 had game_state = 0x140ac6d40. dump_live.py find_pid returned the FIRST match, so --at-select polled the launcher forever and printed nothing.';

UPSERT finding:mode_lives_in_host_device_session SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The game MODE is carried by the host-device session block at DAT_140acd3a8+0xd03xx, NOT by blk',
  statement='FUN_140037b50 derives the whole mode configuration from the host device: config+0x828 = 0 and config+0x82c = 0 unless session+0xd0330 != 0, in which case config+0x828 = session+0xd0334 + 1 and config+0x82c = FUN_14004b130(host). config+0x80c = 0 unless session+0xd0338 != 0, in which case config+0x80c = 1, +0x810 = 9, +0x82c = FUN_14004b130(host). Then a four-way ladder on config+0x7bc / +0x828 / +0x80c / +0x7c8 (with session+0xd0384) / +0x7f8 (with session+0xd0348) sets config+0x794, the MODE ENUM, to 1, 2 or 3, and sets config+0x7c0 and config+0x4e0. Other session bytes read by that ladder: +0xd0374, +0xd03a0, +0xd03ec.',
  consequence='A SUPERGUN anchor carries its mode inside host_device.bin, which the runner already restores (sg_runner main.rs reserves and loads it from meta.host_device / host_device_size). To run a different mode, CAPTURE THAT MODE at character select. Converting one mode into another by writing bytes is working against the design.';

UPSERT finding:select_load_state_differs_per_mode SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='Measured per-mode select state: ARCADE vs VERSUS differ in the session block, config, and the select turn token -- but NOT in G+0x4c',
  statement='Both captured at G+2 == 1 (character select), same build, same exe base. ARCADE (select-01 anchor): session+0xd0330 = 1, +0xd0334 = 0, +0xd0338 = 0; config+0x794 = 1, +0x828 = 1, +0x82c = 0, +0x7c0 = 1, +0x80c = 0; G+0x4c = 0, G+0x84 = 0, G+0x85 = 3; blk+0x33a00 = 1. VERSUS (live, pid 7376): session+0xd0330 = 0, +0xd0334 = 0, +0xd0338 = 1; config+0x794 = 1, +0x828 = 0, +0x82c = 0, +0x7c0 = 1, +0x80c = 1; G+0x4c = 0, G+0x84 = 0, G+0x85 = 3; blk+0x33a00 = 255 (0xFF). The single differing input is session+0xd0338 vs +0xd0330; everything downstream follows from FUN_140037b50 and FUN_14062bc80.',
  consequence='YES -- character select is a different load state per mode. The token blk+0x33a00 = 0xFF (both sides local) in Versus arises purely because config+0x828 == 0 lets FUN_14062bc80 reach its G+0x84 == 0 && G+0x8c == 0 branch. config+0x794 is 1 in BOTH, so the mode enum alone does not identify the mode.';

UPSERT finding:select_mode_byte_correction SET status='inferred', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='CORRECTION: G+0x4c is 0 in Versus as well as Arcade -- mode 1 is NOT two-human-players',
  statement='finding:select_mode_byte (seed 175) read the branch if (G+0x4c == 1 || DAT_142edf2fc == 2) { blk+0x33a00 = 0xFF; G+0x8c = 0 } in FUN_14062bc80 and concluded G+0x4c == 1 is the two-human-player mode. A live read of an actual Versus character select measures G+0x4c = 0, identical to the arcade anchor, with blk+0x33a00 = 0xFF reached through the OTHER branch (config+0x828 == 0, G+0x84 == 0, G+0x8c == 0). G+0x4c == 1 is compared in FUN_140618ab0, FUN_140629490 and FUN_14062bc80 and remains UNIDENTIFIED; it is not Versus.',
  supersedes='The mode-1 interpretation in finding:select_mode_byte. The dispatch-table and branch STRUCTURE recorded there is unaffected -- only the claim about what value 1 means.',
  caveat='Writing G+0x4c = 1 into an arcade anchor (SUPERGUN --two-local) does not reproduce Versus; it produces a third, unmeasured state. It reached battle in a headless run, which is evidence of nothing about correctness.';

UPSERT finding:phase_masks_swap_at_battle SET status='inferred', confidence='high', date='2026-09-05',
  title='G+0x84 and G+0x85 swap values across the select-to-battle transition',
  statement='Live Versus trace, 1 Hz: G = [2,1,1,3,0,0] then [2,1,1,4,1,0] then [2,1,1,4,2,0] (select advancing) then [2,1,2,2,1,0], [2,1,2,2,2,0], [2,1,2,3,0,0], [2,1,2,3,2,0] (battle). Across that boundary G+0x84 goes 0 -> 3 and G+0x85 goes 3 -> 0, while blk+0x33a00 stays 255 and G+0x4c stays 0.',
  consequence='The select advance test in FUN_14062c480 uses (G+0x84 | G+0x85), which is 3 on both sides of the swap, so the test is stable. Any code that reads G+0x84 ALONE means something different before and after the phase change.';

UPSERT finding:two_processes_share_the_image_name SET status='inferred', confidence='high', date='2026-09-05',
  title='TOOLING: the Fighting Collection runs two processes of the same name; only one has game state',
  statement='tasklist reports MarvelVsCapcomFightingCollection.exe twice (measured pids 37020 and 7376). Both map the image at 0x140000000 and both read MZ. *(base+0xACD3A0) is 0x0 in the launcher and 0x140ac6d40 in the game. dump_live.py find_pid returned the first row, and its wait_for_select prints only when it SUCCEEDS in reading G, so attaching to the launcher produced a silent 600-second wait indistinguishable from not being at select.',
  consequence='find_pid now selects the pid whose game_state pointer reads back non-null and prints the candidate list; wait_for_select now reports a null or unreadable blk instead of waiting silently. Any future live tool must pick the process by a LIVE GLOBAL, never by image name alone.';

RELATE finding:mode_lives_in_host_device_session->cites->source:live_mode_probe_20260905 SET id='e176_mode_live';
RELATE finding:mode_lives_in_host_device_session->cites->source:select_decompiles_20260905 SET id='e176_mode_code';
RELATE finding:select_load_state_differs_per_mode->cites->source:live_mode_probe_20260905 SET id='e176_diff_live';
RELATE finding:select_load_state_differs_per_mode->cites->source:select_decompiles_20260905 SET id='e176_diff_code';
RELATE finding:select_mode_byte_correction->cites->source:live_mode_probe_20260905 SET id='e176_corr_live';
RELATE finding:select_mode_byte_correction->cites->source:select_decompiles_20260905 SET id='e176_corr_code';
RELATE finding:select_mode_byte_correction->supersedes->finding:select_mode_byte SET id='e176_supersede';
RELATE finding:phase_masks_swap_at_battle->cites->source:live_mode_probe_20260905 SET id='e176_swap_live';
RELATE finding:two_processes_share_the_image_name->cites->source:live_mode_probe_20260905 SET id='e176_pid_live';

UPDATE finding:mode_lives_in_host_device_session SET status='confirmed';
UPDATE finding:select_load_state_differs_per_mode SET status='confirmed';
UPDATE finding:select_mode_byte_correction SET status='confirmed';
UPDATE finding:phase_masks_swap_at_battle SET status='confirmed';
UPDATE finding:two_processes_share_the_image_name SET status='confirmed';
UPDATE finding:select_mode_byte SET status='superseded';
