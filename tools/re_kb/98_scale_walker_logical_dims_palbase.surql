USE NS re DB kb;

-- ============================================================================
-- 98_scale_walker_logical_dims_palbase.surql
-- 2026-09-02 — Two Steam D3D measurements during Storm Lightning Storm answered
-- from the ROM: (Q1) the 32x88 quad for a 32x128 part = GFX1 LOGICAL dims, no
-- per-node reveal/clip; (Q2) char+0x12E is the PALETTE-BANK BASE (slot table),
-- NOT a hit-flash word; sid 0x8000 on a fighter = scale walker on GFX2 cell 0.
-- Sources: marvelous2 bank03/bank04/bank0e/bank13 (label==PC), PL2A/PL34/PL17
-- DAT segments (dasm_PLDAT/Output), atlas/chars/PL34.json + PL2A.json.
-- Apply after 97_rotation_path_node104.surql, then 07_dedup_edges.surql.
-- ============================================================================

UPSERT source:marv_scale_walker_dims_disasm SET kind='disassembly',
  ref='_marv_re/build/bank03.asm loc_8c0348c8 (10800-11245): UV span fdiv 10971-10995 (hdr[0]/hdr[2] -> stack@0x2C, hdr[1]/hdr[3] -> stack@0x30), scaled 10997-11008 (@0x1C=Uspan*node+0xEC, @0x20=Vspan*node+0xF0); alloc index 10934-10967 (r5=*(node+0x180)+0x220+u16 tmpl[(*(node+0x244)&0x1FF)*2]; idx=u8 r5[ctr] + (u8 node+0x1A4<<8) + 0x390); mirror flags 11045-11069 / 11113-11129 (0x4000 -> PCW|0x20 + V flip, 0x8000 -> PCW|0x10 + U flip); dispatcher loc_8c034bea 11259-11300 (mask loc_8c034c34=0x8000); Render Main Sprite loc_8c03093c calls loc_8c034bea via pool loc_8c030aec (bank03:1442)',
  note='Scale walker reads NO per-node clip/reveal/timer field. Per-node inputs: +0x144 sid, +0x160 GFX2, +0x15C GFX1, +0x104 angle, +0x110 facing, +0xE0/+0xE4 or +0x134/+0x136, +0xE8, +0x108, +0xEC/+0xF0 scale, +0x180 template, +0x244 template cell select, +0x1A4 slot namespace byte.';

UPSERT source:pl_dat_offline_headers SET kind='data',
  ref='dasm_PLDAT/Output/PL2A_DAT/PL2A_DAT_GFX_DATA_00.BIN (GFX1) / _01.BIN (GFX2); PL34_DAT; PL17_DAT; atlas/chars/PL34.json sid 0, PL2A.json sid 0/28',
  note='PL2A GFX1 hdr sel156=[4,11,4,16] (logical 32x88, storage 32x128), sel157=[2,4,2,4], sel158=[1,2,1,2]. PL2A GFX2 cell28 = 3 recs [(25,14,0x20,157),(-16,4,0x20,156),(-32,-15,0x20,158)]. Cells 6..29 are 24 DISTINCT bolt arts (different sels, logical heights 8..88px), e.g. cell24/25 single part [3,10,4,16], cell26 [2,9,2,16]. PL34 GFX2 cell0 = 22 recs (sels 10,1,5,8,18,14,19,2,12,4,20,7,...) = the 192x166 pointing pose in PL34.json sid 0; PL2A cell0 = 15 recs = the tornado art. PL17 GFX1 sel54 = [1,1,1,1] (8x8) — does NOT match the Steam-side "part 54 32x64" label.';

UPSERT source:steam_d3d_lstorm_measurement SET kind='measurement',
  ref='Steam state+D3D same-frame capture (d3dcap/replay lane), Storm Lightning Storm; two bolt nodes sid 0x801C (cell 28) each submit a 32x88 quad for part 156; fighter slot 4 (PL34 Sentinel) sid=0x8000, DC+0x12C=1, DC+0x104(Steam+0x148)=0, DC+0x12E(Steam+0x172)=0x0030',
  note='Steam offsets = DC + 0x44 in the 0xDC..0x172 range (user-confirmed at 0xDC->0x120 and 0x12E->0x172).';

-- ---------------------------------------------------------------------------
UPSERT finding:scale_walker_quad_is_logical_dims SET date='2026-09-02', confidence='high', status='confirmed',
  contribution_candidate=true,
  statement='The scale walker (bit15 sids, loc_8c0348c8) draws ONE quad per GFX2 record sized LOGICAL dims x scale: W = hdr[0]*8*node+0xEC, H = hdr[1]*8*node+0xF0, sampling U in [0, hdr[0]/hdr[2]) and V in [0, hdr[1]/hdr[3]) of the STORAGE texture (hdr = GFX1 part header [lw][lh][sw][sh]; bank03:10971-11008). PL2A part 156 = [4,11,4,16] => 32x88 of a 32x128 storage texture, EXACTLY the Steam-measured quad. This is a STATIC per-part property; there is NO per-node clip height, NO per-node tile budget, NO progressive texel reveal for scale-walker sprites — the walker reads no such field (source:marv_scale_walker_dims_disasm). Two nodes with the same sid draw identical geometry.',
  note='The visible bolt "growth" on DC is a CELL SEQUENCE (Storm GFX2 cells 6..29 are 24 distinct bolt arts with different parts/heights), not a texture reveal. Texel rows >= 88 of the 32x128 storage block are outside the sampled V span; anything seen there on a Steam page is other packed data, not part 156.';

UPSERT finding:effect_sheet_registered_once_per_slot SET date='2026-09-02', confidence='high', status='confirmed',
  contribution_candidate=true,
  statement='Effect-sheet textures are decoded and registered ONCE per character slot by bank03 loc_8c033d78 (9092-9370), NOT per frame and NOT per node. It walks GFX2 cells 0..N-1, N = u16 bank0e.loc_8C0EFD4A[char_id] (Storm 0x2A -> 0x42=66 cells, Sentinel 0x34 -> 0x27=39, Cable 0x17 -> 0x28=40), dedups sels through the list 0x8c1f9d9c, writes the per-slot TEMPLATE at *(char+0x180) (bank13.loc_8c135594[slot], 0x3000 arena): u16 tmpl[cell] = first record index; u8 tmpl[0x220+recordIndex] = alloc-index low byte; u32 tmpl[0xA20+k] = palette id; 0x10-byte tmpl[0xE20+k] = [W*8 u16][H*8 u16][0x0502 u32][byte offset][pal]; decodes each unique part via loc_8c0354c0 (LZSS) into 0x0CE60000, then bank11.loc_8c11b800(r4 = (slot<<8)+0x390) + bank12.loc_8c1240a0(entries, palids) register the VRAM textures; counts to 0x8c26aa24[slot*2], bytes to 0x8c26aa34[slot*4]. Callers: bank03 loc_8c03dd6c (6-slot loop at match init, 33244-33262) and bank03 34675 (per-char reload). CONSEQUENCE: on DC the texel content of a scale-walker sprite is fixed for the match; a per-node difference can only come from a DIFFERENT texture slot being addressed: alloc = u8 tmpl[0x220 + u16 tmpl[(*(node+0x244)&0x1FF)*2] + ctr] + (u8 node+0x1A4 << 8) + 0x390 -> idxtab -> rectab (bank03:10934-10967).',
  note='FALSIFIABLE on Steam: for the two same-sid bolt nodes read DC+0x1A4 (slot byte), DC+0x244 (template cell select, must == sid&0x7FFF for the right parts), DC+0x180 (template base). The node whose page is transparent should differ in one of these (or its idxtab entry is 0xFF/unassigned). Pool-node stride caveat: DC objpool stride is 0x1D0 (re-catalog) so +0x244 on a pool node reads past the node — OPEN whether effect nodes live in a wider pool or +0x244 is a deliberate overlap.';

UPSERT field:char_0x12E SET name='pal_bank_base', offset='+0x12E', owner='char_struct', type='u16', class='logical',
  semantics='PVR palette-bank base of this character slot = bank13.loc_8c1355d4[slot] = {0x10,0x18,0x20,0x28,0x30,0x38} for slots 0..5 (P1C1,P2C1,P1C2,P2C2,P1C3,P2C3). 0x30 = slot 4 (P1C3) at rest — NOT a flash state. Per-part palette bank = +0x12E + ((rec.flags & 0x3FF) >> 4) when +0x12D != 0 (descriptor builders bank03 loc_8c033ba8 8811-8845 and loc_8c033e3e 9228-9244; +0x12D is set to 0xFF by bank04 loc_8c04ec0c/loc_8c04ed68/loc_8c04f0b8). Palette staging line for the char = 0x8c2659dc + (+0x12E)*0x30 (loc_8c035162 bank03:12124-12137, 8 lines per char).',
  writers='bank03 loc_8c031fa0 4648-4653 (= loc_8c1355d4[char+0x524 Control_ID]); bank04 loc_8c04ec0c 36031-36043 (= loc_8c1355d4[char+0x1A4]); reset in loc_8c035162 mode 0 (12201-12206) and mode 1 (12457-12462) to loc_8c1355d4[char+0x1A4].',
  status='confirmed';

UPSERT field:char_0x1A4 SET name='slot_index', offset='+0x1A4', owner='char_struct', type='u8', class='logical',
  semantics='Character slot index 0..5 (== Control_ID char+0x524 for fighters). Indexes loc_8c1355d4 for the palette base and forms the effect alloc-index namespace (slot<<8)+0x390 used by both loc_8c033d78 (registration) and loc_8c0348c8 (scale walker).',
  status='confirmed';

UPSERT field:char_0x180 SET name='effect_template_ptr', offset='+0x180', owner='char_struct', type='ptr', class='engine',
  semantics='Per-slot effect display-list template base, bank13.loc_8c135594[slot] (0x0C565000, 0x0C955000, 0x0C6B5000, 0x0CAA5000, 0x0C805000, 0x0CBF5000, 0x0CD45000); written by loc_8c031fa0 bank03:4641-4642. Layout per finding:effect_sheet_registered_once_per_slot.',
  status='confirmed';

UPSERT field:char_0x148 SET name='cell_counters', offset='+0x148', owner='char_struct', type='u32 x3 (+0x148/+0x14C/+0x150)', class='engine',
  semantics='Per-keyframe counters zeroed on every cell load: load-anim loc_8c034c38 bank03:11343-11350 and anim tick loc_8c034dc6 bank03:11549-11555 (pool 0x0148). Value 0 = fresh cell; not a render gate.',
  status='confirmed', confidence='medium';

UPSERT finding:kb33_hitflash_selector_correction SET date='2026-09-02', confidence='high', status='confirmed',
  statement='CORRECTION to source:hitflash_palette_chain (re_kb/33): loc_8c035162 does NOT read char+0x12E as the hurt-effect-type selector. r14 = u16 char+0x12E is the palette-bank base used to locate the staging line (0x8c2659dc + base*0x30, bank03:12124-12137); the MODE dispatch cmp/eq 0x00..0x0A is on the r5 ARGUMENT (bank03:12139-12200). Every bank04 caller found passes r5=1 (e.g. loc_8c0442cc 9959 site, loc_8c046d46, loc_8c047b4c, loc_8c048c7c, ...) except bank04 loc_8c04c02c which passes r5=6 when s16 char+0x1E > 0 (bank04 29290-29300) then r5=1 after clearing +0x1E to 0xFF. So the flash VARIANT lives in the caller state (char+0x1E and the bank04 hit-reaction routines), and the Oracle-hook note "0x12E: idle 0x10/0x18, 0x01..0x0A flash variants" conflates the r5 mode with the field.',
  note='Values 0x10/0x18/0x20/0x30 seen on Steam are exactly slots 0,1,2,4 of loc_8c1355d4.';

UPSERT finding:fighter_sid_0x8000_scale_walker_cell0 SET date='2026-09-02', confidence='high', status='confirmed',
  statement='A fighter whose +0x144 == 0x8000 is routed by Render Main Sprite loc_8c03093c -> dispatcher loc_8c034bea (sel & 0x8000 != 0, bank03:11265-11272) to the SCALE walker loc_8c0348c8 on its OWN GFX2 cell 0 with the slot effect template. Cell 0 is a REAL asset, not a placeholder: PL34 (Sentinel) cell 0 = 22 records (sels 1..20 range) = the 192x166 pointing pose (atlas PL34.json sid 0); it lies inside the registered effect range (N=39 cells) so its textures are resident. With +0x104==0 the sprites are axis-aligned single quads (logical dims x +0xEC/+0xF0) anchored at floor(+0xE0)/floor(+0xE4); same submit loc_8c1244b0 and same palette-indexed path as the Lightning bolts. Expected Steam quads for Sentinel cell 0: e.g. sel 1 [14,4,16,4] -> 112x32, sel 5 [10,2,16,2] -> 80x16, sel 10 [1,1,1,1] -> 8x8, sel 14 [2,2,2,2] -> 16x16, sel 8 [1,4,1,4] -> 8x32.',
  note='OPEN: the game-state reason a fighter carries sid 0x8000 during an opponent Lightning Storm (DAT anim tables here are not decodable offline — the dasm_PLDAT ANIMATION_DATA segment is 1.8KB; S_PL34.asm carries 0x8000 only inside data record DAT_ce32e28 {3,0x8000,4,0,0x1000,0x1400,0x400,0x60}, referenced from PTR_ce3047c). If the 22 quads are absent on Steam, check +0x12C, +0xEC/+0xF0 and the slot-4 template bytes (0xFF terminator => unassigned idxtab => nothing drawn). The "135 vs_world RGBA draws" are the 3D machine (stage / list-7/8 POL effects), unrelated to the body.';

UPSERT finding:steam_cable_part54_mismatch SET date='2026-09-02', confidence='medium', status='open',
  statement='The Steam-side label "Cable beam sid cell 25: part 54 32x64" does not match PL17 (Cable, char_id 0x17): GFX1 sel 54 = [1,1,1,1] (8x8) and GFX2 cell 25 records use sels 144..167. Either the Steam capture used a different PL/char mapping or the part index was mis-read; re-derive before using it as evidence for a reveal mechanism.';

-- edges
RELATE finding:scale_walker_quad_is_logical_dims->cites->source:marv_scale_walker_dims_disasm SET kind='cites';
RELATE finding:scale_walker_quad_is_logical_dims->cites->source:pl_dat_offline_headers SET kind='cites';
RELATE finding:scale_walker_quad_is_logical_dims->cites->source:steam_d3d_lstorm_measurement SET kind='cites';
RELATE finding:scale_walker_quad_is_logical_dims->about->routine:loc_8c0348c8 SET kind='about';
RELATE finding:effect_sheet_registered_once_per_slot->cites->source:marv_scale_walker_dims_disasm SET kind='cites';
RELATE finding:effect_sheet_registered_once_per_slot->about->routine:loc_8c0348c8 SET kind='about';
RELATE finding:effect_sheet_registered_once_per_slot->about->field:char_0x180 SET kind='about';
RELATE finding:effect_sheet_registered_once_per_slot->about->field:char_0x1A4 SET kind='about';
RELATE finding:kb33_hitflash_selector_correction->about->field:char_0x12E SET kind='about';
RELATE finding:kb33_hitflash_selector_correction->about->source:hitflash_palette_chain SET kind='about';
RELATE finding:fighter_sid_0x8000_scale_walker_cell0->cites->source:pl_dat_offline_headers SET kind='cites';
RELATE finding:fighter_sid_0x8000_scale_walker_cell0->cites->source:steam_d3d_lstorm_measurement SET kind='cites';
RELATE finding:fighter_sid_0x8000_scale_walker_cell0->about->routine:loc_8c0348c8 SET kind='about';
RELATE finding:fighter_sid_0x8000_scale_walker_cell0->about->finding:rotation_path_node104 SET kind='about';
RELATE finding:steam_cable_part54_mismatch->cites->source:pl_dat_offline_headers SET kind='cites';
