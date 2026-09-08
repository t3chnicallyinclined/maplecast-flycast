-- 187_supergun_input_audio_ring.surql
-- Cross-examination of the 2026-09-05 docs-only SUPERGUN/netcode claim set, 2026-09-06.
-- Contents: (a) the full raw->sim pad table and the reachable mask, correcting
-- supergun/docs/DETERMINISM-CONTRACT.md lines 206-207; (b) the two per-fighter input
-- distribution routines; (c) the shipped GGPO save callback computes NO checksum;
-- (d) THE AUDIO COMMAND RING the frame function writes OUTSIDE blk, which corrects the
-- "debug ring" label on finding:runner_host_heap_objects and closes the netcode-eval
-- UNKNOWN U-L7; (e) scoping of the "8 bytes short => player desyncs" inference.
-- Paths use forward slashes: a backslash in a single-quoted SurrealQL string is an invalid
-- escape, the request returns HTTP 400, and rekb.sh used to report that as success.
USE NS re DB kb;

-- ---------------------------------------------------------------------------
-- sources
-- ---------------------------------------------------------------------------

UPSERT source:xexam_20260906_ghidra SET kind='static', strength='decompile',
  ref='ghidra mvc_dump.bin (base 0x140000000, file offset == RVA) plus direct byte reads of C:/Users/trist/ghidra_projects/mvc_dump.bin',
  note='2026-09-06 cross-examination pass. Functions read end to end: FUN_140118950, FUN_14060B7D0, FUN_140048630, FUN_14003AD50, FUN_140623B40, FUN_140634920, FUN_140118290, FUN_140119380, FUN_1401188C0, FUN_140607D60, FUN_140608A00, FUN_140609D60, FUN_14006B5D0, FUN_14006B690, FUN_14006B6E0, FUN_14006BD40, FUN_14011EA90, FUN_140039DE0. Table DAT_140A4F780 and the 25-entry case table at 0x14060D010 read as raw bytes.';

-- ---------------------------------------------------------------------------
-- (a) the pad translation table, in full
-- ---------------------------------------------------------------------------

UPSERT finding:pad_raw_to_sim_table_full SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='DAT_140A4F780 is twelve (raw mask, sim bits) pairs; the four DIRECTION bits ARE in it and the reachable raw mask is exactly 0xFAF9',
  statement='FUN_140048630 (0x140048630) is twelve unrolled tests over the pair array at 0x140A4F780..0x140A4F7DF. Read out of mvc_dump.bin: 0x8->0x8000, 0x1->0x4000, 0x10->0x2000, 0x40->0x1000, 0x80->0x800, 0x20->0x400, 0x8000->0x200, 0x1000->0x100, 0x200->0x80, 0x4000->0x40, 0x2000->0x20, 0x800->0x10. Union of the raw masks = 0xFAF9 (12 bits). Union of the sim bits = 0xFFF0. Independently, the host pad builder FUN_14003AD50 produces exactly the same 12 bits: a 12-binding loop (CMP R14,0xc at 0x14003AF00) over the per-seat binding config at game_state+0x2DC + seat*0x80 + i*4 (0x14003AF1B) feeding the action table DAT_140A33F50 + action_id*4 for action ids 1..19 (gate CMP EDX,0x14 at 0x14003AF5A), plus two device-query bits OR R12D,0x8 at 0x14003B263 and OR R12D,0x1 at 0x14003B27C, plus four direction bits merged at 0x14003B285 (0x10), 0x14003B292 (0x20), 0x14003B29F (0x40) and BTS R8D,0x7 at 0x14003B2B2 (0x80). Two independent derivations, same 0xFAF9.',
  correction='This CORRECTS supergun/docs/DETERMINISM-CONTRACT.md lines 206-207, which lists six of the twelve pairs and then asserts "direction bits are NOT in this table -- where the sim takes directions from is UNKNOWN here". Four of the six pairs it lists (0x10, 0x40, 0x80, 0x20) ARE the direction bits, so the sentence contradicts its own table. Directions are in the table; nothing about the direction source is UNKNOWN.',
  consequence='receipt_gate.py --selftest --negative --flip-bit defaults to raw bit 0x8, which maps to sim 0x8000. Sim 0x8000 survives the per-fighter mask 0xBFF0 (finding:fighter_input_shift_register) so it is not masked away, but it produced no px/py/hp movement over 20 frames and the run was recorded INCONCLUSIVE. Raw 0x20 = sim 0x400 = the direction the facing-XOR treats as RIGHT, and finding:mvc2_zero_internal_input_latency measured raw 0x20 moving world X at TICK 1, so raw 0x20 is a flip bit that must diverge at tick 1 and makes the negative arm able to fail (rule M4). NOT APPLIED: the gate script is not edited by this seed.',
  caveat='0xFAF9 bounds what the SIM can consume, not what the pad word can carry. FUN_14003AD50 tail at 0x14003B695 tests game_state+0x828 and, when it is non-zero, ORs in raw bits 0x100000/0x200000/0x400000/0x800000; FUN_140118950 masks each seat word to 0xFFFFFF, not to 0xFAF9. See finding:pad_word_upper_bits_gated_by_gs828.';

UPSERT finding:pad_word_upper_bits_gated_by_gs828 SET status='confirmed', confidence='high', date='2026-09-06',
  title='The raw pad word carries four bits above 0xFAF9 when game_state+0x828 is non-zero, so "2 bytes per seat is lossless" is a bounded claim',
  statement='FUN_14003AD50 at 0x14003B695: CMP dword ptr [RAX + 0x828], 0. When non-zero the function BTS-es bits 0x14, 0x15, 0x16 and 0x17 (0x100000, 0x200000, 0x400000, 0x800000) into the returned word from the corresponding bits of its own param_3. FUN_140118950 stores each seat word masked with 0xFFFFFF (AND ECX,0xffffff at 0x1401189FD, 0x140118A20, 0x140118A43, 0x140118A66), so those bits reach game_state+0x218+4k. FUN_140048630 discards them, because they are not in DAT_140A4F780.',
  consequence='A 2 byte per seat netcode wire is lossless FOR THE BATTLE SIM, because the only reader of game_state+0x218..0x224 on the traced battle frame is FUN_140048630 (FRAME-READSET section 3.3 read-and-written list). It is NOT lossless for the raw word in general, and game_state+0x828 IS inside the tick read set. Whoever ships the 2 byte wire owns the assertion that no other consumer of game_state+0x218 exists in a shipped mode.',
  status_note='UNKNOWN: whether any non-battle load state (character select, results) has a second reader of game_state+0x218 that consumes the 0x100000 band. Test without a live game: search the disassembly cache for indexed reads of PTR_DAT_140acd3a0 + 0x218 outside FUN_140048630.';

-- ---------------------------------------------------------------------------
-- (b) per-fighter input distribution
-- ---------------------------------------------------------------------------

UPSERT finding:fighter_input_shift_register SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='FUN_140623B40 is the per-fighter 4-deep input history plus a facing-normalised cur/prev/pressed triple',
  statement='FUN_140623B40(fighter) reads the fighter latched input word fighter+0x668, masks it with 0xBFF0, and maintains: a shift register fighter+0x488 (current) -> +0x48A -> +0x48C -> +0x48E, and the triple fighter+0x492 (cur), +0x494 (prev), +0x496 (pressed = ~prev & cur), with the un-normalised pressed also at +0x490. When fighter+0x282 is non-zero each of +0x492/+0x494/+0x496 that has any of 0xC00 set is XORed with 0xC00, which swaps the two horizontal direction bits. Sim 0x400 and 0x800 are the two horizontal directions from DAT_140A4F780 (raw 0x20 and raw 0x80), so fighter+0x282 is the facing byte and this is the facing normalisation that makes forward and back relative.',
  consequence='All motion, charge and command history the sim uses lives inside blk (fighter = blk+0x3DB8 + slot*0x738, so +0x488 = blk+0x4240 + slot*0x738). A rollback that restores blk regenerates every input edge exactly; no separate input history has to be saved. The mask 0xBFF0 drops sim bit 0x4000, i.e. raw bit 0x1, from all motion history.',
  caveat='INFERRED that fighter+0x282 is the facing byte: derived from the meaning of the two bits it swaps, not from the site that WRITES +0x282. Confirm at the set site before relying on the polarity.';

UPSERT finding:input_record_fanout_six_fighters SET status='confirmed', confidence='high', date='2026-09-06',
  title='FUN_140634920 builds a 20-byte input record per seat and copies it into fighter+0x668 of all six fighters, even slots from seat 0 and odd slots from seat 1',
  statement='FUN_140634920 reads the decoded seat words blk+0x3C66 (seat 0) and blk+0x3C7A (seat 1) and writes cur/prev/pressed/released into blk+0x4420..0x4427 and blk+0x4B58..0x4B5F, then copies the 20-byte record blk+0x4420..0x4433 to blk+0x5290 and blk+0x6100, and blk+0x4B58..0x4B6B to blk+0x59C8 and blk+0x6838. Every one of those six addresses is exactly fighter+0x668 for the six fighters at blk+0x3DB8 + slot*0x738: 0x4420 = slot0+0x668, 0x4B58 = slot1+0x668, 0x5290 = slot2+0x668, 0x59C8 = slot3+0x668, 0x6100 = slot4+0x668, 0x6838 = slot5+0x668. So EVEN slots take seat 0 and ODD slots take seat 1, which is the same P1-even / P2-odd side rule the portrait pages use.',
  consequence='Seat to fighter routing is a fixed parity, not a lookup. Any seat remap in a SUPERGUN shell must happen at game_state+0x258..0x264 (finding:input_seat_route_confirmed); by the time the record reaches fighter+0x668 the side is already baked in.';

-- ---------------------------------------------------------------------------
-- (c) the shipped save callback has no checksum
-- ---------------------------------------------------------------------------

UPSERT finding:shipped_ggpo_save_has_no_checksum SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='The shipped MvC2 GGPO save_game_state callback writes NO checksum, so the shipped build has no desync detection at all',
  statement='FUN_140119380 is the GGPO save_game_state callback (registered in FUN_140118AE0, cited by finding:ggpo_advance_frame_callback). Upstream GGPO calls it as save_game_state(unsigned char **buffer, int *len, int *checksum, int frame), i.e. RCX, RDX, R8, R9. Disassembled 0x140119380..0x1401193DD in full: the body touches RCX and RDX only. 0x14011938D writes *RDX = 0x100004; 0x14011939B allocates; 0x1401193A0 writes *RCX = buffer; 0x1401193BA gathers through FUN_1401188C0; 0x1401193CC memcpys 0x100004 bytes; 0x1401193D6 returns 1. R8 and R9 are never read or written, and the function contains no arithmetic that could be a hash.',
  consequence='Upstream GGPO detects a desync by comparing the checksum the title returns for a saved frame. With no checksum written, that comparison operates on whatever the caller left in the out slot, so the shipped build cannot report a desync and cannot log one. Any statement of the form "this is the source of the desyncs players report" is therefore unfalsifiable from the shipped telemetry: the game does not measure it. It also means an adopted rollback library that offers app-supplied checksums is strictly ADDING a capability the shipped build never had, not replicating one.',
  caveat='Read from the offline image. The falsification is one breakpoint on 0x140119380 during a real online match with R8 inspected on entry and on return; that needs a human-attached debugger and has not been done.';

-- ---------------------------------------------------------------------------
-- (d) the audio command ring the frame writes OUTSIDE blk
-- ---------------------------------------------------------------------------

UPSERT finding:frame_writes_audio_command_ring_outside_blk SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='The frame function DOES emit audio commands into a 64-entry host ring outside blk, and the shell drains it once per presented frame',
  statement='Object *(0x142EBC010) is the sound system. Layout read from its own routines: +0x34260 = the BGM/stream handle, +0x34268 + bank*8 = 128 SE bank handles, +0x3466C + n*4 = 8 slots, +0x346AC + n*0x10 = a 64-entry 16-byte command ring, +0x34AAC = the ring count. WRITER: FUN_14006B5D0(op, a, b, c) appends one record and increments the count, guarded by "if (count < 0x40)" -- an over-cap write is SILENTLY DROPPED, not stalled. DISPATCHER: FUN_14006B6E0 switches on the opcode; case 2 scales its argument by DAT_1408E33E4 = 0.0078740157 = exactly 1/127 with an "< 0x80" guard, cases 8/9/10/11 index the 128 SE bank handles and pass volume (also /127) and pan, cases 6/7 set and clear the 8 slots. DRAIN: FUN_14006B690 loops until the count reaches 0, called from EXACTLY ONE site, 0x14003A4A1, inside the shell loop FUN_140039DE0 -- outside the tick. The count is zeroed at 0x140039DFD (top of that loop) and at mode entry (0x14004B625 in FUN_14004B600, 0x14004E678 in FUN_14004E5C0). IDENTIFICATION as audio: FUN_14006BD40, in the same band, populates the very same +0x34268 handle table by formatting the path string at 0x1408E33A0 = "sound/se/%s/snd_%s/snd_%s" (backslashes in the image).',
  frame_path='FUN_140607D60 -> FUN_140608A00 -> FUN_140609D60 is UNCONDITIONAL, and FUN_140609D60 integrates the float ramp blk+0x33AAC (step blk+0x33AB0, ceiling blk+0x33AD4, clamped to 0) and calls FUN_14006B5D0(2, 0, (int)ramp & 0x7F, 0xFFFFFFFF) -- a BGM volume command, on every single tick. The other in-tick emitters are FUN_1406098B0, FUN_140609DF0, FUN_140609E30, FUN_140609E80 and the NaomiLib wrappers FUN_140845190/420/460/4B0/520.',
  consequence='This CLOSES netcode-eval UNKNOWN U-L7 with the opposite answer to the one the eval assumed. (1) A depth-N rollback re-runs the whole frame function N times between two drains and appends up to N times the commands. (2) Because the cap is a silent drop, the failure at large N is not only duplicated audio but LOST audio: speculative ticks can fill 64 slots and the correcting ticks commands are discarded. (3) game_state+0x770, the shipped render suppression, does NOT suppress it -- +0x770 is not in the tick read set at all (FRAME-READSET section 3.3) and FUN_140609D60 is called unconditionally -- so the shipped catch-up loop at 0x140039DFD..0x14003A4A1 already has this behaviour at its own depth. (4) The fix costs nothing and needs no library fork: the ring count at *(0x142EBC010)+0x34AAC is one dword; zero it at the start of a catch-up burst, or carry it in the save set, so only the last confirmed tick commands survive to the drain.',
  correction='This CORRECTS the label on finding:runner_host_heap_objects, which calls *(DAT_142ebc010) "the debug ring". The whole-frame p-code trace DID observe the write; only the classification was wrong. That is a labelling miss, not a measurement miss.',
  caveat='CONFIRMED that the frame writes a host command ring outside blk, ctx and DC-RAM, and that the drain is outside the tick: those are direct reads of FUN_14006B5D0 / FUN_14006B690 / FUN_140039DE0. INFERRED-strong that the ring is AUDIO: the 1/127 volume normaliser, the 128 SE bank handle table, the play/stop/volume/pan opcode shape and the sound/se path format in the same band. The remaining confirmation is one breakpoint on FUN_14006B5D0 during a hit, reading the four record dwords -- human-attached debugger, not done.';

-- ---------------------------------------------------------------------------
-- (e) scoping the "8 bytes short => player desyncs" inference
-- ---------------------------------------------------------------------------

UPSERT finding:blkonly_insufficiency_is_not_a_desync_attribution SET status='confirmed', confidence='medium', date='2026-09-06',
  contribution_candidate=true,
  title='SCOPING: "the shipped save set is 8 bytes short" is settled; "and that is why players desync" is NOT supported by anything in the graph',
  statement='Three separate claims are being carried as one. (1) The shipped GGPO save/load set is exactly one region {blk, 0x33B18}, registered at 0x1401182EB..0x140118302 in FUN_140118290 -- CONFIRMED and already recorded as finding:ggpo_saves_exactly_blk. (2) blk-only resimulation is not byte-exact from depth 2 on the stage-9 receipt, and blk plus ctx+0x1F8230..0x1F8238 is -- CONFIRMED and already recorded as finding:rollback_min_state_set, and SCOPED by finding:rollback_min_state_set_is_stage_dependent (Carnival passes blk-only at every depth). (3) That this is a plausible cause of the desyncs players report -- NOT SUPPORTED. The single diverging byte range is blk+0x1D6FC = object-pool node 144 field +0x124, which GATE-N1 section 1.1 calls a sprite-walker placement field; the eight ctx bytes sit in the NaomiLib projection block written by FUN_140846A40 (MOVUPS [RAX+0x1F8230],XMM1 at 0x140846BF7) and RECEIPT-RUNNER-RE.md line 141 classifies ctx+0x1F8230..0x1F8268 as render; finding:rollback_vs_replay_sufficiency states in the graph already that the only effect of a wrong ctx matrix entry state is 49 render-output bytes, the walker screen x and y. A render-output difference is a desync only if it feeds back into gameplay, and GATE-N1 section 6 records the per-frame READER of those eight bytes as UNKNOWN.',
  consequence='Keep shipping the wider save set -- the engineering conclusion is unchanged and well supported. Drop the causal story. Adding it to a design document converts a CONFIRMED byte-exactness result into an UNFALSIFIABLE claim about a population we cannot observe, and finding:shipped_ggpo_save_has_no_checksum shows the shipped build cannot even detect the event being attributed.',
  next='The one cheap experiment that would settle it: take the failing stage-9 event, restore blk-only, run the resim FORWARD past the event with the wrong node 144 +0x124 in place, and check whether any field the sim reads (not the walker rewrites) ever differs. If nothing but render output ever differs, the mechanism is a one-frame sprite wobble and the desync attribution is dead.';

-- ---------------------------------------------------------------------------
-- (f) the aux-bank rollback hazard: what is already settled
-- ---------------------------------------------------------------------------

UPSERT finding:bank_loader_reachable_from_frame_function SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='FUN_14060C370 is reachable from INSIDE the frame function, so a rollback window that spans a match-flow transition can replay a bank load whose target is in no save set',
  statement='FUN_14060C370 has 16 call sites and they include caseD_0 (0x14061D96C), caseD_2 (0x14061139F), caseD_4 (0x140610EDA), caseD_5 (0x14060ED23/2B/40) and caseD_6 (0x140611898) -- labels inside the G-machine dispatch bodies that the frame function runs, plus FUN_14060E250 (0x14060E388) and FUN_140615020 (0x1406150AC). So bank placement is not a load-time-only activity: it happens inside a tick, on a match-flow transition. What it writes lies OUTSIDE every rollback save set: the bank bytes themselves in DC-RAM, and the 17 bank-pointer globals 0x142EDF588..0x142EDF620 that finding:load_path_static_global_set records FUN_14060C370 and FUN_14060B230 as writing.',
  what_is_already_settled='The case table is 25 dword RVAs at 0x14060D010 indexed ECX-1 (finding:c370_switch_resolved). Case 10 is the results bank and its rule is finding:results_bank_loader_rule, including the reason it must be placed demand-driven and the fact that G+0xAD is only meaningful when case 10 last ran. Six further aux banks are enumerated and unplaced in finding:arc_gap_aux_banks_open, whose stated next step -- trace the shared switch tail for each (POL, TEX) destination pair -- is pure Ghidra and needs no capture. finding:runner_rollback_dcram_not_needed proved DC-RAM does not need rolling back, but it was measured on a 200-tick mid-round stage-9 receipt in which no bank load occurs, so it does not cover this case: rule M4, a gate that could not have exercised the mechanism says nothing about it.',
  gate='The gate that would settle it, stated so it can fail: an anchor plus input tape whose 200-tick window CONTAINS a KO / round transition / results entry; run the gate_n1 ladder over it at depths 0/2/8/30/60 with save set rr; byte-compare blk, blk2, the game_state page, the exe page, the 32 MB DC-RAM image AND the 17 bank-pointer globals 0x142EDF588..0x142EDF620, which the current gate does not compare. Control arm at depth 0 must PASS, proving the transition itself is deterministic; a deliberately narrowed arm that excludes the bank-pointer globals must FAIL if the mechanism is real. Copy-then-rebase (FUN_14060DCF0 then FUN_14060D770) is idempotent as a pair, so the hazard is NOT double-rebase; it is that a resimulation taking a different branch may not re-run the load, leaving DC-RAM and the 17 globals holding a bank the rewound simulation does not expect.',
  blocked_on='A capture: no existing receipt spans a match-flow transition. Everything above is derivable and derived without one.';

-- ---------------------------------------------------------------------------
-- (g) the heap-registry accessor question, answered from the graph
-- ---------------------------------------------------------------------------

UPSERT finding:heap_registry_accessor_not_on_frame_path SET status='confirmed', confidence='high', date='2026-09-06',
  title='FUN_14011EA90 is a one-line heap-registry accessor and no frame path reaches it; the graph already said so',
  statement='FUN_14011EA90(descriptor) is exactly "return ((void**)0x142E12CD0)[ *(uint*)(descriptor + 0x30) >> 23 & 0x3F ]". The index is a field of the class descriptor passed in, not a fixed address; 0x142EBCE68 quoted elsewhere is simply 0x142EBCE38 + 0x30 for the one descriptor whose global is 0x142EBCE38. finding:load_path_static_global_set already records 0x142E12CD0 as read by FUN_14011EA90 and as one of exactly five pointer globals a BATTLE TICK never touches, so the answer to "does any frame path reach it" was in the graph before the question was asked. Of the 131 call sites, the allocate(0xA8, 0x10, tag) shape occurs at exactly four: FUN_14026FA70 (0x14026FA89, descriptor 0x142EBCE38), FUN_14036B5E0 (0x14036B5F9, 0x142EBEF90), FUN_14046D4F0 (0x14046D507, 0x142EC1480) and FUN_140489E50 (0x140489E69, 0x142EC21B0) -- four 168-byte class factories, all in the render/host bands, and FUN_14026FA70 has no code xref at all, only a data reference from 0x14092C840.',
  consequence='The instability of registry slot 10 across sessions is a host-heap fact, not a determinism fact, and finding:default_allocator_is_in_image already gives the fix: repoint every non-carryable slot at the in-image Default heap 0x142E12EF0. Nothing on the frame path observes it.';

-- ---------------------------------------------------------------------------
-- edges
-- ---------------------------------------------------------------------------

RELATE finding:pad_raw_to_sim_table_full->cites->source:xexam_20260906_ghidra SET id='e187_pad_src';
RELATE finding:pad_raw_to_sim_table_full->cites->finding:input_seat_route_confirmed SET id='e187_pad_seat';
RELATE finding:pad_word_upper_bits_gated_by_gs828->cites->source:xexam_20260906_ghidra SET id='e187_upper_src';
RELATE finding:pad_word_upper_bits_gated_by_gs828->cites->finding:pad_raw_to_sim_table_full SET id='e187_upper_pad';
RELATE finding:fighter_input_shift_register->cites->source:xexam_20260906_ghidra SET id='e187_shift_src';
RELATE finding:fighter_input_shift_register->cites->finding:mvc2_zero_internal_input_latency SET id='e187_shift_lat';
RELATE finding:input_record_fanout_six_fighters->cites->source:xexam_20260906_ghidra SET id='e187_fanout_src';
RELATE finding:input_record_fanout_six_fighters->cites->finding:fighter_input_shift_register SET id='e187_fanout_shift';
RELATE finding:shipped_ggpo_save_has_no_checksum->cites->source:xexam_20260906_ghidra SET id='e187_cksum_src';
RELATE finding:shipped_ggpo_save_has_no_checksum->cites->finding:ggpo_advance_frame_callback SET id='e187_cksum_cb';
RELATE finding:shipped_ggpo_save_has_no_checksum->cites->finding:ggpo_save_cost_correction SET id='e187_cksum_cost';
RELATE finding:frame_writes_audio_command_ring_outside_blk->cites->source:xexam_20260906_ghidra SET id='e187_ring_src';
RELATE finding:frame_writes_audio_command_ring_outside_blk->cites->finding:runner_host_heap_objects SET id='e187_ring_hostheap';
RELATE finding:frame_writes_audio_command_ring_outside_blk->cites->finding:frame_readset SET id='e187_ring_readset';
RELATE finding:blkonly_insufficiency_is_not_a_desync_attribution->cites->finding:ggpo_saves_exactly_blk SET id='e187_scope_ggpo';
RELATE finding:blkonly_insufficiency_is_not_a_desync_attribution->cites->finding:rollback_min_state_set SET id='e187_scope_min';
RELATE finding:blkonly_insufficiency_is_not_a_desync_attribution->cites->finding:rollback_min_state_set_is_stage_dependent SET id='e187_scope_stage';
RELATE finding:blkonly_insufficiency_is_not_a_desync_attribution->cites->finding:rollback_vs_replay_sufficiency SET id='e187_scope_replay';
RELATE finding:blkonly_insufficiency_is_not_a_desync_attribution->cites->finding:shipped_ggpo_save_has_no_checksum SET id='e187_scope_cksum';
RELATE finding:bank_loader_reachable_from_frame_function->cites->source:xexam_20260906_ghidra SET id='e187_bank_src';
RELATE finding:bank_loader_reachable_from_frame_function->cites->finding:c370_switch_resolved SET id='e187_bank_switch';
RELATE finding:bank_loader_reachable_from_frame_function->cites->finding:results_bank_loader_rule SET id='e187_bank_results';
RELATE finding:bank_loader_reachable_from_frame_function->cites->finding:arc_gap_aux_banks_open SET id='e187_bank_open';
RELATE finding:bank_loader_reachable_from_frame_function->cites->finding:runner_rollback_dcram_not_needed SET id='e187_bank_dcram';
RELATE finding:heap_registry_accessor_not_on_frame_path->cites->source:xexam_20260906_ghidra SET id='e187_heap_src';
RELATE finding:heap_registry_accessor_not_on_frame_path->cites->finding:load_path_static_global_set SET id='e187_heap_globals';
RELATE finding:heap_registry_accessor_not_on_frame_path->cites->finding:default_allocator_is_in_image SET id='e187_heap_default';

-- statuses last, so a replay converges rather than demoting
UPDATE finding:pad_raw_to_sim_table_full SET status='confirmed';
UPDATE finding:pad_word_upper_bits_gated_by_gs828 SET status='confirmed';
UPDATE finding:fighter_input_shift_register SET status='confirmed';
UPDATE finding:input_record_fanout_six_fighters SET status='confirmed';
UPDATE finding:shipped_ggpo_save_has_no_checksum SET status='confirmed';
UPDATE finding:frame_writes_audio_command_ring_outside_blk SET status='confirmed';
UPDATE finding:blkonly_insufficiency_is_not_a_desync_attribution SET status='confirmed';
UPDATE finding:bank_loader_reachable_from_frame_function SET status='confirmed';
UPDATE finding:heap_registry_accessor_not_on_frame_path SET status='confirmed';
