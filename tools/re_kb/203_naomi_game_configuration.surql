-- 203_naomi_game_configuration.surql -- THE NAOMI MvC2 GAME CONFIGURATION: the second program image in
-- the EPROM, the twelve operator settings, where each is stored, and WHICH OF THEM CAN CHANGE FRAME
-- OUTCOMES with the arithmetic for each. Answers the SUPERGUN question "what must a NAOMI cartridge
-- normalise before an anchor is shareable". 2026-09-07.
-- RE METHOD: step 1 the menu was NOT in the main p-code corpus at all, which forced the discovery of a
-- SECOND load table in the EPROM; step 2 seeded on the menu row table and the per-row handler literals;
-- step 3 each setting is given at record offset, unpacked address AND game-global offset; step 4
-- CONFIRMED = read in the image AND cross-checked against three independent work-RAM captures.
-- Forward slashes only in ref/note; no apostrophes.
USE NS re DB kb;

UPSERT source:naomi_game_config_re_20260907 SET kind='disasm', strength='code',
  ref='Ghidra SuperH4 over BOTH images of epr-23085a.ic11: main (base 0x0C021000) and the GAME TEST MODE overlay (base 0x0C020000, EPROM 0x003C0000, len 0x040000, guest = EPROM_off + 0x0BC60000). Menu row table 0x0C053E08; per-row handlers 0c022028 / 0c02219c / 0c0222f8 / 0c02246c / 0c0225ae / 0c0226de / 0c022842 / 0c02297e / 0c022ac6 / 0c022c1e / 0c022d52 / 0c022ea8; validator FUN_0c021fd4; pack 0c021e84; unpack 0c02203c; globals copy 0c02a8c0..0c02a976; battle init FUN_0c02bba8; damage FUN_0c048520 site 0c048642; difficulty 0c047c22..0c047c40; timer FUN_0c039156 site 0c03915c; event 0c02b75e / 0c031258 / 0c03523c; game speed 0c02e0fc.',
  note='The two NAOMI load tables sit at EPROM 0x000360 (main game, rom 0x00001000 to RAM 0x0C021000, len 0x250000) and 0x0003C0 (test mode, rom 0x003C0000 to RAM 0x0C020000, len 0x040000). The BIOS loads the test overlay INSTEAD of the game, which is why no amount of searching the main image finds the configuration menu.';

UPSERT source:naomi_config_capture_crosscheck_20260907 SET kind='reproduction', strength='reproduction',
  ref='Three 16 MB NAOMI work-RAM captures at the character-select screen: capture/out/sel_ram.bin, capture/out/lv0b/sel_ram.bin, capture/out/anchor_03_0_f009623.bin, plus the normalised recapture capture/out/norm/norm_anchor_0.bin.',
  note='Every one of the twelve settings was read at three independent locations per capture (packed record 0xC283F80, unpacked array 0xC283FD4, game globals G+offset) and all three agree in all captures. The level-0 capture record is BYTE-IDENTICAL to factory template 2, which is the control. EVENT ON is corroborated downstream: only the EVENT-ON captures show the forced-two-seat effect G+0x18 = G+0x85 = 3. The DAMAGE/TIMER unpack swap is visible in the data: record +0x0A = 3 and +0x0B = 1 produce 0xC283FDA = 1 and 0xC283FDB = 3.';

UPSERT region:naomi_test_overlay SET lo=201457664, hi=201719807,
  note='The GAME TEST MODE program image, guest 0x0C020000..0x0C05FFFF, EPROM offset 0x003C0000, length 0x040000. Loaded by the BIOS INSTEAD of the main game image. Contains the entire GAME CONFIGURATION menu, its row table, its per-row handlers, the defaults template table and the hidden secret-level button code.';

UPSERT global:naomi_config_record SET addr=203275648, type='0x14 byte record',
  note='0xC283F80, magic 0x42424730 (0GBB, the SAME magic as the unrelated save record at 0xC283F94 - do not confuse them). Holds the twelve operator settings plus a region/template index at +0x04 and the secret-level assignment at +0x11. Unpacked to 0xC283FD4 with DAMAGE LEVEL and TIMER SPEED SWAPPED (0c02203c writes +0x0A to 0xC283FDB, 0c022042 writes +0x0B to 0xC283FDA). Persisted in the BIOS EEPROM partition, which is mie_eeprom in MAME, NOT in the 32 KB sram file.';

UPSERT global:naomi_match_context SET addr=203424568, type='0xC0 byte struct',
  note='H = *(u32*)0xC2F83F8 = 0xC2F8338, memset at battle init FUN_0c02bba8 (0c02bbd0). H+0x18 + i*4 = the six fighter pointers; H+0x40 = the round timer counting 99 down to 0; H+0x41 = the sub-tick counter; H+0x87 = its reload constant. Fighter array base 0xC2D7088 stride 0x5A4 - the DC stride, NOT the Steam 0x738.';

UPSERT finding:naomi_eprom_has_two_program_images SET
  title='The NAOMI cart EPROM carries TWO program images, and the whole GAME CONFIGURATION menu is in the second one',
  statement='epr-23085a.ic11 has the standard NAOMI pair of load descriptors. EPROM 0x000360 loads the MAIN game from rom 0x00001000 to RAM 0x0C021000 for 0x250000 bytes. EPROM 0x0003C0 loads a GAME TEST MODE overlay from rom 0x003C0000 to RAM 0x0C020000 for 0x040000 bytes, and the BIOS loads it INSTEAD of the game. Address map for the overlay is guest = EPROM_off + 0x0BC60000.',
  consequence='A p-code corpus built from the main image alone cannot contain the configuration menu, its row table, its handlers, its defaults or its hidden secret-level code. Searching harder in the main image was never going to work. Any future question about NAOMI test menus, operator settings or bookkeeping screens must be asked of the SECOND image. Generalises: check for multiple load descriptors before concluding something is absent from a NAOMI cart.',
  status='confirmed', confidence='high', date='2026-09-07', contribution_candidate=true;

UPSERT finding:naomi_six_settings_are_battle_relevant SET
  title='Six of the twelve NAOMI operator settings can change frame-level outcomes, and DAMAGE LEVEL is a direct multiplier',
  statement='Copied into the game globals at 0c02a8c0..0c02a976. DAMAGE LEVEL (G+0x1B) indexes table 0xC23C77E = 10 20 30 40 at 0c048642 inside the damage function FUN_0c048520, applied as tbl/32, so 0.5x / 1.0x / 1.5x / 2.0x with the default neutral at 1.0x. DIFFICULTY (G+0x4F, working copy G+0x83) indexes a 32-bit probability bitmask table 0xC23BFA8 of stride 0x80 at 0c047c32. TIMER SPEED (G+0x46) indexes 0xC23B6EC = B4 78 5A 46 at 0c03915c, reloading the round-clock sub-counter with 180 / 120 / 90 / 70. EVENT (G+0x2A) forces both seats at 0c02b75e, disables continue at 0c031258, AND early-returns from FUN_0c03522c at 0c03523c, which is the FIRST call in the battle scene tick. JOIN-IN (G+0x4A) gates mid-match join at 0c0352ac. GAME SPEED (G+0x49) derives the live turbo flag G+0x89 at 0c02e0fc. The remaining six settings are menu, attract, audio or VMU only.',
  consequence='Two anchors captured on different operator settings are NOT interchangeable, and the difference is not cosmetic - at the extremes damage differs by a factor of four. A NAOMI cartridge must normalise the configuration record as well as the unlock masks. The first anchor captured in this session was on damage level 0 (0.5x) and difficulty 6 because the values drifted while navigating the menu, and was recaptured on factory template 2. Note for determinism work: rand() is consumed at 0c047c22 BEFORE the difficulty mask is tested at 0c047c40, so DIFFICULTY does NOT perturb the RNG stream, it only changes the CPU decision.',
  status='confirmed', confidence='high', date='2026-09-07', contribution_candidate=true;

UPSERT finding:naomi_damage_and_timer_swap_on_unpack SET
  title='DAMAGE LEVEL and TIMER SPEED swap places when the NAOMI config record is unpacked, so a positional read reports one as the other',
  statement='0c02203c writes record+0x0A (DAMAGE LEVEL) to 0xC283FDB, and 0c022042 writes record+0x0B (TIMER SPEED) to 0xC283FDA. The inverse pack at 0c021e84 / 0c021e88 does the same. Visible in the captures: a record with +0x0A = 3 and +0x0B = 1 unpacks to 0xC283FDA = 1 and 0xC283FDB = 3, and the globals then read G+0x1B (damage) = 3 and G+0x46 (timer) = 1.',
  consequence='Any exporter or normaliser that walks the unpacked array at 0xC283FD4 positionally will silently report damage as timer and timer as damage. Read the PACKED record at 0xC283F80 in menu order, or read the game globals by their individual offsets. Do not walk the unpacked array.',
  status='confirmed', confidence='high', date='2026-09-07';

UPSERT finding:naomi_config_lives_in_the_bios_eeprom_not_sram SET
  title='The NAOMI operator configuration is stored in the BIOS EEPROM partition, not in the 32 KB cart SRAM',
  statement='SAVE AND EXIT (test overlay FUN_0c023072) prints SAVING NEW CONFIGURATION / IN EEPROM, commits the working copy to the baseline at 0c0235f0, and calls the EEPROM writer at 0c029660. The record reaches the game through a BIOS read call at 0c1f6902 into buffer 0xC344DB8 with the data at +0x2C = 0xC344DE4. In MAME that partition is the mie_eeprom nvram file. The 32 KB sram file holds a DIFFERENT 0x40 byte record at 0x218 carrying play and unlock counters, which merely shares the same 0GBB magic.',
  consequence='A normalised NAOMI cartridge needs BOTH files: sram for the unlock masks and EXP, mie_eeprom for the twelve settings. This is also the strong lead for an earlier open item - swapping only mie_eeprom swaps the config record including the region byte the validator FUN_0c021fd4 checks, and a rejected record forces defaults. That mechanism is INFERRED, not measured; the named test still stands.',
  status='confirmed', confidence='high', date='2026-09-07';

RELATE finding:naomi_eprom_has_two_program_images->about->region:naomi_test_overlay SET note='the second image';
RELATE finding:naomi_eprom_has_two_program_images->cites->source:naomi_game_config_re_20260907 SET note='raw load-table read';
RELATE finding:naomi_six_settings_are_battle_relevant->about->global:naomi_config_record SET note='the twelve settings';
RELATE finding:naomi_six_settings_are_battle_relevant->about->global:naomi_match_context SET note='round timer lives here';
RELATE finding:naomi_six_settings_are_battle_relevant->cites->source:naomi_game_config_re_20260907 SET note='arithmetic sites';
RELATE finding:naomi_six_settings_are_battle_relevant->cites->source:naomi_config_capture_crosscheck_20260907 SET note='three captures agree';
RELATE finding:naomi_damage_and_timer_swap_on_unpack->about->global:naomi_config_record SET note='landmine';
RELATE finding:naomi_damage_and_timer_swap_on_unpack->cites->source:naomi_config_capture_crosscheck_20260907 SET note='visible in the data';
RELATE finding:naomi_config_lives_in_the_bios_eeprom_not_sram->about->global:naomi_config_record SET note='storage location';
RELATE finding:naomi_config_lives_in_the_bios_eeprom_not_sram->cites->source:naomi_game_config_re_20260907 SET note='save path';
