-- 123_receipt_runner_gate4.surql -- GATE 4: H1 falsified, DC-RAM classes E/L/X proven unread by perturbation,
-- the stage-0x10 fix-up exercised for the first time, and a receipt run on a stage with no memory dump.
-- SUPERGUN ENGINE lane, 2026-09-04 (odd seed numbers are this lane).
-- Doc: mvc-live-skins-quarters/docs/RECEIPT-RUNNER-GATE4.md
-- Tools: d3dcap/receipt/h1_gate.py ; d3dcap/receipt/dcram_build.py (--stage / --fill / --poison-e)
-- Idempotent and replayable; explicit edge ids; no note owned by another seed is overwritten.
USE NS re DB kb;

-- ---------------------------------------------------------------- sources
UPSERT source:tool_h1_gate SET kind='tool', strength='code',
  ref='mvc-live-skins-quarters/d3dcap/receipt/h1_gate.py',
  note='Perturbation test for hypothesis H1. Scrambles the VERTEX PAYLOAD (rec+0x50 .. rec+0x50+rec[0x4C]) of every record of every destination object at *(node+0xA0) for the System-A nodes of the chosen lists, leaving the 0x50-byte record headers intact so the NaomiLib iterator can still walk. Arm 1 compares blk per tick. Arm 2 reconstructs DC-RAM at tick N by replaying rr_runner --harvest-dump page deltas and asks how much of each payload is still poisoned, classifying each object STATIC / PARTIAL / REGENERATED.';
UPSERT source:tool_dcram_poison SET kind='tool', strength='code',
  ref='mvc-live-skins-quarters/d3dcap/receipt/dcram_build.py --fill HH --poison-e HH --stage N',
  note='--fill sets every DC-RAM byte the loader does NOT write (classes L and X); --poison-e fills the six base+0x145000 engine regions (class E); --stage overrides the stage id from blk so any stage bank and its fix-ups can be built and exercised without a dump of that stage.';
UPSERT source:gate4_run_20260904 SET kind='capture', strength='runtime',
  ref='%TEMP%/rr_h1 and %TEMP%/rr_g4 ; stage-9 run d3dcap/ttd/runs/receipt-20260903-stage9-anchor ; Carnival anchor from tape local_1788482771252_stage3_76561197999665347.json.gz (stage 3, roster 42/34/44/6/50/16, assists 0/0/1/1/1/2, agent 0.3.50, 4461 rows, 777 rollbacks)',
  note='2026-09-04. The Carnival run has NO corresponding dump_live: its DC-RAM was built entirely from game_50.arc.';

-- ---------------------------------------------------------------- findings
UPSERT finding:h1_falsified SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='H1 is false: a drawn node destination object is NOT write-before-read within the frame',
  statement='RECEIPT-RUNNER-RE s1.5 hypothesised that every drawn list node had its +0x18 callback run earlier in the same frame, so the destination object is always rewritten before it is read and its previous content never matters. Perturbing the vertex payload of all 16 destination objects of the stage-9 anchor lists 5/6/7 (26,072 B scrambled to 0xA5) and replaying the DC-RAM deltas to tick 60 shows: 14 objects STATIC (never rewritten at all, including the 14,864 B and 8,480 B stage deck objects and eleven effect quads), 2 objects PARTIAL (only the animated vertex fields rewritten -- 0x0D853158 TCW 0xC12 leaves 244 of 352 B untouched, 0x0D855800 TCW 0xC1C leaves 312 of 408 B), 0 objects fully regenerated. RE s1.5 also declared the test unrunnable for want of a real-stage dump with list-5 props; that was stale, the stage-9 receipt run has five.',
  consequence='Two parts. (1) SIMULATION: unaffected -- blk is byte-identical on 61 of 61 ticks with all 26 KB scrambled, so the anchor never needed these objects. (2) PIXELS: their prior content IS read every frame, which is not a gap because GATE 3 rebuilds exactly those bytes from the user arc byte-exactly. The arc loader path is therefore LOAD-BEARING for pixels, not merely convenient. This also supplies the mechanism for the GATE 3 class-A residual: the vertex fields a callback does not touch are inherited forever, so pristine arc bytes can never match a live mid-match phase in those fields.',
  gate='h1_gate.py --lists 5 6 7 --mode verts --ticks 60: blk 0 of 61 dumps differ; object census 14 STATIC / 2 PARTIAL / 0 REGENERATED.';

UPSERT finding:ctx_out_blind_to_submitted_geometry SET status='confirmed', confidence='high', date='2026-09-04',
  title='ctx_out is a BLIND criterion for whether DC-RAM bytes reach the renderer',
  statement='Scrambling 26 KB of drawn-object vertex payload left rr_runner ctx_out.bin byte-EQUAL, which would have read as H1 holding. It does not: the submitted geometry goes to the host staging buffer at *(game_state+0x208), past the DC-RAM image, which rr_runner does not dump (FRAME-READSET s3.5). The perturbation demonstrably does reach the renderer input -- the harvested tape changed (54,585 to 43,206 B gz, aobjs 153 to 154). Use the harvest, never ctx_out, to decide whether DC-RAM bytes are consumed.';

UPSERT finding:dcram_classes_elx_proven_unread SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='DC-RAM classes E, L and X are PROVEN unread, not merely measured unread',
  statement='Two arc-built images of the SAME Carnival anchor differing only in bytes the loader never writes -- arm A fill 0xCD with the six base+0x145000 engine regions 0x00, arm B fill 0x5A with those regions 0xA5, about 6.9 MB of difference -- produce 300 of 300 byte-identical blk dumps over 300 ticks, with gs_out, exe_dat_out and ggpo_out equal and the RNG identical at tick 300. ctx_out differs by 102 B, entirely in the page-table-records bucket (38 runs from ctx+0x10D1B8, stride 0x130), i.e. the GATE1 s3.5 nondeterminism family, below the ~128 B run-to-run floor. So the 12 KB per PL slot written by FUN_140612180, the leftovers past every loaded file, and the select/VS/result banks are not on the frame read path. Tested on a stage with morph props and a roster never previously gated.',
  consequence='The GATE 3 s7 item (call the game own loaders) is no longer needed to CLOSE class E. It remains the right end state for fidelity but it is not blocking.';

UPSERT finding:stage10_isp_tsp_fixup_exercised SET status='confirmed', confidence='high', date='2026-09-04',
  title='The stage-0x10 ISP/TSP fix-up executes correctly -- first execution, on real arc data',
  statement='dcram_build.py --stage 16 builds the River Raft bank (AFS 833/834) from the arc with no dump of that stage. The FUN_14060d470 stage-0x10 branch changes exactly 16 bytes: models 1..8, two records per model, one of each branch -- texIndex == 10 sets rec+0x4 bit 0x4000000 (byte rec+0x7, 0x83 to 0x87) and texIndex == 5 clears rec+0x0 bit 0x2000000 (byte rec+0x3, 0x82 to 0x80). MODEL 0 IS NOT TOUCHED, which exercises on real data the correction to RECEIPT-RUNNER-DCRAM s2.3 (models 0..7): the counter starts at -1 and is tested before its post-increment, so the range is 1..8. Structural gate on the resulting image: 26 models, 104 records walked with 0 malformed, every record satisfies TCW == 0xC10 + texIndex, 11 texture records with 0 locs outside the TEX span.',
  open_question='The stage-0x10 image has never been compared against a LIVE River Raft dump; none exists.';

UPSERT finding:receipt_runs_without_any_dump SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='A receipt runs on a stage for which no memory dump exists, from battle anchor plus the user arc',
  statement='The Carnival anchor came from a tape (local_1788482771252_stage3, agent 0.3.50); no dump_live run of that match exists, and the stage-9 dump DC-RAM is wrong for it in both stage bank and PL roster. Its DC-RAM was built entirely from game_50.arc by dcram_build.py, and rr_runner executed 300 ticks cleanly: 18 of 18 C1 self-checks, clock 2756 to 3056, RNG advancing, 0 import traps, tick p50 about 0.065 ms. This is the GATE 3 claim demonstrated end to end on bytes that were never captured.',
  open_question='anchor_to_run.py still needs a SAME-BOOT dump for exe_image.bin, the three ctx self pointers and ctx+0x1F80A0..0x1F8280. Once the agent carries that 480 B block in the battle anchor, every anchored tape becomes runnable with no dump at all -- gs-cache-local already holds anchored tapes for stages 0, 1, 2, 3, 7, 10, 15 and 16.';

UPSERT finding:rollback_min_state_set_is_stage_dependent SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='The GATE N1 8-byte ctx requirement is stage and situation dependent, which makes the superset mandatory',
  statement='Re-running the N1 ladder on the Carnival anchor (depths 0/2/8/30/120, about 200 events each): save set blk PASSES at every depth, where on stage 9 it FAILED from depth 2 up at object-pool node 144 field +0x124. Save set rr (blk plus ctx+0x1F8230..0x1F8238) PASSES on both. So blk-only rollback is not wrong everywhere -- it is wrong SOMETIMES, depending on stage, camera and situation, which is exactly the failure mode that only appears online under load. The shippable conclusion is unchanged and better supported: take the superset rr = 211,760 B. Carnival cost, worst case with a rollback every frame, set rr: depth 2 = 0.129, depth 8 = 0.405, depth 30 = 1.048, depth 120 = 1.823 ms per frame against 16.667. The N1 control arm also re-ran here: 39 of 40 events show ctx render-scratch nondeterminism, 7,501 B, 100 percent in the page-table-records bucket, 0 fatal mismatches.';

-- scope note on the N1 finding (additive field; the original statement is left intact)
UPSERT finding:rollback_min_state_set SET
  scope_note='2026-09-04 GATE 4: the blk-only FAILURE was measured on the stage-9 anchor. On the Carnival (stage 3) anchor blk-only PASSES at every depth. The 8-byte requirement is therefore CONDITIONAL, not universal; the sufficient-everywhere set is still rr. See finding:rollback_min_state_set_is_stage_dependent.';

-- ---------------------------------------------------------------- edges
RELATE finding:h1_falsified->cites:g4_h1_tool->source:tool_h1_gate;
RELATE finding:h1_falsified->cites:g4_h1_run->source:gate4_run_20260904;
RELATE finding:ctx_out_blind_to_submitted_geometry->cites:g4_blind_tool->source:tool_h1_gate;
RELATE finding:dcram_classes_elx_proven_unread->cites:g4_elx_tool->source:tool_dcram_poison;
RELATE finding:dcram_classes_elx_proven_unread->cites:g4_elx_run->source:gate4_run_20260904;
RELATE finding:stage10_isp_tsp_fixup_exercised->cites:g4_s10_tool->source:tool_dcram_poison;
RELATE finding:stage10_isp_tsp_fixup_exercised->about:g4_s10_routine->steam_routine:FUN_14060d470;
RELATE finding:receipt_runs_without_any_dump->cites:g4_nodump_tool->source:tool_dcram_poison;
RELATE finding:receipt_runs_without_any_dump->cites:g4_nodump_run->source:gate4_run_20260904;
RELATE finding:rollback_min_state_set_is_stage_dependent->cites:g4_n1stage_tool->source:tool_rr_rollback;
RELATE finding:rollback_min_state_set_is_stage_dependent->cites:g4_n1stage_run->source:gate4_run_20260904;
RELATE finding:rollback_min_state_set_is_stage_dependent->about:g4_n1stage_glob->global:steam_ctx_1F8230;
