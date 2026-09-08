-- 190_select_assist_text_records.surql -- the TEXT inside the ASSIST TYPE boxes at character select: which node draws
-- it, why its records are frame-arena COPIES (ctx+0x1f855c), what node +0x5C/+0x60/+0x64 are (rotations), the entry
-- UV-mode fields, and that the sheets 0x817/0x818 are static TEX 838 pages. 2026-09-06, SUPERGUN lane.
USE NS re DB kb;

UPSERT source:select_assist_text_decompiles_20260906 SET kind='disasm', strength='code',
  ref='Ghidra dumpproj/mvc_dump.bin: FUN_1408499e0, FUN_140848ee0, FUN_1408436a0 (:8080 bridge), FUN_1408479e0, FUN_140847a70, FUN_140847b00, FUN_140845150, FUN_1408444d0, FUN_140847f20, FUN_140620740, FUN_140620cd0; capstone of LAB_1407413d0.',
  note='vs-01 dcram pages 0x817 (DC 0x0D153000) and 0x818 (0x0D173000) decoded as 256x256 RGB565 twiddled: 13 type-name strips in two columns + TURBO/NORMAL (EN) and the Japanese sheet; scratchpad tcw817_EN.png / tcw818_JP.png.';

UPSERT steam_routine:FUN_140848ee0 SET addr='0x140848ee0', name='FUN_140848ee0',
  role='MODEL SUBMIT (records -> display-list entries). For each record (while PCW < 0): if ctx+0x1f855c != 0, memcpy the record (rec[0x13]+0x50 bytes) to ctx+0x100030+*(ctx+0x1e0080) (per-frame record arena inside ctx, limit 0xE0000) and submit THE COPY, else submit the record in place; FUN_1408436a0(kind, &entry{recptr}, 0x130). Disarms the UV-mode state ctx+0x1f8524 / ctx+0x1f8540 unless the model == ctx+0x1f8528 / +0x1f8548. Clears ctx+0x1f855c at the end. Called by FUN_1408499e0(model) with kind 0, alpha 1.0, flags from ctx+0x1f8518.';
UPSERT steam_routine:FUN_1408436a0 SET addr='0x1408436a0', name='FUN_1408436a0',
  role='DISPLAY-LIST ENTRY BUILDER (0x130 B): +0 record ptr; +0x08.. matrices from ctx+0x1f816c../+0x1f812c../+0x1f80ec..; +0xD0/+0xD4/+0xD8 = ctx+0x1f825c/60/64; +0xDC.. = ctx+0x1f84e8..; +0x100/+0x108 = ctx+0x1f84dc/+0x1f84e4; +0x10C = *(ctx+0x1f8524) (UV MODE: 4 = add +0x110/+0x114 to every UV, 1/2 = overwrite the first quad from +0x110..+0x11C); +0x110..+0x118 = ctx+0x1f8530/+0x1f8538; +0x120 = *(ctx+0x1f8540); +0x124/+0x12C = ctx+0x1f8550/+0x1f8558. Bails if the arena would exceed 0xE0000.';
UPSERT steam_routine:FUN_1408479e0 SET addr='0x1408479e0', name='FUN_1408479e0', role='rotate X by a u16 angle: sin = DAT_142ef0ac0[a] (FUN_140845150), cos = DAT_142f30ac0[a] (FUN_1408444d0), multiplied in by FUN_140846ee0. Fed node+0x5C when +0xF0 bit1.';
UPSERT steam_routine:FUN_140847a70 SET addr='0x140847a70', name='FUN_140847a70', role='rotate Y by a u16 angle (same tables). Fed node+0x60 when +0xF0 bit2, and FUN_140844390(dx/dz) for look-at nodes (+0xF0 bit8).';
UPSERT steam_routine:FUN_140847b00 SET addr='0x140847b00', name='FUN_140847b00', role='rotate Z by a u16 angle (same tables). Fed node+0x64 when +0xF0 bit3.';

UPSERT finding:select_assist_text_is_arena_copied_records SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='The text inside the ASSIST TYPE boxes is the C_k nodes: static sheet 0x817/0x818, per-draw UV rewrite, and records submitted as ctx frame-arena COPIES -- the only select records on that path',
  statement='Per row k: FUN_140620cd0 calls node+0x40 = FUN_1407ed640 -> FUN_14062b620 rewrites the scratch model 0x66/0x6C UVs to name n = DAT_140aacbf0[cid*3+k] (DAT_140aacb80[n] - [0]) and sets ctx+0x1f855c = 1; FUN_1408499e0 -> FUN_140848ee0 then memcpy s the 0xD8-byte record to ctx+0x100030+*(ctx+0x1e0080) and the display-list entry points at that copy. The B_k boxes/labels (models 0x63+k) are referenced in place. Sheets: TEX 838 records 7/8 (0x817 EN @0x0D153000, 0x818 JP @0x0D173000), 13 strips of 32 px in two columns (Projectile, Ground, Expansion, Dash, Anti-air, Variety, Throw, Air Throw; Heal, Enhance, Capture, Launcher, Balance) + TURBO/NORMAL, present in the vs-01 anchor, never re-uploaded by any FUN_1406163e0 kind. Node +0x5C/+0x60/+0x64 are ROTATION angles (FUN_1408479e0/FUN_140847a70/FUN_140847b00 by +0xF0 bits 1/2/3; sin/cos tables DAT_142ef0ac0/DAT_142f30ac0); LAB_1407413d0 loads them per side from DAT_140a99b58/5C/60 (+0x50 position DAT_140a99b40), FUN_1407412a0 from DAT_140a99790[(cid+side*0x3B)*8]. The entry UV modes (+0x10C = ctx+0x1f8524, +0x110.. = ctx+0x1f8530/38) are armed per model (ctx+0x1f8528) for the scrolling ring and are NOT used by the box text.',
  consequence='A headless raster that draws the boxes and the TYPE labels but blank boxes with no garbage is dropping entries whose record pointer lies in ctx+0x100030..+0x1E0030. Minimal change: accept those entries and rasterise from the copied record. No game write is needed. Gate: after a confirm, three arena entries per side with TCW 0x817, 4 vertices, UVs = name-0 window shifted by (du, -dv) for n = DAT_140aacbf0[cid*3+k].';

RELATE finding:select_assist_text_is_arena_copied_records->cites->steam_routine:FUN_140848ee0 SET id='e190_t_submit';
RELATE finding:select_assist_text_is_arena_copied_records->cites->steam_routine:FUN_1408436a0 SET id='e190_t_entry';
RELATE finding:select_assist_text_is_arena_copied_records->cites->steam_routine:FUN_1407ed640 SET id='e190_t_cb';
RELATE finding:select_assist_text_is_arena_copied_records->cites->steam_routine:FUN_140620cd0 SET id='e190_t_walker';
RELATE finding:select_assist_text_is_arena_copied_records->cites->steam_routine:FUN_1408479e0 SET id='e190_t_rotx';
RELATE finding:select_assist_text_is_arena_copied_records->cites->finding:select_assist_names_are_static_page_models SET id='e190_t_prior';
RELATE finding:select_assist_text_is_arena_copied_records->cites->finding:list0c_texture_patch_rule SET id='e190_t_arena';
RELATE finding:select_assist_text_is_arena_copied_records->cites->source:select_assist_text_decompiles_20260906 SET id='e190s_a0';
RELATE finding:select_assist_text_is_arena_copied_records->cites->source:select_assist_anchor_read_20260906 SET id='e190s_b0';
UPDATE finding:select_assist_text_is_arena_copied_records SET status='confirmed';
