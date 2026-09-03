-- 37: LIST 0xC part nodes = the COMBO COUNTER / rating / round text; part walker FUN_140653a70 <-> loc_8C0F215E (2026-09-03).
-- Source doc: mvc-live-skins-quarters/docs/PARTS-LIST0C-GHIDRA.md. Tools: d3dcap/replay/rip_parts.py, parts_gate.py, blkstate.pnodes.
-- Apply with tools/re_kb/apply_seed.py (per statement) after the documented export backup. Idempotent: UPSERT for nodes,
-- RELATE with explicit edge ids. No apostrophes inside string literals.
USE NS re DB kb;

UPSERT source:doc_parts_list0c_ghidra SET kind='doc', strength='code', ref='mvc-live-skins-quarters/docs/PARTS-LIST0C-GHIDRA.md', note='Ghidra read-set of the list-0xC part walker FUN_140653a70, its allocators and state machine, the HUD-bank model selection, the header-patch texture rule, the D3D matrix path, and the SH4 pair loc_8C0F215E. Every CONFIRMED claim was decompiled/disassembled and read; the render rule was gated bit-for-bit on five captured frames.';
UPSERT source:tool_parts_gate SET kind='tool', strength='code', ref='mvc-live-skins-quarters/d3dcap/replay/parts_gate.py', note='Derives the list-0xC part draws of a captured frame from blk (blkstate.pnodes), the exe-static part lists (read from the dump), the scale table and the ripped HUD bank (rip_parts.py), and compares CBWorld (48 B), vertex positions, uvs and texture pages with the frame .pack bit-for-bit.';
UPSERT source:tool_rip_parts SET kind='tool', strength='code', ref='mvc-live-skins-quarters/d3dcap/replay/rip_parts.py', note='Rips the HUD/common bank POL (AFS 835, TCW base 0xC90, 130 models) to per-model JSON (tri list pos/uv, record headers, runtime TCW) and, with --lists, every static part list the list-0xC writers use (5 roots, 32 distinct lists, scale table, x-offset tables) from the exe dump. Output under tcw_pages/parts/ (gitignored, BYOR).';

-- SH4 routines
UPSERT routine:loc_8c0f215e SET label='loc_8C0F215E', pc='0x8C0F215E', bank='bank0f', role='list-0xC part draw (combo counter / text parts)', summary='gate +0x12C, list +0xD4; push, identity, translate(+0x34), store XMTRX to +0x88, pop; HUD camera 8c02e334; two passes (second: list +0xD8, translate(+0x64, -6.0, 0)); per 8-B entry translate(entry+4,0,0), optional scale 0x8C1605F0[idx&7] (+8c0301b4 max3, 8c122710), colour 8c123780(1,1,1), model 0x8C26A90C[entry+3 if flags<0 else node+0x19C[k]], header patch 8c032fae(model,count-1), draw 8c1235b0. Pool holds -6.0 (0xC0C00000).';
UPSERT routine:loc_8c0f2104 SET label='loc_8c0f2104', pc='0x8C0F2104', bank='bank0f', role='combo counter digit formatter', summary='8c129740 sprintf of *(i16*)(node+0x12E) with "%3d"; three chars minus 0x30 (negative -> 0xFF) into node+0x19C..; node+0xD4 = 0x8c160610 (static part list), node+0xD8 = 0.';
UPSERT routine:loc_8c1210b0 SET label='loc_8c1210B0', pc='0x8C1210B0', bank='bank12', role='XMTRX translate(x,y,z)', summary='fr4,fr5,fr6 = x,y,z; pre-multiplies a translation into XMTRX (Steam FUN_140847bf0).';
UPSERT routine:loc_8c1210e0 SET label='loc_8c1210E0', pc='0x8C1210E0', bank='bank12', role='XMTRX translate by vector at r4', summary='Steam FUN_140847c40(ptr).';
UPSERT routine:loc_8c120ff0 SET label='loc_8c120FF0', pc='0x8C120FF0', bank='bank12', role='XMTRX scale by vector at r4', summary='Steam FUN_140847b90(ptr).';
UPSERT routine:loc_8c11fa80 SET label='loc_8c11FA80', pc='0x8C11FA80', bank='bank11', role='store XMTRX to r4', summary='Steam FUN_140846a00(ptr).';
UPSERT routine:loc_8c1201e0 SET label='loc_8c1201E0', pc='0x8C1201E0', bank='bank12', role='load XMTRX from r4', summary='Steam FUN_140846c30(ptr).';
UPSERT routine:loc_8c123780 SET label='loc_8c123780', pc='0x8C123780', bank='bank12', role='vertex colour multipliers (r,g,b)', summary='Steam FUN_140849b00.';
UPSERT routine:loc_8c032fae SET label='loc_8c032FAe', pc='0x8C032FAE', bank='bank03', role='model record header patch (texture page select)', summary='r4 = model, r5 = k: copies table entry k over PCW/ISP/TSP/TCW of every record (Steam FUN_14060d8d0 -> FUN_140619760 with DAT_142eed370).';
UPSERT routine:loc_8c1235b0 SET label='loc_8c1235B0', pc='0x8C1235B0', bank='bank12', role='draw NaomiLib object (kind 0)', summary='Steam FUN_1408499e0 -> FUN_140848ee0(obj, 0, 1.0, kind).';
UPSERT routine:loc_8c0301b4 SET label='loc_8c0301B4', pc='0x8C0301B4', bank='bank03', role='max of three floats at r4', summary='Steam FUN_140620720.';

-- Steam routines
UPSERT steam_routine:FUN_140653a70 SET addr='0x140653a70', name='FUN_140653a70', role='list-0xC part draw (combo counter / rating / round text)', note='CONFIRMED (decompiled + disassembled): if node+0x170 and node+0x110: W = T(node+0x50) stored to node+0xA8; FUN_14061d5b0 (P 0x4000, V = I); two passes (pass 2: list node+0x118, extra T(node+0x80, -6.0, 0)); per 8-B entry {i8 count, i8 flags, i8 scaleIdx, i8 modelIdx, f32 xoff}: T(xoff,0,0) pre-multiplied, S(PTR_DAT_140a7a2b0[scaleIdx&7]) when scaleIdx, colour (1,1,1), ctx+0x1f855c = 1, model = PTR_DAT_142edf598[modelIdx] if flags < 0 else PTR_DAT_142edf598[node[0x228+k++]] (negative = skip), FUN_14060d8d0(model, count-1) if count > 0, FUN_1408499e0(model). No CPU vertex transform: CBWorld = the composed W. SH4 pair loc_8C0F215E (both read; pool constant -6.0). Gated bit-exact on 5 frames (parts_gate.py).';
UPSERT steam_routine:FUN_1406539a0 SET addr='0x1406539a0', name='FUN_1406539a0', role='allocate the combo-counter node of slot 0/1 in list 0xC', note='CONFIRMED: FUN_14061dbe0(0, 0xC, 1); +0x170 = 1; +0x18 = LAB_140653940 (state dispatcher on node+4, also copies *(blk+0x32500+slot*0x18) to +0x100); +0xA0 = 0; +0xF0 = 0; +0x50..58 = DAT_140a7a280[slot] = (-40,5,-40) / (40,5,-40); +2 = slot; +0x6C = (1.5,1.5,1) (unused by the draw); +0x28 = blk+0x3DB8+slot*0x738; +0x228..0x237 = 0. Called by FUN_140653970 for slots 0 and 1 when G+0x14 != 0x40.';
UPSERT steam_routine:FUN_140653970 SET addr='0x140653970', name='FUN_140653970', role='allocate both combo-counter nodes', note='CONFIRMED: if *(G+0x14) != 0x40: FUN_1406539a0(0); FUN_1406539a0(1). Callers FUN_140620670, FUN_140626c20, FUN_1406277b0, FUN_140627390, FUN_14073d490 (init paths, not traced).';
UPSERT steam_routine:FUN_140652cc0 SET addr='0x140652cc0', name='FUN_140652cc0', role='combo counter digit formatter', note='CONFIRMED: FUN_14003a4c0(buf, "%3d" (DAT_14097e87c), *(i16*)(node+0x172)); node+0x228/0x229/0x22A = char - 0x30 (space -> -16 = blank); node+0x110 = DAT_140a7a2f0; node+0x118 = 0. SH4 pair loc_8c0f2104 (node+0x12E, +0x19C, +0xD4 = 0x8c160610).';
UPSERT steam_routine:FUN_140652c50 SET addr='0x140652c50', name='FUN_140652c50', role='combo rating word + points line lists', note='CONFIRMED: node+0x110 = DAT_140a7a3b0[node+0x180 & 0xF] (16 rating lists); if DAT_140a7a470[idx]: node+0x118 = DAT_140a7a330 (8-digit "%8x" points line) when G+0x8B == 0 and fighter+0x6B1 == 0, else 0.';
UPSERT steam_routine:FUN_14060d8d0 SET addr='0x14060d8d0', name='FUN_14060d8d0', role='model record header patch = texture page select', note='CONFIRMED: FUN_140619760(model, DAT_142eed370 + k*0x10): for every record (rec = model+0x18; next = rec + rec[0x13] + 0x50; while *rec != 0) copy the 16-B entry over PCW/ISP/TSP/TCW. Table built in FUN_14060c370 case 4 by FUN_140619720 from the first record of HUD models 0..3: TCW = 0xC92 + k. SH4 pair loc_8c032fae.';
UPSERT steam_routine:FUN_140619760 SET addr='0x140619760', name='FUN_140619760', role='copy 16-B header table entries INTO model records', note='CONFIRMED: inverse of FUN_140619720.';
UPSERT steam_routine:FUN_140619720 SET addr='0x140619720', name='FUN_140619720', role='copy model record headers OUT to a 16-B table', note='CONFIRMED: used by FUN_14060c370 case 4 to build DAT_142eed370 from HUD models 0..3.';
UPSERT steam_routine:FUN_140847bf0 SET addr='0x140847bf0', name='FUN_140847bf0', role='matrix translate(x,y,z) pre-multiply', note='CONFIRMED: builds identity with row 3 = (x,y,z,1) and calls FUN_140846ee0. SH4 loc_8c1210B0.';
UPSERT steam_routine:FUN_140847c40 SET addr='0x140847c40', name='FUN_140847c40', role='matrix translate by vector at ptr', note='CONFIRMED. SH4 loc_8c1210E0.';
UPSERT steam_routine:FUN_140847b90 SET addr='0x140847b90', name='FUN_140847b90', role='matrix scale by vector at ptr', note='CONFIRMED: diag(ptr[0], ptr[1], ptr[2], 1) pre-multiplied. SH4 loc_8c120FF0.';
UPSERT steam_routine:FUN_140620720 SET addr='0x140620720', name='FUN_140620720', role='max of three floats', note='CONFIRMED. SH4 loc_8c0301B4. Its result feeds FUN_140036560, an empty ret on Steam.';
UPSERT steam_routine:FUN_1408499e0 SET addr='0x1408499e0', name='FUN_1408499e0', role='draw object, kind 0 (or 2 when ctx+0x1f8518)', note='CONFIRMED: FUN_140848ee0(obj, 0, 1.0, ctx+0x1f8518 ? 2 : 0). Used by the HUD list 0xB/0xD walkers and by the list-0xC part draw. SH4 loc_8c1235B0.';
UPSERT steam_routine:FUN_14073dfb0 SET addr='0x14073dfb0', name='FUN_14073dfb0', role='allocate a list-0xC round/intro text node', note='CONFIRMED: list 0xC, +0x170 = 0, callback LAB_14073dfa0 (switch on node+4), +0x3A = 7, +0x34 = kind & 0xF; kind bit 7 frees it at once (and kind 3 calls FUN_1407ec920). Its caseD_0: +0x170 = 1, +0x54 = -6.25, +0x58 = 0, +0x50 = DAT_140a98870[state] * -0.5, +0x110 = PTR_LAB_140a987f0[state], +0x118 = 0.';
UPSERT steam_routine:FUN_1407ec920 SET addr='0x1407ec920', name='FUN_1407ec920', role='allocate the results-screen time-line nodes in list 0xC', note='CONFIRMED: nodes at +0x54 = -10/0/5/10, +0x58 = -40, digits from +0x270/+0x278 (h:mm:ss of blk+0x324f8) via FUN_1407ec6b0 ("%1x" + "%02d"/"%08x"); caseD_0 of their callback sets +0x110 = DAT_140aac980[state] (7 lists).';
UPSERT steam_routine:FUN_1408482a0 SET vertex_bit0='CONFIRMED (2026-09-03): after copying each vertex to the VB the consumer does x_word &= 0xfffffffe and v_word &= 0xfffffffe (bit 0 of x is the NaomiLib direct-vertex flag). 95/95 captured part vertices on frame 4445 differ from the AFS file by exactly that bit on x and v and by nothing on y, z, u. The earlier "copied verbatim" wording in TSP-RENDER-STATE-GHIDRA.md s3 is corrected by this: verbatim except bit 0 of x and v.';

-- pairs
RELATE steam_routine:FUN_140653a70->recompiles:FUN_140653a70_loc_8c0f215e->routine:loc_8c0f215e SET confidence='confirmed', method='both-read', evidence='unique float -6.0 (0xC0C00000) in both pools; identical call skeleton (push/identity/translate/store/pop, HUD camera, two passes, per-entry translate/scale/colour/header-patch/draw); field deltas +0x12C->+0x170, +0xD4->+0x110, +0xD8->+0x118, +0x34->+0x50, +0x64->+0x80, +0x88->+0xA8, +0x19C->+0x228', inlined=false;
RELATE steam_routine:FUN_140652cc0->recompiles:FUN_140652cc0_loc_8c0f2104->routine:loc_8c0f2104 SET confidence='confirmed', method='both-read', evidence='sprintf %3d of the i16 count, three chars minus 0x30 into the digit array, then list pointer + second list 0', inlined=false;
RELATE steam_routine:FUN_140847bf0->recompiles:FUN_140847bf0_loc_8c1210b0->routine:loc_8c1210b0 SET confidence='high', method='call-slot', evidence='same argument shape and position in loc_8C0F215E / FUN_140653a70', inlined=false;
RELATE steam_routine:FUN_140847c40->recompiles:FUN_140847c40_loc_8c1210e0->routine:loc_8c1210e0 SET confidence='high', method='call-slot', evidence='translate(node+0x34) / translate(node+0x50) slot', inlined=false;
RELATE steam_routine:FUN_140847b90->recompiles:FUN_140847b90_loc_8c120ff0->routine:loc_8c120ff0 SET confidence='high', method='call-slot', evidence='scale(table[idx&7]) slot', inlined=false;
RELATE steam_routine:FUN_140846a00->recompiles:FUN_140846a00_loc_8c11fa80->routine:loc_8c11fa80 SET confidence='high', method='call-slot', evidence='store XMTRX to node+0x88 / node+0xA8 slot', inlined=false;
RELATE steam_routine:FUN_140846c30->recompiles:FUN_140846c30_loc_8c1201e0->routine:loc_8c1201e0 SET confidence='high', method='call-slot', evidence='load XMTRX from node+0x88 / node+0xA8 slot', inlined=false;
RELATE steam_routine:FUN_140849b00->recompiles:FUN_140849b00_loc_8c123780->routine:loc_8c123780 SET confidence='high', method='call-slot', evidence='colour (1,1,1) slot', inlined=false;
RELATE steam_routine:FUN_14060d8d0->recompiles:FUN_14060d8d0_loc_8c032fae->routine:loc_8c032fae SET confidence='high', method='call-slot', evidence='(model, count-1) slot before the draw', inlined=false;
RELATE steam_routine:FUN_1408499e0->recompiles:FUN_1408499e0_loc_8c1235b0->routine:loc_8c1235b0 SET confidence='high', method='call-slot', evidence='draw(model) slot', inlined=false;
RELATE steam_routine:FUN_140620720->recompiles:FUN_140620720_loc_8c0301b4->routine:loc_8c0301b4 SET confidence='high', method='call-slot', evidence='max3(scale) slot', inlined=false;

-- globals (Steam side; DC where read)
UPSERT global:list0c_head SET addr='0x8c287a8c', steam_off='0x2EE48', type='ptr', note='System-A list 12 head (blk+0x2EDE8 + 12*8). Nodes: the combo counter (FUN_1406539a0 x2), round/intro text (FUN_14073dfb0), results time lines (FUN_1407ec920/cc50). Walked by FUN_140620ea0 after the HUD when G+0x14 != 0x40 -> FUN_140653a70.';
UPSERT global:node_part_list SET addr='node+0xD4', steam_off='node+0x110', type='ptr', note='list-0xC node: pointer to the exe-STATIC part list (8-B entries {i8 count, i8 flags, i8 scaleIdx, i8 modelIdx, f32 xoff}, terminated by count < 0). Values seen: DAT_140a7a2f0 (hits), DAT_140a7a3b0[16] (rating words), PTR_LAB_140a987f0[16] (round text), DAT_140aac980[7] (results time). 0 = nothing drawn.';
UPSERT global:node_part_list2 SET addr='node+0xD8', steam_off='node+0x118', type='ptr', note='second-line part list, drawn in pass 2 after T(node+0x80, -6, 0). DAT_140a7a330 (8-digit points line) or 0.';
UPSERT global:node_part_x2 SET addr='node+0x64', steam_off='node+0x80', type='f32', note='second-line x offset (DAT_140a7a430[rating] - 24.0).';
UPSERT global:node_combo_count SET addr='node+0x12E', steam_off='node+0x172', type='i16', note='the combo hit count the counter displays; copied from blk+0x3254E + slot*2 by caseD_e of the combo state machine; formatted by FUN_140652cc0 with "%3d".';
UPSERT global:node_part_digits SET addr='node+0x19C', steam_off='node+0x228', type='i8[16]', note='model indices for the non-fixed entries of the part list (flags >= 0), in entry order; negative = draw nothing (the x advance still applies). Digits 0..9 = HUD models 0..9.';
UPSERT global:node_part_pos SET addr='node+0x34', steam_off='node+0x50', type='f32[3]', note='list-0xC node position = the only per-frame matrix input of FUN_140653a70 (W = T(pos) then the per-entry chain). x eases toward node+0x8C each frame (caseD_e). Init DAT_140a7a280[slot] = (-40, 5, -40) / (40, 5, -40).';
UPSERT global:combo_count_per_slot SET addr='UNKNOWN', steam_off='0x3254E', type='i16[6]', note='per-slot combo hit count read by the combo counter state machine (caseD_e: node+0x172 = *(i16*)(blk+0x3254E + slot*2)). Writer not traced.';
UPSERT global:combo_points_per_fighter SET addr='UNKNOWN', steam_off='0x3256C', type='u32[6]', note='per-fighter points value shown as the 8-digit "%8x" line when a combo ends (caseD_10). Writer not traced.';
UPSERT global:part_list_hits SET addr='0x8c160610', steam_off='0x140a7a2f0', type='exe-static part list', note='"%3d" HITS: (2,0,S1,-,2.5) (2,1,-,-,3.5) (2,2,-,-,3.5) digit slots (page 0xC93, scale 1.5) then (4,ff,S2,17,2.81) (4,ff,-,18,2.68) (4,ff,-,29,2.38) word models (page 0xC95, scale 0.66). Dumped by rip_parts.py --lists.';
UPSERT global:part_scale_table SET addr='0x8C1605F0', steam_off='0x140a7a2b0', type='ptr[8] -> f32[3]', note='[0,1,6,7] -> (1.5,1.5,1); 2 -> (0.66,0.66,1); 3 -> (0.5,1,1); 4 -> (1,0.5,1); 5 -> (2.5,2.5,1). Indexed by entry.scaleIdx & 7.';
UPSERT global:part_header_patch_table SET addr='UNKNOWN', steam_off='0x142eed370', type='u32[4][4]', note='DAT_142eed370[k] = {PCW 0x8000002C, ISP 0x83000000, TSP 0x2009A45B, TCW 0xC92+k} = first record header of HUD models 0..3, built by FUN_14060c370 case 4 (FUN_140619720). FUN_14060d8d0(model, count-1) copies entry k over every record of the model before a part draw: the part texture page is 0xC92 + count - 1.';
UPSERT global:hud_bank_model_table SET addr='0x8C26A90C', steam_off='0x142ee0c50', type='ptr[130] (+0xB00 count, +0xB08 texHdrs)', note='PTR_DAT_142edf598: host model table of the HUD/common bank (AFS 835 POL, TCW base 0xC90). Models 0..9 digits, 10..46 glyphs, 47..49 3-D pieces, 50..129 empty. Indexed by list-0xC part entries.';

-- findings
UPSERT finding:list0c_is_combo_counter SET status='confirmed', confidence='high', date='2026-09-03', contribution_candidate=true, note='List 0xC (blk+0x2EE48) holds the COMBO HIT COUNTER, not hit sparks: FUN_1406539a0 allocates one node per point slot with +0xA0 = +0xE8 = 0; caseD_e copies the per-slot count blk+0x3254E to node+0x172 and FUN_140652cc0 formats it with "%3d" into node+0x228 and points node+0x110 at DAT_140a7a2f0; at combo end caseD_10 / FUN_140652c50 switch to a rating-word list (DAT_140a7a3b0[node+0x180]) plus an 8-digit points line (DAT_140a7a330). Other list-0xC users: round/intro text (FUN_14073dfb0) and results time lines (FUN_1407ec920). Captured evidence: frames 4431..4504 show the digit array counting 2..28 in step with the combo; frame 4445 = "9 HITS".';
UPSERT finding:list0c_part_transform SET status='confirmed', confidence='high', date='2026-09-03', contribution_candidate=true, note='FUN_140653a70 draws each part with CBWorld = the composed matrix chain (T(node+0x50), then per entry T(xoff,0,0) and optional S(scale) pre-multiplied, cumulative), P = FUN_14061d5b0 (angle 0x4000, far 12000), V = I, colour (1,1,1), alpha 1.0, kind 0. There is NO CPU vertex transform: the VB holds the model vertices. Gate (parts_gate.py): frames 4445/4447/4474/7441/26380 -> CBWorld bit-exact 24/24, group positions 82/82, uvs 82/82, texture pages 82/82, unexplained z=-40 HUD-camera draws 0. The identity-CBWorld z=-71 draws are list-0xB HUD objects.';
UPSERT finding:list0c_texture_patch_rule SET status='confirmed', confidence='high', date='2026-09-03', contribution_candidate=true, note='Part texture page = 0xC92 + (entry.count - 1): FUN_14060d8d0(model, count-1) copies DAT_142eed370[count-1] (first record header of HUD model count-1, TCW 0xC92+count-1) over the model records before the draw (with ctx+0x1f855c = 1 so the frame arena gets the patched copy). Gated 82/82 pages byte-exact (digits 0xC93, word 0xC95) against rip_texbank.decode_host of AFS 836.';
UPSERT finding:consumer_clears_vertex_bit0 SET status='confirmed', confidence='high', date='2026-09-03', contribution_candidate=true, note='FUN_1408482a0 clears bit 0 of the x word and of the v word of every vertex it copies into the D3D VB (x &= ~1, v &= ~1; the NaomiLib direct-vertex flag). Found by the parts gate: before the mask 0/16 groups matched with all 95 vertices exactly -1 ulp on x and v and identical on y/z/u; after the mask 82/82. Applies to every kind-0..3 object (stage, props, HUD, parts): a 1-ulp effect invisible in pixels, required for bit-exact geometry gates. Corrects the "copied verbatim" wording of TSP-RENDER-STATE-GHIDRA.md s3.';
UPSERT finding:harvest_drops_list0c_by_construction SET status='confirmed', confidence='high', date='2026-09-03', contribution_candidate=true, note='RetroReceipts-agent reader.rs harvest_anodes keeps a node only if +0xA0 != 0 || +0xE8 != 0; every list-0xC allocator (FUN_1406539a0, FUN_14073dfb0, FUN_1407ec920, FUN_1407ecc50) writes +0xA0 = 0 and never sets +0xE8, so no combo counter / rating / round text ever reaches a tape. Fix = a separate 44-B pnodes record {slot, drawn, state, list1 RVA, list2 RVA, pos f32[3], x2 f32, digits i8[16]} walked from head 12 (docs/PARTS-LIST0C-GHIDRA.md s5 has the diff); +0xA8 is not carried because a post-walk block holds the previous draw matrix (frame 4445: +0xA8.x = -40.180 vs the draw at +0x50.x = -40.135307).';
UPSERT finding:list0c_not_hit_sparks SET status='confirmed', confidence='high', date='2026-09-03', note='The "3D hit sparks are a later cut" note and the "list 0xC = per-fighter 3D part models (hit effects)" reading are wrong: list 0xC is HUD-camera text (combo counter, ratings, round text, results time). Missing hit sparks on real-match tapes have a different owner (System-B sprite objects: objs cap / effect decode / classing) -- not diagnosed here.';
UPSERT finding:list0c_pass2_ungated SET status='open', confidence='medium', date='2026-09-03', note='The second-line pass (node+0x118, T(node+0x80, -6, 0)), the 16 rating lists, the round-text and results-time nodes are INFERRED from the same walker code; no captured frame had node+0x118 != 0 or a state-3 node, so they are not gated. Also open: which init path calls FUN_140653970 in a match (5 callers), the writers of blk+0x3254E / blk+0x3256C, and the agent sampling phase vs the callback pass (a one-frame lag on pnodes.pos.x would show it).';

-- about edges
RELATE finding:list0c_is_combo_counter->about:l0c_about_1->steam_routine:FUN_1406539a0;
RELATE finding:list0c_is_combo_counter->about:l0c_about_2->steam_routine:FUN_140652cc0;
RELATE finding:list0c_is_combo_counter->about:l0c_about_3->steam_routine:FUN_140652c50;
RELATE finding:list0c_is_combo_counter->about:l0c_about_4->global:list0c_head;
RELATE finding:list0c_is_combo_counter->about:l0c_about_5->global:node_combo_count;
RELATE finding:list0c_part_transform->about:l0c_about_6->steam_routine:FUN_140653a70;
RELATE finding:list0c_part_transform->about:l0c_about_7->routine:loc_8c0f215e;
RELATE finding:list0c_part_transform->about:l0c_about_8->global:node_part_pos;
RELATE finding:list0c_part_transform->about:l0c_about_9->global:part_scale_table;
RELATE finding:list0c_texture_patch_rule->about:l0c_about_10->steam_routine:FUN_14060d8d0;
RELATE finding:list0c_texture_patch_rule->about:l0c_about_11->global:part_header_patch_table;
RELATE finding:consumer_clears_vertex_bit0->about:l0c_about_12->steam_routine:FUN_1408482a0;
RELATE finding:harvest_drops_list0c_by_construction->about:l0c_about_13->global:node_part_list;
RELATE finding:harvest_drops_list0c_by_construction->about:l0c_about_14->global:node_part_digits;
RELATE finding:harvest_drops_list0c_by_construction->about:l0c_about_15->steam_routine:FUN_1406539a0;
RELATE finding:list0c_not_hit_sparks->about:l0c_about_16->global:list0c_head;
RELATE finding:list0c_pass2_ungated->about:l0c_about_17->global:node_part_list2;
RELATE finding:list0c_pass2_ungated->about:l0c_about_18->steam_routine:FUN_140653970;

-- provenance
RELATE finding:list0c_is_combo_counter->cites:l0c_cite_1->source:doc_parts_list0c_ghidra;
RELATE finding:list0c_part_transform->cites:l0c_cite_2->source:doc_parts_list0c_ghidra;
RELATE finding:list0c_part_transform->cites:l0c_cite_3->source:tool_parts_gate;
RELATE finding:list0c_texture_patch_rule->cites:l0c_cite_4->source:tool_parts_gate;
RELATE finding:list0c_texture_patch_rule->cites:l0c_cite_5->source:tool_rip_parts;
RELATE finding:consumer_clears_vertex_bit0->cites:l0c_cite_6->source:tool_parts_gate;
RELATE finding:consumer_clears_vertex_bit0->cites:l0c_cite_7->source:doc_parts_list0c_ghidra;
RELATE finding:harvest_drops_list0c_by_construction->cites:l0c_cite_8->source:doc_parts_list0c_ghidra;
RELATE finding:list0c_not_hit_sparks->cites:l0c_cite_9->source:doc_parts_list0c_ghidra;
RELATE finding:list0c_pass2_ungated->cites:l0c_cite_10->source:doc_parts_list0c_ghidra;
RELATE source:tool_parts_gate->confirms:l0c_conf_1->steam_routine:FUN_140653a70;
RELATE source:tool_parts_gate->confirms:l0c_conf_2->steam_routine:FUN_14060d8d0;
RELATE source:tool_parts_gate->confirms:l0c_conf_3->steam_routine:FUN_1408482a0;
RELATE source:doc_parts_list0c_ghidra->confirms:l0c_conf_4->routine:loc_8c0f215e;
RELATE source:doc_parts_list0c_ghidra->confirms:l0c_conf_5->routine:loc_8c0f2104;

-- reads / writes
RELATE steam_routine:FUN_140653a70->reads:l0c_r_1->global:node_part_list;
RELATE steam_routine:FUN_140653a70->reads:l0c_r_2->global:node_part_list2;
RELATE steam_routine:FUN_140653a70->reads:l0c_r_3->global:node_part_x2;
RELATE steam_routine:FUN_140653a70->reads:l0c_r_4->global:node_part_digits;
RELATE steam_routine:FUN_140653a70->reads:l0c_r_5->global:node_part_pos;
RELATE steam_routine:FUN_140653a70->reads:l0c_r_6->global:part_scale_table;
RELATE steam_routine:FUN_140653a70->reads:l0c_r_7->global:hud_bank_model_table;
RELATE steam_routine:FUN_14060d8d0->reads:l0c_r_8->global:part_header_patch_table;
RELATE steam_routine:FUN_140652cc0->writes:l0c_w_1->global:node_part_list;
RELATE steam_routine:FUN_140652cc0->writes:l0c_w_2->global:node_part_digits;
RELATE steam_routine:FUN_140652cc0->reads:l0c_r_9->global:node_combo_count;
RELATE steam_routine:FUN_140652c50->writes:l0c_w_3->global:node_part_list;
RELATE steam_routine:FUN_140652c50->writes:l0c_w_4->global:node_part_list2;
RELATE steam_routine:FUN_1406539a0->writes:l0c_w_5->global:node_part_pos;
RELATE steam_routine:FUN_1406539a0->writes:l0c_w_6->global:list0c_head;
RELATE steam_routine:FUN_140620ea0->reads:l0c_r_10->global:list0c_head;
RELATE steam_routine:FUN_140653a70->calls:l0c_c_1->steam_routine:FUN_14060d8d0;
RELATE steam_routine:FUN_140653a70->calls:l0c_c_2->steam_routine:FUN_1408499e0;
RELATE steam_routine:FUN_140653a70->calls:l0c_c_3->steam_routine:FUN_14061d5b0;
RELATE steam_routine:FUN_140653a70->calls:l0c_c_4->steam_routine:FUN_140847bf0;
RELATE steam_routine:FUN_140653a70->calls:l0c_c_5->steam_routine:FUN_140847b90;
RELATE steam_routine:FUN_140620ea0->calls:l0c_c_6->steam_routine:FUN_140653a70;
RELATE steam_routine:FUN_140653970->calls:l0c_c_7->steam_routine:FUN_1406539a0;
