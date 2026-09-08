-- 189_select_assist_names_and_bases.surql -- CHARACTER SELECT follow-up to seed 188: the ASSIST TYPE name rows are
-- static-page MODELS (TCW 0x817 EN / 0x818 JP) rewritten per draw by a node callback, not texture uploads; TCW 0x910 at
-- select is a one-record exe list (AFS 0x97/0x98 at DC 0x0CE60000); kinds 0xD..0x1C belong to the case-9 screen;
-- and the texture BASE -> bank pointer-global map for every FUN_14060c370 case.
-- 2026-09-06, SUPERGUN lane. Ghidra decompiles/disassembly of mvc_dump.bin + the vs-01 anchor's own DC-RAM
-- (POL 837 model records, TEX 838 headers, ctx slot table). Idempotent UPSERTs; RELATEs carry fixed ids.
USE NS re DB kb;

UPSERT source:select_assist_decompiles_20260906 SET kind='disasm', strength='code',
  ref='Ghidra dumpproj/mvc_dump.bin: FUN_140741510, FUN_1407ed850, FUN_1407ed560, FUN_1407ed740, FUN_1407ed640, FUN_1402e1ad0, FUN_14062b620, FUN_1406196e0, FUN_140619580/530/6a0, FUN_140620cd0, FUN_140620740, FUN_14060ba00, FUN_14060b960, FUN_14060b8c0, FUN_1406185e0, FUN_140845fe0, FUN_140846080, FUN_1408435d0, FUN_140844dc0, FUN_14060c370 (all cases, :8080 bridge); capstone over 0x1407413d0..0x140741510, 0x14062a712..0x14062a802, 0x140632b70..; tables DAT_140aacbf0, DAT_140aacb80, DAT_140a6aad0, DAT_140a6aaf0, LAB_140a6ab10, DAT_140a6ab30, DAT_140a6ab98 read from mvc_dump.bin.',
  note='re_kb consts query: the only routines carrying immediate 0x910 are FUN_14060c370, FUN_1406163e0, FUN_140632b70 (case-9 screen) and two unmatched CRT-band routines.';
UPSERT source:select_assist_anchor_read_20260906 SET kind='capture', strength='reproduction',
  ref='mvc-live-skins-quarters/d3dcap/ttd/runs/vs-01/pre: POL 837 parsed from dcram.bin (DC 0x0D0C6000: 181 models, table @0x0D0C6010, texHdrs @0x0D0C62E8); ctx.bin slot table ctx+0x1e00a0+tcw*0x18; blkstate.anodes() over blk.bin list 5; print ring 0x142ec4370.. from exe_image.bin.',
  note='vs-01: both sides inner state 1 (no assist panel spawned); 168 drawn list-5 nodes, TCW tally over their models = 0x810..0x81D only; slot 0x910 used (256x256 fmt 0x0100), 0x911..0x913 unused, 0x914 used (128x128 fmt 0x0101), 0xB used (64x64 fmt 0x0102); print ring 0 entries.';

UPSERT steam_routine:FUN_140741510 SET addr='0x140741510', name='FUN_140741510',
  role='ASSIST TYPE SELECT panel root node, spawned at confirm from FUN_14062aa00. FUN_14061dbe0(0,5,1); +0x20 = current team-slot fighter (blk+0x3db8+(side+2*slot)*0x738); +0x34 = side; +0x35 = slot; model = PTR_DAT_142edf5a0[0x62] (P1) / [0x69] (P2); +0xF0 = 0x80F; pos DAT_140a99b40+side*0xC; tick LAB_1407413d0 (spawns FUN_1407ed560(node,1) and FUN_1407ed740(node)); calls FUN_1407ed850(node) and sfx side+0x4C.';
UPSERT steam_routine:FUN_1407ed850 SET addr='0x1407ed850', name='FUN_1407ed850',
  role='spawns the three assist rows: for k in 0..2: B_k (tick LAB_1407ed720, model PTR_DAT_142edf5a0[0x63+k] = row box + TYPE label, +1 = cid, key 0x813, pos LAB_140aacb00[(side*9+k*3)]) and C_k (tick LAB_1407eda60, model [0x66] if G+0x29 == 0 else [0x6C], +1 = cid, +0x34 = side, +0x35 = k, key 0x813, node+0x40 callback = FUN_1407ed640 via FUN_1402e1ad0) = THE TYPE NAME row.';
UPSERT steam_routine:FUN_1407ed640 SET addr='0x1407ed640', name='FUN_1407ed640',
  role='pre-draw callback of an assist name row: n = DAT_140aacbf0[cid*3 + k]; FUN_14062b620(node, template 0x67 (JP) / 0x6D (EN), du = DAT_140aacb80[n].u - [0].u, dv = [0].v - [n].v); ctx+0x1f855c = 1. DAT_140aacbf0 = u8[56*3] per-character assist-type NAME index (13 names); DAT_140aacb80 = float (u,v) per name: 0..7 at u=0.062 v=1-row/8, 8..12 at u=0.562.';
UPSERT steam_routine:FUN_14062b620 SET addr='0x14062b620', name='FUN_14062b620',
  role='copy model PTR_DAT_142edf5a0[idx] into the node scratch model (node+0xA0) with every vertex (u,v) at +0x18/+0x1C shifted by (+du, -dv): FUN_1406196e0 sets the iterators DAT_142eee568..590, FUN_140619580 walks records/vertices, FUN_140619530 reads, FUN_1406196a0 writes (and ORs bit0 of v).';
UPSERT steam_routine:FUN_140620cd0 SET addr='0x140620cd0', name='FUN_140620cd0',
  role='LIST DRAW WALKER: for each node of blk+0x2ede8[list] with +0x170 and +0xA0: if node+0x40 call it(node); FUN_140847950(0); FUN_140846c30(node+0xA8); scale/colour from +0xF0 flags; submit +0xA0 via FUN_1408499e0 / FUN_140849be0 / FUN_140849ac0. Reached from FUN_140620ad0 (registered by FUN_14060ba00 into PTR_FUN_142ec6780[], run by FUN_14060b960 inside FUN_140608a00 = the frame build, before the host flush).';
UPSERT steam_routine:FUN_14060b8c0 SET addr='0x14060b8c0', name='FUN_14060b8c0',
  role='on-screen PRINT queue: (x, y, attr, fmt, ...) vsprintf into 0x142ec4380 + n*0x40 (n = DAT_142ec4374, max 0x80); rendered per entry by FUN_1406185e0 (16-column glyph grid) from FUN_14060b960; reset by FUN_14060b9f0. Strings: "NEW HERO" 0x14097e868, "WINS %2d" 0x14097e838, "STAGE   : %02X" 0x14097e8d0 (stage-select sub-state at 0x14062a7eb).';
UPSERT steam_routine:FUN_1408435d0 SET addr='0x1408435d0', name='FUN_1408435d0',
  role='register ONE 16-B texture record at an ABSOLUTE slot: if slot < 0xfff and slot unused: slot table {used=1, fmt|type<<8 = rec+4, palflag, w = rec+0, h = rec+2}, host tex = FUN_140048460(slot, w, h, 0, fmtType, ctx+0x1e0088 + (rec.loc & 0x1ffffff)). Record = {u16 w, u16 h, u16 fmt|type<<8, u16 0, u32 loc(DC), pad}; lists end at w <= 0.';

UPSERT global:select_text_slot_0x910 SET addr='n/a', steam_off='TCW 0x910 (select)', type='texture slot',
  note='At character select: ONE-record exe list DAT_140a6aad0 registered by FUN_14060c370 case 5 (FUN_1408458a0(0x910); FUN_140845fe0): 256x256 fmt 0x0100 (ARGB1555 twiddled) at DC 0x0CE60000 = AFS 0x97 (G+0x29 == 0) or 0x98. 0x911..0x913 are NOT registered at select (vs-01 slot table used = 0). No POL 837 model and no select-scene emitter references it (unknown user). The case-9 screen re-registers 0x910..0x913 from DAT_140a6ab30 (loc patched from G+0x94) and re-uploads atlas pages into 0x910/0x911 via kinds 0xD..0x1C (caseD_3 @0x140632c7d in FUN_140632b70).';
UPSERT global:assist_name_index_table SET addr='n/a', steam_off='0x140aacbf0', type='u8[56*3]',
  note='Per-character ASSIST TYPE NAME index for rows alpha/beta/gamma (13 distinct names 0..12). Read by FUN_1407ed640 as [cid*3 + k]. UV window per name in DAT_140aacb80 (float u,v): names 0..7 column u=0.062, rows v = 1.0 - n*0.125; names 8..12 column u=0.562.';

UPSERT finding:select_assist_names_are_static_page_models SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='The ASSIST TYPE names are list-5 nodes drawing the STATIC common-bank sheets 0x817 (EN) / 0x818 (JP) through a scratch model rewritten per draw -- no texture upload is involved',
  statement='Confirm spawns FUN_140741510 -> FUN_1407ed850: per row k a box/label node (model 0x63+k: records 0x81A, 0x818, 0x816) and a name node C_k whose model is the SCRATCH 0x66 (JP) / 0x6C (EN) and whose node+0x40 callback FUN_1407ed640 copies the TEMPLATE 0x67 / 0x6D into it with the UV window shifted to name n = DAT_140aacbf0[cid*3+k] (DAT_140aacb80[n] - [0]). Read from the vs-01 POL: 0x66/0x67 = one 4-vertex record TCW 0x818 (256x256 RGB565 @0x0D173000), 0x6C/0x6D = one record TCW 0x817 (@0x0D153000), quad x -26.2..26.2 z -10.5..10.5, pristine UV (0.0645..0.4355, 0.877..0.998) = name 0. No model of the 181 references 0x910. The callback runs inside the frame build: FUN_140620cd0 (draw walker) calls node+0x40 before submitting node+0xA0, reached from FUN_140620ad0 <- PTR_FUN_142ec6780[] <- FUN_14060b960 <- FUN_140608a00, before the host flush FUN_140607e90. The callback sets ctx+0x1f855c = 1, which makes FUN_140848ee0 COPY the rewritten records into the frame arena (finding:list0c_texture_patch_rule) instead of referencing the model in place.',
  consequence='No request kind and no DC-RAM write can make the names appear; the pages are already resident and registered. A shell that draws the boxes but not the names is dropping the frame-arena record copies (the only select nodes on that path) or ignoring the per-draw scratch rewrite. Test: after a confirm, list nodes with tick 0x1407eda60 (drawn = 1, model = PTR_DAT_142edf5a0[0x6C]) and check that the consumer sees a 4-vertex record with TCW 0x817 and UVs shifted per DAT_140aacbf0[cid*3+k].';
UPSERT finding:select_kinds_0d_1c_are_case9_screen SET status='inferred', confidence='high', date='2026-09-06',
  title='CORRECTION to seed 188: FUN_1406163e0 kinds 0xD..0x1C are the case-9 screen (atlas pages into 0x910/0x911), not character select',
  statement='Requested only by caseD_3 @0x140632c7d inside FUN_140632b70 (kind = 0xD + *(*(blk+0x32bc8)+1)); the same routine spawns nodes with TCW 0x910+n (add ebx,0x910 @0x140632c99). Source pages = dcram + (G+0x94-0x0C000000) + (kind-0xD)*0x20000 where case 9 set G+0x94 = 0x0C810000 (AFS 0x3E LZSS). At character select the assist state (0x14062a460.., PTR_LAB_140a6fbc8[2]) writes no request; slots 0x911..0x913 are unused.',
  consequence='Do not issue kinds 0xD..0x1C at select: they would overwrite the select text slot 0x910 with atlas pages.';
UPSERT finding:texture_base_bank_map SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='Texture BASE -> bank object map: which pointer global or exe record list owns every FUN_1408458a0 base',
  statement='Slot table ctx+0x1e00a0+TCW*0x18 holds dims/format for any TCW; the bank only supplies the pixel source. POL banks (FUN_14060d080/FUN_14060d8f0 host table: count @+0xB00, texHdrs @+0xB08 = the POL+8 record array; TCW = base + texIndex, loc = texHdrs[i].loc): 0x810 = PTR_DAT_142edf5a0 (case 5 select, POL 0x0D0C6000 AFS 0x345/0x346), PTR_DAT_142edf5a8 (case 6, 0x0D25C000, AFS 0x347), PTR_DAT_142edf5c0+5d0 (case 9, 0x0D7CC000, AFS 0x34B/0x34C), PTR_DAT_142edf5c8 (case 10, 0x0D2BB000), PTR_DAT_142edf5d8 (case 0xC, 0x0D4CE000, AFS 0x351/0x352), PTR_DAT_142edf5e0 (case 0xD, 0x0D6CC000, AFS 0x353/0x354); 0x850 = PTR_DAT_142edf5f0 (case 0xF, 0x0D7A9000, AFS 0x357/0x358), PTR_DAT_142edf5f8 (case 0x10, 0x0CE80000, AFS 0x35E/0x35F), PTR_DAT_142edf608/610/618/620 (cases 0x12..0x15, 0x0CE80000, AFS 0x362..0x369); 0x860 = DAT_142edf600 (case 0x11, 0x0CE80000, AFS 0x360/0x361; the same case registers RAM record lists at dcram+0xC00000 under 0x850 and dcram+0xE60000 under 0xD); 0xD10 = PTR_DAT_142edf5b8 (case 8, 0x0D2B1000, AFS 0x349/0x34A), PTR_DAT_142edf5e8 (case 0xE, 0x0D720000, AFS 0x355/0x356 or 0x359/0x35A); 0xC10/0xC50/0xC90 = PTR_DAT_142edf588/590/598. Exe record LISTS (FUN_140845fe0/FUN_140846080/FUN_1408435d0, 16-B {w,h,fmt|type<<8,0,loc}): 0x910 = DAT_140a6aad0 (case 5: 256x256 fmt 0x0100 @0x0CE60000) or DAT_140a6ab30 (case 9: 4 records, loc from G+0x94); 0x914 = DAT_140a6aaf0 (128x128 fmt 0x0101 @0x0CC60000, stage thumbnails; kind 0x1D re-uploads dcram+(blk+0x6D04+0x18C)*0x8000); absolute 0xB = LAB_140a6ab10 (64x64 fmt 0x0102 @0x0CCF0000, case 5); 0xD50 = DAT_140a6ab98 (128x128 fmt 0x0102 @0x0CC80000, case 0xE).',
  consequence='A resolver that maps a TCW to a POL bank by nearest lower base mis-resolves 0x910/0x914/0xB/0xD50: those are one-record exe lists, and 0x910 at select is NOT the 0x810 bank at index 0x100.';

RELATE finding:select_assist_names_are_static_page_models->cites->steam_routine:FUN_140741510 SET id='e189_a_root';
RELATE finding:select_assist_names_are_static_page_models->cites->steam_routine:FUN_1407ed850 SET id='e189_a_rows';
RELATE finding:select_assist_names_are_static_page_models->cites->steam_routine:FUN_1407ed640 SET id='e189_a_cb';
RELATE finding:select_assist_names_are_static_page_models->cites->steam_routine:FUN_14062b620 SET id='e189_a_copy';
RELATE finding:select_assist_names_are_static_page_models->cites->steam_routine:FUN_140620cd0 SET id='e189_a_walker';
RELATE finding:select_assist_names_are_static_page_models->cites->global:assist_name_index_table SET id='e189_a_tbl';
RELATE finding:select_assist_names_are_static_page_models->cites->finding:list0c_texture_patch_rule SET id='e189_a_arena';
RELATE finding:select_assist_names_are_static_page_models->supersedes->finding:select_assist_panel_no_upload SET id='e189_a_super', note='the panel object and its text mechanism are now read to the record level';
RELATE finding:select_kinds_0d_1c_are_case9_screen->cites->steam_routine:FUN_1406163e0 SET id='e189_k_disp';
RELATE finding:select_kinds_0d_1c_are_case9_screen->cites->global:select_text_slot_0x910 SET id='e189_k_910';
RELATE finding:texture_base_bank_map->cites->steam_routine:FUN_14060c370 SET id='e189_b_loader';
RELATE finding:texture_base_bank_map->cites->steam_routine:FUN_1408435d0 SET id='e189_b_reg';
RELATE finding:texture_base_bank_map->cites->steam_routine:FUN_140845fe0 SET id='e189_b_list';
RELATE finding:texture_base_bank_map->cites->global:select_text_slot_0x910 SET id='e189_b_910';
RELATE finding:texture_base_bank_map->cites->finding:texbank_tcw_rule SET id='e189_b_prior';
RELATE finding:select_assist_names_are_static_page_models->cites->source:select_assist_decompiles_20260906 SET id='e189s_a0';
RELATE finding:select_assist_names_are_static_page_models->cites->source:select_assist_anchor_read_20260906 SET id='e189s_b0';
RELATE finding:select_kinds_0d_1c_are_case9_screen->cites->source:select_assist_decompiles_20260906 SET id='e189s_a1';
RELATE finding:select_kinds_0d_1c_are_case9_screen->cites->source:select_assist_anchor_read_20260906 SET id='e189s_b1';
RELATE finding:texture_base_bank_map->cites->source:select_assist_decompiles_20260906 SET id='e189s_a2';
RELATE finding:texture_base_bank_map->cites->source:select_assist_anchor_read_20260906 SET id='e189s_b2';

UPDATE finding:select_assist_names_are_static_page_models SET status='confirmed';
UPDATE finding:select_kinds_0d_1c_are_case9_screen SET status='confirmed';
UPDATE finding:select_assist_panel_no_upload SET status='superseded';

UPDATE finding:texture_base_bank_map SET status='confirmed';
