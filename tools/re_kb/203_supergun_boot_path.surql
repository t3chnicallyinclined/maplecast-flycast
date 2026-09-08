-- 203_supergun_boot_path.surql -- the BOOT path from the PE entry point to the MvC2 mode entry,
-- and the measured packing state of the retail on-disk exe. 2026-09-07.
-- Sources: Ghidra dumpproj/mvc_dump.bin (decompiles cited inline), the on-disk retail PE parsed
-- byte by byte, and the select-01 anchor files. RE METHOD step 4.
USE NS re DB kb;

UPSERT source:ondisk_pe_20260907 SET kind='binary',
  ref='C:/Program Files (x86)/Steam/steamapps/common/MARVEL vs. CAPCOM Fighting Collection/MarvelVsCapcomFightingCollection.exe (8,826,960 B, 2025-06-12)',
  strength='code';

UPSERT source:ghidra_20260907_boot SET kind='ghidra', strength='code',
  ref='Ghidra dumpproj mvc_dump.bin via mcp ghidra bridge, 2026-09-07: decompiles of 0x1407fe050, 0x140046790, 0x14004b600, 0x14004e5c0, 0x140054810, 0x14003fa60 and xrefs to 0x140607b50 / 0x1408e0770 / 0x1408dd558.';
UPSERT source:anchor_select01_20260905 SET kind='capture', strength='reproduction',
  ref='mvc-live-skins-quarters/d3dcap/ttd/runs/select-01/pre (meta.json G [2,1,1,3,0], scene_id 5, arena 0x7fc1000); byte comparison of game_state.bin against exe_image.bin offset 0xAC6D40.';

UPSERT steam_routine:FUN_140046790 SET addr='0x140046790', name='FUN_140046790',
  role='WinMain. Called by FUN_1407fe050 = __scrt_common_main_seh at 0x1407fe151 with (0x140000000, 0, narrow command line, show mode); FUN_1407fe050 is called by FID_conflict:WinMainCRTStartup at 0x1407fe1d1.';
UPSERT steam_routine:FUN_1407fe050 SET addr='0x1407fe050', name='FUN_1407fe050',
  role='MSVC __scrt_common_main_seh: __scrt_initialize_crt, _initterm_e/_initterm over 0x1408dd268..0x1408dd2a8 and 0x1408db908..0x1408dd260, then WinMain FUN_140046790.';
UPSERT steam_routine:FUN_14004b600 SET addr='0x14004b600', name='FUN_14004b600',
  role='MvC2 MODE ENTRY (short form). If *(PTR_DAT_140acd3a0+8) == 5 (scene id 5 = MvC2): *(DAT_142ebc010+0x34aac) = 0 then FUN_140607b50 (the arena carve). Else config+0x280 = -1 and FUN_1401135e0.';
UPSERT steam_routine:FUN_14004e5c0 SET addr='0x14004e5c0', name='FUN_14004e5c0',
  role='MvC2 MODE ENTRY (long form). Copies the per-game match settings out of the host-device settings record (hostdev[8]+0x5a2f0..+0x5a307) into config+0x284..0x2c8 through FUN_14003fa60, then the same scene-id-5 test and FUN_140607b50.';

UPSERT finding:steam_boot_entry_chain SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='The boot path from the PE entry point to WinMain, read out of the image: Steam first, then window, then the app object graph',
  statement='Decompiled from mvc_dump.bin. FID_conflict:WinMainCRTStartup calls FUN_1407fe050 at 0x1407fe1d1; FUN_1407fe050 is the MSVC __scrt_common_main_seh (__scrt_initialize_crt, _initterm_e over 0x1408dd268..0x1408dd2a8, _initterm over 0x1408db908..0x1408dd260) and calls WinMain FUN_140046790 at 0x1407fe151 with (0x140000000, 0, _get_narrow_winmain_command_line(), __scrt_get_show_window_mode()). WinMain FUN_140046790, in order: (1) cVar3 = (*DAT_1408db8b8)(0x28348a) and RETURN 1 if non-zero -- 0x28348a = 2634890 = the Steam app id, so this is INFERRED SteamAPI_RestartAppIfNecessary; the image also carries the interface strings SteamClient017/SteamUser019/SteamMatchMaking009 at 0x1408de2a8.. and the messages "Steam API is not initialized!" 0x1408e0a68 and "Steam client is not launched!" 0x1408e0a88; (2) FUN_14026c200(window object, 0); (3) (*DAT_1408db1f0)("avrt.dll") = LoadLibrary; (4) (*DAT_1408db5b0)(0)/(1) = GetSystemMetrics, clamped to 0x780 x 0x438; (5) the DISPLAY/Resolution/Aspect/RefreshRate/ScreenMode/VSYNC config table at PTR_PTR_140a5cb48 and FUN_14012d540(&PTR_PTR_140a5ca30, 1, ...) -- the ini object whose path string is 0x140a5ca38 (config.ini in the install root); (6) a command-line loop honouring -connect_lobby, -Resolution, -RefreshRate, -ScreenMode; (7) FUN_14026e200(window, title, COYOTE_2634890, &PTR_PTR_140ad3ac0, ...) and, if it returns true, FUN_14026d960 = the main loop, then FUN_14026c320 = shutdown. The app object itself is built by FUN_140054810 (allocate 0x40198 through FUN_14011ea90(&PTR_PTR_140ad3ac0) vt+0x38, then FUN_140054860 = finding:host_app_object_graph); FUN_140054810 has NO code xref, only the data xref 0x1408e0770, so it is reached through a vtable or registration table.',
  consequence='The BOOT host boundary is strictly larger than the load-path boundary of finding:supergun_load_host_boundary and it is larger at the FIRST INSTRUCTION of WinMain, not deep inside it: the Steam client, LoadLibrary, GetSystemMetrics, an ini file and a window all precede any game state. A shell that wants the game to construct its own select state must either satisfy those or enter BELOW WinMain, at the app-object / mode-entry level.',
  caveat='DAT_1408db8b8 = SteamAPI_RestartAppIfNecessary is INFERRED from the app-id constant and the early-return-1 semantics only. Confirm by resolving the live IAT value at 0x1408db8b8 to a module and export (sg_runner main.rs already does same-boot export resolution).';

UPSERT finding:mvc2_mode_entry_has_two_callers SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='The arena carve FUN_140607B50 has exactly TWO callers, both front-end mode entries gated on scene id 5',
  statement='get_xrefs_to 0x140607B50 returns exactly two: 0x14004b62d in FUN_14004b600 and 0x14004e680 in FUN_14004e5c0. Both call it only when *(PTR_DAT_140acd3a0 + 8) == 5, i.e. game_state+8 == 5; the select-01 anchor meta.json records scene_id 5, so 5 is MvC2 inside the Collection. FUN_14004b600 has six callers, all Collection front-end screens: FUN_140038430 (0x14003845c), FUN_1400388f0 (0x140038931), FUN_1400afeb0 (0x1400b019d), FUN_140087b00 (0x140087c4a), FUN_1400a1370 (0x1400a19d0), FUN_1400ac540 (0x1400ac763). FUN_14004e5c0 additionally copies the per-game match settings from the host-device settings record at hostdev[8]+0x5a2f0 into config+0x284..0x2c8 (the same config+0x28c..0x2b0 column finding:supergun_ranked_preset_write_set writes by hand).',
  consequence='There is a NARROW seam between the Collection front-end and the MvC2 cartridge: everything MvC2-specific begins at FUN_140607B50, and the only things the front-end hands it are game_state+8 = 5, the config settings column, and the host-device session block (finding:mode_lives_in_host_device_session). A shell that wants to reach character select through game code does not need the front-end, it needs those three inputs plus a constructed host device (finding:native_render_host_device_ctor allocates the 0x10000000 arena that FUN_140607B50 then carves).',
  caveat='NOT RUN. No SUPERGUN run has ever started from a carved-but-empty arena; every run to date restores a captured anchor. The gate for any such attempt is unchanged: the select-phase blk it produces must match a captured cold select blk for the same mode.';

UPSERT finding:retail_exe_is_packed_measured SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='The retail on-disk exe is packed and cannot be statically mapped: measured from its own PE headers',
  statement='Parsed the retail PE directly. File 8,826,960 B; OptionalHeader SizeOfImage 0x40E8000 = 68,059,136 = exactly the live SizeOfImage and exactly the size of pre/exe_image.bin, so the on-disk file is 7.7x smaller than the image it produces. ImageBase 0x140000000. DllCharacteristics 0x8020 = HIGH_ENTROPY_VA plus TERMINAL_SERVER_AWARE, with DYNAMIC_BASE (0x40) CLEAR, and there is NO base relocation directory at all -- the image is position-dependent, which is why sg_runner main.rs asserts exe_base == 0x140000000 (contract C1). Ten sections, NINE of them with a blank name; section 1 VA 0x1000 VirtualSize 0x8DA000 against RawSize 0x367A00, section 3 VA 0xA31000 VirtualSize 0x2540000 (37.6 MB) against RawSize 0x28400 (165 KB). Entry point RVA 0x40E45D8 lies in the LAST section (0x3D6C000..0x40E8000), not in the code section that holds WinMainCRTStartup (RVA 0x7FE1D1). The import directory at RVA 0x3D6F0B4 has EXACTLY 17 descriptors -- kernel32, user32, advapi32, oleaut32, gdi32, shell32, version, ole32, D3DCOMPILER_43, d3d11, DINPUT8, WS2_32, IPHLPAPI, WINMM, IMM32, MFPlat, MFReadWrite -- confirming the WORKSTREAM-SUPERGUN-CORE 6.5 note that the PE import table is a 17-stub rebuild and the real IAT is the runtime-filled band at 0x1408DB000. The file embeds an X.509 certificate whose issuer is Enigma Protector CA and organisation Enigma Protector (first occurrence at file offset 5,165,863), and the install directory contains NO steam_api64.dll even though WinMain calls the Steam API at its first instruction.',
  consequence='exe_image.bin cannot be produced by a shell doing the Windows loader job against the on-disk file: the code sections are compressed, the entry point is the protector stub, there are no relocations, and at least one DLL the process needs is not on disk. The only two honest routes to not shipping 68 MB are (a) let the player run their own installed game once and snapshot the unpacked image locally, or (b) the decomp track. Rebuilding the unpacker is a research problem, not a port.',
  caveat='The doc RECEIPT-RUNNER-LIVE-DEBUG.md:41 names the protection Enigma Virtual Box from the runtime temp file evb199e.tmp; the file itself carries an Enigma PROTECTOR certificate. Both are the same vendor and both conclusions (packed code, virtualised file system) point the same way, but which product does which job here is UNKNOWN and does not change the consequence.';

UPSERT finding:game_state_bin_duplicates_the_exe_image SET confidence='high', date='2026-09-07',
  contribution_candidate=true,
  title='game_state.bin is a byte-for-byte duplicate of one page of exe_image.bin, and that page carries live session pointers',
  statement='select-01/pre: game_state.bin (0x1000 B, dumped from *(0x140ACD3A0) = 0x140AC6D40) compared against exe_image.bin at file offset 0xAC6D40 differs in ZERO of 4096 bytes. Its contents on that anchor: +0x00 arena 0x07FC1000, +0x08 scene id 5, +0x10 frame function 0x140607D60, +0x1B0 blk 0x152C1000, +0x208 staging 0x123C1000. So the config/G object that carries the arena base, the blk pointer, the mode column and the capture roster (finding:supergun_select_anchor_carries_capture_roster) lives INSIDE the exe image, which is what --gs exe means and why it is the oracle configuration.',
  consequence='Two consequences for the anchor product. (1) game_state.bin is pure redundancy -- 4 KB, always recoverable from exe_image.bin. (2) exe_image.bin is NOT code-only: it is the carrier of the capture session address layout and roster. Any plan that replaces exe_image.bin with a shipped or compiled artefact must also produce that page, and any plan to hash two player anchors for equality must treat this page as session state, not as image bytes.',
  caveat='An indicative qword scan of exe_image.bin (includes false positives) finds 465 pointer-shaped values below the image inside the game globals band 0x142D00000..0x142F40000, 193 of them inside the captured arena. The exact set of SEMANTICALLY live host pointers in the image remains bounded only by finding:load_path_static_global_set (the five pointer globals) plus the 17 bank globals 0x142EDF588..0x142EDF620 and the render tables 0x142EC6884..0x142EC6DE0; a full inventory is UNKNOWN.';

RELATE finding:steam_boot_entry_chain->cites->source:ghidra_20260907_boot SET id='e203_boot_ghidra';
RELATE finding:steam_boot_entry_chain->cites->routine:FUN_140046790 SET id='e203_boot_winmain';
RELATE finding:mvc2_mode_entry_has_two_callers->cites->source:ghidra_20260907_boot SET id='e203_entry_ghidra';
RELATE finding:mvc2_mode_entry_has_two_callers->cites->source:anchor_select01_20260905 SET id='e203_entry_anchor';
RELATE finding:retail_exe_is_packed_measured->cites->source:ondisk_pe_20260907 SET id='e203_pe_file';
RELATE finding:game_state_bin_duplicates_the_exe_image->cites->source:anchor_select01_20260905 SET id='e203_gs_anchor';

UPDATE finding:steam_boot_entry_chain SET status='confirmed';
UPDATE finding:mvc2_mode_entry_has_two_callers SET status='confirmed';
UPDATE finding:retail_exe_is_packed_measured SET status='confirmed';
UPDATE finding:game_state_bin_duplicates_the_exe_image SET status='confirmed';
