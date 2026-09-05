-- 164_render_coverage_layer_assignment.surql -- LAYER ASSIGNMENT for the 74.6% draw-coverage gap
-- measured on the 2026-09-05 real-game reference capture. 2026-09-05, senior-re-generalist.
-- Method: docs/RE-METHOD.md. Attribution is by BYTE-EXACT CBWorld identity (48-B VS CB vs the
-- row-major 3x4 transpose of node+0xA8), independent of the harvest/emitter under test.
-- Idempotent and replayable; explicit edge ids.
USE NS re DB kb;

UPSERT source:cap_rrcap_4219_4518 SET kind='reproduction', strength='reproduction',
  ref='%TEMP%/rrcap frames 4219..4518 (300 consecutive), stage 9 triple super, d3dcap shim',
  note='Per frame: frame_<n>.ndjson (1297 draws on 4219), cb_<n>_<hash>.bin (VS/PS constant buffers keyed by CONTENT hash), blk_<n>_full/delta.bin, alist_<n>.bin. ROM-derived, TEMP only, never in a repo (BYOR).';

UPSERT source:tool_cbworld_attribution SET kind='measurement', strength='reproduction',
  ref='scratchpad attr2.py / sweep2.py / gate0c.py over %TEMP%/rrcap (blkstate.load_frame + blkstate.anodes + blkstate.cbworld)',
  note='For every Draw, take the first 48-byte VS constant buffer bound and test it for BYTE equality against blkstate.cbworld(node+0xA8) over all 16 System-A lists. Deliberately independent of the agent harvest and of the rr-render emitter, so it cross-checks their coverage number rather than restating it.';

-- ---------------------------------------------------------------------------
-- THE MEASUREMENT
-- ---------------------------------------------------------------------------

UPSERT finding:render_coverage_is_snapshot_phase SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The 74.6 percent draw-coverage gap is a blk SNAPSHOT-PHASE error, not a missing harvest structure',
  statement='Over 300 consecutive real frames (1,690,052 vertices), attributing every draw to a System-A node by byte-exact CBWorld identity gives 75.29 percent against the blk snapshot LABELLED THE SAME FRAME and 97.23 percent against the snapshot labelled N+1. Same draws, same nodes, one-frame shift. The independently measured 74.62 percent coverage of the harvest+emitter lane reproduces to 0.7 pp, so the two are measuring the same phenomenon.',
  worked_example='Frame 4360, prop node blk+0x9D58 (list 5): the bound CB C787BA7C is row0 [0.57581 0 0.81758 -1007]; node+0xA8 in blk_4360 is [0.59570 0 0.80321 -1007]; node+0xA8 in blk_4361 is BYTE-IDENTICAL to the bound CB. The prop spins 1.406 deg/frame about Y at a FIXED integer translation (-1007,1020,-2235), so the phase is unambiguous.',
  falsifier='If the gap were a missing structure, shifting which snapshot the draws are compared against could not move coverage. It moved it 22 points.',
  caveat='CBWorld identity is NECESSARY, not SUFFICIENT, for reconstructability: the tape also needs the polygon-list OBJECT bytes. 97.23 percent is therefore an UPPER BOUND on what fixing the phase alone buys.';

UPSERT finding:render_dispatcher_pass_order SET status='confirmed', confidence='high', date='2026-09-05',
  title='FUN_140620960 pass order read from the binary -- the sprite-walker hook fires at call 4 of 34',
  statement='Disassembled from mvc_dump.bin (file offset = VA - 0x140000000), body 0x140620960..0x140620ACC, straight-line, no loop in the call spine. FUN_140619960 returns a mode. mode==0 (in match) -> 0x1406209D9 sprite walker FUN_140620F10, blackout test [G+0x98], stage-deck direct draw (0x140847950 / 0x140847CA0 / 0x140849B00 with blk+0x6CA8..0x6CB0 / 0x140849C10 / 0x1408478C0), then lists cl=5,6,7,8 through FUN_140620740 (models) + FUN_140620CD0 (quads), then FUN_140620EA0. mode==1 -> 0x140620983 list 9 models then list 8 quads then FUN_140620EA0. mode>=2 -> 0x140620996 sprite walker then lists 7,8,9, and the parts pass is SKIPPED. FUN_140620EA0 = FUN_14061D5B0 (HUD scene block) + list 0xB models/quads, then if *(G+0x14) != 0x40 walk blk+0x2EE48 (== ALIST_HEADS + 12*8, next +0x10, gate +0x170) calling FUN_140653A70 per node.',
  consequence='The d3dcap shim hooks FUN_140620F10 (dllmain.cpp RR_WALK_FN 0x140620F10) and dumps blk in hkWalk AFTER oWalk() returns -- i.e. at 0x140620996 / 0x1406209D9, before EVERY System-A pass of that frame. blk_N therefore holds the PREVIOUS frame System-A matrices by construction. The lag is a property of WHERE THE SNAPSHOT IS TAKEN, not of the game and not of the harvest walk.',
  note='The dispatcher never walks lists 0xA / 0xC / 0xD through FUN_140620740 / FUN_140620CD0. List 0xB and list 0xC are reached only through FUN_140620EA0. A harvest that walks 5..=13 uniformly is walking four lists the dispatcher does not use on this path and is correct only incidentally.';

UPSERT finding:harvest_residual_is_list0c SET status='confirmed', confidence='high', date='2026-09-05',
  title='After the phase shift the entire remaining structural gap is the list-0xC parts (2.02 percent of vertices)',
  statement='Residual after matching against N or N+1: 39,590 of 1,690,052 vertices (2.34 percent). Of that, 33,985 vertices / 5,866 draws (2.02 percent of the frame) bind a 64x64 page with a composed translate-only CBWorld that appears NOWHERE in blk in any form. Gate: every one of those 5,866 draws has a drawn list-0xC node on frame N or N+1 whose +0x50 position matches the CB (y,z) exactly -- 5866/5866, 0 untraced. Worked example: pnode blk+0x11558 pos=(-40,5,-40) on frame 4360 -> first part CB T=(-39.0,5,-40); pos=(-77,5,-40) on frame 4312 -> T=(-76.0,5,-40).',
  mechanism='FUN_140653A70 composes W = T(node+0x50) and STORES it to node+0xA8, overwriting it per part, so a single blk snapshot can never hold more than the last part of the last node. This is why the class is unreachable from a snapshot in principle and needs the 44-B pnodes record (docs/PARTS-LIST0C-GHIDRA.md).',
  why_harvest_misses_it='harvest_anodes (RetroReceipts-agent agent/src/harvest.rs:478) keeps a node only when node+0xA0 (obj) or node+0xE8 (model) is non-zero. List-0xC nodes have both zero. Dropped by construction -- not a walk-range problem.',
  remainder='About 3,750 vertices (0.22 percent) untraced: ~2,400 = the host post chain (CB is viewport params 1.0,-1.0,0.5,-0.5,1024,768, not a world matrix -- correctly not emitted), ~1,716 = frame 4518 with no blk_4519 to shift to (boundary artefact), ~1,258 = a vs=...621D2AF8 family of degenerate / squashed 2D matrices at about 4 vertices per frame -- UNKNOWN, not yet identified.';

UPSERT finding:anodes_cap_96_not_the_gap SET status='confirmed', confidence='high', date='2026-09-05',
  title='FALSIFIED: ANODES_CAP_PER_FRAME=96 is not the coverage gap on this capture',
  statement='harvest_anodes caps at 96 nodes per frame across lists 5..=13 walked in ascending order, which would starve the HIGH-numbered lists (0xB HUD, 0xC parts) first -- the same shape as the 2026-09-03 AOBJ_MAX_BYTES / AOBJ_MAX_RECS prop-truncation bug. Measured from alist_<n>.bin (the shim in-process dump, same predicate, no 96 cap): 50-62 drawn nodes per frame (4219: lists 5=7, 6=2, 11=41). The cap never binds. Hypothesis dead.',
  why_recorded='Recorded because it is the most plausible-looking wrong answer here and the team has been burned by exactly this failure once already; the next reader should not re-derive it.';

UPSERT finding:agent_anode_phase_nondeterministic SET status='open', confidence='medium', date='2026-09-05',
  title='INFERRED / OPEN: the live agent tape has the same phase hazard, and non-deterministically',
  statement='The agent is an out-of-process poller (agent/src/reader.rs about 1560-1605): clock-edge detect, then a torn-list retry loop (1 ms steps, up to 6) and a 0.5 ms stability loop (up to 4 extra reads) before harvest_anodes runs. Nothing synchronises it with FUN_140620960, so whether node+0xA8 has been written by this frame passes is decided by wall-clock timing -- 0 or 1 frame stale, varying frame to frame. This is the MATRIX analogue of the already-recorded Gate-2 finding that animated-prop VERTICES are stale in every live tape.',
  status_note='NOT PROVEN. The 97.23 percent number above was measured on the SHIM snapshot, whose phase is fixed by the hook; it says nothing directly about the agent.',
  falsification_test='Run the agent against the same session as a d3dcap capture (both key on blk+0x3CC8) and, per frame, test whether the tape anode matrix equals the shim blk_N or blk_N+1 matrix. A CONSTANT answer across frames falsifies non-determinism; a mixture confirms it. A deterministic gate for the fix must freeze the input -- the receipt runner replaying a fixed anchor -- because a live A/B here is exactly the confounded comparison that has produced false wins on this project before.';

-- ---------------------------------------------------------------------------
-- EDGES
-- ---------------------------------------------------------------------------

RELATE finding:render_coverage_is_snapshot_phase -> cites -> source:cap_rrcap_4219_4518 SET id='cov_phase_cap';
RELATE finding:render_coverage_is_snapshot_phase -> cites -> source:tool_cbworld_attribution SET id='cov_phase_tool';
RELATE finding:harvest_residual_is_list0c -> cites -> source:cap_rrcap_4219_4518 SET id='resid0c_cap';
RELATE finding:anodes_cap_96_not_the_gap -> cites -> source:cap_rrcap_4219_4518 SET id='cap96_cap';
RELATE finding:render_dispatcher_pass_order -> about -> steam_routine:FUN_140620960 SET id='dispatch_order_960';
RELATE finding:render_dispatcher_pass_order -> about -> steam_routine:FUN_140620ea0 SET id='dispatch_order_ea0';
RELATE finding:render_coverage_is_snapshot_phase -> about -> steam_routine:FUN_140620f10 SET id='cov_phase_f10';
RELATE finding:harvest_residual_is_list0c -> about -> steam_routine:FUN_140653a70 SET id='resid0c_653a70';

RELATE finding:render_dispatcher_pass_order -> cites -> source:bin_mvc_dump SET id='dispatch_order_bin';
RELATE finding:render_coverage_is_snapshot_phase -> cites -> source:bin_mvc_dump SET id='cov_phase_bin';
RELATE finding:harvest_residual_is_list0c -> cites -> source:bin_mvc_dump SET id='resid0c_bin';
UPSERT finding:render_dispatcher_pass_order SET status='confirmed';
