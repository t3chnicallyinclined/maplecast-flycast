-- 127_receipt_runner_n1bc.surql -- N1b (per-event cost, eligibility weighting withdrawn), N1c (the three-arm
-- shipped-window check), and the project-wide ctx_out rule. SUPERGUN ENGINE lane, 2026-09-04 (odd seeds).
-- Docs: mvc-live-skins-quarters/docs/RECEIPT-RUNNER-GATE-N1.md s1.2, s8, s9 ; -GATE4.md s4
-- Tool: d3dcap/receipt/runner/gate_n1c.py ; rr_runner.cpp (--heap-mb, per-event cost vectors)
-- Idempotent and replayable; explicit edge ids; additive fields only on findings owned by earlier seeds.
USE NS re DB kb;

UPSERT source:tool_gate_n1c SET kind='tool', strength='code',
  ref='mvc-live-skins-quarters/d3dcap/receipt/runner/gate_n1c.py',
  note='Three arms at ONE depth instead of a bisection. A = blk + GGPO counter (the shipped game save set). B = A + the proven 8 B ctx+0x1F8230..0x1F8238. C = A + the shipped 64 KB window ctx[0x1F0000..0x200000). Falsification condition: the save set survives iff arm C passes on EVERY stage tested; one arm-C failure kills it. Escalate to the full ladder only where B fails and C passes. Reports per-EVENT cost with p99 and max, the count of events over the 16.667 ms budget, and the tick count of every run.';

UPSERT finding:n1b_cost_is_per_event_not_amortised SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='The rollback cost column must be per-EVENT with p99 and max; the amortised ms/frame figure is withdrawn',
  statement='The amortised ms per frame column divided total rollback work by ALL ticks including the first N that carried no rollback, so it was ELIGIBILITY-WEIGHTED: it understated cost by (ticks-N)/ticks, was undefined for a run of <= N ticks, and read 4.5 ms per event at 200 ticks versus 3.0 at 300 for the same depth-120 data. A frame budget is a deadline, not an average. rr_runner now records event = save + restore + resim and frame = event + the straight-line tick of the same frame, and reports p50/p99/max of both plus the number of events over 16.667 ms and the tick count. Measured, save set rr, 300 ticks per run, a rollback after EVERY eligible tick (the pathological worst case), heap_wraps 0: worst single FRAME is 9.51 ms at depth 120 on stage 9 and 7.46 ms at depth 120 on Carnival, and ZERO events exceed the budget at any depth on either stage. At depth 30 the worst frame is 3.22 ms on stage 9 and 2.02 ms on Carnival.',
  consequence='Per-stage cost varies in BOTH directions -- stage 9 is dearer at depth 120 by 27 percent while Carnival is cheaper at depth 30 by 37 percent -- so cost is a function of stage content rather than an engine constant, and a single-stage table cannot size a speculation cap. For a cap of 32 the depth-30 row gives a worst-case frame of about 3.2 ms on the worse of the two stages measured, about 19 percent of budget.';

UPSERT finding:n1b_heap_wrap_timing_artefact SET status='confirmed', confidence='high', date='2026-09-04',
  title='The runner bump heap wrapping is a timing artefact that produced a false over-budget event',
  statement='The first per-event emission showed 1 event over the 16.667 ms budget at depth 120 (max 18.31 ms) and an 11.78 ms outlier at depth 60. Both were the runner own bump heap WRAPPING: a resim consumes it depth+1 times faster and a wrap memsets up to 64 MiB inside one tick. summary.json records heap_wraps, which read exactly 1 at those two depths and 0 everywhere else. With --heap-mb 1024 the outliers vanish (depth 120 max falls from 18.31 to 9.46 ms) and heap_wraps is 0 at every depth. RULE: any future rr_runner timing run must check heap_wraps before its numbers are quoted; both driver scripts now pass --heap-mb 1024.';

UPSERT finding:n1c_shipped_window_survives SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='N1c: the shipped 64 KB ctx window passes on every stage tested so far',
  statement='Three arms at depth 8, 300 ticks, a rollback after every eligible tick, verify all. Stage 9: arm A FAIL (tick 61, blk+0x1D6FC = object-pool node 144 field +0x124), arm B PASS, arm C PASS. Carnival stage 3 (arc-built, no dump exists): arm A PASS, arm B PASS, arm C PASS. Arm C -- blk + GGPO + the shipped 64 KB window ctx[0x1F0000..0x200000) -- has therefore passed on EVERY stage tested and the netcode lane s9.2 save set survives. Arm B has never failed, so the escalate case (B fails, C passes) has not been reached and the full ladder was not needed. Cost at depth 8, 293 events: arm C event p50/p99/max 0.514/1.672/1.896 ms on stage 9 and 0.341/0.678/0.725 ms on Carnival, 0 events over budget; the 64 KB window costs about 0.5 MB more ring per slot and no measurable per-event penalty beyond noise.',
  open_question='River Raft (stage 16) is not yet tested -- it needs the live dump that is with Tris. One arm-C failure on any stage kills the save set.';

UPSERT finding:ctx_hash_never_proves_pixels SET status='confirmed', confidence='high', date='2026-09-04',
  contribution_candidate=true,
  title='A save-set or ctx hash proves SIM agreement, never PIXEL agreement',
  statement='Submitted geometry lands in the host staging buffer at *(game_state+0x208), past the DC-RAM image, which nothing dumps (FRAME-READSET s3.5). Demonstrated in GATE 4: scrambling 26,072 B of drawn-object vertex payload left rr_runner ctx_out.bin byte-EQUAL while the harvested tape changed (54,585 to 43,206 B gz, aobjs 153 to 154). RULE for the whole SUPERGUN workstream: no result may be reported as "frames match" on the strength of a ctx or save-set hash; pixel claims need the harvest or a rendered comparison. This is the project long-standing failure of offline geometry standing in for live pixels, arriving from a new direction.',
  consequence='For the netcode resync design: the static geometry objects are read for pixels every frame and are NOT in any save set, so a state transfer cannot reconstitute a renderable peer -- the receiver must already hold them from its own arc.';

-- additive scope notes on findings owned by earlier seeds (original statements untouched)
UPSERT finding:rollback_interleaved_cost SET
  superseded_note='2026-09-04: the amortised ms/frame figures in this finding are WITHDRAWN as eligibility-weighted. See finding:n1b_cost_is_per_event_not_amortised for the per-event table with p99, max, over-budget counts and tick counts.';
UPSERT finding:rollback_min_state_set SET
  n1c_note='2026-09-04: settled per stage by the cheaper three-arm N1c check (finding:n1c_shipped_window_survives), not by re-running the full ladder.';

RELATE finding:n1b_cost_is_per_event_not_amortised->cites:n1b_tool->source:tool_gate_n1c;
RELATE finding:n1b_cost_is_per_event_not_amortised->cites:n1b_run->source:gate4_run_20260904;
RELATE finding:n1b_heap_wrap_timing_artefact->cites:n1bheap_tool->source:tool_rr_rollback;
RELATE finding:n1c_shipped_window_survives->cites:n1c_tool->source:tool_gate_n1c;
RELATE finding:n1c_shipped_window_survives->cites:n1c_run->source:gate4_run_20260904;
RELATE finding:n1c_shipped_window_survives->about:n1c_glob->global:steam_ctx_1F8230;
RELATE finding:ctx_hash_never_proves_pixels->cites:ctxrule_tool->source:tool_h1_gate;
RELATE finding:ctx_hash_never_proves_pixels->cites:ctxrule_run->source:gate4_run_20260904;
