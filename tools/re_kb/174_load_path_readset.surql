-- 174_load_path_readset.surql -- FILLING finding:load_path_readset_unknown, 2026-09-05.
-- Static whole-image call-graph closure of the select->battle LOAD path, measured on the
-- Ghidra disassembly cache (29,678 functions) + mvc_dump.bin, plus the 257-slot live IAT map.
-- Method: RE-METHOD step 2 (propagate along the call graph) + step 3 (translate globals).
-- NOT a dynamic read-before-write inventory: that arm is still open (see the last finding).
USE NS re DB kb;

UPSERT source:load_closure_20260905 SET kind='analysis', strength='code',
  ref='C:\\Users\\trist\\AppData\\Local\\Temp\\sg_load\\{closure.py,paths.py,readset.py,tables.py} over d3dcap/replay/re_map/cache/steam_disasm.jsonl (29,678 functions, whole image) and C:\\Users\\trist\\ghidra_projects\\mvc_dump.bin (base 0x140000000, file offset == RVA)',
  note='Transitive DIRECT-CALL closure from the load-path roots (the G+2/G+3/G+4 dispatch-table targets read out of the binary, FUN_14062BB30, FUN_14060F450, FUN_140628570, FUN_14060C370, FUN_14060DCF0). 487 functions. Every CALL/JMP through an absolute IAT slot was collected; small-DLL slots were identified by clustering the 257 live IAT values of sg_boot/iat_map.txt into module bands. Over-approximation by construction: it includes branches that may never be taken, and it STOPS at vtable dispatch.';

UPSERT source:blk_cold_select_dump SET kind='capture', strength='reproduction',
  ref='C:\\Users\\trist\\AppData\\Local\\Temp\\claude\\C--Users-trist-projects\\193726f3-46b8-433c-937d-43b196c708ab\\scratchpad\\loadcap\\{blk_cold_select.bin, blk_at_load.bin, blk_cold_select.meta.json}',
  note='THE TWO DUMPS OF finding:live_cold_blk_is_91_percent, LOCATED 2026-09-05. 2,168,630 B each (DumpMemory dumps the whole containing region; the first 0x33B18 B are blk). blk_cold_select: G=[2,1,1,3,0], stage 6, blk 0x14CC1000, ctx 0x0FFC1000, arc 0x07FC1000, dcram 0x103C1000, roster cids 42/23/20/22/21/6. They are blk-ONLY: there is NO select-phase ctx, dcram, exe or game_state image anywhere on disk.';

-- ---------------------------------------------------------------------------
-- 1. THE LOAD-PATH EXTERNAL-CALL SET  (the make-or-break answer for option C)
-- ---------------------------------------------------------------------------

UPSERT finding:load_path_external_call_set SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The LOAD path reaches ZERO file, D3D, socket, HID or audio imports -- 24 IAT slots, all kernel32/KERNELBASE/ntdll',
  statement='487-function direct-call closure from the select->battle roots. It calls exactly 24 absolute IAT slots. Clustering the 257 live IAT values by module band gives kernel32/KERNELBASE (192 slots), ntdll (13) and one 34-slot band, plus 18 slots in eight small bands = the d3d11 / DINPUT8 / WS2_32 / WINMM / IPHLPAPI / IMM32 / MFPlat / ole32 imports. NONE of the 24 is one of those 18. Stronger: a whole-image scan for CALL or JMP qword ptr [small-DLL slot] finds every reference and NOT ONE of the referencing functions is in the closure (0x1408db048 <- FUN_14026E200 and FUN_14026F6E0 <- FUN_140046790, the device-init function, which is not called from the load path; 0x1408db760/768/770/778 <- FUN_140066C00 / FUN_14026D960 / others). None of those functions appears as a qword anywhere in the image, so they are not vtable members either and cannot be reached by the closure vtable dispatch. Named slots on the path: ntdll!RtlAllocateHeap 0x1408db240, ntdll!RtlEnterCriticalSection 0x1408db190, kernel32!HeapFree 0x1408db238, GetLastError 0x1408db2d8, SetLastError 0x1408db510, TlsSetValue 0x1408db218. 0x1408db8f8 is NOT an import: it holds 0x140036560, the in-image CFG __guard_dispatch_icall thunk.',
  method='Names from RECEIPT-RUNNER-GATE1.md section 3.1 (same-boot export resolution); bands from sg_boot/iat_map.txt; the exe PE import table itself is USELESS (Enigma Virtual Box rebuilt it to 17 stub descriptors at RVA 0x3d6f21c) -- the real IAT is the runtime-filled 257-slot band 0x1408db000..0x1408db880.',
  caveat='Static over-approximation of REACHABILITY, under-approximation of BEHAVIOUR: it stops at vtable/register dispatch (32 closure functions make one, listed in finding:load_path_indirect_dispatch_boundary). It does not prove the calls fire.';

UPSERT finding:load_path_new_external_calls_vs_tick SET status='confirmed', confidence='high', date='2026-09-05',
  title='What the LOAD path adds over the tick 24 UCRT calls: a critical section, from the host-texture allocator',
  statement='Resolving the tick 634 executed-function names (scratchpad/frame_20260903-000941_tick_r0.executed.txt) against the closure: 392 of the 487 closure functions are never executed by a battle tick. The two external slots whose ONLY callers are load-only, non-UCRT functions are 0x1408db190 = ntdll!RtlEnterCriticalSection (callers FUN_140270930, FUN_140272700, FUN_140272EA0, FUN_1402B34A0, FUN_140396D00, FUN_140397050) and 0x1408db108 (same caller set minus two; value 0x7ffc8c2613f0 sits 0x100 B after RtlEnterCriticalSection in ntdll and pairs 1:1 with it -- INFERRED ntdll!RtlLeaveCriticalSection). Everything else the load path calls externally is the same UCRT sprintf/locale surface finding:determinism_external_calls already bounds and the p-code harness already stubs.',
  caveat='The UCRT-band half of the load-only split is NOISY: 420 of the 634 tick-executed names are mangled C++ or gap@ labels that do not resolve to a FUN_ address, so UCRT leaves look load-only when they are not. Only the non-UCRT result (the 31 host-graphics/allocator functions 0x140048460..0x140397050) is trustworthy, and those are named FUN_* on both sides.';

-- ---------------------------------------------------------------------------
-- 2. WHAT THE LOAD PATH DOES THAT THE TICK NEVER DOES: create host textures
-- ---------------------------------------------------------------------------

UPSERT finding:load_path_host_texture_creation SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The load path CREATES host texture objects -- the ctx slot table is the gate, and 0x1402704C8 is the fault SUPERGUN already hit',
  statement='FUN_140844DC0 (bank loader FUN_14060C370 -> here, one call per bank record) reads ctx = *(0x142EF0AB0) and the bank TCW base ctx+0x1E0098, computes TCW = texIndex + base, stores it to rec+0x0C (its only DC-RAM write), then indexes the texture-slot table at ctx + (TCW*3 + 0x3C014)*8 = ctx + 0x1E00A0 + TCW*0x18 and TESTS ITS FIRST u16: CMP word ptr [slot],0 / JNZ skip at 0x140844E40. NON-ZERO = already populated = SKIP. ZERO = fill the record (flags/w/h at +0x2/+0x4/+0x6/+0x8) and CALL FUN_140048460, whose return value is stored at slot+0x10 -- the host texture handle. FUN_140048460 dereferences the host device object RBP = *(0x140ACD3A8), indexes its texture table at RBP+0xD0500+idx*8 (idx < 0x1000), and on NULL allocates via FUN_140270490 -- whose last instruction 0x1402704C8 is JMP qword ptr [R10+0x38] on the vtable of the object returned by FUN_14011EA90(0x142EBCE38), i.e. EXACTLY the fault WORKSTREAM-SUPERGUN-CORE.md 6.5 recorded as fault at 0x1402704C8, table[10] = 0x51b3000 matching the fault RAX. The observed SUPERGUN crash IS the load path creating a texture.',
  method='Disassembly of FUN_140844DC0 / FUN_140048460 / FUN_140270490 from the Ghidra cache; the ctx offset arithmetic checks against the ctx+0x1E0030..0x1E31CC texture-slot table of finding:determinism_battle_anchor_carry item 4 and the 123 flags at ctx+0x1E2624+n*0x18 of finding:determinism_hidden_state_inventory.',
  consequence='This is the mechanism behind finding:anchor_phase_validity. A select-phase anchor carries a slot table populated for the SELECT banks. At load, every slot that is already non-zero is SKIPPED and keeps the capture session handle and dimensions -- which is literally the ctx slot table reports 32x32 for 8x8 tiles symptom -- while every zero slot triggers a real host texture creation. Grafting a battle-phase slot table in is the opposite error. The only correct options are (a) run the load machine with the slot table ZEROED for the banks being loaded, or (b) trap FUN_140048460 and hand back a synthetic handle.';

UPSERT finding:load_path_texture_upload_is_an_in_process_ring SET status='confirmed', confidence='high', date='2026-09-05',
  title='The load path GPU work is an in-process command ring, not a D3D call -- and its EXECUTOR is not on the load path',
  statement='After creating the object, FUN_140048460 calls FUN_14004D290, which calls FUN_140272700 (map), FUN_14004BA50 (1,381 instructions; touches host-device fields +0xDBD10 and +0xDBD18) and FUN_140272EA0 (unmap). global:d3d_ring already records the ring: base +0xDBD08, write cursor +0xDBD20, byte count +0xDBD28 (limit 0x200000), record count +0xDBD2C, sequence +0xDBD38, written by FUN_1400483C0 and FUN_14004B9A0, reset by FUN_140048810, EXECUTED by FUN_140070940. FUN_140070940, FUN_1400483C0, FUN_14004B9A0 and FUN_140048810 are ALL OUTSIDE the 487-function load closure. So the load path stages commands into process memory; the host renderer drains them elsewhere.',
  consequence='A memory snapshot CAN carry the whole of what the load path does to the graphics side, because all of it is process memory: the device object, its texture table, the slot table, the ring. Nothing on the load path is a HANDLE, a device pointer obtained from d3d11.dll, or a kernel object.';

UPSERT finding:load_path_indirect_dispatch_boundary SET status='open', confidence='high', date='2026-09-05',
  title='OPEN: 32 closure functions dispatch through a vtable or register -- the static closure stops there',
  statement='The 487-function closure contains 32 functions whose call is not a direct CALL 0x140xxxxxx: the game own FSM tables (FUN_140611D60 CALL [RAX+RCX*8], FUN_140628570, FUN_14062BAF0, FUN_14061F030, FUN_140628320 -- these are the G-machine dispatches and their targets ARE in the closure via the tables read from the binary), the four in-image function-pointer tables at 0x140A6E4B8, 0x140A6E520, 0x140A75750 and 0x140A75800 (values 0x140619FE0, 0x140619DD0, 0x1406355A0, 0x140635420 -- all in-image, so snapshot-carryable), and the host-graphics band FUN_140270930, FUN_140272700, FUN_140272EA0, FUN_140396D00, FUN_14026FAE0, FUN_14026FCF0, FUN_1406364F0, FUN_14063A4A0 and FUN_14063E4A0, which dispatch on the vtables of *(0x142EBD8F0) and *(0x142E12CF8) at offsets up to +0x180.',
  next='Falsifiable and cheap: read the first qword of each object those globals point at in a live or captured process and confirm the vtable address is inside 0x140000000..0x1404E8000 (in-image .rdata). If it is, the dispatch cannot leave the image and finding:load_path_external_call_set is closed absolutely. If any vtable lies in a loaded DLL, that path is a genuine external dependency.';

-- ---------------------------------------------------------------------------
-- 3. THE STATIC READ SET, and the correction to the anchor spec
-- ---------------------------------------------------------------------------

UPSERT finding:load_path_static_global_set SET status='confirmed', confidence='medium', date='2026-09-05',
  title='The load path absolute-global reference set: 689 addresses, and the five pointers that matter',
  statement='Every absolute [0x1........] operand in the 487-function closure: 689 distinct addresses, 547 in exe .data/.rsrc, 140 in the 0x142D00000..0x142F40000 globals band, 2 in .text/.rdata. The pointer-valued ones the load path dereferences that a BATTLE TICK never touches are exactly five: 0x140ACD3A8 (host device object; readers FUN_140048460, FUN_140048890, FUN_140048910), 0x142EBD8F0 (graphics backend object; 8 reads), 0x142E12CF8 (MtObject vtable/registry; 3 reads), 0x142EBCE38 (allocator registry passed to FUN_14011EA90; 2 reads) and 0x142E12CD0 (read by FUN_14011EA90). The load path additionally WRITES the 17 bank-pointer globals 0x142EDF588..0x142EDF620 (FUN_14060C370 and FUN_14060B230) that finding:determinism_battle_anchor_carry item 3 says must be rebased, and the 0x142EC6884..0x142EC6DE0 render-table block via FUN_14060DF80.',
  caveat='STATIC and address-only: it cannot see [base+index] tables (e.g. LEA RDX,[0x140000000] then [RDX+RCX*4+0xA4F770] in FUN_140048460 = the table at 0x140A4F770), and it says nothing about read-before-write ORDER. It is a superset of the exe-globals half of the read set and NOT a substitute for the dynamic inventory.';

UPSERT finding:anchor_arena_does_not_subsume_host_device SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='CORRECTION to WORKSTREAM-SUPERGUN-CORE 6.5: the arena carve does NOT subsume the host device object, and it is NOT write-only',
  statement='6.5 says the arena at *(0x140AC6D40) subsumes everything except the exe image and two host-heap bands (0x6797EB0..0x67BE4F0 and 0x74BEC0 / 0x74C550, both BELOW the arena). The host device object *(0x140ACD3A8) is a third region: global:naomi_host_device records it live at 0x2A1DC010 in the stage-9 dump, which is ABOVE the arena (live arena 0x07FC1000 + 0x10000000 = 0x17FC1000). global:naomi_host_device further says WRITE-ONLY from the tick, a zero page suffices and size UNKNOWN. BOTH are wrong for the LOAD path: FUN_140048460 READS *(0x140ACD3A8) and then READS its texture table at +0xD0500+idx*8 BEFORE writing it (the NULL test is the create gate), and writes +0xDBD04. Its minimum extent is +0xDBD08 (d3d_ring base) + 0x200000 (the ring byte limit at +0xDBD28) = at least 0x2DBD08 bytes, about 3.0 MiB -- not a page.',
  consequence='6.5 item 3 (walk VirtualQueryEx and snapshot every MEM_COMMIT + MEM_PRIVATE region) still covers it, which is why completeness without enumeration is the right rule; but the six-region and arena-only shorthands do NOT, and a zero page for 0x140ACD3A8 that is harmless for a tick makes the load path allocate into a zeroed table.';

-- ---------------------------------------------------------------------------
-- 4. WHAT IS STILL OPEN -- the dynamic half, and why it could not be run
-- ---------------------------------------------------------------------------

UPSERT finding:load_path_dynamic_readset_blocked_on_images SET status='open', confidence='high', date='2026-09-05',
  title='OPEN: the dynamic read-before-write inventory for the load path cannot be produced -- no select-phase IMAGE SET exists',
  statement='The specified method (emu_gate.py frame over a character-select image with G+2=2, G+3=0, trace to G+4==2, then hidden_state.py) needs what emu_frame.load_run() needs: exe_image.bin, ctx.bin, dcram.bin, blk.bin, blk2.bin and game_state.bin at the addresses of ONE session, as d3dcap/ttd/runs/20260903-000941/pre has for a BATTLE frame. The 2026-09-05 x64dbg session dumped blk ONLY (blk_cold_select.bin and blk_at_load.bin, source:blk_cold_select_dump). There is no select-phase ctx, dcram, exe or game_state anywhere. Substituting the battle run ctx/dcram is not a shortcut but the error class finding:anchor_phase_validity names: the battle ctx already has the texture-slot table populated, so FUN_140844DC0 CMP word ptr [slot],0 takes the SKIP branch and the trace would under-report the load path work -- wrong in the dangerous direction.',
  next='One capture unblocks it, using the already-proven breakpoint recipe (finding:live_frame_coherent_dump): breakpoint FUN_140607D60 at CHARACTER SELECT, and while stopped dump the full set (exe 0x140000000 size 0x40E8000; arena *(0x140AC6D40) size 0x10000000, which carries ctx, dcram, staging, arc and blk; game_state page 0x140AC6D40+0x1000; the exe globals page 0x142EDF300..0x142EDF700; AND *(0x140ACD3A8) for at least 0x2DBD08 B per finding:anchor_arena_does_not_subsume_host_device) into a runs/<ts>/pre with a meta.json of the same shape. Then the specified emu_gate and hidden_state run is mechanical.';

UPSERT finding:load_path_readset_unknown SET status='open', confidence='high', date='2026-09-05',
  title='GAP (PARTIALLY FILLED): load-path EXTERNAL-CALL set measured; the dynamic read-before-write inventory still open',
  next='The external-call half is answered by finding:load_path_external_call_set and finding:load_path_new_external_calls_vs_tick (static, whole-image, no debugger). The read-set half is blocked on a capture, specified in finding:load_path_dynamic_readset_blocked_on_images. The static superset is finding:load_path_static_global_set. The one genuinely open reachability question is finding:load_path_indirect_dispatch_boundary.';

-- ---------------------------------------------------------------------------
-- EDGES
-- ---------------------------------------------------------------------------
RELATE finding:load_path_external_call_set->cites:lpecs_src->source:load_closure_20260905;
RELATE finding:load_path_new_external_calls_vs_tick->cites:lpnec_src->source:load_closure_20260905;
RELATE finding:load_path_host_texture_creation->cites:lphtc_src->source:load_closure_20260905;
RELATE finding:load_path_texture_upload_is_an_in_process_ring->cites:lptu_src->source:load_closure_20260905;
RELATE finding:load_path_indirect_dispatch_boundary->cites:lpidb_src->source:load_closure_20260905;
RELATE finding:load_path_static_global_set->cites:lpsgs_src->source:load_closure_20260905;
RELATE finding:anchor_arena_does_not_subsume_host_device->cites:aadsh_src->source:load_closure_20260905;
RELATE finding:load_path_dynamic_readset_blocked_on_images->cites:lpdrb_src->source:blk_cold_select_dump;
RELATE finding:live_cold_blk_is_91_percent->cites:lcb91_dumps->source:blk_cold_select_dump;

RELATE finding:load_path_host_texture_creation->about:lphtc_r1->steam_routine:FUN_140844dc0;
RELATE finding:load_path_host_texture_creation->about:lphtc_r2->steam_routine:FUN_140048460;
RELATE finding:load_path_host_texture_creation->about:lphtc_r3->steam_routine:FUN_14060c370;
RELATE finding:anchor_arena_does_not_subsume_host_device->about:aadsh_g1->global:naomi_host_device;
RELATE finding:load_path_texture_upload_is_an_in_process_ring->about:lptu_g1->global:d3d_ring;
