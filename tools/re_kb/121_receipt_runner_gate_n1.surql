-- 121_receipt_runner_gate_n1.surql -- GATE N1: rollback-state SUFFICIENCY for the receipt runner, and the
-- interleaved save/restore/resim cost (SUPERGUN ENGINE lane, 2026-09-04; odd seed numbers are this lane's).
-- Deliverables: d3dcap/receipt/runner/rr_runner.cpp (--rollback / --rb-set / --rb-verify / --rb-ctx-extra),
-- d3dcap/receipt/runner/gate_n1.py, docs/RECEIPT-RUNNER-GATE-N1.md (mvc-live-skins-quarters).
-- Idempotent and replayable: every statement is an UPSERT of a node this seed owns, or a RELATE with an explicit
-- edge id (a replay reports "already exists" for those, which is benign). No note that another seed may later
-- refine is overwritten.
USE NS re DB kb;

-- ---------------------------------------------------------------- sources
UPSERT source:tool_rr_rollback SET kind='tool', strength='code',
  ref='mvc-live-skins-quarters/d3dcap/receipt/runner/rr_runner.cpp --rollback N --rb-set S --rb-verify V --rb-ctx-extra LO-HI; driver d3dcap/receipt/runner/gate_n1.py',
  note='Interleaved rollback harness. Every tick the chosen save set goes into a ring of N+1 slots; after tick k >= N the slot from tick k-N is restored, the same N inputs are re-ticked, and EVERY candidate region (blk, blk2, game_state page, exe page 0x142edf300, the whole 4 MB ctx, the whole 32 MB DC-RAM and every lazily committed host page including the device object at *(0x140acd3a8)) is byte-compared against the straight-line state at the same clock. Save/restore/resim are timed inside that loop, not composed from separate measurements. --rb-ctx-extra takes arbitrary ctx sub-ranges so a failure can be bisected without a rebuild.';
UPSERT source:gate_n1_run_20260904 SET kind='capture', strength='runtime',
  ref='%TEMP%/rr_n1 (control_full_1, corr_*/time_* for sets rr and blk at depths 0,2,4,8,16,30,60,120, bisect arms bi_*, bx); run d3dcap/ttd/runs/receipt-20260903-stage9-anchor, 200 receipt ticks from clock 1716',
  note='2026-09-04, stage 9, roster [42,23,52,12,44,46], real receipt inputs.';

-- ---------------------------------------------------------------- findings
UPSERT finding:rollback_min_state_set SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='Rollback for the receipt runner needs blk PLUS eight bytes of ctx -- the shipped blk-only set is NOT sufficient',
  statement='The shipped MvC2 netcode persists only the state block in its GGPO save/load callbacks. For the receipt runner that is NOT enough: with only blk (+ the wrapper GGPO counter) restored, a resimulation of the same inputs diverges in blk itself on 3 of ~193 events at every depth from 2 to 120 -- at object-pool node 144 field +0x124, a sprite-walker placement field (blk+0x1D6FC = DC 0x8C27B034 through blkmap). Restoring ctx+0x1F8230..0x1F8238 (EIGHT bytes, two f32) as well makes the resim byte-exact. Bisection: the failure survives blk+blk2+game_state page+exe page+the ctx texture-slot table+the ctx matrix state; it survives restoring all 32 MB of DC-RAM (set simdc); it disappears when the whole 4 MB ctx is restored (set simctx); inside ctx it narrows to 0x1F8230..0x1F8238, and EACH 4-byte half fails on its own, so both dwords are load-bearing. Minimal sufficient set = blk (0x33B18) + GGPO counter (0x10) + those 8 bytes = 211,760 B.',
  gate='gate_n1.py, run receipt-20260903-stage9-anchor, 200 ticks, depths 0/2/4/8/16/30/60/120: save set rr = PASS at every depth (0 fatal mismatches on blk, blk2, game_state page, exe page, the 32 MB DC-RAM image and every committed lazy host page); save set blk = FAIL from depth 2 upward (1-3 events per depth, always node 144 +0x124).';

UPSERT finding:rollback_interleaved_cost SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='Interleaved rollback cost for the receipt runner, worst case = a rollback on EVERY frame',
  statement='Measured with the sufficient save set (rr, 211,760 B) and a blk-only verification copy, i.e. what a real rollback does. Per-event p50: save 0.006-0.023 ms, restore 0.004-0.024 ms, resim = depth x the tick. Amortised over every straight-line frame, with a rollback fired on EVERY frame: depth 0 = 0.018, depth 2 = 0.088, depth 4 = 0.187, depth 8 = 0.286, depth 16 = 0.721, depth 30 = 1.083, depth 60 = 2.316, depth 120 = 2.327 ms per frame, against a 16.667 ms budget -- so even a 120-frame ring costs about 14 percent of one frame. The save/restore cache effect the netcode lane measured separately is reproduced interleaved: restore p50 rises from 0.0045 ms at depth 0 (207 KB ring) to about 0.024 ms at depth 120 (25 MB ring), roughly 5x, while the per-event copy stays 211 KB. Context for the design decision: the shipped game pays 4 frames = 66.7 ms of unconditional input delay to avoid this cost.';

UPSERT finding:tick_ctx_render_scratch_nondeterministic SET status='confirmed', confidence='high', date='2026-09-04',
  title='Re-executing the identical frame from a fully restored state rewrites two ctx render-scratch regions differently',
  statement='CONTROL: depth 1 with save set full -- blk, blk2, game_state page, exe page, the whole 4 MB ctx, the whole 32 MB DC-RAM and the GGPO counter all restored -- then ONE tick re-executed with the same input word. blk, blk2, game_state and exe come back byte-identical on every event, but ctx does not: 39 of 40 events differ, 5,796 B total, entirely inside ctx+0x100030..0x108FC0 (decoded texture pages, 124 B) and ctx+0x108FC0..0x1E0030 (page-table records, 5,672 B). Nothing is left that could carry the difference, and neither range is on the frame ctx READ set (FRAME-READSET s3.4: reads are ctx+0x8, +0x1E0030..0x1E31CA and +0x1F80A4..0x1F8564). This is the RECEIPT-RUNNER-GATE1 s3.5 uninitialised-stack family, now reproduced INSIDE one process with everything else held identical, and it is the same family as the 128 B ctx_out residual of RECEIPT-RUNNER-GATE3 s5. Consequence: these two ctx buckets must be excluded from any byte gate; rr_runner classifies them automatically (ctxBucketNondet) and never fails on them, while any OTHER ctx bucket differing -- especially the texture-slot table, which the tick reads as well as writes -- is a real failure.';

UPSERT finding:frame_readset_ctx_incomplete SET status='confirmed', confidence='high', date='2026-09-04',
  title='FRAME-READSET s3.4 understates the ctx state a frame depends on',
  statement='FRAME-READSET s3.4 was derived from one training-stage idle frame and lists the ctx reads as ctx+0x8, the texture-slot table and the matrix slots. GATE N1 shows a frame also depends on ctx+0x1F8230..0x1F8238 across a rollback: restoring everything except those 8 bytes changes a blk walker field. They sit inside the NaomiLib projection/viewport block that FUN_140846a40 initialises wholesale (MOVUPS [ctx+0x1F8230],XMM1 at 0x140846BF7, called from FUN_14060b550, caseD_9 and caseD_0). No literal 0x1F8230 or 0x1F8234 displacement occurs anywhere in the 10,803-function disassembly cache, so the per-frame access is indexed and the READER is UNKNOWN; the requirement is CONFIRMED by bisection, the identity of the fields is INFERRED. Neighbours for orientation: ctx+0x1F8200 and +0x1F8214 = +/-812.357 (the world-camera focal length), +0x1F8220 = -320.0, +0x1F8224 = -338.4 (the ground offset).',
  open_question='Which routine reads ctx+0x1F8230..0x1F8238 during a frame, and what are the two floats? Find it by watchpoint or by searching for indexed accesses with a base of 0x1F81C0/0x1F8200/0x1F8220.';

UPSERT finding:runner_rollback_dcram_not_needed SET status='confirmed', confidence='high', date='2026-09-04',
  title='DC-RAM and the host device page do NOT need to be in the rollback set',
  statement='At every depth from 2 to 120, with only blk + 8 ctx bytes + the GGPO counter restored, the whole 32 MB DC-RAM image and every lazily committed host page -- including the NaomiLib device object at *(0x140acd3a8) whose PALETTE_RAM the tick writes (RECEIPT-RUNNER-GATE1 s3.4) -- come back byte-identical after the resim. Falsification arm: save set simdc (blk + blk2 + gs + exe + ctx read set + ALL of DC-RAM) still FAILS, and simctx (the same without DC-RAM but with the whole ctx) PASSES, so DC-RAM carries none of the missing state. This closes the open question of whether the runner render-side host state has to be rolled back: it does not.';

-- ---------------------------------------------------------------- routines / globals touched
UPSERT steam_routine:FUN_140846a40 SET addr='0x140846a40',
  role='NaomiLib matrix-stack / projection-block initialiser: sets ctx+0x1F81B0 storage pointer, +0x1F81B8/BC capacity, +0x1F80A4 mode, the matrix slots from the 0x140AB20C0 constants, and MOVUPS the 16 bytes at ctx+0x1F8220 and ctx+0x1F8230; called from FUN_14060b550, caseD_9, caseD_0';
UPSERT global:steam_ctx_1F8230 SET addr='ctx+0x1F8230', name='NaomiLib projection/viewport pair (2 x f32)',
  confidence='inferred', evidence='required by GATE N1 bisection (both dwords); written wholesale by FUN_140846a40 at 0x140846BF7; per-frame reader UNKNOWN (no literal displacement in the disassembly cache)';

-- ---------------------------------------------------------------- edges
RELATE finding:rollback_min_state_set->cites:n1_minset_tool->source:tool_rr_rollback;
RELATE finding:rollback_min_state_set->cites:n1_minset_run->source:gate_n1_run_20260904;
RELATE finding:rollback_min_state_set->about:n1_minset_ctx->global:steam_ctx_1F8230;
RELATE finding:rollback_interleaved_cost->cites:n1_cost_tool->source:tool_rr_rollback;
RELATE finding:rollback_interleaved_cost->cites:n1_cost_run->source:gate_n1_run_20260904;
RELATE finding:tick_ctx_render_scratch_nondeterministic->cites:n1_nondet_tool->source:tool_rr_rollback;
RELATE finding:tick_ctx_render_scratch_nondeterministic->cites:n1_nondet_run->source:gate_n1_run_20260904;
RELATE finding:frame_readset_ctx_incomplete->cites:n1_readset_tool->source:tool_rr_rollback;
RELATE finding:frame_readset_ctx_incomplete->about:n1_readset_init->steam_routine:FUN_140846a40;
RELATE finding:frame_readset_ctx_incomplete->about:n1_readset_glob->global:steam_ctx_1F8230;
RELATE finding:runner_rollback_dcram_not_needed->cites:n1_dcram_tool->source:tool_rr_rollback;
RELATE finding:runner_rollback_dcram_not_needed->cites:n1_dcram_run->source:gate_n1_run_20260904;
