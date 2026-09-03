USE NS re DB kb;
-- 112: receipt runner -- DC-RAM write set, iterator API, callbacks, loader queue (docs/RECEIPT-RUNNER-RE.md s9)
-- sources
UPSERT source:dcram_diff_20260903_000941 SET kind='measurement', ref='d3dcap/ttd/runs/20260903-000941 pre vs post dcram/ctx/game_state', strength='bytes';
UPSERT source:ghidra_2026_09_03_runner SET kind='ghidra', ref='mvc_dump.bin via :8080 bridge + capstone on pre/exe_image.bin', strength='code';
-- routines (roles) — CONFIRMED by decompile
UPSERT steam_routine:FUN_1406196e0 SET role='object-record iterator: open SOURCE model / DESTINATION object cursors (DAT_142eee568..590)';
UPSERT steam_routine:FUN_140619580 SET role='iterator: advance both cursors one vertex (0x50 hdr, 0x20 vertex; count hdr+4, x3 unless flag 0x10)';
UPSERT steam_routine:FUN_140619510 SET role='iterator: read SOURCE vertex xyz';
UPSERT steam_routine:FUN_140619530 SET role='iterator: read SOURCE vertex u,v';
UPSERT steam_routine:FUN_140619680 SET role='iterator: write DESTINATION vertex xyz, set bit0 marker on x';
UPSERT steam_routine:FUN_1406196a0 SET role='iterator: write DESTINATION vertex u,v, set bit0 marker on v';
UPSERT steam_routine:FUN_140845150 SET role='sin table lookup DAT_142ef0ac0[u16 angle]';
UPSERT steam_routine:FUN_140619800 SET role='load-time: clear TSP bits 0x18000 on every record of a model';
UPSERT steam_routine:FUN_14061e520 SET role='list callback pass: for node in blk+0x2EDE8+L*8: (*(node+0x18))(node)';
UPSERT steam_routine:FUN_1406283a0 SET role='per-frame callback pass over lists 3,4,2,5,6,10,7,8,9 (sim half; callers FUN_14060e3f0, caseD_7)';
UPSERT steam_routine:FUN_14060dcf0 SET role='AFS entry -> DC RAM copy from the in-memory AFS image at ctx[0] (dir at ctx[0]+8+8*entry)';
UPSERT steam_routine:FUN_14060dd40 SET role='async load-queue stepper (0x150000 PL slot jobs; post-process records into DAT_142ec6d00)';
UPSERT steam_routine:FUN_14060d980 SET role='load-queue pump (calls FUN_14060dd40 x2)';
UPSERT steam_routine:FUN_140634600 SET role='HUD/option pass: gs+0x964/0x968 countdown decrement, entity+0x291=0x50 when gs+0x888';
UPSERT steam_routine:FUN_140639200 SET role='sets gs+0x964|0x968 = 0x1E (HUD countdown)';
-- undefined callbacks as routines (capstone-read)
UPSERT steam_routine:LAB_140625920 SET addr=5375154464, role='list-6 blackout-quad callback: +0x170=G+0x98; UV scroll dst.v = c - counter*step + src.v via iterator API';
UPSERT steam_routine:LAB_14064cb20 SET addr=5375314720, role='stage-5 prop-1 animation: phase +0x30 (wrap 360) / +0x32 (wrap 2000); y = sin(phase)*k - c; u = src.u + phase*k (src = stage POL model 2, dst = model 1)';
UPSERT steam_routine:LAB_140654790 SET addr=5375346576, role='training list-6 backdrop callback: node fields only from owner fighter';
-- globals
UPSERT global:gs_0x964 SET addr=5380011684, name='game_state+0x964 HUD countdown seat0';
UPSERT global:gs_0x968 SET addr=5380011688, name='game_state+0x968 HUD countdown seat1';
UPSERT global:gs_0x000 SET addr=5380009280, name='game_state+0 = arena base (ctx = +0x8000000, dcram = +0x8400000, staging = +0xA400000)';
UPSERT global:DAT_142ebc010 SET name='heap object with 64-entry debug ring at +0x346AC (FUN_14006b5d0); runner must re-point';
-- findings
UPSERT finding:dcram_writeset_regenerated SET status='confirmed', text='237 frames change 12,939 B of DC-RAM: tile buffer + 50 B u/v (effects POL) + 10 TCW bytes (HUD POL); all writers regenerate from blk phase + pristine source model; PL slots 0 B';
UPSERT finding:bank_load_rebase_measured SET status='confirmed', text='live POL banks differ from AFS by 0.9-2.5 KB (pointer/TCW rebase FUN_14060d770/FUN_140844dc0); TEX banks 0 B';
UPSERT finding:h1_drawn_implies_callback_ran SET status='inferred', text='callback pass FUN_1406283a0 precedes the render half; stale destination objects never drawn — gate on a real-stage dump';
UPSERT finding:pl_loader_candidate SET status='inferred', text='FUN_14060dd40 is the only function with 0x150000; tail builder = consumer of DAT_142ec6d00 (UNKNOWN)';
-- edges (writes / reads / calls / about / cites)
RELATE steam_routine:LAB_140625920->calls->steam_routine:FUN_1406196e0;
RELATE steam_routine:LAB_140625920->calls->steam_routine:FUN_140619530;
RELATE steam_routine:LAB_140625920->calls->steam_routine:FUN_1406196a0;
RELATE steam_routine:LAB_140625920->calls->steam_routine:FUN_140619580;
RELATE steam_routine:LAB_140625920->calls->steam_routine:FUN_14061dbe0;
RELATE steam_routine:LAB_140625920->calls->steam_routine:FUN_140619800;
RELATE steam_routine:LAB_14064cb20->calls->steam_routine:FUN_1406196e0;
RELATE steam_routine:LAB_14064cb20->calls->steam_routine:FUN_140619510;
RELATE steam_routine:LAB_14064cb20->calls->steam_routine:FUN_140619680;
RELATE steam_routine:LAB_14064cb20->calls->steam_routine:FUN_140619530;
RELATE steam_routine:LAB_14064cb20->calls->steam_routine:FUN_1406196a0;
RELATE steam_routine:LAB_14064cb20->calls->steam_routine:FUN_140845150;
RELATE steam_routine:FUN_1406283a0->calls->steam_routine:FUN_14061e520 SET sites=15;
RELATE steam_routine:FUN_140634600->writes->global:gs_0x964; RELATE steam_routine:FUN_140639200->writes->global:gs_0x968; RELATE steam_routine:FUN_14010d0b0->writes->global:gs_0x964 SET note='shell-side (INFERRED)';
RELATE finding:dcram_writeset_regenerated->cites->source:dcram_diff_20260903_000941; RELATE finding:dcram_writeset_regenerated->cites->source:ghidra_2026_09_03_runner;
RELATE finding:dcram_writeset_regenerated->about->steam_routine:LAB_140625920;
RELATE finding:dcram_writeset_regenerated->about->steam_routine:LAB_14064cb20;
RELATE finding:dcram_writeset_regenerated->about->steam_routine:FUN_14060d8d0;

-- evidence edges required by the finding status assert
RELATE finding:bank_load_rebase_measured->cites->source:ghidra_2026_09_03_runner;
RELATE finding:bank_load_rebase_measured->cites->source:dcram_diff_20260903_000941;
