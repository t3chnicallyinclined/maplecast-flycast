-- 204_collection_corrections.surql -- CORRECTIONS to seed 178, applied after seed 200 measured the
-- boot closure and the ROM layout. Seed 200 already attached its corrections as caveats, but two
-- findings still carry the superseded claim in the part a reader actually scans: one in the TITLE and
-- one in a caveat that says the opposite of what was later measured. A knowledge graph whose titles
-- disagree with its caveats is worse than one with a gap, so both are rewritten here.
-- Nothing new is claimed. Every fact below is already CONFIRMED by seed 200; this seed only moves it
-- to where it will be read. 2026-09-07.
-- Forward slashes only in ref/note; no apostrophes.
USE NS re DB kb;

-- ---------------------------------------------------------------------------------------------------
-- CORRECTION 1 -- the shipped arc states are slots 1 and 2, NOT slot 0
-- ---------------------------------------------------------------------------------------------------
-- Seed 178 juxtaposed two separately-true claims in one title and made them read as one false one:
-- the arcs ship two states, AND the boot restores slot 0. Both are true, but the states Capcom ships
-- are NOT the ones slot 0 receives. FUN_140112d90 keys the slot by the TRAILING DIGIT of the
-- romset-dash-digit name (format string 0x1408dd5a0), so state/ROMSET-1 and state/ROMSET-2 land in
-- slots 1 and 2. Slot 0 is the RUNTIME anchor, filled by FUN_14003fa60 out of the host-device session
-- record before the machine starts. That distinction is the whole product point: slot 0 is the seat
-- netplay overwrites so two peers begin byte-identical.
UPDATE finding:collection_ships_boot_anchors_for_cps SET
  title='Capcom SHIPS two jacksave anchors inside every CPS arc as SLOTS 1 AND 2, while the boot restores SLOT 0, which is the runtime anchor filled from the host-device session record',
  caveat='CORRECTED 2026-09-07 by seed 200, restated here because seed 178 put the superseded claim in the title. (a) The arc entries state/ROMSET-1 and state/ROMSET-2 are SLOTS 1 AND 2 -- FUN_140112d90 keys the slot by the trailing digit of the name, format string at 0x1408dd5a0. Slot 0 is a separate, runtime anchor that FUN_14003fa60 fills from the host-device session record before machine_start, and it is the slot netplay overwrites. (b) FUN_14004e5c0 param_1 is the host device *(0x140ACD3A8) and the boot-state record is at *(hostdev+0xd04b8)+0x5a2f0, reached as FUN_14003fa60(hostdev+0xd04b0, game, rev, slotbuf, out), NOT *(hostdev+8)+0x5A2F0. (c) A slot whose valid-size u32 at slot+0x100004 is zero makes FUN_140111d10 a SILENT NO-OP, so an empty slot 0 lets the machine run from the reset vector instead of failing. (d) A shipped jacksave can NEVER substitute for the ROM: 0.83 MB of state against 46-50 MB of regions, and CPS2 region 12 exists in neither the blob nor the state -- see finding:cps_savestate_cannot_substitute_for_rom.';

-- ---------------------------------------------------------------------------------------------------
-- CORRECTION 2 -- machine start WAS measured, and the ROM path is not an import at all
-- ---------------------------------------------------------------------------------------------------
-- Seed 178 carried the honest caveat that machine start had not been measured and that the ROM blob
-- arrives through host file I/O. Seed 200 measured it: the machine-start closure reaches EXACTLY the
-- same 29 IAT slots, empty difference in both directions, and the arc read is a VTABLE call on the
-- resource-manager pointer global 0x142ebc118 -- not an import, and not reached at all by a shell that
-- places the regions itself. Leaving the old caveat in place would keep telling readers the boundary
-- grows, which is the single most decision-relevant fact in this whole subgraph and is wrong.
UPDATE finding:cps_frame_import_surface_equals_mvc2 SET
  caveat='SUPERSEDED 2026-09-07 by seed 200. The old caveat said machine START was unmeasured and that the ROM blob arrives through host file I/O. Both are now wrong. Machine start WAS measured: closure from FUN_140112200 and FUN_1401135e0, plus every body reached indirectly through the driver table, gives 2707 blocks / 787 entries / the SAME 29 IAT slots, with an EMPTY difference in both directions -- see finding:cps_machine_start_import_closure. The whole boot path including the ROM loader adds exactly ONE slot, 0x1408db398 = KERNEL32 DebugBreak, at a single refcount assert at 0x1402093ef that never fires on a valid object. And the arc read is NOT an import: it is a vtable call at offset +0x60 on the resource-manager pointer global 0x142ebc118, structurally the same kind of host object as 0x142EBD8F0, and a shell that places the ROM regions itself never calls it. ⚠ Also note the earlier closure tool UNDER-COVERED: it drove Ghidra disassemble_function, and Ghidra has no function defined at the six CPS driver inits 0x1400fb710 / 0x1400fc080 / 0x1400fc920 / 0x1400fd170 / 0x1400fd9a0 / 0x1400f9630, which are reachable only through driver_desc+0x20, so those bodies silently contributed nothing. The replacement tool does recursive descent over the raw image with no Ghidra dependency and was validated by reproducing the MvC2 29-slot set exactly.';

-- ---------------------------------------------------------------------------------------------------
-- The one trap that must never be lost, promoted to its own finding
-- ---------------------------------------------------------------------------------------------------
UPSERT finding:cps_frame_gate_config_0x780 SET
  title='config+0x780 GATES the CPS frame: leave it non-zero and the tick returns cleanly having done nothing, which reads exactly like a crash-free success',
  statement='FUN_140113dd0 returns *(int*)(config+0x780) and thunk_FUN_140112610 runs the CPS frame body ONLY when it is zero. Fifteen sites in the image set it non-zero. Two further one-shot guards have the same character: config+0x1ec must be 0 before machine_start or FUN_1400f7f10 skips the region-12 opcode copy and the 68000 then fetches from a zero-filled region, and config+0x280 is set to 0xFFFFFFFF by FUN_14004b600 immediately before FUN_1401135e0.',
  consequence='This is a FALSE-WIN TRAP of exactly the shape the project has been burned by before. A shell that wires up a CPS title, forgets config+0x780, and reports that it ticked N frames without faulting has measured nothing at all. Any CPS bring-up must gate on OUTPUT changing -- the framebuffer at *(config+0x1a8), or a state diff -- never on the absence of a crash. The available gate is cheap and already on disk: restore a shipped state, tick N, save through FUN_140111e20, and compare byte-for-byte against the shipped game doing the same restore and tick.',
  status='confirmed', confidence='high', date='2026-09-07', contribution_candidate=true;

RELATE finding:cps_frame_gate_config_0x780->cites->source:cps_boot_closure_20260907 SET id='e204_c1', note='read at FUN_140113dd0 and thunk_FUN_140112610';
RELATE finding:cps_frame_gate_config_0x780->refines->finding:cps_ready_to_tick_write_set SET id='e204_r1', note='the gate the write set must not omit';
RELATE finding:collection_ships_boot_anchors_for_cps->cites->source:cps_boot_closure_20260907 SET id='e204_c2', note='slot keying corrected here';
RELATE finding:cps_frame_import_surface_equals_mvc2->cites->source:cps_boot_closure_20260907 SET id='e204_c3', note='machine start measured, caveat superseded';
