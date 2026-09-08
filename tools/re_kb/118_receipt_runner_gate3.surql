-- 118_receipt_runner_gate3.surql -- GATE 3: the DC-RAM image is rebuilt from the USER'S OWN arc
-- (SUPERGUN ENGINE lane, 2026-09-04). RE METHOD step 4: store the pairs/findings as graph edges.
-- Evidence: mvc-live-skins-quarters/d3dcap/receipt/dcram_build.py (the offline builder + differ),
-- run d3dcap/ttd/runs/receipt-20260903-stage9-anchor (stage 9, roster [42,23,52,12,44,46]),
-- live tape d3dcap/replay/packs/local_stage9/tape.json.gz, runner d3dcap/receipt/runner/rr_runner.exe.
USE NS re DB kb;

-- ---------------------------------------------------------------- sources
UPSERT source:tool_dcram_build SET kind='tool', strength='code',
  ref='mvc-live-skins-quarters/d3dcap/receipt/dcram_build.py',
  note='Rebuilds the 32 MB Steam DC-RAM battle image from game_50.arc + the anchor blk by replaying FUN_14060d100 (11 PL files x 6 slots), FUN_14060c370 case 6 (tail), FUN_14060d770 (POL relocation), FUN_140844dc0 (TCW assign), FUN_14060d470 (stage bank + stage-0x10 fixup), FUN_14060d560 (HUD bank + 12 portrait/name pages, LZSS-16 FUN_140611e90); --gate classifies every remaining difference.';
UPSERT source:gate3_run_20260904 SET kind='capture', strength='runtime',
  ref='%TEMP%/rr_gate3 (arm1, arm_ctx{a,b,c,d,e,f}, gate3_tape, gate3_tape_s, gate2_baseline)',
  note='2026-09-04: rr_runner.exe ticked 300 receipt frames (clock 1716..2016) on an arc-built dcram.bin instead of the captured one; blk/gs/ctx/exe_dat/ggpo compared against the CONFIRMED gate2_300 dumps.';

-- ---------------------------------------------------------------- routines newly read (Steam side)
UPSERT steam_routine:FUN_14060d770 SET addr='0x14060d770',
  role='POL relocation: delta = POL[0] - dstDC - 0x10; POL[0], POL[2] and the whole model table (POL[1] entries) -= delta; then every 16-byte texture record loc (+8) -= (firstLoc - TEXdstDC), terminator = the u16 width == 0';
UPSERT steam_routine:FUN_14060d8f0 SET addr='0x14060d8f0',
  role='builds the HOST bank struct from a relocated POL: bank[i] = host(modelTable[i]) for POL[1] models, bank[0x160] = count, bank[0x161] = host(POL[2] texHdrs). Writes NO DC-RAM.';
UPSERT steam_routine:FUN_140844dc0 SET addr='0x140844dc0',
  role='TCW assign + host slot registration. Its ONLY DC-RAM write is rec+0xC = *(ctx+0x1e0098) + rec+0x20 (texIndex). Record walk: first record = model+0x18, next = rec + 0x50 + rec[+0x4C], terminator = the signed word at rec+0 is >= 0.';
UPSERT steam_routine:FUN_14060d470 SET addr='0x14060d470',
  role='stage bank register (FUN_1408458a0(0xC10) + FUN_14060d8f0(PTR_DAT_142edf588, 0x0D82D000) + FUN_140844dc0 per model); when blk+0x6D04 == 0x10 it also patches stage MODELS 1..8: texIndex 10 -> rec+0x4 |= 0x4000000, texIndex 5 -> rec+0x0 &= ~0x2000000';
UPSERT steam_routine:FUN_140619720 SET addr='0x140619720',
  role='COPIES OUT the first 0x10 bytes of every record of a model into a caller array (param_2 += 0x10 per record). It is a SAVE of the record headers, NOT a DC-RAM patch.';
UPSERT steam_routine:FUN_140619800 SET addr='0x140619800',
  role='per-model TSP clear: for every record, rec+0x8 &= 0xFFFE7FFF (clears bits 0x18000). Called from ~20 effect/prop node-init routines, i.e. it fires at NODE SPAWN, not at bank load.';
UPSERT steam_routine:FUN_140612180 SET addr='0x140612180',
  role='builds the 12 KB PL region at fighter+0x1F0 (base+0x145000): walks Dat_GFX2 (fighter+0x1B0) for DAT_140a75240[cid] cells, writes a u16 offset table at +0, per-part indices at +0x220, and interns unique part selectors into blk+0x32BF4. Deterministic in (cid, GFX2); it WRITES blk, so replaying it on an anchored blk is not free.';

-- ---------------------------------------------------------------- findings
UPSERT finding:gate3_arc_dcram SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  note='GATE 3 (loader replaces the dump). The 32 MB DC-RAM battle image is rebuildable from the user own game_50.arc + the anchor blk. Offline differ vs the live dump: ZERO differing bytes among all loader-placed bytes (6 PL slots x 11 AFS files, effects/HUD/common/stage POL+TEX with relocation and TCW assignment, and the 12 HUD portrait/name-plate pages). Remaining differences are only: B = 1708 B of bank POL (per-frame vertex/UV/TCW rewrites + spawn-time model patches), E = 39261 B in the six +0x145000 engine regions, L = 393281 B of leftovers past each placed file, R = 21347 B of tile buffer 0x0CE60000, X = 6462704 B of non-battle banks. RUNNER GATE: rr_runner.exe ticked the arc-built image 300 receipt frames and produced blk dumps byte-identical to the captured-image run on 300/300 ticks, plus identical gs_out/ctx_out/exe_dat_out/ggpo_out -- so classes B/E/L/R/X are provably not on the sim read path for this stage/roster.';
UPSERT finding:gate3_tape_arm SET status='confirmed', confidence='high', date='2026-09-04',
  note='GATE 3 tape arm: runner_tape_gate.py over the arc-built image PASSES (P 14, A 1281, F 1, 0 unexplained) and its runner tape is BYTE-IDENTICAL to the tape produced from the captured image, once the 1708 differing bank-POL bytes are carried. Without them, only anodes.obj / aobjs / aobjs_n differ: the harvest interns the animated stage props (TCW 0xC12, 0xC1C) and the UV-animated effect quads at TICK 0, before the callbacks regenerate them, so the arc bytes (pristine, never a real phase) enter the tape. Same class as the already-known 0.3.44 first-sighting artefact that makes EVERY live tape carry stale prop vertices; the fix is Track R pose-from-keys or a harvest that does not intern tick 0.';
UPSERT finding:runner_ctx_minimal SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  note='The 4 MB NaomiLib ctx the runner restores is reducible to ~5.2 KB: ctx+0x00..0x18 (the three self pointers: AFS image, dcram base, blk), ctx+0x1E0030..0x1E31CC (the texture-slot table the battle anchor already carries) and ctx+0x1F80A0..0x1F81C0 (288 B: matrix mode at +0x1F80A4, current index at +0x1F80A8, two 4x4 matrices, storage pointer = blk at +0x1F81B0, capacity 0x40/0x40 at +0x1F81B8/BC). Everything else may be ZERO. Gated on 300 receipt ticks: keeping ctx+0x1F8000..0x1F9000 -> blk 300/300 byte-exact; keeping only 0x1F80A0..0x1F81C0 -> 300/300; keeping only 0x1F80A0..0x1F8100 or only 0x1F8100..0x1F81C0 -> FAILS at tick 1 (blk+0x3EDC = fighter 0 walker field). So the whole 288 B is required and is a candidate battle-anchor field.';
UPSERT finding:dcram_spawn_time_model_patches SET status='confirmed', confidence='high', date='2026-09-04',
  note='The bank-POL bytes that a load-time arc build cannot reproduce split into two mechanisms, both located by matching the residual bytes to the anchor pool-node lists: (a) FUN_140619800 rec+0x8 &= 0xFFFE7FFF on effects models 71/73/74 -- exactly the models bound to list-6 nodes in the anchor blk; (b) FUN_140660f40 rec+0x8 |= 0x2000 on effects model 20 -- exactly the model bound to a list-7 node. Both fire at NODE SPAWN and PERSIST. Models with residuals that are NOT bound to any live node (effects 77/79/80/85/86/165/167, common bank, HUD models 0..39 TCW low byte) are stale output of dead effects / of the at-draw combo-counter patch and are overwritten before use.';
UPSERT finding:portrait_pages_dcram_gate SET status='confirmed', confidence='high', date='2026-09-04',
  note='The character-DAT portrait rule of docs/PORTRAIT-PAGES-GHIDRA.md now has a RAW-BYTE gate against live game memory, not only against the capture-derived library: decompressing AFS 3+cid sub-blob 0 with the ported LZSS-16 and copying page {1,0,3,0}[*(fighter+0x655)] to HUD texHdr[10+k].loc and page 2 to texHdr[16+k].loc, k = {0,3,1,4,2,5}[slot], reproduces all 12 pages (12 x 0x800 B) of the live DC-RAM image byte-exactly for roster [42,23,52,12,44,46]. Cross-checked on a second dump (roster [42,52,44,23,52,53]): the same cid+assist yields the same live page in a different slot.';

-- ---------------------------------------------------------------- corrections
UPSERT finding:dcram_inplace_rewrites SET
  note_gate3='2026-09-04: measured exactly on the stage-9 anchor -- effects POL 197 B, HUD POL 73 B, common POL 1366 B, stage POL 72 B differ from the pristine arc bytes; the HUD 31 single bytes are all rec+0xC TCW low bytes (0x92 -> 0x93/0x94/0x95, the at-draw combo-counter patch FUN_140653a70), the rest are vertex/UV floats and the two spawn-patch bit ops.';
UPSERT finding:pl_slot_recipe SET
  note_gate3='2026-09-04: the slot map is DAT_140a6ab80 = {0,3,1,4,2,5} read as slot -> pos (its inverse {0,2,4,1,3,5} is what pl_rebuild.py iterates as pos -> slot). Re-gated end to end from the arc: all 66 PL files plus the 6 tails are byte-exact in the arc-built image.';

-- ---------------------------------------------------------------- edges
RELATE finding:gate3_arc_dcram->cites->source:tool_dcram_build;
RELATE finding:gate3_arc_dcram->cites->source:gate3_run_20260904;
RELATE finding:gate3_arc_dcram->about->steam_routine:FUN_14060d770;
RELATE finding:gate3_arc_dcram->about->steam_routine:FUN_140844dc0;
RELATE finding:gate3_arc_dcram->about->steam_routine:FUN_14060d100;
RELATE finding:gate3_tape_arm->cites->source:gate3_run_20260904;
RELATE finding:runner_ctx_minimal->cites->source:gate3_run_20260904;
RELATE finding:dcram_spawn_time_model_patches->about->steam_routine:FUN_140619800;
RELATE finding:dcram_spawn_time_model_patches->about->steam_routine:FUN_140660f40;
RELATE finding:portrait_pages_dcram_gate->about->steam_routine:FUN_14060d560;
RELATE finding:portrait_pages_dcram_gate->cites->source:tool_dcram_build;

-- the schema requires a cites->source of strength 'code'/'reproduction' before status='confirmed'
RELATE finding:gate3_tape_arm->cites->source:tool_dcram_build;
RELATE finding:runner_ctx_minimal->cites->source:tool_dcram_build;
RELATE finding:dcram_spawn_time_model_patches->cites->source:tool_dcram_build;
UPDATE finding:gate3_tape_arm SET status='confirmed';
UPDATE finding:runner_ctx_minimal SET status='confirmed';
UPDATE finding:dcram_spawn_time_model_patches SET status='confirmed';

-- 2026-09-04 falsification arm: the ctx_out residual is the RUNNER's own nondeterminism, not the arc build
UPSERT finding:runner_ctx_pagetable_nondeterminism SET status='confirmed', confidence='high', date='2026-09-04',
  note='Two rr_runner.exe runs of the SAME dump image (20260903-000941, 20 idle ticks) produce ctx_out.bin differing in 128 B = 32 dwords at stride 0x130 inside ctx+0x107864..0x108EF4, while every blk dump, gs_out and exe_dat_out are byte-identical. Carrying the ENTIRE live dcram into an arc-built image does not remove the difference. So this family (the same shape as RECEIPT-RUNNER-GATE1 s3.5: ctx page-table records memcpy-d from a stack buffer whose +0x6C/+0x114 fields hold leftovers) is run-dependent host state, never read by the tick, and must be EXCLUDED from any ctx byte gate. On the stage-9 300-tick receipt run ctx_out happened to be byte-EQUAL between the dump and arc arms, so the residual is intermittent.';
RELATE finding:runner_ctx_pagetable_nondeterminism->cites->source:tool_dcram_build;
RELATE finding:runner_ctx_pagetable_nondeterminism->cites->source:gate3_run_20260904;
