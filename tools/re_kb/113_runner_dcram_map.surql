-- 113: receipt runner -- DC-RAM map, PL slot recipe, stage animation drivers, corrections (docs/RECEIPT-RUNNER-DCRAM.md s8)
USE NS re DB kb;
-- sources
UPSERT source:doc_receipt_runner_dcram SET kind='doc', strength='code', ref='mvc-live-skins-quarters/docs/RECEIPT-RUNNER-DCRAM.md', note='DC work-RAM map for a battle frame, SH4 -> Steam by function matching; PL slot recipe gated 66/66 byte-exact on runs/20260903-000941.';
UPSERT source:dcram_diff_20260903_000941 SET kind='capture', strength='runtime', ref='d3dcap/ttd/runs/20260903-000941 pre/post dcram.bin', note='237-frame diff: 13 pages, 0x0CE60000 tile table + 8x8 B effects UVs (models 80/86) + 10x1 B HUD TCW bytes; PL slots and stage POL unchanged.';
-- routines (Steam side)
UPSERT steam_routine:FUN_14060d100 SET addr='0x14060d100', role='PL slot loader: 10 AFS files per character to base+0/0x130000/0x13C000/0x13D000/0x13E000/0x140000..0x144000, fighter pointer cluster +0x1A8..+0x220, LUT rows', consts=['0xC420000','0x150000'];
UPSERT steam_routine:FUN_14060dd40 SET addr='0x14060dd40', role='select-time character load queue (8-B descriptors; 0x80 = slot copy 0x150000)';
UPSERT steam_routine:FUN_14064a4a0 SET addr='0x14064a4a0', role='Carnival vertex morph: lerp keyframes from the POL table 0x0D852020/0x0D851918 into model 1';
UPSERT steam_routine:FUN_14064a840 SET addr='0x14064a840', role='Carnival prop callback: node+0x30/+0x32 phase clock, 34 phases';
UPSERT steam_routine:FUN_14064e8b0 SET addr='0x14064e8b0', role='River Raft init: parent + 8 rocked children, callback LAB_14064e5e0';
UPSERT steam_routine:FUN_140751220 SET addr='0x140751220', role='River Raft water init: 3 nodes, models 9/11/13, callback FUN_1407511c0';
UPSERT steam_routine:FUN_140750eb0 SET addr='0x140750eb0', role='River Raft water rewrite: dst = pristine model 10/12/14 + sin(node+0x30/+0x32/+0xF4..FC)';
UPSERT steam_routine:FUN_1406196e0 SET addr='0x1406196e0', role='NaomiLib vertex iterator begin (src,dst)';
UPSERT steam_routine:FUN_140619580 SET addr='0x140619580', role='NaomiLib vertex iterator step';
UPSERT steam_routine:FUN_140797690 SET addr='0x140797690', role='effects quad UV rewrite: model 80/86 uv = sibling 81/87 uv + DAT_140aa2860[node+0x30]';
UPSERT steam_routine:FUN_140620200 SET role='per-stage prop initialiser dispatcher (PTR_PTR_140a6ec20 / PTR_DAT_140a6ecb0)';
-- routines (SH4 side)
UPSERT routine:loc_8c031fa0 SET pc='0x8c031fa0', bank='bank03', name='PL pointer-cluster loader', note='+0x168/16C/170/174/188.. = base + header words +0x14..+0x34; +0x12E/+0x30/+0x132 tables; jsr loc_8c033aca LUT rows; loads S_PLxx overlay to 0x0CE30000+n*0x8000';
UPSERT routine:loc_8c0275f6 SET pc='0x8c0275f6', bank='bank02', name='character file-load state machine';
UPSERT routine:loc_8c108430 SET pc='0x8c108430', bank='bank10', name='per-stage init dispatcher loop (tables bank16.loc_8c165a78 / loc_8c165c64)';
UPSERT routine:loc_8c10eb3e SET pc='0x8c10eb3e', bank='bank10', name='Carnival prop callback';
UPSERT routine:loc_8c10ec26 SET pc='0x8c10ec26', bank='bank10', name='Carnival vertex morph (table 0x0CEC5020 / 0x0CEC4918)';
UPSERT routine:loc_8c1123a0 SET pc='0x8c1123a0', bank='bank11', name='River Raft init';
UPSERT routine:loc_8c1126a8 SET pc='0x8c1126a8', bank='bank11', name='River Raft water init';
UPSERT routine:loc_8c11284c SET pc='0x8c11284c', bank='bank11', name='River Raft water rewrite';
UPSERT routine:loc_8c108060 SET pc='0x8c108060', bank='bank10', name='vertex iterator begin (globals 0x8C28C864..)';
UPSERT routine:loc_8c108086 SET pc='0x8c108086', bank='bank10', name='vertex iterator step';
-- pairs
RELATE steam_routine:FUN_14060d100->recompiles->routine:loc_8c031fa0 SET confidence='confirmed', method='both read', evidence='same header->pointer sequence, +0x12E/+0x30/+0x132 (0x100/0x200 if cid 0x34) and LUT-row call; Steam splits the DC sections into AFS 268/327/386/445/504/563/681/622/740+cid';
RELATE steam_routine:FUN_14060dd40->recompiles->routine:loc_8c0275f6 SET confidence='confirmed', method='both read', evidence='8-B descriptor {flags,fileid,dest}, tst 0x80 -> 0x150000 slot copy via 12-B ring (0x8c1f932c / DAT_142ec6d80), else File Load; post-load 8-B ring 0x8c1f92ac / DAT_142ec6d00';
RELATE steam_routine:FUN_140620200->recompiles->routine:loc_8c108430 SET confidence='confirmed', method='both read', evidence='table-of-tables + index list until 0xFF; loc_8c045ce0 is the CALLER (corrects map row)';
RELATE steam_routine:FUN_14064a840->recompiles->routine:loc_8c10eb3e SET confidence='confirmed', method='both read';
RELATE steam_routine:FUN_14064a4a0->recompiles->routine:loc_8c10ec26 SET confidence='confirmed', method='both read', evidence='morph table constants 0x0CEC5020/0x0CEC4918 <-> 0x0D852020/0x0D851918 (delta 0x98D000 = stage POL relocation)';
RELATE steam_routine:FUN_14064e8b0->recompiles->routine:loc_8c1123a0 SET confidence='confirmed', method='both read', evidence='8 children loop, flags 0x80F, +0x1E=1';
RELATE steam_routine:FUN_140751220->recompiles->routine:loc_8c1126a8 SET confidence='confirmed', method='both read', evidence='3 nodes, flags 0x81F, 0x3C*i counters';
RELATE steam_routine:FUN_140750eb0->recompiles->routine:loc_8c11284c SET confidence='high', method='both read (pool constants 0x5A0/0x168/0x100/0x80, iterator calls)';
RELATE steam_routine:FUN_1406196e0->recompiles->routine:loc_8c108060 SET confidence='confirmed', method='both read';
RELATE steam_routine:FUN_140619580->recompiles->routine:loc_8c108086 SET confidence='confirmed', method='both read';
-- globals
UPSERT global:dc_8c2659dc SET addr='0x8C2659DC', name='palette staging rows (DC, stride 0x30)', steam='blk+0x1040 stride 0x38', confidence='inferred', evidence='loc_8c035162 pool + FUN_1406146d0/FUN_140612930';
UPSERT global:dc_8c28c864 SET addr='0x8C28C864..0x8C28C87C', name='NaomiLib vertex iterator cursor', steam='DAT_142eee568..0x142eee590', confidence='confirmed';
-- findings
UPSERT finding:pl_slot_recipe SET status='confirmed', confidence='high', date='2026-09-03', contribution_candidate=true, note='Each PL slot 0x0C420000+pos*0x150000 = AFS 209+cid at +0, 268+cid at +0x130000, 327+cid +0x13C000, 386+cid +0x13D000, 445+cid +0x13E000, 504+cid +0x140000, 563+cid +0x141000, 681+cid +0x142000, 622+cid +0x143000, 740+cid +0x144000, 3+cid +0x148000 (case 6), plus an engine-written 12 KB region at +0x145000 (fighter+0x1F0; writer INFERRED FUN_140612180). Loader FUN_14060d100 = loc_8c031fa0. 66/66 byte-exact on the live image. Closes the loader-UNKNOWN of determinism_pl_image_rule.';
UPSERT finding:dcram_inplace_rewrites SET status='confirmed', confidence='high', date='2026-09-03', note='Per-frame in-place rewrites of arc data on Steam: stage POL vertices (list-5 callbacks), effects POL UVs (FUN_140797690), HUD POL TCW bytes (FUN_140653a70), plus the 0x0CE60000 tile table. All absolute functions of blk node state + static tables + pristine sibling data, written before the draw in the same frame; nothing incremental lives in the DC image.';
UPSERT finding:stage_prop_animation_state SET status='confirmed', confidence='high', date='2026-09-03', contribution_candidate=true, note='Stage props animate from pool-node fields (+0x30/+0x32/+0x34/+0xF4..FC, parent +0x60), exe tables, the sin table and pristine POL data; no frame counter/RNG. Renderer can pose from keys by porting FUN_14064a4a0 / FUN_140750eb0 / LAB_14064e5e0. Only stages 3/0xC and 7/0x10 read; 15 tables remain to classify.';
UPSERT finding:determinism_pl_image_rule SET note_loader='2026-09-03: loader found (finding:pl_slot_recipe).';
RELATE finding:pl_slot_recipe->cites->source:doc_receipt_runner_dcram; RELATE finding:pl_slot_recipe->about->steam_routine:FUN_14060d100; RELATE finding:pl_slot_recipe->about->routine:loc_8c031fa0;
RELATE finding:dcram_inplace_rewrites->cites->source:dcram_diff_20260903_000941; RELATE finding:stage_prop_animation_state->about->steam_routine:FUN_14064a4a0; RELATE finding:stage_prop_animation_state->about->steam_routine:FUN_140750eb0;
-- corrections to 101_steam_function_map rows (apply as UPDATE of the recompiles edges, do not re-run 07_dedup_edges)
-- FUN_14060dd40 -> loc_8c042d0c : demote (falsified) ; FUN_140620200 -> loc_8c045ce0 : reclass as 'called_by' ; FUN_1408435d0 -> loc_8c03552a : demote (TEXTURE-BANKS s1)

-- evidence edges required by the finding.status ASSERT
RELATE finding:dcram_inplace_rewrites->cites->source:doc_receipt_runner_dcram;
RELATE finding:stage_prop_animation_state->cites->source:doc_receipt_runner_dcram;
RELATE finding:stage_prop_animation_state->cites->source:dcram_diff_20260903_000941;
