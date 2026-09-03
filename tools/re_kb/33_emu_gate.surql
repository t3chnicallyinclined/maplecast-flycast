-- 33: EMU-GATE -- Steam functions emulated (Ghidra p-code) on captured frame state and gated bit-for-bit (2026-09-02).
-- Source doc: mvc-live-skins-quarters/docs/EMU-GATE.md. Harness: d3dcap/replay/emu_gate.py + re_map/ghidra_emu/EmuGate.java.
-- Apply with tools/re_kb/apply_seed.py (per statement) after the documented export backup. Idempotent: UPSERT for nodes,
-- RELATE with explicit edge ids for new edges. No apostrophes inside string literals.
USE NS re DB kb;

UPSERT source:ghidra_pcode_emulation SET kind='ghidra', strength='code', ref='mvc-live-skins-quarters/d3dcap/replay/emu_gate.py', note='Ghidra 12.1.2 p-code emulation (EmulatorHelper, headless, scratch project emuproj = raw import of mvc_dump.bin at 0x140000000) of Steam MvC2 functions with the memory of a captured frame (blkstate.load_frame block at its live base, DAT_142edf560/580 pointed at it, NaomiLib ctx initialised by running FUN_140846a40(blk,0x40), near = 1.0, UCRT SSE2 path). Outputs compared bit-for-bit with the same frame capture. Every uninitialised read is logged, so the synthesised memory set is explicit. Results in docs/EMU-GATE.md.';

-- routines whose behaviour the gate established or that the harness depends on
UPSERT steam_routine:FUN_140846a40 SET role='NaomiLib matrix stack init', note='FUN_140846a40(storage, depth): ctx+0x1f81b0 = storage, +0x1f81b8 = +0x1f81bc = depth, modes 0, all four slots and both scratch matrices = identity. CONFIRMED call site 0x14060b628 in FUN_14060b550: FUN_140846a40(DAT_142edf560, 0x40) -- the push/pop storage is the FIRST 0x1000 BYTES OF blk (0x40 slots x 64 B), so a rollback block carries the matrix stack. Emulated as the prologue of every EMU-GATE job; the push slots it produces equal the captured bytes.';
UPSERT steam_routine:FUN_140847950 SET role='matrix push (+ optional load)', note='if ctx+0x1f81bc < 1: return 0 WITHOUT loading the argument; else copy *cur to *(ctx+0x1f81b0), advance by 0x40, depth--, then if arg: *cur = *arg. CONFIRMED (decompile + gate): with a zeroed ctx every point projection (FUN_140848120) returns (0,0) through its w == 0 branch because the composite is never loaded.';
UPSERT steam_routine:FUN_1408478c0 SET role='matrix pop n', note='while n: if depth < max: storage -= 0x40, *cur = *storage, depth++. Balanced with FUN_140847950. CONFIRMED.';
UPSERT steam_routine:FUN_140613390 SET role='LayerZ (blk+0x6d08..0x6d44) init table', note='writes blk+0x6d08.. = DAT_140a6d888[0..7] = 15,17,19,21,23,25,27,29 and 0x6d28.. = 10,11,12,13,30,31,32,33 (immediates). Only caller FUN_14060a1f0 (init). The PER-FRAME resetter of the table is UNKNOWN (no other function carries these displacements); EMU-GATE restores the table from these constants before the walk and the post-walk values then reproduce exactly.';
UPSERT steam_routine:FUN_14060fb90 SET role='world point -> screen for game logic (INFERRED pre-walker composite writer)', note='FUN_14060fb90(point, out): FUN_14061d7e0 (current camera) then, unless G+0x4c == 5, FUN_140847d10 (composite V.P.Screen into ctx+0x1f8200) and FUN_140848120. One of four writers of ctx+0x1f8200 (with FUN_140613cd0 debug, FUN_140620f10, FUN_14062fdc0). EMU-GATE shows the composite holds the CURRENT frame camera when the walker starts, and FUN_140620960 does not compose before the walk -- so this (or FUN_14062fdc0) runs earlier in the frame. INFERRED owner; falsify by logging FUN_140847d10 callers live.';
UPSERT steam_routine:FUN_140620f10 SET emu_gate='2026-09-02: bit-exact on frames 4445/4505/5168/7279 (62 nodes, all ten output fields, 0 block bytes differ); cell-override nodes need the DC-RAM host image (docs/EMU-GATE.md s5)';
UPSERT steam_routine:FUN_14061d7e0 SET emu_gate='2026-09-02: P and V 16/16 bit-identical to scene CB 7793F141 of frame_4445.pack (docs/EMU-GATE.md s3)';
UPSERT steam_routine:FUN_14061d6a0 SET emu_gate='2026-09-02: P and V 16/16 bit-identical to scene CB AD5C2A43 of frame_4445.pack (docs/EMU-GATE.md s3)';
UPSERT steam_routine:FUN_14061d5b0 SET emu_gate='2026-09-02: P and V 16/16 bit-identical to scene CB 04E19F4C of frame_4445.pack (docs/EMU-GATE.md s3)';

-- globals
UPSERT global:nl_matrix_push_storage SET addr='blk+0x0', steam_off='0x0', type='f32[0x40][16]', note='NaomiLib matrix push/pop storage = the first 0x1000 bytes of blk (FUN_140846a40(DAT_142edf560, 0x40) at 0x14060b628). ctx+0x1f81b0 walks through it; ctx+0x1f81b8/+0x1f81bc = max/remaining depth. Rollback therefore restores the matrix stack too. CONFIRMED by call site and by EMU-GATE (emulated pushes reproduce the captured slot bytes).';
UPSERT global:nl_composite_vps SET addr='ctx+0x1f8200', steam_off='ctx+0x1f8200', type='f32[16]', note='V x P x Screen composite written by FUN_140847d10, read by the point projectors FUN_140848120 / FUN_1408480a0. At sprite-walker entry it already holds the CURRENT frame camera (EMU-GATE 4.2: current = 47/47 sx/sy exact, previous-frame camera = 0.164 px off on 43 nodes, identity = wrong). Writers: FUN_14060fb90, FUN_140613cd0 (debug), FUN_140620f10 (per cat-0 node), FUN_14062fdc0.';
UPSERT global:crt_fma3_flag SET addr='0x142eefbd8', steam_off='DAT_142eefbd8', type='u32', note='UCRT __use_fma3 dispatch flag; 1 in the dump. The FMA/AVX branches of tanf/atanf/sinf/cosf use vfmadd213sd, which is an unimplemented CALLOTHER in Ghidra x86 p-code; EMU-GATE sets it to 0 so the SSE2 branch of the same CRT runs (bit-identical results; Java Math hooks also bit-identical).';
UPSERT global:layerz_table SET addr='0x8C26A974', steam_off='0x6D08', type='f32[16]', note='per-layer depth bases (LayerZ). Init constants from FUN_140613390: 15,17,19,21,23,25,27,29,10,11,12,13,30,31,32,33. The sprite walker adds 0.001 x quad count per submitted node (FUN_140620f10 via FUN_1406129f0 return). Post-walk dumps hold base + accumulation; the per-frame reset is UNKNOWN.';
UPSERT global:anim_cell_table SET addr='fighter+0x1C0', steam_off='0x1C0', type='ptr', note='per-fighter pointer (blk+0x3F78 + slot*0x738) to the animation-cell table in the DC-RAM host image (host ptr, e.g. 0x12531000 on frames 5227/5232). 16-B cells: u16 scaleX, scaleY, angle, hotX, hotY, flip. Read by the sprite walker when node+0x191 != 0 (objects use the OWNER slot at node+0x230). NOT inside blk: the one thing the walker needs that no capture holds.';

-- findings (source: ghidra_pcode_emulation)
UPSERT finding:emu_gate_camera_bitexact SET status='confirmed', confidence='high', date='2026-09-02', contribution_candidate=true, note='Emulating FUN_14061d7e0 / FUN_14061d6a0 / FUN_14061d5b0 on the frame-4445 block reproduces the PROJ (ctx+0x1f816c) and VIEW (ctx+0x1f812c) matrices 16/16 floats bit-identical to the scene CBs the D3D capture holds (7793F141 rows 15-18 and 7-10; AD5C2A43; 04E19F4C), and their float32 product equals rows 0-3 bit-identical. Closed form (WORLD-CAMERA-GHIDRA.md s2) differs by 3.94e-7 (its float64 rounding). 1149 / 1156 / 637 instructions. Both the UCRT SSE2 path and Java-math hooks give the same bits.';
UPSERT finding:emu_gate_walker_bitexact SET status='confirmed', confidence='high', date='2026-09-02', contribution_candidate=true, note='Emulating the sprite walker FUN_140620f10 on a post-walk block (state_<f>.json at:walk) reproduces every drawn node field it writes -- +0x124/+0x128 screen, +0x12C depth, +0x130/+0x134 scale, +0x148 angle, +0x150, +0x154 facing, +0x178/+0x17A hotspot -- bit-exact on frames 4445 (47 nodes), 4505 (6), 5168 (5), 7279 (4), with the LayerZ table 16/16 and G+0x24 quad totals 133/118/112/96 equal, and ZERO of 211736 block bytes differing after the walk (the post-walk block is a fixed point of the walk). Synthesised: ctx via FUN_140846a40(blk,0x40) prologue, near 1.0, LayerZ restored from FUN_140613390 constants, G+0x24 = 0, FUN_1406129f0 stubbed with quad counts recovered from the dump depth chain, composite = current camera.';
UPSERT finding:emu_gate_walker_composite_is_current_frame SET status='confirmed', confidence='high', date='2026-09-02', contribution_candidate=true, note='Cat 1..4 sprite nodes walked before the first fighter are projected through ctx+0x1f8200 without calling the camera. Seeding it with the CURRENT frame camera (blk+0x6914..) gives 47/47 exact sx/sy on frame 4445; the PREVIOUS frame camera (blk+0x6920..) is 0.164 px off on all 43 such nodes; the identity is wrong. So a reconstruction must use the current camera for every sprite; the one-frame lag of blk+0x6920 never reaches sprite positions. FUN_140620960 does not compose before the walker: the writer is INFERRED to be FUN_14060fb90 (game-logic projection) or FUN_14062fdc0.';
UPSERT finding:emu_gate_matrix_stack_lives_in_blk SET status='confirmed', confidence='high', date='2026-09-02', contribution_candidate=true, note='The NaomiLib matrix push/pop storage is blk+0x0..0x1000: FUN_14060b550 calls FUN_140846a40(DAT_142edf560, 0x40) at 0x14060b628. The emulated walk (which pushes and pops around every fighter projection) leaves those bytes identical to the capture. Consequence: the first 4 KB of the rollback block are renderer scratch, not game state, and the ctx counters (ctx+0x1f81b0/b8/bc) are the only matrix-stack state outside the block.';
UPSERT finding:emu_gate_push_without_depth_is_noop SET status='confirmed', confidence='high', date='2026-09-02', note='FUN_140847950 returns 0 and does not even load its argument when ctx+0x1f81bc < 1. In an emulation with a zeroed ctx this made FUN_140848120 return (0,0) for every node (w == 0 branch) while all other walker fields were exact -- the failure mode to recognise before blaming the projection.';
UPSERT finding:emu_gate_cell_override_needs_dc_ram SET status='open', confidence='high', date='2026-09-02', note='On frames 5227/5232 the walker read 12 bytes at 0x12531020 and 0x12531050 = *(blk+0x3F78 + 0*0x738) + cell*16, the owner-slot animation-cell table in the DC-RAM host image (ram = *(*(0x142ef0ab0)+8)), which no capture holds. Zero-filled, exactly the 3/5 nodes with node+0x191 != 0 got scale 0 (expected 1.6667/2.1429), angle 0 (expected 0x1400 = 5120) and hotspot 0 (expected 65480/65412 on the cat-3 node); every other field and node stayed bit-exact. Smallest missing piece: the 16-B cell per drawn override node (or the owner table), dumped capture-side.';
UPSERT finding:emu_gate_submit_stubbed SET status='open', confidence='high', date='2026-09-02', note='FUN_1406129f0 (sprite submit) was NOT emulated: it walks GFX1/GFX2/template pointers (+0x1A8/+0x1B0/+0x1F0) into DC RAM. Its return (quad count) was recovered from the dump depth chain (depth_i+1 - depth_i = 0.001 x count). The count feeds only later nodes +0x12C and G+0x24, which then reproduce exactly. Emulating it needs the DC-RAM host image plus the texture-slot tables.';
UPSERT finding:emu_gate_layerz_reset_unknown SET status='open', confidence='medium', date='2026-09-02', note='Only FUN_140613390 (init, caller FUN_14060a1f0) and FUN_140620f10 carry blk+0x6d08..0x6d44 displacements, yet the table is reset every frame (post-walk dumps show base + one frame of accumulation). The per-frame resetter is UNKNOWN (register-based loop or block copy). EMU-GATE restores the table from the init constants.';
UPSERT finding:emu_gate_avx_crt_paths SET status='confirmed', confidence='high', date='2026-09-02', note='Ghidra 12.1.2 x86 p-code has no semantics for vfmadd213sd (CALLOTHER vfmadd213sd_fma): the first camera run died at 0x14081761f inside FUN_140817230 (tanf) after 99 instructions because DAT_142eefbd8 = 1 selects the FMA3 UCRT branch. Setting the flag to 0 runs the SSE2 branch; results bit-identical to the capture, and identical to Java Math hooks.';

-- about edges
RELATE finding:emu_gate_camera_bitexact->about:eg_about_1->steam_routine:FUN_14061d7e0;
RELATE finding:emu_gate_camera_bitexact->about:eg_about_2->steam_routine:FUN_14061d6a0;
RELATE finding:emu_gate_camera_bitexact->about:eg_about_3->steam_routine:FUN_14061d5b0;
RELATE finding:emu_gate_camera_bitexact->about:eg_about_4->global:nl_matrix_slots;
RELATE finding:emu_gate_walker_bitexact->about:eg_about_5->steam_routine:FUN_140620f10;
RELATE finding:emu_gate_walker_bitexact->about:eg_about_6->global:layerz_table;
RELATE finding:emu_gate_walker_composite_is_current_frame->about:eg_about_7->global:nl_composite_vps;
RELATE finding:emu_gate_walker_composite_is_current_frame->about:eg_about_8->steam_routine:FUN_14060fb90;
RELATE finding:emu_gate_walker_composite_is_current_frame->about:eg_about_9->steam_routine:FUN_140620f10;
RELATE finding:emu_gate_matrix_stack_lives_in_blk->about:eg_about_10->global:nl_matrix_push_storage;
RELATE finding:emu_gate_matrix_stack_lives_in_blk->about:eg_about_11->steam_routine:FUN_140846a40;
RELATE finding:emu_gate_push_without_depth_is_noop->about:eg_about_12->steam_routine:FUN_140847950;
RELATE finding:emu_gate_cell_override_needs_dc_ram->about:eg_about_13->global:anim_cell_table;
RELATE finding:emu_gate_cell_override_needs_dc_ram->about:eg_about_14->steam_routine:FUN_140620f10;
RELATE finding:emu_gate_submit_stubbed->about:eg_about_15->steam_routine:FUN_1406129f0;
RELATE finding:emu_gate_layerz_reset_unknown->about:eg_about_16->global:layerz_table;
RELATE finding:emu_gate_layerz_reset_unknown->about:eg_about_17->steam_routine:FUN_140613390;
RELATE finding:emu_gate_avx_crt_paths->about:eg_about_18->global:crt_fma3_flag;

-- provenance
RELATE finding:emu_gate_camera_bitexact->cites:eg_cite_1->source:ghidra_pcode_emulation;
RELATE finding:emu_gate_walker_bitexact->cites:eg_cite_2->source:ghidra_pcode_emulation;
RELATE finding:emu_gate_walker_composite_is_current_frame->cites:eg_cite_3->source:ghidra_pcode_emulation;
RELATE finding:emu_gate_matrix_stack_lives_in_blk->cites:eg_cite_4->source:ghidra_pcode_emulation;
RELATE finding:emu_gate_push_without_depth_is_noop->cites:eg_cite_5->source:ghidra_pcode_emulation;
RELATE finding:emu_gate_cell_override_needs_dc_ram->cites:eg_cite_6->source:ghidra_pcode_emulation;
RELATE finding:emu_gate_submit_stubbed->cites:eg_cite_7->source:ghidra_pcode_emulation;
RELATE finding:emu_gate_layerz_reset_unknown->cites:eg_cite_8->source:ghidra_pcode_emulation;
RELATE finding:emu_gate_avx_crt_paths->cites:eg_cite_9->source:ghidra_pcode_emulation;
RELATE source:ghidra_pcode_emulation->confirms:eg_conf_1->steam_routine:FUN_14061d7e0;
RELATE source:ghidra_pcode_emulation->confirms:eg_conf_2->steam_routine:FUN_14061d6a0;
RELATE source:ghidra_pcode_emulation->confirms:eg_conf_3->steam_routine:FUN_14061d5b0;
RELATE source:ghidra_pcode_emulation->confirms:eg_conf_4->steam_routine:FUN_140620f10;
RELATE source:ghidra_pcode_emulation->confirms:eg_conf_5->steam_routine:FUN_140846a40;

-- reads / writes the gate established
RELATE steam_routine:FUN_140846a40->writes:eg_w_6a40_stack->global:nl_matrix_push_storage;
RELATE steam_routine:FUN_140847950->writes:eg_w_7950_stack->global:nl_matrix_push_storage;
RELATE steam_routine:FUN_1408478c0->reads:eg_r_78c0_stack->global:nl_matrix_push_storage;
RELATE steam_routine:FUN_140847d10->writes:eg_w_7d10_comp->global:nl_composite_vps;
RELATE steam_routine:FUN_140848120->reads:eg_r_8120_comp->global:nl_composite_vps;
RELATE steam_routine:FUN_14060fb90->writes:eg_w_fb90_comp->global:nl_composite_vps;
RELATE steam_routine:FUN_140620f10->writes:eg_w_0f10_comp->global:nl_composite_vps;
RELATE steam_routine:FUN_140620f10->writes:eg_w_0f10_layerz->global:layerz_table;
RELATE steam_routine:FUN_140613390->writes:eg_w_3390_layerz->global:layerz_table;
RELATE steam_routine:FUN_140620f10->reads:eg_r_0f10_cell->global:anim_cell_table;
