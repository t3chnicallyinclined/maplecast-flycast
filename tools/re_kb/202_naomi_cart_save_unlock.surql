-- 202_naomi_cart_save_unlock.surql -- THE NAOMI CART BATTERY RECORD on Marvel vs Capcom 2 arcade
-- (MAME romset mvsc2, Export/Korea Rev A, program EPROM epr-23085a.ic11, load base 0x0C021000):
-- where the save record lives, its checksum algorithm, the EXP/level machinery, and the ACTUAL
-- character-select roster gate, which is a 59-bit unlock mask and NOT the level. Answers the
-- SUPERGUN question "can we produce a deterministic full-roster select-screen anchor from a
-- user BYOR NAOMI cart". 2026-09-07.
-- RE METHOD: step 1-2 seeded on the P2 SRAM alias constants 0xA0200000/0x0218 and on the magic
-- 0x42424730, propagated along the call graph to the select-screen tile routines; step 3 every
-- SRAM offset is given with its work-RAM shadow address; step 4 CONFIRMED = read in the image AND
-- reproduced by an independent measurement in MAME, INFERRED = read only.
-- Forward slashes only in ref/note; no apostrophes.
USE NS re DB kb;

-- ---------------------------------------------------------------------------------------------------
-- SOURCES
-- ---------------------------------------------------------------------------------------------------
UPSERT source:naomi_cart_save_re_20260907 SET kind='disasm', strength='code',
  ref='Ghidra SuperH4 p-code over the NAOMI cart program image (naomi_prog.bin, load base 0x0C021000, from MAME romset mvsc2 chip epr-23085a.ic11): cksum 0c1f5d80, bupWrite 0c1f8ed0, backup init/erase 0c1f6650, record write path 0c1f7714 / 0c1f772e / 0c1f7798 / 0c1f7836 / 0c1f7848 / 0c1f7900 / 0c1f7920, bounds check 0c1f7974, record establish 0c021d9c, dirty-flag test 0c021d34, AddEXP 0c022b00, RecomputeLevel 0c022bc6, secret-level assignment route 0c022c14, select mask build 0c02cf04 and 0c02f714, CharSelectMaskForTile 0c1c5334, THE GATE tst at 0c1c537a.',
  note='SRAM window found from constant-pool literals 0xA0200000 / 04 / 08 / 0100 / 01F8 / 0208 / 0218 loaded at 0c1f65ae, 0c1f8988, 0c1f676a, 0c1f6778, 0c1f7714, 0c1f772e, 0c1f7798; the erase-all loop at 0c1f6650 writes 0x2000 longwords = 0x8000 bytes from 0xA0200000, and the bounds check at 0c1f7974 computes 0x218 + 2*padded and errors above 0x8000.';

UPSERT source:naomi_unlock_reproduction_20260907 SET kind='reproduction', strength='reproduction',
  ref='MAME 0.289 headless runs, scratchpad capture lane: unlock.lua (the poke), sel_dump.lua (select-screen work-RAM dump), peek.lua (SRAM device read at frames 2/600/1800/3000). Doc supergun/docs/NAOMI-CART-SAVE-AND-UNLOCK.md.',
  note='Measured: writing the mask filled all 56 tiles; a clean re-boot with NOTHING poking anything showed the full roster, LV 99, EXP 039600, so the game persisted it itself through bupWrite. Checksum reimplemented in python from the p-code and run over the resulting image: computed 0x9110, stored at both 0x1F8 and 0x208 = 0x9110. Negative arm: poking EXP alone drove LV to 99 and changed ZERO tiles. Level-24 capture cross-check: record+0x20 = 0x00030500 and record+0x24 = 0x00010414 = bits 8,10,16,17,34,36,42,48 = exactly reward groups 0 and 1, which is what mode B awards over levels 0..23.';

-- ---------------------------------------------------------------------------------------------------
-- REGIONS + GLOBALS
-- ---------------------------------------------------------------------------------------------------
UPSERT region:naomi_sram SET lo=2686779392, hi=2686812159,
  note='NAOMI cart battery SRAM, 32 KB. Physical 0x00200000..0x00207FFF, addressed by this game ONLY through the P2 uncached alias 0xA0200000. MAME nvram file sram maps 1:1: file offset X == SH4 0xA0200000 + X.';

UPSERT global:naomi_game_globals_ptr SET addr=203255684, type='u32 pointer',
  note='0x0C2D6F84 holds G, the NAOMI game-globals pointer. Measured live as 0x0C2D6F88. G+0x29 = the EXP mode game assignment (0 = threshold table A, nonzero = table B); G+0x30/G+0x34 = the unlock mask the select screen is rebuilt from; G+0x38/G+0x3C = the second reward pool.';

UPSERT global:naomi_save_record_shadow SET addr=203275668, type='0x40 byte record',
  note='0x0C283F94 = the work-RAM shadow of the SRAM game record at SRAM 0x218. Record byte N == SRAM 0x218+N == work RAM 0x0C283F94+N. Established at 0c021d9c with mov #0x40,r4 and r5 = 0xC283F94.';

UPSERT global:naomi_unlock_mask_lo SET addr=203275700, type='u32 bitmask',
  note='0x0C283FB4 = record+0x20 = unlock mask for characters 0..31. Full-roster value 0xF8FFFFFF. Written by every unlock at 0c0229b2, read back at 0c1e6808.';

UPSERT global:naomi_unlock_mask_hi SET addr=203275704, type='u32 bitmask',
  note='0x0C283FB8 = record+0x24 = unlock mask for characters 32..58. Full-roster value 0x07FFFFFF. Written at 0c0229ba, read at 0c1e6810.';

UPSERT global:naomi_exp_record SET addr=203275696, type='s32',
  note='0x0C283FB0 = record+0x1C = EXP as stored in the cart save. Written at 0c022b1e, read at 0c1e682a. 39600 is level 99 under threshold table B.';

UPSERT global:naomi_save_dirty_flag SET addr=203275564, type='s32',
  note='0x0C283F2C. Set to -1 by every unlock (0c0229bc, 0c022ab8) and to 1 by the credit routine (0c021d2c). Tested at 0c021d34, which calls bupWrite 0x0C1F8ED0. bupWrite rewrites BOTH record copies AND BOTH checksums, so setting this flag is how a runtime poke is persisted correctly without doing checksum arithmetic.';

UPSERT global:naomi_select_mask_p1 SET addr=203437416, type='u32 pair',
  note='0x0C2FB168 / 0x0C2FB16C = the mask words the character-select screen actually tests for player 1; P2 at 0x0C2FB170 / 0x0C2FB174; a second copy at +0x20. Stride is 0xC2FB158 + 0x10 + side*8, proven by the CPU/random picker indexing at 0c02ff90 and 0c02ffec. Rebuilt on every screen entry, so writing here alone does not survive leaving the screen.';

UPSERT global:naomi_exp_struct SET addr=203424920, type='struct',
  note='0x0C2F8498 = the live EXP struct. +0x00 = EXP s32; +0x20 = current level u16 at 0x0C2F84B8.';

-- ---------------------------------------------------------------------------------------------------
-- ROUTINES
-- ---------------------------------------------------------------------------------------------------
UPSERT routine:naomi_cksum_0c1f5d80 SET pc='0x0C1F5D80', role='u16 cksum(u8 *p, u32 len) -- THE cart save checksum. r = 0xDEBDEB00; per byte r = (r AND 0xFFFFFF00) + p[i] then 8 rounds of r += r with r += 0x10210000 on carry-out; then ONE more 8-round tail; return r >> 16. NOTE mov.w sign-extends, so the mask is 0xFFFFFF00 and not 0xFF00 -- getting that wrong is the classic way to fail to reproduce it. Constants at 0c1f5d88 / 0c1f5d8a / 0c1f5d8c / 0c1f5d8e. Ranges: cksum(sram+0x010, 0xF0) is stored at SRAM 0x008 and mirror 0x100; cksum(sram+0x218, 0x40) is stored at SRAM 0x1F8 and 0x208. Callers 0c1f7836, 0c1f7bec, 0c1f78b2, 0c1f65e6, 0c1f67a4.';

UPSERT routine:naomi_bupwrite_0c1f8ed0 SET pc='0x0C1F8ED0', role='bupWrite -- rewrites the two SRAM game-record copies at 0x218 and 0x258 and the two headers at 0x1F8 and 0x208, recomputing both checksums. Reached from the credit routine when the dirty flag 0x0C283F2C is nonzero (test at 0c021d34). This is the supported way to make a runtime save edit stick.';

UPSERT routine:naomi_addexp_0c022b00 SET pc='0x0C022B00', role='AddEXP(s32 delta) -- the ONLY routine that grants unlock rewards. Caps EXP at 0xF423F = 999999 at 0c022b0a, mirrors EXP into the SRAM record at 0c022b1e, stops when level == 0x63 at 0c022b28, then walks while (EXP >= tbl[level]) { GrantReward(level); level++ } at 0c022b3e..0c022b58. Threshold table selected on G+0x29: 0xC238EB8 when zero, 0xC239048 (400*(n+1)) otherwise.';

UPSERT routine:naomi_recomputelevel_0c022bc6 SET pc='0x0C022BC6', role='RecomputeLevel() -- the SAME threshold walk as AddEXP but grants NOTHING. Called at boot from 0c1e682c and 0c022c64. This asymmetry is the entire trap: boot recomputes the DISPLAYED level from EXP but never re-grants rewards, so raising EXP after boot moves the number on screen and unlocks no characters.';

UPSERT routine:naomi_selectmask_build_0c02cf04 SET pc='0x0C02CF04', role='Builds the character-select mask words. mask_lo = (netplay/dip words) OR G->0x30 OR 0xC0FCFAFF at 0c02cf1e..0c02cf88; mask_hi = (netplay/dip words) OR G->0x34 OR 0x01803300 at 0c02cf34..0c02cfa4. The two literals carry 22 + 6 = 28 always-available starters; plus 28 secrets = 56 playable. Companion builder at 0x0C02F714. Indices 24, 25 and 26 are skipped explicitly at 0c022a04 / 0c022a0c / 0c022a16, which is what turns a 59-slot table into a 56-character roster.';

UPSERT routine:naomi_charselect_maskfortile_0c1c5334 SET pc='0x0C1C5334', role='CharSelectMaskForTile(obj) -- reads the tile char-id from obj+0x21; if >= 0x20 returns *(0xC2FB16C) OR *(0xC2FB174) with bit index id-0x20 (0c1c5340..0c1c534e), else returns *(0xC2FB168) OR *(0xC2FB170) with bit index id (0c1c5350..0c1c535a). It ORs BOTH sides masks together.';

UPSERT routine:naomi_charselect_gate_0c1c537a SET pc='0x0C1C537A', role='THE ROSTER GATE. tst r2,r3 with r3 = 1 << bitidx and r2 = the mask word. Bit set -> obj[0x12C] = 1 playable at 0c1c5386; bit clear -> obj[0x12C] = 0 locked at 0c1c5382. The same test appears at 0c1c53aa inside the locked-tile visual routine 0x0C1C5392 and at 0c1c5420 inside the restore-tile counterpart 0x0C1C5408. Dispatchers 0c1c4df6/0c1c4df8/0c1c4e00 and 0c1c503c/0c1c5042/0c1c504a branch on obj+0x26.';

UPSERT routine:naomi_secretlevel_assignment_0c022c14 SET pc='0x0C022C14', role='The legit full-unlock route. Called from 0c02197e. Reads the secret-level game assignment at *(u8*)0xC283F91 (range 0..4), indexes 0xC2285C9 = {0,19,39,59,83} in mode A or 0xC2285CE = {0,23,47,71,83} in mode B at 0c022c76/0c022c80, and if EXP is below that level threshold calls AddEXP(threshold - EXP) at 0c022c92, which grants EVERY reward properly including the G+0x38/0x3C pool. Setting the byte to 4 through the game test menu also makes the BIOS recompute the 0x008 system-block checksum.';

-- ---------------------------------------------------------------------------------------------------
-- FINDINGS
-- ---------------------------------------------------------------------------------------------------
UPSERT finding:naomi_roster_gate_is_a_mask_not_the_level SET
  title='The NAOMI character-select roster is gated by a 59-bit unlock mask, NOT by the secret level -- poking EXP moves the on-screen LV and unlocks nothing',
  statement='The select screen bit-tests a mask at 0xC2FB158 + 0x10 + side*8 (tst at 0x0C1C537A, bit index from the tile char-id at obj+0x21 via 0x0C1C5334). That mask is rebuilt on every screen entry by 0x0C02CF04 / 0x0C02F714 as (netplay/dip) OR G->0x30 OR 0xC0FCFAFF and (netplay/dip) OR G->0x34 OR 0x01803300, where G = *(u32*)0x0C2D6F84. G->0x30 / G->0x34 are loaded from the cart save record at SRAM 0x238 / 0x23C, shadowed in work RAM at 0x0C283FB4 / 0x0C283FB8. The level is a SEPARATE quantity derived from EXP by a threshold walk. Boot calls RecomputeLevel 0x0C022BC6, which walks the same thresholds but grants no rewards, so a raised EXP updates the displayed level and never touches the mask.',
  consequence='MEASURED NEGATIVE ARM: writing EXP = 65535 into the record shadow drove the on-screen LV to 99 and changed ZERO roster tiles across a full select-screen capture. MEASURED POSITIVE ARM: writing 0xF8FFFFFF / 0x07FFFFFF into 0x0C283FB4 / 0x0C283FB8 filled all 56 tiles. The full-roster value is the exact union of the 28 starters and the 28 secrets -- do NOT write 0xFFFFFFFF, which sets nonexistent slots 24..26 and 59..63. General lesson: a number the game prints on screen is not evidence of the gate; find the comparison site first.',
  status='confirmed', confidence='high', date='2026-09-07', contribution_candidate=true;

UPSERT finding:naomi_cart_validates_and_rebuilds_its_save_at_boot SET
  title='The cart validates its save record early in boot and REBUILDS it, so an offline nvram edit is silently reverted and looks exactly like the emulator ignoring the file',
  statement='Instrumented read of the MAME sram share at frames 2, 600, 1800 and 3000 on a boot with a hand-edited record: the edited value was PRESENT at frames 2 and 600, the record was ZEROED by frame 1800, and a valid record with the previous value was back by frame 3000. MAME had loaded the file correctly the whole time. The rejection is the cart checking cksum(sram+0x218, 0x40) against the u32 stored at SRAM 0x1F8 and 0x208.',
  consequence='Any offline edit of the NAOMI nvram must recompute the checksum with routine 0x0C1F5D80, or be done at runtime with the dirty flag 0x0C283F2C set to -1 so the cart own bupWrite 0x0C1F8ED0 writes both copies and both checksums. Debugging lesson: one instrumented read of the device across boot replaced a chain of guesses about whether the emulator was loading the file.',
  status='confirmed', confidence='high', date='2026-09-07', contribution_candidate=true;

UPSERT finding:naomi_sram_0x00_and_0x04_are_counters_not_checksums SET
  title='SRAM 0x000 and 0x004 are a session counter and its previous value, NOT checksums -- the real checksums are at 0x008, 0x1F8 and 0x208',
  statement='0x000 is copied from the work-RAM global 0xC01F130 at 0c1f8980; 0x004 is set to the previous value of 0x000 at 0c1f8988. The checksums are: 0x008 = cksum(sram+0x010, 0xF0) at 0c1f65e6 (mirrored at 0x100), and 0x1F8 / 0x208 = cksum(sram+0x218, 0x40) at 0c1f7836 / 0c1f7848. Confirmed by reimplementing 0x0C1F5D80 in python and matching both stored values on two independent capture files, and again on the image the game wrote after the unlock (computed 0x9110 == stored 0x9110).',
  consequence='RETRACTS an earlier session hypothesis that 0x000 and 0x004 were the guard. A brute-force search for a checksum rule explaining 0x000 returned zero candidates precisely because it is not a checksum -- when a search over a wide rule space finds nothing, question the premise rather than widening the space.',
  status='confirmed', confidence='high', date='2026-09-07';

UPSERT finding:naomi_unlock_state_is_cart_settings_not_rom SET
  title='NAOMI character unlock state lives in cart battery RAM, not in the ROM -- a BYOR dump boots with a locked roster and cannot produce a matching anchor',
  statement='The unlock masks are fields of the 0x40 byte save record at SRAM 0x218, backed by the cart battery and by MAME as the nvram file sram. The program EPROM epr-23085a.ic11 is byte-identical before and after any unlock. Separately, the widely circulated MvC2_Unlocked IC22.Bin is 4,194,304 bytes, i.e. the size of the PROGRAM EPROM epr-23085a.ic11 and not the 8 MB data chip mpr-23053.ic22 it is named after; it is build 2000-02-17 against this cart 2000-02-25, 58 percent of bytes differ and the code is relocated by 0x160, so it is a patched copy of an OLDER revision and not this cart with a flag set.',
  consequence='For the SUPERGUN cartridge, a select-screen anchor depends on cart settings that a user own ROM dump does not carry, so the cartridge must ship a NORMALISED backup-RAM set alongside the anchor or anchors will not be byte-identical between users and netplay will desync. That set contains no game data, only cart settings, so shipping it does not violate BYOR. Also: comparing any program EPROM to the Steam game_50.arc is a category error, because the arc contains ZERO code -- what matches the arc is the cart DATA chips through the cart own TOC (58 of 59 containers byte-exact, one differing byte).',
  status='confirmed', confidence='high', date='2026-09-07', contribution_candidate=true;

UPSERT finding:naomi_export_vs_japanese_unlock_codes SET
  title='The MvC2 arcade unlock codes are region-specific, and the widely quoted Sound Mode / Difficulty / Damage set is for JAPANESE boards',
  statement='Export and USA boards use Game Configuration rows Demo Sound (level 24), Damage Level (48), Continue (72) and Difficulty (84). Japanese boards use Sound Mode, Difficulty, Damage and Timer Speed. Sources publish the Export codes in both button form and numeric SHOT form -- Demo Sound is A2 A1 HK HP LK LP and also 635241 -- which independently pins the panel mapping SHOT1..SHOT6 = LP, HP, A1, LK, HK, A2. Every code requires EXITING Game Configuration before the level jump is applied.',
  consequence='Entering the Japanese set on the Export cart used here silently does nothing, which is indistinguishable from mis-typing the code. Combined with the exit-between-codes rule, only the first of four codes ever landed by hand across a long session. Writing the unlock mask directly is both faster and verifiable, and is what was actually used.',
  status='inferred', confidence='medium', date='2026-09-07';

UPSERT finding:mame_naomi_harness_gotchas SET
  title='MAME NAOMI harness gotchas that each cost a debugging cycle: momentary TEST, persistent set_value overrides, the gameinfo stall, and unsupported save states',
  statement='1) :MIE.5 Service Mode (the TEST switch) is MOMENTARY, not a toggle; the BIOS system menu only shows while it is asserted and press TEST is a brief drop to 0 and back. 2) MAME ioport_field:set_value() installs a PERSISTENT override -- releasing a held line requires clear_value(), and merely stopping the writes leaves it stuck, which presents as the cart rebooting into test mode forever. 3) MAME stalls on its startup info screen when an autoboot script is attached, so -skip_gameinfo is mandatory for unattended runs; without it frames never advance and the script looks dead. 4) mame -listxml mvsc2 reports savestate="unsupported" for the NAOMI driver, so manager.machine:save() always fails -- the anchor is the 16 MB work-RAM dump, not a MAME savestate. 5) The BIOS system menu has 12 items, not 9, and the cursor starts on EXIT, so GAME TEST MODE is 11 SERVICE taps away.',
  consequence='Ports for scripting: :MIE.5 (Service Mode = F2, Service 1 = 9, Service 2 = 0), :P1 (Start = 1, Buttons 1..6 = LCtrl, LAlt, Space, LShift, Z, X), :837_13551:COINS (Coin 1 = 5). Work RAM is the SH4 area-3 window at 0x0C000000, 16 MB, readable through manager.machine.devices[":maincpu"].spaces["program"]. The SRAM is the MAME share :sram, 32 KB.',
  status='confirmed', confidence='high', date='2026-09-07';

-- ---------------------------------------------------------------------------------------------------
-- EDGES
-- ---------------------------------------------------------------------------------------------------
RELATE finding:naomi_roster_gate_is_a_mask_not_the_level->about->routine:naomi_charselect_gate_0c1c537a SET note='the tst that decides playable vs locked';
RELATE finding:naomi_roster_gate_is_a_mask_not_the_level->about->global:naomi_unlock_mask_lo SET note='chars 0..31';
RELATE finding:naomi_roster_gate_is_a_mask_not_the_level->about->global:naomi_unlock_mask_hi SET note='chars 32..58';
RELATE finding:naomi_roster_gate_is_a_mask_not_the_level->cites->source:naomi_cart_save_re_20260907 SET note='addresses';
RELATE finding:naomi_roster_gate_is_a_mask_not_the_level->cites->source:naomi_unlock_reproduction_20260907 SET note='both arms measured';
RELATE finding:naomi_cart_validates_and_rebuilds_its_save_at_boot->about->routine:naomi_cksum_0c1f5d80 SET note='the guard';
RELATE finding:naomi_cart_validates_and_rebuilds_its_save_at_boot->about->global:naomi_save_dirty_flag SET note='the supported way in';
RELATE finding:naomi_cart_validates_and_rebuilds_its_save_at_boot->cites->source:naomi_unlock_reproduction_20260907 SET note='instrumented device read across boot';
RELATE finding:naomi_sram_0x00_and_0x04_are_counters_not_checksums->about->region:naomi_sram SET note='layout correction';
RELATE finding:naomi_sram_0x00_and_0x04_are_counters_not_checksums->cites->source:naomi_cart_save_re_20260907 SET note='0c1f8980 / 0c1f8988';
RELATE finding:naomi_unlock_state_is_cart_settings_not_rom->about->region:naomi_sram SET note='unlocks are battery state';
RELATE finding:naomi_unlock_state_is_cart_settings_not_rom->cites->source:naomi_unlock_reproduction_20260907 SET note='clean reboot showed full roster';
RELATE finding:naomi_export_vs_japanese_unlock_codes->about->routine:naomi_secretlevel_assignment_0c022c14 SET note='the in-game route the codes drive';
RELATE finding:mame_naomi_harness_gotchas->cites->source:naomi_unlock_reproduction_20260907 SET note='each earned by a failed run';
RELATE routine:naomi_addexp_0c022b00->writes->global:naomi_exp_record SET note='0c022b1e mirrors EXP into the save record';
RELATE routine:naomi_addexp_0c022b00->writes->global:naomi_unlock_mask_lo SET note='0c0229b2';
RELATE routine:naomi_addexp_0c022b00->writes->global:naomi_unlock_mask_hi SET note='0c0229ba';
RELATE routine:naomi_selectmask_build_0c02cf04->writes->global:naomi_select_mask_p1 SET note='rebuilt on every select-screen entry';
RELATE routine:naomi_charselect_gate_0c1c537a->reads->global:naomi_select_mask_p1 SET note='tst r2,r3';
RELATE routine:naomi_bupwrite_0c1f8ed0->reads->global:naomi_save_dirty_flag SET note='gated at 0c021d34';
RELATE global:naomi_unlock_mask_lo->lives_at->region:naomi_sram SET note='SRAM 0x238, copy 2 at 0x278';
RELATE global:naomi_unlock_mask_hi->lives_at->region:naomi_sram SET note='SRAM 0x23C, copy 2 at 0x27C';
RELATE global:naomi_exp_record->lives_at->region:naomi_sram SET note='SRAM 0x234, copy 2 at 0x274';

-- Region-split of the unlock codes is documented, not code-derived: downgrade to inferred and
-- attach its actual provenance rather than borrowing the reproduction source.
UPSERT source:mvc2_arcade_unlock_codes_docs_20260907 SET kind='doc', strength='doc',
  ref='https://gist.github.com/hagure/7519e88e33ff68487cb3e838acfc04cd and https://www.aussiearcade.com/topic/8676-how-to-unlock-all-characters-on-marvel-vs-capcom-2-naomi/',
  note='Two independent write-ups agree on the split: USA/Export rows Demo Sound / Damage Level / Continue / Difficulty, Japanese rows Sound Mode / Difficulty / Damage / Timer Speed, and both insist Game Configuration must be EXITED between codes. The Aussie Arcade page gives the same Export codes in numeric SHOT form (635241, 563525, 121342, 243161), which is what independently pins SHOT1..SHOT6 = LP, HP, A1, LK, HK, A2. Our own cart reached secret level 24 after a code-entry attempt, but which of the two sets was typed was not recorded, so the region split itself remains documented rather than measured here.';
RELATE finding:naomi_export_vs_japanese_unlock_codes->cites->source:mvc2_arcade_unlock_codes_docs_20260907 SET note='two independent write-ups plus the numeric SHOT forms';
