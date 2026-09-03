USE NS re DB kb;

-- ============================================================================
-- 38_palette_source.surql
-- 2026-09-03 -- WHERE the palette page a character draw binds comes from (Steam
-- MvC2, Ghidra decompile of the sprite submit chain), why the v3/v4 tape `pal`
-- (32 B at *(node+0x1B8) = DatPal cl+0x4C) is the WRONG read (it is costume 0
-- row 0 of the character palette block), and the deterministic gates.
-- Doc: mvc-live-skins-quarters/docs/PALETTE-SOURCE-GHIDRA.md ; gate tool:
-- mvc-live-skins-quarters/d3dcap/replay/palette_gate.py.
-- Apply with tools/re_kb/apply_seed.py after the documented backup. Idempotent.
-- ============================================================================

UPSERT source:ghidra_palette_chain SET kind='ghidra', strength='code',
  ref='mvc_dump.bin via :8080 bridge decompile_function: FUN_1406146d0, FUN_140614130, FUN_1406144f0, FUN_140613390, FUN_140845670->FUN_140048940->FUN_14004d350, FUN_140048970 (caller FUN_140843eb0), FUN_140612180, FUN_140845fe0, FUN_140845e20, FUN_1406129f0; DAT_140a6d188 bytes read from mvc_dump.bin',
  note='Steam palette chain read end to end: staging lines blk+0x1040+bank*0x38 (+8 u32 flag 1 raw/2 dim/0 uploaded, +0x18 16 x u16 ARGB4444) -> FUN_140613390 uploads flagged lines (dim = (c>>1)&0x777|c&0xF000) via FUN_140845670(bank<<4, cols, 16) -> FUN_14004d350 writes u32 entries at *(DAT_140acd3a8)+0xd8d04+entry*4 (host PALETTE_RAM, 1024 entries) -> FUN_140048970 (from the flush FUN_140843eb0) flips *(dev+0xd8d00), memcmps each 16-entry bank against the per-flip copy at dev+0xd9d04+flip*0x1000 and Maps the changed bank into its own D3D texture dev+0xd8500+(bank+flip*0x40)*8 (0x40 B, or 0x400 B for banks == 0 mod 16). Per draw: slot table ctx+0x1e00a0+idx*0x18 +4 = u16 palette bank (written by FUN_140845fe0 from the FUN_140612180 palids = ((node+0x172)*16 + rec.flags)>>4 & 0x3F when node+0x171 != 0), packed by FUN_140845e20 as (bank+1)<<16|idx into the polygon word -> t1 = LUT[bank].';

UPSERT source:marv_palette_chain SET kind='disassembly', strength='code',
  ref='_marv_re/build/bank03.asm loc_8c035000 (11891-11990), loc_8c035162 (12114-12215), loc_8c03544c (12588-12660), 0x8c2659dc refs 8608/8799/12271/12600/12658/13417; bank04.asm loc_8c04f0ec 36742-36747 (plmem[0x25] = plmem[0x52d]); bank1a.asm loc_8c1A8092 (9729, lit 0xA05F9000)',
  note='DC side of the same chain: loc_8c035000 r5 = (u8 char+0x25 palid)<<8 + Dat_Pal (char+0x164), char_id (+0x1) special cases 1/0x22/0x37/0x14 with +0x202/+0x30 flags (+0x300 palette extras); loc_8c035162 staging line = 0x8c2659dc + (char+0x12E)*0x30 (+8 flag=1, +0x10 16 colours), mode dispatch on r5 0..0xA; loc_8c03544c copies r7 rows from Dat_Pal+0x300+... into lines base+r5..; bank1a VBlank writes PVR PALETTE_RAM 0xA05F9000.';

UPSERT source:palette_gate_capgate SET kind='measurement', strength='gate',
  ref='mvc-live-skins-quarters/d3dcap/replay/palette_gate.py 4445 4505 5168 7279 (capgate packs + state dumps, 2026-09-03)',
  note='Frame 4445: 240/240 indexed draws bind a LUT byte-equal (R8G8B8A8 = nibble*17) to a staged line (banks 0x20/0x23 = slot 2 rows 0/3, 0x28/0x29 = slot 3 rows 0/1). Frame 7279: 134/134 (0x20, 0x28). Frame 4505: 120/144 -- the 24 misses are one texture object (748E0CE0) holding the PREVIOUS content of bank 0x23 while the staging line changed and no flag is pending. Frame 5168: state dump only, no .pack, not gateable. Total 494/518. The captures are themselves a same-character mirror (slots 0/3 PL34, 1/2 PL2A, 4/5 PL2C with variants 1/0).';

UPSERT source:palette_gate_ttd SET kind='measurement', strength='gate',
  ref='mvc-live-skins-quarters/d3dcap/replay/palette_gate.py --ttd d3dcap/ttd/runs/20260903-000941/pre (blk.bin + dcram.bin dumped in the same 0.33 s window)',
  note='6 fighters x 8 rows: staged row k == dcram[DatPal + (node+0x39)*0x100 + k*0x20] on 46/48 (the two misses are slot 0 PL2A rows 1-2, the engine-authored lightning sub-palette rewrite already in finding:gsta_storm_no_body_divergence). For all 5 fighters with variant != 0, DatPal+0 (the v3/v4 tape pal) != the rendered row 0: 5/5.';

-- Steam routines (pairs) -------------------------------------------------------
UPSERT steam_routine:FUN_1406146d0 SET addr='0x1406146d0', name='FUN_1406146d0', role='palette staging (per-char palette mode dispatch) = loc_8c035162',
  note='CONFIRMED both read. line = blk+0x1040+(node+0x172)*0x38; mode 0: +0x172 = DAT_140a6d188[node+0x230] (0x10+8*slot), row 0 = FUN_140614130(node), rows 1..7 = *(node+0x1B8)+0x20+(node+0x39)*0x100+k*0x20; mode 2/10 set flag 2 (dim)/1 on all 8 lines; modes 1,3..7 copy +0x4c (+1/+2) rows from DatPal+0x600.. (flash/extra palettes, frame parity G+0x1C); 8/9 fill white/0xff00.';
UPSERT steam_routine:FUN_140614130 SET addr='0x140614130', name='FUN_140614130', role='palette row-0 source select = loc_8c035000',
  note='CONFIRMED both read. default DatPal + (node+0x39)*0x100; char 1/0x14 with +0x2b2, 0x22 with +0xf0, 0x37 with +0x374 -> DatPal + ((+0x4c+6)*0x80 + (var+8|+6|+0x14)*0x10 | var*0x60)*2 (the +0x300-entry extras block).';
UPSERT steam_routine:FUN_1406144f0 SET addr='0x1406144f0', name='FUN_1406144f0', role='staging line writer (count rows from src, flag=1; remaining lines flag=1)', note='INFERRED pair of loc_8c03544c (DC computes the source inline).';
UPSERT steam_routine:FUN_140613390 SET addr='0x140613390', name='FUN_140613390', role='per-frame render begin: frame parity, LayerZ, texture-table base swap, palette staging upload',
  note='CONFIRMED read. Called from FUN_14060a1f0 <- FUN_140608a00 (per-frame) and at init. Loops 0x80 lines blk+0x1040..0x2C40: flag 1 copies raw, flag 2 dims ((c>>1)&0x777)|(c&0xF000), then FUN_140845670(line[0]<<4, scratch blk+0x2C40+i*0x20, line[1]) and flag=0. DC pair of the upload = bank1a loc_8c1A8092 (INFERRED).';
UPSERT steam_routine:FUN_14004d350 SET addr='0x14004d350', name='FUN_14004d350', role='host PALETTE_RAM write: u16 -> u32 at *(DAT_140acd3a8)+0xd8d04+entry*4', note='CONFIRMED read (SIMD expander; format arg 0 from FUN_140613390). Expansion measured on the LUT pages: ARGB4444 -> R8G8B8A8 with nibble*17.';
UPSERT steam_routine:FUN_140048970 SET addr='0x140048970', name='FUN_140048970', role='D3D palette LUT refresh: per-bank textures dev+0xd8500+(bank+flip*0x40)*8, flip dev+0xd8d00', note='CONFIRMED read. Sole caller FUN_140843eb0 (frame flush) BEFORE it executes the queued draws; the draws still bound a bank content one captured frame old on 4505 -> the bound LUT lags a mid-frame staging change by >= 1 frame (double-buffered textures, INFERRED mechanism).';
UPSERT steam_routine:FUN_140845fe0 SET addr='0x140845fe0', name='FUN_140845fe0', role='texture registration: slot table ctx+0x1e00a0+idx*0x18, writes +4 = palette bank from the palids array', note='CONFIRMED read. Called by FUN_140612180 with (entries, template+0xA20 palids) after FUN_1408458a0(slot*0x100+0x390).';
UPSERT steam_routine:FUN_140845e20 SET addr='0x140845e20', name='FUN_140845e20', role='quad submit: reads slot table +4 (palette bank) and packs (bank+1)<<16|texIdx into the polygon word before FUN_1408436a0(0xc)', note='CONFIRMED read. FUN_1406129f0 itself carries NO palette: it passes only the texture-table index (blk+0x32bec + node+0x120 + tile, or slot*0x100+0x390+alloc).';

RELATE steam_routine:FUN_1406146d0->recompiles:FUN_1406146d0_loc_8c035162->routine:loc_8c035162 SET confidence='confirmed', method='both-read', evidence='same mode dispatch 0..0xA, same line stride (0x30->0x38), same +0x12E/+0x172 base, same row sources';
RELATE steam_routine:FUN_140614130->recompiles:FUN_140614130_loc_8c035000->routine:loc_8c035000 SET confidence='confirmed', method='both-read', evidence='palid<<8 + Dat_Pal; char_id 1/0x14/0x22/0x37 special cases; +0x202->+0x2b2, +0x30->+0x4c';
RELATE steam_routine:FUN_1406144f0->recompiles:FUN_1406144f0_loc_8c03544c->routine:loc_8c03544c SET confidence='high', method='callgraph', evidence='row copier into staging lines base+r5.., flag=1 per line; DC computes src inline';
RELATE steam_routine:FUN_140612180->recompiles:FUN_140612180_loc_8c033d78->routine:loc_8c033d78 SET confidence='confirmed', method='both-read', evidence='palid = ((+0x172)*16 + rec.flags)>>4 & 0x3F when +0x171 != 0 == DC +0x12E + ((flags&0x3FF)>>4) when +0x12D != 0 (bank03 9228-9244); template +0x220/+0xA20/+0xE20 layout identical';

-- fields / globals ------------------------------------------------------------
UPSERT field:char_0x25 SET name='palid', offset='+0x25', owner='char_struct', type='u8', class='logical',
  semantics='Costume / colour-variant index (0..5). Copied from char+0x52D at fighter init (bank04 loc_8c04f0ec). Selects the 0x100-byte block (8 rows x 16 x u16 ARGB4444) at Dat_Pal + palid*0x100 (loc_8c035000). Steam = node+0x39 (FUN_140614130 / FUN_1406146d0).',
  status='confirmed';
UPSERT field:steam_node_0x39 SET name='palid', offset='+0x39', owner='steam_object', type='u8', class='logical',
  semantics='Steam recompile of DC char+0x25: costume variant 0..5. Read by FUN_140614130 and FUN_1406146d0 (DatPal + var*0x100). In the 0.3.3x reader this is real_base+0x39 = cl-0x133 (cl = blk+0x3F24+i*0x738 is 0x16C inside the object). NOTE: reader OFF_COLOR cl+0x6 = node+0x172 is the palette-bank BASE (0x10..0x38), not the variant.',
  status='confirmed';
UPSERT field:char_0x164 SET name='Dat_Pal', offset='+0x164', owner='char_struct', type='ptr', class='asset',
  semantics='Pointer to the character palette block in the DAT bank: 6 costume blocks of 0x100 B (8 rows x 32 B) at +0, extras/flash palettes from +0x600 (DC loc_8c035000/loc_8c03544c). Steam = node+0x1B8 (8-B host pointer into the DC-RAM host image; = reader cl+0x4C DatPal). It is a load-time SOURCE, not the rendered palette: rendered = staging rows (runtime rewrites, e.g. Storm rows 1-2).',
  status='confirmed';
UPSERT global:steam_blk_0x1040 SET name='palette_staging_lines', addr='blk+0x1040..0x2C40', note='128 lines x 0x38: +0 u32 bank idx, +4 u32 count (16), +8 u32 flag (1 raw pending / 2 dim pending / 0 uploaded), +0x18 16 x u16 ARGB4444. Fighter slot s owns lines 0x10+8*s .. +7 (blk+0x13C0 + s*0x1C0, 0x540 B for all six). DC = 0x8c2659dc + bank*0x30 (+8 flag, +0x10 colours). Scratch for the upload at blk+0x2C40+i*0x20.', status='confirmed';
UPSERT global:steam_DAT_140a6d188 SET name='slot_palette_bank_base', addr='0x140a6d188', note='u8[8] = 10 18 20 28 30 38 10 18 (read from mvc_dump.bin) = DC bank13.loc_8c1355d4. node+0x172 = table[node+0x230].', status='confirmed';
UPSERT global:steam_dev_palette_ram SET name='host_PALETTE_RAM', addr='*(DAT_140acd3a8)+0xd8d04', note='1024 x u32 expanded palette entries (Steam analogue of PVR 0xA05F9000). LUT textures: 128 x 8-B D3D texture ptrs at dev+0xd8500 (bank + flip*0x40); flip u32 at dev+0xd8d00; per-flip previous copies at dev+0xd9d04 + flip*0x1000. NOT inside blk and NOT in ctx (DAT_142ef0ab0) -> no capture holds it; the agent would need the pointer chase to read it.', status='confirmed';
UPSERT global:steam_ctx_0x1e00a4 SET name='texture_slot_palette_bank', addr='ctx+0x1e00a0+idx*0x18 (+4)', note='u16 palette bank of a registered texture slot (-1 = none); written by FUN_140845fe0, read by FUN_140845e20 at every quad submit.', status='confirmed';

-- findings --------------------------------------------------------------------
UPSERT finding:palette_source_rule SET date='2026-09-03', confidence='high', status='confirmed', contribution_candidate=true,
  statement='THE PALETTE A CHARACTER DRAW BINDS IS NOT READ FROM THE FIGHTER: the sprite submit FUN_1406129f0 (= loc_8c034bea) passes only a texture-table index; FUN_140845e20 looks up the slot table entry (ctx+0x1e00a0+idx*0x18) and its +4 palette BANK (set at sheet registration by FUN_140612180 to ((node+0x172)*16 + rec.flags)>>4 & 0x3F, i.e. slot base 0x10+8*slot + record sub-row); the D3D layer binds t1 = the per-bank 256x1 R8G8B8A8 LUT texture that FUN_140048970 refreshes from host PALETTE_RAM; PALETTE_RAM is filled by FUN_140613390 (per frame) from the STAGING LINES blk+0x1040+bank*0x38 that FUN_1406146d0 (= loc_8c035162) fills per character: row 0 = FUN_140614130 (= loc_8c035000) = DatPal + (node+0x39)*0x100 with char-specific extras, rows 1..7 = DatPal + var*0x100 + k*0x20, flash modes rewrite/dim them. GATES: capgate 494/518 draws byte-exact (4445 240/240, 7279 134/134, 4505 120/144; 5168 has no pack), TTD same-moment 46/48 rows.',
  note='CONFIRMED on both binaries. The LUT expansion (nibble*17, RGBA order) is measured on the captured pages, not derived from the SIMD in FUN_14004d350.';
UPSERT finding:tape_pal_is_costume0_row0 SET date='2026-09-03', confidence='high', status='confirmed', contribution_candidate=true,
  statement='The v3/v4 tape field pal (agent reader.rs read_pal(*(H+0x1B8)) = 32 B at DatPal+0) is costume 0 row 0 of the character palette block, NOT the rendered palette: DatPal+0 != rendered row 0 for 5/5 fighters with variant != 0 (TTD gate). This is the mirror-match symptom (both same-character fighters get costume 0) and it is equally wrong for any non-mirror fighter on a non-default colour. The memory note "DatPal cl+0x4c != the rendered palette" (mvc-live-skins-mirror-re) was correct; its example bytes are Storm variant 1 (rendered) vs variant 0 (DatPal+0).',
  note='Fix = ship the staging rows (engine-resolved, includes runtime rewrites), not a DatPal-derived row: blk+0x13C0 + slot*0x1C0, 8 lines x 0x38, colours at +0x18, flag at +8. Per-part row = rec.flags>>4 (the atlas asm row_of). See docs/PALETTE-SOURCE-GHIDRA.md section 4 for the reader diff and the tape_to_seq consumer.';
UPSERT finding:mirror_colour_selector SET date='2026-09-03', confidence='high', status='confirmed',
  statement='The per-fighter colour set is selected by the palid byte: DC char+0x25 (copied from char+0x52D at fighter init, bank04 loc_8c04f0ec), Steam node+0x39. A same-character mirror renders two palettes because the two objects carry different +0x39 (captures: slots 1/2 PL2A var 0/1, 0/3 PL34 var 1/0, 4/5 PL2C var 1/0), each staged into its OWN bank range (slot base 0x10+8*slot) from its OWN DatPal (distinct DAT bank per slot). The character-select rule that assigns +0x52D (forced alternate when both pick the same colour, or the player choice) was NOT traced on Steam: UNKNOWN; the tape does not need it once it ships the resolved rows.',
  note='Reader caveat: OFF_COLOR cl+0x6 = node+0x172 = bank base (0x10/0x18/..), which happens to differ per slot and so masqueraded as a variant discriminator.';
UPSERT finding:lut_lags_staging_one_frame SET date='2026-09-03', confidence='medium', status='inferred',
  statement='When a staging line changes within a frame, the LUT texture the frame draws bind still holds the previous content (frame 4505: 24/144 draws bound the 4445-era bank 0x23 while the staging line had changed and no flag was pending). Mechanism INFERRED: FUN_140048970 flips dev+0xd8d00 and refreshes the NEW flip set while the executing draws bind the other set (double-buffered LUT textures). Exact lag (1 vs 2 frames) UNKNOWN: needs consecutive-frame packs with a palette change between them (capgate has none).',
  note='Consumer implication: a renderer using frame-N staging rows is <= 1-2 frames early on palette changes (hit-flash dims, Storm aura rows). tape_to_seq --pal-lag N selects earlier rows; default 0.';

RELATE finding:palette_source_rule->cites->source:ghidra_palette_chain SET kind='cites';
RELATE finding:palette_source_rule->cites->source:marv_palette_chain SET kind='cites';
RELATE finding:palette_source_rule->cites->source:palette_gate_capgate SET kind='cites';
RELATE finding:palette_source_rule->cites->source:palette_gate_ttd SET kind='cites';
RELATE finding:palette_source_rule->about->routine:loc_8c034bea SET kind='about';
RELATE finding:palette_source_rule->about->routine:loc_8c035162 SET kind='about';
RELATE finding:palette_source_rule->about->routine:loc_8c035000 SET kind='about';
RELATE finding:palette_source_rule->about->global:steam_blk_0x1040 SET kind='about';
RELATE finding:palette_source_rule->about->global:steam_dev_palette_ram SET kind='about';
RELATE finding:tape_pal_is_costume0_row0->cites->source:palette_gate_ttd SET kind='cites';
RELATE finding:tape_pal_is_costume0_row0->cites->source:ghidra_palette_chain SET kind='cites';
RELATE finding:tape_pal_is_costume0_row0->about->field:char_0x164 SET kind='about';
RELATE finding:tape_pal_is_costume0_row0->about->field:steam_node_0x39 SET kind='about';
RELATE finding:mirror_colour_selector->cites->source:marv_palette_chain SET kind='cites';
RELATE finding:mirror_colour_selector->cites->source:palette_gate_capgate SET kind='cites';
RELATE finding:mirror_colour_selector->about->field:char_0x25 SET kind='about';
RELATE finding:mirror_colour_selector->about->field:steam_node_0x39 SET kind='about';
RELATE finding:lut_lags_staging_one_frame->cites->source:palette_gate_capgate SET kind='cites';
RELATE finding:lut_lags_staging_one_frame->cites->source:ghidra_palette_chain SET kind='cites';
RELATE finding:lut_lags_staging_one_frame->about->global:steam_dev_palette_ram SET kind='about';
