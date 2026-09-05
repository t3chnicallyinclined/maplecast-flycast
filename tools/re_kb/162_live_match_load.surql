-- 162_live_match_load.surql -- the match-load path MEASURED on the running game, 2026-09-05.
-- First live-debugger session against Steam MvC2 (x64dbg + x64dbg-mcp-server, HTTP JSON-RPC).
-- Doc: supergun/docs/RECEIPT-RUNNER-LIVE-DEBUG.md
-- Idempotent and replayable; explicit edge ids.
USE NS re DB kb;

-- ─────────────────────────────────────────────────────────────────────────────
-- SOURCES
-- ─────────────────────────────────────────────────────────────────────────────

UPSERT source:tool_x64dbg_mcp SET kind='tool', strength='code',
  ref='x64dbg + duty1g/x64dbg-mcp-server (MIT, Zig) built from source with Zig 0.15.2; plugin at C:\\Users\\trist\\tools\\re\\x64dbg\\release\\x64\\plugins\\x64dbg-MCP-Server.dp64',
  note='Zig 0.16.0 CANNOT build it: std.os.windows.ws2_32 in 0.16 is 248 lines with ZERO function declarations, the whole Winsock surface having been removed from the stdlib. 0.15.2 has 83 pub extern ws2_32 functions and pub const SOCKET = *opaque{} and builds the source unmodified. Do not hand-patch a plugin that loads into the debugger address space.';

UPSERT source:meas_live_session_20260905 SET kind='reproduction', strength='reproduction',
  ref='Steam MvC2 (MarvelVsCapcomFightingCollection.exe) pid 15560, windowed, training mode, human at the controls',
  note='REPRODUCIBLE: the recipe is documented in supergun/docs/RECEIPT-RUNNER-LIVE-DEBUG.md section 6 and can be re-run by anyone (attach by hand, read *(0x142edf560), breakpoint FUN_140607D60, dump). Two of the readings cross-check against the two independent prior captures 20260903-000941 and receipt-20260903-stage9-anchor. Every value in this seed was read off the RUNNING process. EvalExpression resolves memory reads while the target is RUNNING (no pause); DumpMemory/ReadMemory/Disassemble/registers require PAUSED.';

-- ─────────────────────────────────────────────────────────────────────────────
-- METHOD / SAFETY -- how to drive this game under a debugger without killing it
-- ─────────────────────────────────────────────────────────────────────────────

UPSERT finding:live_debug_safety_rules SET status='confirmed', confidence='high', date='2026-09-05',
  title='Breakpoint stops are SAFE on Steam MvC2; PauseDebug and duplicate attach are FATAL',
  statement='Measured, not assumed, and each failure mode killed the game once. (1) AttachProcess while a human is already attached to the same pid is FATAL -- two debuggers cannot hold one process and the started/stopped churn terminated it. (2) PauseDebug on a running frame is FATAL -- it killed the game at character select. (3) Attaching by hand in the x64dbg GUI is fine, done repeatedly. (4) A BREAKPOINT STOP is fine: the game stopped at FUN_14060ECD0, held while 2 MB was read and dumped, and resumed normally. There is NO anti-debug problem: the Enigma Virtual Box protection layer (module evb199e.tmp at 0x3B400000) does not interfere with attach or breakpoints.',
  method='Rule for any agent driving this: the HUMAN attaches; the tool never calls AttachProcess or PauseDebug. To stop the game, set a breakpoint and let it hit.';

UPSERT finding:live_no_aslr SET status='confirmed', confidence='high', date='2026-09-05',
  title='The Steam MvC2 module loads at 0x140000000 live -- no ASLR, no rebase',
  statement='EvalExpression "MarvelVsCapcomFightingCollection.exe:base" returns 0x140000000, identical to the base of the Ghidra image mvc_dump.bin. Every FUN_140... address from static analysis is directly usable against the live process with no translation.';

UPSERT finding:live_frame_coherent_dump SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='A frame-coherent blk dump IS obtainable -- the "unmeasurable" gate arm is open',
  statement='The Ghidra-side report of 2026-09-05 declared byte-equality against live memory at battle entry unmeasurable, because dump_live.py snapshots by ReadProcessMemory while the game runs and tears across ~20 frames (run 20260903-000941: clock_value 2239 -> clock_value_after 2259 over a 0.33 s dump). That is true of dump_live and is NOT true of a breakpoint. Recipe: SetBreakpoint 0x140607D60 (FUN_140607D60, the whole-frame function) -> WaitForPause (hit in 47 ms) -> read blk from *(0x142edf560) -> DumpMemory blk,211736 -> DeleteBreakpoint -> run. The process is STOPPED at the frame function, so the image is frame-coherent BY CONSTRUCTION.',
  method='DumpMemory writes RAW binary and dumps the whole containing region (2,168,630 B for a 211,736 B request); the first 0x33B18 bytes are blk and validated against every known offset (G, stage, all six fighter structs).';

-- ─────────────────────────────────────────────────────────────────────────────
-- POINTERS, CONFIRMED LIVE
-- ─────────────────────────────────────────────────────────────────────────────

UPSERT finding:live_pointer_set SET status='confirmed', confidence='high', date='2026-09-05',
  title='The exe-relative pointer set, dereferenced on the live process',
  statement='ctx = *(0x142ef0ab0) = 0x0FFC1000; ctx[0] (arc image) = 0x07FC1000; ctx[8] (DC-RAM base) = 0x103C1000; G = *(0x142edf580); entity list = *(0x142edf628); GGPO session = *(0x142E10B98) = 0 in training/offline. G - blk = 0x3CB8 EXACTLY, and entity_list - blk = 0x324E0 EXACTLY, both confirmed by arithmetic on live values.',
  caveat='blk is NOT inside the DC-RAM window. If it were, the block map would place it at ctx[8] + 0x264588 = 0x10625588 (since G maps to DC 0x8C268240 and G = blk+0x3CB8). It is at 0x147C1000. blk is a separate host allocation -- do NOT translate blk addresses through the DC map.';

UPSERT finding:live_blk_is_allocator_assigned SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='blk MOVES within a single boot -- it is allocator-assigned, and "allocated once per BOOT" is wrong',
  statement='Three consecutive observations in ONE session with no relaunch: character select #1 blk = 0x13FC1000; match load #1 blk = 0x147C1000 (+0x800000); character select #2 blk = 0x14CC1000 (+0x500000). Monotonically increasing with a NON-UNIFORM stride. That rules out fixed per-phase slots, rules out double-buffering, and rules out a per-mode region: it is an allocator handing out successive blocks. blk address carries no meaning -- always dereference *(0x142edf560), never cache it.',
  supersedes='The previously held belief that blk is allocated once per BOOT. That is wrong within a session.',
  method='Also checked for a ring of state buffers at blk +/- N*0x800000 for N in -2..+3: only the live block has a valid G signature, every neighbour is zeros. GGPO is not cycling them -- the session pointer is 0 offline.';

UPSERT finding:live_blk_self_pointers SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='blk contains ABSOLUTE pointers to itself -- relocation must fix them up',
  statement='Scanning the two dumps for qwords whose value falls in [blk, blk+0x40000): 804 self-pointers at character select, 287 at the loader entry. First at +0x3DE0 (inside fighter slot 0, which begins at +0x3DB8), then +0x4518, +0x6A08, +0x6A10, +0x6DE0. This makes the tape anchor documented relocation blk_new - anchor_blk NECESSARY rather than merely prudent, and it is now a measured count rather than an assumption. 2,593 of the 17,498 bytes differing between the two dumps sit inside these pointers, i.e. that portion of the delta is pure relocation noise from the two different base addresses.';

-- ─────────────────────────────────────────────────────────────────────────────
-- THE STATE MACHINE
-- ─────────────────────────────────────────────────────────────────────────────

UPSERT finding:live_charselect_is_g2_eq_1 SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='G+2 == 1 IS character select -- the claim the whole SUPERGUN roster plan rests on',
  statement='Read live while a human sat at the MvC2 character-select screen: G[0..4] = [2,1,1,3,0]. In a match the same bytes read [2,1,2,3,2]. The SH4-side report predicted this from table 0x140A6BFD8 whose entry [1] = 0x140611E80 -> FUN_140628D60 (character select), a SIBLING table entry to battle at [2] = 0x140611B50 -> FUN_14060E170. Upgrade from INFERRED to CONFIRMED.',
  consequence='SUPERGUN can run the REAL character-select screen by writing ONE BYTE to the tick it already calls (FUN_140118950). No new entry point and no new code path are required.';

UPSERT finding:live_fsm_path_observed SET status='confirmed', confidence='high', date='2026-09-05',
  title='The battle FSM path observed end to end: the VS intro runs BEFORE the loader',
  statement='Sampled live across a real select-to-match transition. G+3 went 3 -> 2 -> 3 and G+4 went 0 -> 1 -> 2. G+3 = 2 is 0x14060F980 -> FUN_140628570, the VS intro, which has its OWN G+4 machine (which is why G+4 read 1 there -- that 1 belonged to the intro machine, not the loader machine). G+3 = 3 is 0x14060F330 -> the load machine, with G+4 walking 0 (load) -> 1 (init) -> 2 (in-round). Phase readings: character select [2,1,1,3,0] stage 6; loading/VS intro [2,1,2,2,1] stage 11; loader entry [2,1,2,3,0] stage 11; in round [2,1,2,3,2] stage 11.',
  closes='The Ghidra-side flagged UNKNOWN "can we enter at G+3=3 directly, or must states 0/1/2 run first?" -- states 0/1/2 DO run. Entering directly at G+3 = 3 would skip the VS intro; whether that is harmless remains open.',
  cross_check='The in-round reading [2,1,2,3,2] is byte-identical to both previously captured dumps (20260903-000941 and receipt-20260903-stage9-anchor), which independently validates the whole reading.';

UPSERT finding:live_dispatch_in_registers SET status='confirmed', confidence='high', date='2026-09-05',
  title='The jump-table dispatch confirmed in live registers at the loader entry',
  statement='At a breakpoint on FUN_14060ECD0: rax = 0x140A6ABF8, which is the G+4 jump table itself, still in the register from LEA RAX,[0x140a6abf8] immediately before JMP [RAX+RCX*8]. The return address [rsp] = 0x140611D7A, inside FUN_140611D60, the G+2 dispatcher. Other registers: rcx 0, rbx 0, rdx 0x140AC6D40, rsp 0x6FBFC08. The chain both RE halves drew from static tables appeared in live registers.';

UPSERT finding:jump_table_bounds_from_binary SET status='confirmed', confidence='high', date='2026-09-05',
  title='Jump-table entry counts read from the binary -- each RE half had one wrong',
  statement='Read directly out of mvc_dump.bin (file offset = VA - 0x140000000). 0x140A6ABD0 (G+3) = 5 entries {f450,f350,f980,f330,f090} -- both halves agreed. 0x140A6ABF8 (G+4) = 6 entries {ecd0,ebf0,ede0,e560,f0a0,f240} then a zero qword terminator -- SH4 right, Ghidra said 4. 0x140A6BFC8 (G+1) = 2 entries -- Ghidra right, SH4 said 4. 0x140A6BFD8 (G+2) = 7 entries {611D90,611E80,611B50,611A60,611A70,611B60,611A80} -- SH4 right.',
  caveat='NEITHER RE half is reliable on table bounds while the Ghidra HTTP bridge is down; both were working from cached disassembly. Read tables from the binary.';

-- ─────────────────────────────────────────────────────────────────────────────
-- THE WRITE SET
-- ─────────────────────────────────────────────────────────────────────────────

UPSERT finding:live_write_set_verified SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='Every roster field offset verified live, by a human pressing buttons',
  statement='Fighter i at blk + 0x3DB8 + i*0x738. Read before and after a human picked Storm/Magneto/Colossus: slot0 cid 19->42 costume 0->1; slot2 cid 20->44 costume 0->1 assist 0->1; slot4 cid 21->50 costume 0->1 assist 0->1. Slots 1,3,5 (P2) untouched at 23/22/6. So +0x6C0 = character id, +0x6C1 = costume, +0x655 = assist all move on human input exactly as derived from the disassembly; +0x6B0 = slot id reads 0..5 and +0x6D3 = team position reads 0,0,1,1,2,2, the predicted i>>1 pattern. Slot order P1C1,P2C1,P1C2,P2C2,P1C3,P2C3 (EVEN = P1) confirmed live.',
  consequence='There is NO separate character-select buffer -- the picks live in the six fighter structs the whole time, which is exactly why SUPERGUN can write them directly.';

UPSERT finding:live_stage_written_at_load SET status='confirmed', confidence='high', date='2026-09-05',
  title='The stage byte is written at LOAD, not at character select',
  statement='blk+0x6D04 sat at 6 through the entirety of character select -- across every character and assist pick -- and became 11 during the transition into the match. It therefore belongs in the LOAD write set, not the select write set. All six costume bytes also normalise to 1 at load, including P2 slots that read 0 throughout select. Neither fact is observable by static analysis.';

UPSERT finding:live_cold_blk_is_91_percent SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='A cold character-select blk is already 91.7 percent of the battle-entry state, and G differs by ONE byte',
  statement='Two frame-coherent dumps of 0x33B18 bytes each -- one taken at character select (breakpoint on FUN_140607D60) and one at the loader entry (breakpoint on FUN_14060ECD0) -- differ in 17,498 of 211,736 bytes = 8.3 percent, across 532 changed regions (gap > 64 B), the largest being +0x0069BC..+0x006B3F at 388 B. G goes [2,1,1,3,0] -> [2,1,2,3,0]: ONE byte, G+2, 1 -> 2. G+3 and G+4 are IDENTICAL at both endpoints. The machine passes through the VS intro in between and returns to precisely the values already sitting there at select. Stage goes 6 -> 11. Of the 17,498 differing bytes, 2,593 sit inside absolute self-pointers, so the real semantic delta is about 14,900 bytes.',
  consequence='Strongest evidence yet for the SUPERGUN plan: take a cold character-select blk, write the roster and stage, set G+2 = 2, keep ticking. 91.7 percent of what the loader needs is already correct, and both endpoints are on disk to check any candidate write set against.',
  caveat='HONEST LIMIT: the two dumps are from DIFFERENT matches and DIFFERENT blk allocations, so part of the 8.3 percent is match-specific (the roster, the stage) and part is relocation. It is a strong indication, NOT a proof that a written cold anchor produces a byte-identical load.';

UPSERT finding:live_ggpo_savestate_lead SET status='open', confidence='medium', date='2026-09-05',
  title='OPEN LEAD: blk is probably the GGPO save-state buffer, and the game contains its own definition of its size',
  statement='Hypothesis (Tris, 2026-09-05): GGPO requires the title to implement save_game_state(buffer, len, checksum, frame) and load_game_state, which forces exactly the shape blk has -- one contiguous blob containing everything the simulation can roll back. Finding the ggpo_start_session call site and reading its callback pointers would yield the GAME OWN definition of its rollback-state size, the number Gate N1 had to find by bisection (211,752 vs 211,760, the 8-byte ctx addendum that made it pass).',
  disconfirmed_part='The blk ADDRESS movement is NOT GGPO: the session pointer *(0x142E10B98) reads 0 in training/offline, and there is no ring of state buffers at blk +/- N*0x800000.',
  caveat='Already-established limit: rollback-sufficiency is not replay-sufficiency (RNG is the counter-example). This would be a definition and a lower bound, not the whole answer.',
  next='Static analysis of the ggpo_start_session call site, or repeat this live session during an ONLINE match where the session pointer is non-zero.';

-- ─────────────────────────────────────────────────────────────────────────────
-- EDGES
-- ─────────────────────────────────────────────────────────────────────────────

RELATE finding:live_charselect_is_g2_eq_1 -> cites -> source:meas_live_session_20260905 SET id='e_live_cs_src';
RELATE finding:live_fsm_path_observed -> cites -> source:meas_live_session_20260905 SET id='e_live_fsm_src';
RELATE finding:live_write_set_verified -> cites -> source:meas_live_session_20260905 SET id='e_live_ws_src';
RELATE finding:live_cold_blk_is_91_percent -> cites -> source:meas_live_session_20260905 SET id='e_live_cold_src';
RELATE finding:live_blk_is_allocator_assigned -> cites -> source:meas_live_session_20260905 SET id='e_live_alloc_src';
RELATE finding:live_blk_self_pointers -> cites -> source:meas_live_session_20260905 SET id='e_live_ptr_src';
RELATE finding:live_frame_coherent_dump -> cites -> source:tool_x64dbg_mcp SET id='e_live_dump_tool';
RELATE finding:live_debug_safety_rules -> cites -> source:tool_x64dbg_mcp SET id='e_live_safe_tool';
RELATE finding:live_stage_written_at_load -> cites -> source:meas_live_session_20260905 SET id='e_live_stage_src';
RELATE finding:live_dispatch_in_registers -> cites -> source:meas_live_session_20260905 SET id='e_live_regs_src';

-- ── ADDENDUM: three findings were rejected on first apply because they carried NO citation.
-- The schema assertion on finding.status (confirmed requires a cites edge to a
-- reproduction/code source or a routine) caught the omission. Fixed, not forced.

UPSERT source:bin_mvc_dump SET kind='binary', strength='code',
  ref='C:\Users\trist\ghidra_projects\mvc_dump.bin -- the unpacked Steam MvC2 image, file offset = VA - 0x140000000',
  note='Jump-table entry counts were read as raw qwords straight out of this file rather than from the Ghidra HTTP bridge, which was DOWN for both RE agents on 2026-09-05 and is why each of them mis-bounded a different table.';

RELATE finding:jump_table_bounds_from_binary -> cites -> source:bin_mvc_dump SET id='e_jt_bin';
RELATE finding:live_no_aslr -> cites -> source:meas_live_session_20260905 SET id='e_aslr_src';
RELATE finding:live_pointer_set -> cites -> source:meas_live_session_20260905 SET id='e_ptrset_src';

UPDATE finding:jump_table_bounds_from_binary SET status='confirmed';
UPDATE finding:live_no_aslr SET status='confirmed';
UPDATE finding:live_pointer_set SET status='confirmed';

-- ── ADDENDUM 2: the character-select COMMITTER, caught by a hardware write breakpoint,
-- and FUN_14060C370's switch table resolved from the binary. 2026-09-05.

UPSERT steam_routine:FUN_14062bb30 SET addr=0x14062bb30, name='FUN_14062bb30',
  note='THE CHARACTER-SELECT COMMIT. sub rsp,0x28; mov rax,[142EDF580]; xor ecx,ecx; INC byte [rax+2] (G+2: 1->2); then mov byte [rax+3..7],0. Found by a hardware write breakpoint on G+2 -- Ghidra cached disassembly has NO function covering this address, it is reached only via a jump table.';

UPSERT finding:live_charselect_committer SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='FUN_14062BB30 commits character select by INCREMENTING G+2 and ZEROING G+3..G+7',
  statement='A hardware write breakpoint on G+2 trapped the exact instruction. 0x14062BB30 sub rsp,0x28; 0x14062BB34 mov rax,[142EDF580]; 0x14062BB3B xor ecx,ecx; 0x14062BB3D FE 40 02 = INC byte ptr [rax+2] (this is the commit, G+2 goes 1 to 2); then 0x14062BB40/4B/56/61/6C store 0 into G+3, G+4, G+5, G+6 and G+7. Confirmed by live disassembly at the stop. Ghidra cached disassembly has NO function covering 0x14062BB40 -- the region is reached only through a jump table and was never analysed.',
  consequence='TWO facts neither RE report had. (1) It is an INCREMENT, not a store -- the same idiom as FUN_14060E560 INC [G+3] at round end; the game steps these dispatch bytes rather than assigning them. (2) It ZEROES G+3, so the battle machine begins at G+3 = 0, which routes to 0x14060F450, the commit-select routine that reads each fighter source slot as (f+0x6B0 AND 1) + f+0x6D3*2 and moves cid/assist/costume into battle order. Full path: G+3 goes 3 (stale leftover) -> 0 (zeroed here) -> FUN_14060F450 -> 1 -> 2 (VS intro) -> 3 (load machine).',
  caveat='This makes the previously flagged "enter at G+3 = 3" shortcut DANGEROUS rather than merely lossy: it would skip FUN_14060F450 itself, so the picks would sit in the select-side fields and never be committed to battle order. SUPERGUN must write G+3 = 0 and let the game run states 0,1,2.';

UPSERT finding:c370_switch_resolved SET status='confirmed', confidence='high', date='2026-09-05',
  title='FUN_14060C370 dispatches through a 25-entry RVA table at 0x14060D010; case 4 builds the HUD portraits',
  statement='Prologue at 0x14060C374: DEC ECX; CMP ECX,0x18; JA default; MOVSXD RAX,ECX; MOV EAX,[R14+RAX*4+0x60D010]; ADD RAX,R14; JMP RAX, with R14 = 0x140000000. So the table is 25 dword RVAs at 0x14060D010 indexed ECX-1. Read from the binary: ECX=1 -> 0x14060C3AF (stage bank, 0xD82D000/0xD85D000); ECX=4 -> 0x14060C40E, which IS the address of the CALL to FUN_14060D560 (HUD portrait and name pages); ECX=5 -> 0x14060C44E (effect/UI banks 0xC420000, 0x95, 0x345, 0x346, 0x96); ECX=6 -> 0x14060C5F5 (per-slot cid to AFS 3+cid copy into PLbase+0x148000).',
  resolves='The direct conflict between the two RE reports of 2026-09-05. The Ghidra side was right that case 4 builds the portraits; the SH4 side placed portraits on case 2 and the effect banks on case 4 (they are on case 5).',
  caveat='The portrait build is CONDITIONAL: if (G+0x14 != 0x40) FUN_14060C370(4). Measured G+0x14 = 0x60 in both live dumps, so it runs. Setting it to 0x40 means the portrait/name pages are never rebuilt from each fighter own DAT and the HUD keeps the previously resident roster -- exactly the production bug fixed on 2026-09-04, reachable by getting one dword wrong.';

RELATE finding:live_charselect_committer -> cites -> source:meas_live_session_20260905 SET id='e_committer_src';
RELATE finding:live_charselect_committer -> cites -> steam_routine:FUN_14062bb30 SET id='e_committer_rtn';
RELATE finding:c370_switch_resolved -> cites -> source:bin_mvc_dump SET id='e_c370_bin';
