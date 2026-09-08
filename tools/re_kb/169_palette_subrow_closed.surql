-- 169_palette_subrow_closed.surql -- CLOSES the "palette sub-row" gap named in the tape's own
-- pals_enc string ("node+0x12d / +0x12e"). 2026-09-05. RE METHOD steps 1-4 all applied:
-- SH4 anchors loc_8c033d78 / loc_8c033b0a / loc_8c031fa0 / loc_8c04ec0c / loc_8c04f0ec /
-- loc_8c035162 / loc_8c035000 / loc_8c033aca ported to Steam by function matching, globals
-- translated (0x8c2659dc staging -> blk+0x1040; loc_8c1355d4 -> DAT_140a6d188;
-- loc_8c13565c -> DAT_140a6d910), every pair read on BOTH sides from the raw dump.
USE NS re DB kb;

UPSERT source:marv_bank03_palette_sites SET kind='disassembly', strength='code',
  ref='_marv_re/build/bank03.asm loc_8c033d78 (literals loc_8c033f6c=0x012d, loc_8c033f6e=0x03ff, loc_8c033f70=0x012e), loc_8c033b0a/loc_8c033ba8, loc_8c031fa0 (loc_8c032100=0x0524, loc_8c032102=0x012e, loc_8c032104=0x052c, loc_8c03210c=0x052d, loc_8c032124=bank13.loc_8c1355d4, loc_8c032128=bank13.loc_8c13565c), loc_8c033aca (staging base loc_8c033b8c=0x8c2659dc), loc_8c035162 (loc_8c035250=0x012e, loc_8c035254=0x01a4, loc_8c03525c=loc_8c1355d4), loc_8c035000; bank04.asm loc_8c04ec0c and loc_8c04f0ec (both store 0xFF to char+0x12D); bank13.asm loc_8c1355d4 and loc_8c13565c',
  note='marvelous2 NTSC-U SH4 disassembly, hand-labeled. Label name == SH4 PC.';

UPSERT source:steam_dump_palette_sites SET kind='binary', strength='code',
  ref='C:/Users/trist/ghidra_projects/mvc_dump.bin (file offset = VA - 0x140000000), disassembled with capstone directly from the dump, NOT from the re_map cache: FUN_140612180 @0x1406122ac..0x1406122d6, FUN_140612430 @0x1406124e0..0x14061250e, FUN_14060d100 @0x14060d1c1..0x14060d22c, FUN_1406233b0 @0x1406234b4..0x1406234c4, FUN_1406146d0 @0x1406146df with mode table @0x140614b48 and mode-0 handler @0x1406149d8, FUN_1406144f0, DAT_140a6d188, DAT_140a6d910',
  note='Steam MvC2 static recompilation of the same code.';

UPSERT source:rr_tape_palrows_measurement SET kind='measurement', strength='reproduction',
  ref='local tapes C:/Users/trist/AppData/Local/Temp/{rr_pal2,rr_cos0,rr_cos1,rr_f2,rr_split}/play_tape.json.gz (Blackheart cid 53 P1 point, stage 9), matched against the offline PL35 LUT/GFX2 in C:/Users/trist/AppData/Local/Temp/pack_bhdoom/chars/',
  note='Read-only decode of already-captured tapes; no new capture, no live probe.';

UPSERT routine:loc_8c033d78 SET pc='0x8c033d78', bank='bank03', name='GFX2 body sheet registration (per unique part)',
  note='Walks the GFX2 cell list of a sprite id, 8-byte records [dx s16][dy s16][flags u16 at +4][sel u16 at +6]. For each NEWLY registered part computes the palette bank: if char[0x12D]!=0 then (((u16)char[0x12E]*16 + flags) & 0x03FF) >> 4 else (flags & 0x03FF) >> 4, and stores it in the sheet slot table at base+0xA20+4*i. Literals: 0x012d gate, 0x03ff mask, 0x012e base.';

UPSERT routine:loc_8c033b0a SET pc='0x8c033b0a', bank='bank03', name='second sheet-registration path (same palette rule)',
  note='At loc_8c033ba8 it runs the identical gate/base/mask/shift sequence on the same 8-byte record layout. Two independent SH4 sites agree on the rule.';

UPSERT steam_routine:FUN_140612430 SET address='0x140612430', name='second sheet-registration path',
  note='At 0x1406124e0: cmp byte [rcx+0x171],0 / shr ebx,4 or (movsx ebx, word [rcx+0x172]; shl 4; add flags; sar 4); and ebx,0x3f. Recompiler folded (&0x3FF then >>4) into (>>4 then &0x3F) -- arithmetically identical.';

UPSERT finding:palette_subrow_selector SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The palette sub-row is a PER-PART field of the GFX2 assembly record, not a per-node field',
  statement='Bank bound for one drawn part = (node+0x12D != 0) ? ((((u16)node+0x12E) * 16 + rec.flags) & 0x03FF) >> 4 : (rec.flags & 0x03FF) >> 4, i.e. (base + (flags>>4)) & 0x3F. rec.flags is the u16 at GFX2 cell-record +4 (the same word that carries the mirror bits 0x4000/0x8000); bits 4..9 are a SIX-bit bank offset. node+0x12E (Steam +0x172) is the per-object bank BASE, not a sub-row; node+0x12D (Steam +0x171) is a boolean gate that says whether to add the base. The 16 colours of bank b live at the staging line DC 0x8C2659DC + b*0x30 (+8 flag, +0x10 colours) / Steam blk+0x1040 + b*0x38 (+8 flag, +0x18 colours), and the 8 rows of a costume block are staged there by loc_8c031fa0 / FUN_14060d100 (count 8, source DatPal + pal_id*0x100) and restaged per effect-mode by loc_8c035162 / FUN_1406146d0 (mode 0: row 0 = loc_8c035000/FUN_140614130, rows 1..7 = DatPal + palid*0x100 + k*0x20).',
  provenance='SET-site derivation, both architectures. node+0x12E: loc_8c031fa0 does char[0x12E] = loc_8c1355d4[char[0x524]] and loc_8c035162 mode 0 re-asserts char[0x12E] = loc_8c1355d4[char[0x1A4]]; Steam FUN_14060d100 at 0x14060d1df does word[node+0x172] = (i8)DAT_140a6d188[(i8)node+0x6b0] and FUN_1406146d0 at 0x1406149e2 re-asserts from node+0x230. Table loc_8c1355d4 == DAT_140a6d188 == {0x10,0x18,0x20,0x28,0x30,0x38,0x10,0x18}, read byte-exact from both. node+0x12D: set to 0xFF at object init (loc_8c04ec0c, loc_8c04f0ec; Steam FUN_1406233b0 at 0x1406234bd mov byte [rbx+0x171],0xFF) and to 1 at PL load (loc_8c031fa0 "add 0xAD,r0" on r0=0x180 -> 0x12D, sign-extended; Steam FUN_14060d100 at 0x14060d1d8 mov byte [rdi+0x171],1). USE-sites: loc_8c033d78 / loc_8c033b0a and Steam FUN_140612180 / FUN_140612430.',
  refutes='The tape comment "the sub-row within a costume 8-row block is still an open gap (node+0x12d/+0x12e)" mis-attributes the sub-row to those two bytes. They select the BLOCK; the sub-row comes from the sprite assembly record.',
  caveat='DC->Steam delta in this region is +0x44 (0x12C->0x170 draw gate, 0x12D->0x171, 0x12E->0x172); elsewhere it differs (0x164 DatPal -> 0x1B8, 0x1A4 slot -> 0x230, 0x524/0x52C/0x52D -> 0x6B0/0x6C0/0x6C1). Do not extrapolate one delta.';

UPSERT finding:palette_bank_space_is_global SET status='confirmed', confidence='high', date='2026-09-05',
  title='Palette banks are a GLOBAL 64-entry space; a slot gets only 8 banks of headroom and parts can index past it',
  statement='Bank base per object = {0x10,0x18,0x20,0x28,0x30,0x38,0x10,0x18}[slot], so slot k nominally owns banks 0x10+8k..0x17+8k. But the record field is 6 bits and the final mask is 0x3F, so base + (flags>>4) can legally land in ANOTHER slot nominal range. Measured on the offline PL35 (Blackheart) GFX2: 17,360 records over 900 cells use (flags>>4)&0x3F in {0:14513, 1:1239, 2:285, 3:566, 4:753, 26:4} -- four records at 26, i.e. bank 0x10+26 = 0x2A for slot 0. Entries 6 and 7 of the base table alias to 0x10/0x18 by design.',
  consequence='Any consumer that masks the record row with &7 and indexes palrows as slot*8+row is wrong for those records. The correct index into the 48-row palrows window (banks 0x10..0x3F) is bank-0x10 where bank = ((base + (flags>>4)) & 0x3F), base = the per-node value the tape already ships as flash (node+0x172). rr-render assets.rs:166 currently does ((r.flags >> 4) & 7).',
  caveat='If node+0x171 == 0 the bank is ABSOLUTE (flags>>4) and can be < 0x10, which is OUTSIDE the palrows window entirely. Every fighter observed has the gate non-zero, but the gate is not in the tape.';

UPSERT finding:palrows_not_reconstructible_from_datpal SET status='confirmed', confidence='high', date='2026-09-05',
  title='A fighter staged 8 rows are NOT DatPal + costume*0x100 + k*0x20 -- rows 1 and 2 diverge to the DAT extras region',
  statement='Measured on five local Blackheart tapes. At the first live frame (rel 304) slot 0 stages exactly the costume block: rows 0..7 == PL35 LUT banks 8c+0..8c+7. TWO FRAMES LATER (rel 306) rows 1 and 2 are overwritten with rows from the DAT EXTRAS region (PL35 LUT bank 77/78 for costume 1, 79/80 for costume 0) and stay there for the rest of the capture; rows 0,3,4,5,6,7 remain the costume rows. Same structure for costume 0 and costume 1, in five independent tapes.',
  consequence='Confirms the TAPE v5 design decision: palrows (the 6x8 staged rows read verbatim from blk+0x13C0) is REQUIRED. Any offline reconstruction from DatPal + costume*0x100 is wrong for rows 1 and 2.',
  open='The writer of those two lines is UNKNOWN. It is not loc_8c031fa0 (count 8, costume block), not loc_8c035162 mode 0 (rows 1..7 = costume block), and not modes 1/3..7 for this character: FUN_1406146d0 mode>=1 copies node+0x4c rows starting at line base+0 (FUN_1406144f0 writes lines base..base+count-1), and node+0x4c for cid 0x35 is 1 (DAT_140a6d910 == loc_8c13565c, read byte-exact on both sides). No drawn node in these tapes carries a base other than 0x10/0x18. Falsifier: an x64dbg write watchpoint on blk+0x1040+0x11*0x38+0x18, or an Oracle probe on the staging line.';

RELATE finding:palette_subrow_selector -> cites -> source:marv_bank03_palette_sites SET id='e_psr_marv';
RELATE finding:palette_subrow_selector -> cites -> source:steam_dump_palette_sites SET id='e_psr_steam';
RELATE finding:palette_bank_space_is_global -> cites -> source:rr_tape_palrows_measurement SET id='e_pbg_meas';
RELATE finding:palette_bank_space_is_global -> cites -> source:marv_bank03_palette_sites SET id='e_pbg_marv';
RELATE finding:palrows_not_reconstructible_from_datpal -> cites -> source:rr_tape_palrows_measurement SET id='e_pnr_meas';
RELATE finding:palrows_not_reconstructible_from_datpal -> cites -> source:steam_dump_palette_sites SET id='e_pnr_steam';
RELATE finding:palette_subrow_selector -> about -> routine:loc_8c033d78 SET id='e_psr_r1';
RELATE finding:palette_subrow_selector -> about -> routine:loc_8c033b0a SET id='e_psr_r2';
RELATE steam_routine:FUN_140612430 -> recompiles -> routine:loc_8c033b0a SET confidence='confirmed', method='both read', evidence='identical gate(+0x12D/+0x171) / base(+0x12E/+0x172) / mask 0x3FF / shift-4 sequence on the same 8-byte record layout, both read from source';
