USE NS re DB kb;

-- ============================================================================
-- 39_translucent_sort.surql
-- 2026-09-03 -- How Steam orders translucent (category 3) draws: the FUN_140843320 sort key, the qsort
-- comparator LAB_1408434d0, the MSVC CRT qsort, the sprite depth key (node+0x12C + 0.001/record) and the
-- flush order FUN_140843eb0 -> FUN_140842e30. Read in Ghidra (mvc_dump.bin) and gated against 3 capgate
-- packs + state dumps with the stage-11 tape (d3dcap/replay/sort_gate.py).
-- Doc: mvc-live-skins-quarters/docs/TRANSLUCENT-SORT-GHIDRA.md. Consumer: d3dcap/replay/tape_to_seq.py.
-- Apply: PYTHONIOENCODING=utf-8 python tools/re_kb/apply_seed.py tools/re_kb/39_translucent_sort.surql
-- ============================================================================

UPSERT source:translucent_sort_ghidra SET kind='disassembly', strength='code',
  ref='mvc_dump.bin (Ghidra dumpproj, bridge :8080; comparator bytes read at file offset VA-0x140000000): FUN_1408436a0 (slot {u32 seq @0, kind @4, entry* @8, size @0x10, f32 key @0x14, cat @0x18, s19 @0x19, blend @0x1a} at ctx+0x30+(cat+pass*4)*0x10000+n*0x20, n = ctx+0x1e0030[cat+pass*4], pass = ctx+0x1f828c; kinds 2/3: key = FUN_140843320(rec+0x10,+0x14,+0x18,+0x1C) for translucent list types; case 0xC: key = entry+0xC BEFORE entry+0xC = FUN_1408432e0(key), cat = entry+0x30 ? 3 : 0). FUN_140843320 (cfg+0x48 == 8 && r > 7000.0 -> 10000.0; r < 0 -> -r; else push, cur x M(ctx+0x1f812c) x M(ctx+0x1f816c) x Screen(DAT_140ab2080), [c 1] x cur via FUN_140846880, pop, return out[3] = w). FUN_1408432e0 (max(0, (ctx+0x1f81a4 - D*ctx+0x1f8194)/D) = (P[3][2]-D*P[2][2])/D). FUN_1406129f0 (entry+0xC = node+0x12C; per record FUN_140845e20 then += 0.001 = DAT_1408e2a3c). FUN_140845e20 (FUN_1408436a0(0xC, copy)). FUN_1408458b0 (vertex z = entry+0xC on all 4 verts). FUN_140620f10 (node+0x12C = blk+0x6928*0.1 + blk[0x6D08+4*layer]; after submit blk[0x6D08+4*layer] += records*0.001; FUN_14061d7e0 per node -> slot 3 = world P). FUN_140842e30 (cat == 3 -> qsort(base, n, 0x20, LAB_1408434d0) = CALL 0x140817f80 @0x140842ea5; then cmd 1 / cmd 8 / consumer by kind). LAB_1408434d0 (comiss key desc, then unsigned seq asc). FUN_140817f80 (MSVC 2015 CRT qsort, FID single match). FUN_140843eb0 (passes 2, 3, 0; cats 0..3 each). Constants DAT_1408dd5a8 1.0, DAT_140990d84 7000, DAT_14096a9f0 10000, DAT_1408e2ac0 sign mask, DAT_1408e2f64 0.1, DAT_1408e2a3c 0.001, DAT_14097dc04 812.3573.',
  note='Read 2026-09-03. CONFIRMED = decompiled and read. The writer of ctx+0x1f828c (pass index) and the meaning of PTR_DAT_140acd3a0+0x48 were not located (UNKNOWN).';

UPSERT source:translucent_sort_gate SET kind='measurement', strength='reproduction',
  ref='mvc-live-skins-quarters/d3dcap/replay/sort_gate.py over capgate/frame_4445, 4505, 7279 .pack + capgate/state dumps vs gs-cache tape 59613506 (stage 11): per cat-3 draw (depth write OFF) the key is derived (records joined by vertex set, W = captured CBWorld, V/P = captured scene CB rows 7-10/15-18; sprites from the dump LayerZ table blk+0x6D08) and the gold sequence checked for key rises and tie order. 4445: 269 draws, 252 keyed, 1 unjoined, 0 key rises, 23 distinct-entry ties (3 with unresolved node identity: obj-139 records under a composed W), 133/133 sprite vertex z reproduced max|dz| 2.4e-7. 4505: 150 draws, 133 keyed, 0 rises, 3 ties / 0 violations; sprite keys from the engine z (no captured dump carries this pack sprite set). 7279: 140 draws, 107 keyed, 0 rises, 4 ties / 0 violations, 96/96 sprite z max|dz| 1.8e-7. Frame 5168 has no pack. Discriminator: 4445 draws 192/193 (radius 32776/33339 on nodes (6,10)/(6,6)) are drawn in w order 40812 > 33062, not in the sequence order the 10000 branch would give.',
  note='2026-09-03. A reproduction of the gold order from derived keys; the mechanism is carried by the Ghidra source.';

-- routines
UPSERT steam_routine:FUN_140843320 SET addr='0x140843320', name='FUN_140843320', role='translucent sort key of a NaomiLib record: w of [centre(rec+0x10) 1] x W x V x P x Screen; radius < 0 -> -radius; cfg+0x48 == 8 && radius > 7000 -> 10000 (inactive in a fight, gate)', matched=false;
UPSERT steam_routine:FUN_1408432e0 SET addr='0x1408432e0', name='FUN_1408432e0', role='depth key -> vertex z: max(0, (P[3][2] - D*P[2][2]) / D) with P = matrix slot 3; applied to kinds 0xC (sprites), 0xD..0x12, 0x13/0x14/0x16', matched=false;
UPSERT steam_routine:FUN_140842e30 SET addr='0x140842e30', name='FUN_140842e30', role='queue flush per (pass, category): category 3 qsort(slots, n, 0x20, LAB_1408434d0) then per slot blend cmd 1, depth cmd 8, consumer by kind', matched=false;
UPSERT steam_routine:FUN_140843eb0 SET addr='0x140843eb0', name='FUN_140843eb0', role='frame flush: uploads, ring reset, fog block, then FUN_140842e30 for pass 2, 3, 0 with categories 0..3 each', matched=false;
UPSERT steam_routine:FUN_140817f80 SET addr='0x140817f80', name='FUN_140817f80', role='MSVC 2015 CRT qsort (Ghidra FID single match): median-of-3, __shortsort below 9, explicit stack; called by FUN_140842e30 for category 3', matched=false;
UPSERT steam_routine:FUN_1408458b0 SET addr='0x1408458b0', name='FUN_1408458b0', role='sprite (kind 0xC) consumer: 4-vertex quad, vertex z = entry+0xC (the converted sort key), FUN_140048370(0x4000, ..)', matched=false;
UPSERT steam_routine:FUN_140845e20 SET addr='0x140845e20', name='FUN_140845e20', role='sprite entry submit: copies the 0x48-B entry, clamps alpha, packs colour, FUN_1408436a0(0xC, copy)', matched=false;
UPSERT steam_routine:LAB_1408434d0 SET addr='0x1408434d0', name='LAB_1408434d0', role='qsort comparator for category 3 slots: f32 key @+0x14 DESCENDING (comiss), tie -> u32 sequence @+0 ASCENDING (total order)', matched=false;

-- globals
UPSERT global:layerz_table SET name='layerz_table', dc_addr='UNKNOWN', steam_addr='blk+0x6D08', type='f32[16]', note='Per-layer sprite depth base (15,17,19,21,23,25,27,29,10,11,12,13,30,31,32,33). The sprite walker FUN_140620f10 sets node+0x12C = zoom*0.1 + table[layer] and then adds 0.001 per submitted record; node+0x12C (+0.001 per record inside FUN_1406129f0) IS the translucent sort key of every sprite quad.';
UPSERT global:nl_queue_pass SET name='nl_queue_pass', steam_addr='DAT_142ef0ab0+0x1f828c', type='i32', note='Queue pass index read by FUN_1408436a0 (slot bank = cat + pass*4). Writer NOT located. One pass in a fight (gold: a single Z-write block precedes a single category-3 block).';
UPSERT global:nl_queue_counts SET name='nl_queue_counts', steam_addr='DAT_142ef0ab0+0x1e0030', type='u32[16]', note='Slot counts per (pass, category), = the sequence number written to slot+0. Per-pass totals at +0x1e0070/78/7c, arena cursor +0x1e0080 (limit 0xe0000).';

-- finding
UPSERT finding:translucent_sort_law SET date='2026-09-03', confidence='high', status='confirmed',
  contribution_candidate=true,
  statement='STEAM DRAWS Z-WRITE CATEGORIES IN SUBMISSION ORDER, THEN CATEGORY 3 SORTED FAR-TO-NEAR BY ONE KEY. Every NaomiLib draw is queued (FUN_1408436a0) in a 0x20-B slot {sequence, kind, entry, size, f32 key, cat}; the frame flush FUN_140843eb0 -> FUN_140842e30 emits categories 0, 1, 2 in submission order and qsorts category 3 (MSVC CRT qsort FUN_140817f80) with LAB_1408434d0 = key DESCENDING, ties by sequence ASCENDING (a total order, so the qsort variant does not matter). World/HUD record key = FUN_140843320 = w of [centre(rec+0x10..0x18) 1] x W x V x P (the Screen matrix column 3 is (0,0,0,1); HUD kind 0 skips V; radius(rec+0x1C) < 0 forces key = -radius). Sprite quad key = the walker depth node+0x12C = zoom*0.1 + LayerZ[layer] (blk+0x6D08) + 0.001 per record submitted (FUN_1406129f0), taken BEFORE FUN_1408432e0 converts it into the vertex z = max(0, (P32 - D*P22)/D) under the world P. Consequences: sprites (D ~ 96..108) sit between the world lists (w ~ 167..40812) and the x0.1 lists 7/8 (w ~ 73..81) and the HUD (71..68.5); later-registered nodes and later records get LARGER keys and are drawn FIRST (the "reverse registration / reverse record" observations); the category-3 phase never writes depth. Gate (sort_gate.py, 3 packs): 0 key rises in 559 cat-3 draws (492 keyed), sprite vertex z reproduced from the state dump on 4445 (133/133, 2.4e-7) and 7279 (96/96, 1.8e-7), radius>7000 pair drawn in w order (the cfg+0x48 == 8 branch is inactive).',
  note='CONFIRMED by decompile of FUN_1408436a0, FUN_140843320, FUN_1408432e0, FUN_1406129f0, FUN_140845e20, FUN_1408458b0, FUN_140620f10, FUN_140842e30, FUN_140843eb0 and the comparator bytes; gate reproduction 2026-09-03. Implemented in tape_to_seq.py (order_draws, sort_key_record, sprite_vertex_z; --legacy-order keeps the old dispatcher order). Open: the writer of ctx+0x1f828c and the meaning of cfg+0x48; the exact D of the HUD digit quads (D <= 2, z clamps to 0).';
RELATE finding:translucent_sort_law->cites->source:translucent_sort_ghidra SET kind='cites';
RELATE finding:translucent_sort_law->cites->source:translucent_sort_gate SET kind='cites';
RELATE finding:translucent_sort_law->about->steam_routine:FUN_140843320 SET kind='about';
RELATE finding:translucent_sort_law->about->steam_routine:FUN_1408432e0 SET kind='about';
RELATE finding:translucent_sort_law->about->steam_routine:FUN_1408436a0 SET kind='about';
RELATE finding:translucent_sort_law->about->steam_routine:FUN_140842e30 SET kind='about';
RELATE finding:translucent_sort_law->about->steam_routine:FUN_140843eb0 SET kind='about';
RELATE finding:translucent_sort_law->about->steam_routine:FUN_1406129f0 SET kind='about';
RELATE finding:translucent_sort_law->about->steam_routine:FUN_1408458b0 SET kind='about';
RELATE finding:translucent_sort_law->about->steam_routine:LAB_1408434d0 SET kind='about';
RELATE finding:translucent_sort_law->about->global:layerz_table SET kind='about';
RELATE finding:translucent_sort_law->about->global:nl_queue_pass SET kind='about';
RELATE steam_routine:FUN_140842e30->calls->steam_routine:FUN_140817f80 SET kind='calls', confidence='confirmed', evidence='CALL 0x140817f80 @0x140842ea5 with R9 = LAB_1408434d0, R8D = 0x20';
RELATE steam_routine:FUN_1408436a0->calls->steam_routine:FUN_140843320 SET kind='calls', confidence='confirmed', evidence='kinds 2/3 translucent branch LAB_140843a21; kind 0xB; kind 0x18';
RELATE steam_routine:FUN_1408436a0->calls->steam_routine:FUN_1408432e0 SET kind='calls', confidence='confirmed', evidence='cases 0xC, 0xD..0x12, 0x13/0x14/0x16';
RELATE steam_routine:FUN_1406129f0->calls->steam_routine:FUN_140845e20 SET kind='calls', confidence='confirmed', evidence='per record, tiled and scale-walker paths';
RELATE steam_routine:FUN_140845e20->calls->steam_routine:FUN_1408436a0 SET kind='calls', confidence='confirmed', evidence='FUN_1408436a0(0xC, &copy)';
RELATE steam_routine:FUN_140843eb0->calls->steam_routine:FUN_140842e30 SET kind='calls', confidence='confirmed', evidence='passes 2, 3, 0 x cats 0..3 (@0x140844105, @0x140844145, @0x140844179)';
RELATE steam_routine:FUN_140620f10->writes->global:layerz_table SET kind='writes', confidence='confirmed', evidence='blk[0x6D08+4*layer] += records*0.001 after FUN_1406129f0';
RELATE steam_routine:FUN_1408436a0->reads->global:nl_queue_pass SET kind='reads', confidence='confirmed', evidence='MOVSXD R15,[R10+0x1f828c] @0x1408436e6';
