-- ============================================================================
-- 87: the second evidence pass, and the demotions that survive it
--
-- 82 recovered the findings that named a marvelous2 PC. 83 section 2 recovered
-- the ones that named a repo file. This handles what was left: findings whose
-- evidence sits in a BESPOKE KEY rather than in the model, and findings that
-- genuinely have none.
--
-- The starting number is 17: `confirmed` findings that can show neither a
-- reproduction/code source NOR a routine cite. (The stricter count -- no
-- qualifying SOURCE at all, ignoring routine cites -- is 62. See the note at
-- the bottom; that gap is a real disagreement about what a routine cite proves
-- and it is deliberately NOT resolved by fiat here.)
--
-- Apply: tools/re_kb/rekb.sh @tools/re_kb/87_evidence_and_demotion.surql
-- ============================================================================
USE NS re DB kb;

-- ============================================================================
-- SECTION 1 -- EVIDENCE THAT EXISTS, IN THE WRONG PLACE
-- ============================================================================
-- Three findings carry a `measured` field holding a deterministic A/B result
-- with exact counts, and two also carry the commit that shipped the fix. That
-- is reproduction-grade by 77's ladder -- it was simply never modelled as a
-- source, so every audit read these as unbacked. They are among the
-- best-evidenced rows in the graph.
--
-- The evidence text is copied VERBATIM into `ref` so the number travels with
-- the citation instead of being re-summarised.

UPSERT source:gsta_ab_palette_bank_census SET
  kind='validation', strength='reproduction',
  title='A/B per-sprite aligned diff -- palette-bank census (GSTA vs engine)',
  ref='palette-bank mismatch 0.00% over 41,163 / 12,682 body tiles (two runs). REAL and GSTA both emit {16,17,18,24,25} incl odd siblings. Pre-fix static override emitted only even {16,24}. commit f2e81a82f',
  note='Recovered 2026-09-01 from finding:gsta_palette_bank_preserve\'s `measured` key. It also carries `disproved`: the \'palette snapshot-timing gap\' doc guess -- the diff showed the cause was the static even-bank override, not timing. The DIFF beat the doc.';

UPSERT source:gsta_ab_carve_census SET
  kind='validation', strength='reproduction',
  title='A/B aligned diff -- carve/tile-coverage census (GSTA vs engine)',
  ref='Aligned A/B: V-orient 0.03%, palette 0.00%, TSP 0.00% -- no tile-coverage/carve fragmentation detected. The only body diff is the 1.46% texU mirror residual.',
  note='Recovered 2026-09-01 from finding:gsta_native_carve_matches_web\'s `measured` key.';

UPSERT source:gsta_ab_texu_facing_census SET
  kind='validation', strength='reproduction',
  title='A/B census -- texU mirror, facing-XOR vs flip4000-alone',
  ref='facing-XOR formula -> U mismatch 1.46% (599/41163); flip4000-ALONE -> U mismatch 35.04% (4551/12989). flip4000-alone un-mirrors every facing==1 tile. commit 7819056df',
  note='Recovered 2026-09-01 from finding:gsta_texu_mirror_facing_xor\'s `measured` key. Its `disproved` key is the interesting half: flip4000-alone was what the loc_8c0347bc disasm suggested, and the DIFF overruled the disasm reading. A measurement beating a static read is exactly the case the ladder exists to adjudicate.';

RELATE finding:gsta_palette_bank_preserve->cites->source:gsta_ab_palette_bank_census;
RELATE finding:gsta_native_carve_matches_web->cites->source:gsta_ab_carve_census;
RELATE finding:gsta_texu_mirror_facing_xor->cites->source:gsta_ab_texu_facing_census;

-- ============================================================================
-- SECTION 2 -- SOURCES MIS-KINDED AS `measurement`
-- ============================================================================
-- 77's discriminator (lines 88-92) is explicit: FROZEN INPUT + pass/fail is a
-- reproduction; a census over a LIVE session is a metric. These three each
-- carry a `ref` naming a RECORDED CAPTURE PAIR -- _live_fx7.{gsta.mcrr,
-- mirror.zcst} and friends. The input is frozen on disk and the comparison
-- re-runs to the same answer, which is the definition on the reproduction side
-- of that line. Re-kinding them is applying the rule, not bending it.
--
-- This is the same call made for eleven other sources in 83 section 4; these
-- three were simply missed there.

UPDATE source:fx7_ab              SET kind='validation', strength='reproduction';
UPDATE source:gsta_render_gaps_ab SET kind='validation', strength='reproduction';
UPDATE source:render_path_audit   SET kind='validation', strength='reproduction';

-- NOT re-kinded, deliberately:
--   source:render_path_diff_trace  kind='expert_trace' -- a reading, not a run.
--   source:headless_crash_22880    kind='crash_dump+log' -- one fault, once.
--                                  A dump shows state at a crash; that is a
--                                  correlation until it is re-run.

-- ============================================================================
-- SECTION 3 -- EVIDENCE RECOVERED FROM THE SEED'S OWN PROSE
-- ============================================================================
-- Two findings keep their whole claim in `summary` and cite nothing. Their
-- seed files state the method in a COMMENT -- weaker provenance than a field,
-- but checkable, and recorded as such rather than laundered into a fact.

UPSERT source:carve_yfirst_vram_bytematch SET
  kind='validation', strength='reproduction',
  title='Y-first non-square tile twiddle -- byte-exact vs engine VRAM (flycast twop)',
  ref='re_kb/46_carve_nonsquare_yfirst_twiddle.surql header: CONFIRMED-BY-MEASUREMENT 2026-06-15, byte-exact vs engine VRAM, flycast twop',
  note='Provenance is the SEED FILE COMMENT, not a field -- recorded 2026-09-01. Weaker than a captured artifact and checkable: re-run the twop comparison for W>32/H>32 non-square parts.';

RELATE finding:carve_nonsquare_yfirst_twiddle->cites->source:carve_yfirst_vram_bytematch;

-- tcw_palsel_patch_pipeline names six writer PCs (0x8C124030, 0x8C12405A,
-- 0x8C124A8E, 0x8C124978, 0x8C1241A8, 0x8C11BB90). Its seed (62) states
-- "CONFIRMED (marvelous2 static disasm, 2026-07-05)" -- code-grade, and the
-- generic disassembly source is the honest citation until someone attaches the
-- specific bank lines.
RELATE finding:tcw_palsel_patch_pipeline->cites->source:marvelous2;

-- ============================================================================
-- SECTION 4 -- THE DEMOTIONS
-- ============================================================================
-- What is left cannot show reproduction- or code-grade evidence, and saying so
-- is the point of the vocabulary. `inferred` does not delete anything: the
-- statement, the note and the history all stay, and each row records what
-- would re-promote it. This is the rule working, not the rule being
-- inconvenient.

UPDATE finding:asmtrace_log_enospc_freeze SET status='inferred',
  triage_note='2026-09-01: true and useful, but an OPS observation about our own tooling (tmpfs fills, ASMTRACE freezes silently) with no cited evidence. The evidence ladder is built for claims about MVC2 and fits this badly -- see the structural note in 83 section 2. Re-promote with a reproduction: fill the tmpfs, show the freeze.';

UPDATE finding:emitter_facing_dump_tool SET status='inferred',
  triage_note='2026-09-01: records that a TOOL was added (webgpu-test.html v=78, sprite-client.mjs). An implementation note, not an RE finding. Cite the source file to re-promote.';

UPDATE finding:hud_texture_orientation_sprite_vs_poly SET status='inferred',
  triage_note='2026-09-01: a one-line assertion (HUD names flip / portraits upright = ParaType-5 sprite vs ParaType-4 poly) with no cite and no measurement. Plausible and unproven.';

UPDATE finding:storm_16x16_parts_reconcile SET status='inferred',
  triage_note='2026-09-01: a RECONCILIATION -- it reasons from finding:storm_tilecount_authoritative to explain why RAM headers disagree with the disc rip. Sound reasoning is what `inferred` is for; the evidence lives in the finding it reasons from.';

UPDATE finding:headless_crash_instrumentation_av SET status='inferred',
  triage_note='2026-09-01: cites source:headless_crash_22880 (kind=crash_dump+log, metric). A dump shows the state at ONE fault. That is evidence it happened once, which is a correlation -- re-run it and record the run to promote.';

UPDATE finding:headless_minimal_stable SET status='inferred',
  triage_note='2026-09-01: same source, same limit. "Restarts stable with minimal instrumentation" is an absence of crashes over an unstated period -- recurrence-grade at best, and 77 ranks recurrence as not sufficient alone.';

UPDATE finding:hud_fill_poly_located SET status='confirmed',
  triage_note='2026-09-01: KEPT confirmed. Cites source:render_path_audit, re-kinded to validation in section 2 above (its ref names the frozen _live_fx7 capture pair), and its own text carries the measurement: the P2 bar poly spans x[438.8..592.5] at p2hp=140 and shrinks to x[438.8..440.5] at p2hp=55.';

UPDATE finding:render_path_verdict SET status='inferred',
  triage_note='2026-09-01: this is a DECISION ("render_frame is the drawer to invest in"), not a claim about the game. A choice has no evidence grade -- it has reasons and consequences. It cites source:render_path_diff_trace (expert_trace, attestation), which is the right strength for the reasoning behind a decision. The graph has no `decision` subject yet; flagged in 83 section 2.';

UPDATE finding:emitter_tile_scale_charq_calibrated SET status='inferred',
  triage_note='2026-09-01: the scale constant was FITTED to CHARQ ground truth, not derived from the engine. Already recorded as approach:emitter_scale_charq_calibration --tried_on--> outcome=masks_only in 83. A masks_only result should not also stand as a confirmed fact about MVC2.';

-- ============================================================================
-- THE 62-vs-17 GAP -- NOT RESOLVED HERE, ON PURPOSE
-- ============================================================================
-- kb.health() reports two numbers and they disagree by design:
--
--   17  confirmed findings with no qualifying evidence, counting a
--       ->cites->routine edge as code-grade
--   62  the same count when ONLY sources are allowed
--
-- The 45 in between rest entirely on a routine cite. The process review's
-- objection is sharp and correct: that edge carries only (in, out) -- no line,
-- no instruction, no excerpt. `finding:x -> cites -> routine:loc_8c0344d4`
-- asserts "this claim MENTIONS that routine", not "this claim is VISIBLE at
-- these instructions". A LOCATOR IS NOT A CITATION.
--
-- The fix is to put the evidence ON the edge:
--
--   RELATE finding:x->cites->routine:loc_8c0344d4
--     SET at='+0x2E', shows='tst r8 / neg r5';
--
-- That cannot be generated -- it requires reading the disassembly at each PC
-- and saying what is visible there. Demoting 45 findings to force the issue
-- would destroy real knowledge to satisfy a metric, and accepting the locator
-- silently would launder it. So both numbers are reported, the gap is named,
-- and it stays open until someone does the reading.
-- ============================================================================
