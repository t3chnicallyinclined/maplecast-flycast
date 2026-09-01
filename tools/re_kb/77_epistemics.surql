-- ============================================================================
-- 77: epistemics -- evidence strength, attempt outcomes, supersession
--
-- Ports three pieces of the Incident Knowledge Base design
-- (resume/incident-kb-spec.html) into re_kb, because that design already
-- solves three problems this graph has:
--
--   1. ATTEMPTS ARE EDGES THAT CARRY OUTCOMES.
--      The graph records conclusions. It has no representation for "we tried
--      X and it did not work" -- which is the knowledge that stops the next
--      session re-walking a dead end. Today those live as prose inside a
--      `note` string, so they are greppable but not queryable.
--      IKB: "Modelling attempts as edges with results, rather than
--      mitigations as a property of causes, is what makes 'what has failed
--      here before' answerable."
--
--   2. EVIDENCE IS TYPED *AND RANKED*.
--      `source.kind` is descriptive (21 distinct values in use). `strength`
--      is ordinal, and it is what a promotion rule can actually test.
--      IKB: attestation is "enough for suspected, never for confirmed alone."
--
--   3. masks_only.
--      A change that produces the right output without the mechanism being
--      understood. Legitimate -- shipping it is often correct -- but
--      dangerous the moment it is mistaken for a real fix. This is the
--      "false win" status this project has needed all along, and it is why
--      senior-re-generalist exists as an agent.
--
-- Also DEFINEs supersedes / corrects / fixes. All three are already IN USE
-- (10 / 1 / 1 edges) but were never declared, so they appear in no schema, no
-- README query pattern, and no agent charter. SCHEMALESS let RELATE create
-- them silently.
--
-- Idempotent (DEFINE ... IF NOT EXISTS + UPDATE ... WHERE). Safe to re-run.
-- Apply: tools/re_kb/rekb.sh @tools/re_kb/77_epistemics.surql
-- ============================================================================
USE NS re DB kb;

-- ---- 1. the attempt subgraph -----------------------------------------------
-- An `approach` is a thing you can try: a probe config, a decode strategy, a
-- transform, a patch. It is NOT a finding. A finding is what you believe; an
-- approach is what you did.
DEFINE TABLE IF NOT EXISTS approach SCHEMALESS
  COMMENT 'something tried: a probe config, decode strategy, transform, patch. fields: name, how, note';

-- The heavy-lifting edge. The OUTCOME lives here, not on the approach --
-- because the same approach can work against one problem and fail against
-- another, and that difference is the most valuable data in the graph.
DEFINE TABLE IF NOT EXISTS tried_on SCHEMALESS
  COMMENT 'approach -> finding/problem. CARRIES outcome(effective/ineffective/masks_only/unproven), date, cost, note. The outcome is the point.';

-- ---- 2. supersession (in use since 2026-06, never declared) -----------------
DEFINE TABLE IF NOT EXISTS supersedes SCHEMALESS
  COMMENT 'finding -> finding. This replaces that. The old row STAYS, queryable, with its reason -- it is a lead, not garbage.';
DEFINE TABLE IF NOT EXISTS corrects SCHEMALESS
  COMMENT 'finding -> finding. Narrower than supersedes: one detail was wrong, the rest stands.';
DEFINE TABLE IF NOT EXISTS fixes SCHEMALESS
  COMMENT 'finding/approach -> finding. This closed that open question.';

-- ---- 3. evidence strength --------------------------------------------------
-- `kind` says what a source IS. `strength` says how much it is allowed to
-- prove. Ordinal, strongest first:
--
--   reproduction  we made it happen again (Oracle capture, ASMTRACE slice,
--                 a deterministic differ pass). Sufficient alone to confirm.
--   code          the mechanism is visible in source (marvelous2 PC+bank
--                 line, pl_mem/work.asm symbol, flycast source). Explains
--                 WHY, not just THAT. Sufficient alone to confirm.
--   metric        a number showing the behaviour (pixel diff, measurement
--                 pass). Good, correlation-prone. NOT sufficient alone.
--   recurrence    it stopped happening after the change. Good over time,
--                 weak immediately after. NOT sufficient alone.
--   attestation   a doc, a third-party page, a note, "I remember". Enough
--                 for `inferred`. NEVER sufficient for `confirmed`.
--
-- NOTE on kind='memory': in this graph that means the marvelous2 memory-map
-- files (pl_mem.asm, work.asm, buildSPL.sh) -- authoritative symbol tables,
-- NOT somebody's recollection. It ranks as `code`.
--
-- NOTE on the ROM/disc kinds (disc, disc_data, pldat): these rank
-- `reproduction`. They are not opinions about the game -- they are the game,
-- read off the user's own disc by a deterministic decoder. Anyone with the
-- ROM re-derives the same bytes, which is exactly the "we can make it happen
-- again" test. (Example: source:pl00_gfx2_cell63 backs
-- finding:ryu_sid63_two_part_is_correct_not_wire_gap by showing the decoded
-- GFX2 cell really does carry two parts.)
--
-- NOTE on kind='validation' vs kind='audit'. Both sound like proof; only one
-- is. `validation` is a deterministic differ pass with a pass/fail (0.00px
-- geometry, byte-exact transpile) -> reproduction. `audit` is a systematic
-- read-through producing a summary -- it establishes coverage, not
-- mechanism -> metric, never sufficient alone.
--
-- NOTE on kind='derivation': reasoned FROM other findings. That is what
-- `inferred` is for. It is attestation-grade no matter how sound the
-- reasoning, because the evidence is elsewhere -- cite that instead.
--
-- NOTE on kind='crash_dump+log': `metric`, not `reproduction`. A dump shows
-- the state at one fault. It is evidence that something happened once, which
-- is the definition of a correlation -- a reproduction is being able to make
-- it happen again. Re-run it and record the run as a capture to promote it.
--
-- The eight kinds above (disc_asset, disc_geometry, flycast_source,
-- client_source, disasm+measurement, crash_dump+log) were added on
-- 2026-09-01. They surfaced by themselves: the seeds that carry them had
-- never been applied to the live graph, and when they were, the `unranked`
-- default below listed them instead of quietly filing them as attestation.
-- That is the default doing its job -- keep it that way.

UPDATE source SET strength='reproduction'
  WHERE kind IN ['oracle','oracle_capture','capture','live_capture','capture_analysis',
                 'transpile_validation','validation',
                 'disc','disc_data','pldat','disc_asset','disc_geometry'];

UPDATE source SET strength='code'
  WHERE kind IN ['code','disasm','disassembly','memory','marvelous2','js_function','implementation',
                 'flycast_source','client_source','disasm+measurement'];

UPDATE source SET strength='metric'
  WHERE kind IN ['measurement','tool','audit','crash_dump+log'];

UPDATE source SET strength='attestation'
  WHERE kind IN ['anotak','doc','reference','recatalog','format','format_decode',
                 'expert_trace','spec','derivation'];

-- Anything still unmapped is 'unranked', NOT 'attestation'.
--
-- Defaulting an unknown kind to attestation is the weakest reading, which
-- sounds safe -- but it makes an unmapped kind INDISTINGUISHABLE from a
-- deliberately-weak one, so it never gets looked at again. `unranked` is
-- also unable to confirm anything, and it is greppable:
--
--     SELECT kind, count() FROM source WHERE strength='unranked' GROUP BY kind;
--
-- If that returns rows, the map above is out of date -- extend it, do not
-- let the default absorb it.
UPDATE source SET strength='unranked' WHERE strength = NONE;

-- ---- 4. the status vocabulary ----------------------------------------------
-- finding.status
--   open        unresolved question. Keep it loud.
--   inferred    reasoned, not yet grounded. Say what would confirm it.
--   confirmed   backed by >=1 cited source with strength reproduction|code.
--   ruled_out   investigated and eliminated. QUERYABLE -- this is the status
--               that saves the next session. Currently these exist only as
--               prose inside `note`.
--   superseded  we believed this, we were wrong, here is what it actually was.
--   resolved    a prior conflict closed.
--
-- tried_on.outcome
--   effective    worked, and the mechanism is understood (or 2+ independent
--                reproductions).
--   masks_only   produces the right output WITHOUT the mechanism being
--                understood. A tuned constant, a fudged coordinate, a
--                clamp that hides the real bug. Ship it if you must -- but
--                it is not a fix and must never be recorded as one.
--   ineffective  tried, did not help. The most under-recorded and most
--                useful outcome in the system.
--   unproven     tried once, seemed to help. Correlation, not causation.
--
-- ---- THE PROMOTION RULE ----------------------------------------------------
-- inferred -> confirmed requires ONE of:
--     * a cited source with strength='reproduction'
--     * a cited source with strength='code'
--
-- NEVER promoted by: a model, consensus, confidence, or a single correlation.
-- A model may PROPOSE promotion. It may not promote.
--
-- WHERE THIS IS ENFORCED -- corrected 2026-09-01.
--
-- This file previously said: "This rule is not enforceable through rekb.sh,
-- which accepts arbitrary SQL with root credentials... It is enforced in
-- tools/re_kb/kb.py instead." THAT WAS FALSE, and it mattered: it justified
-- shipping a convention wrapper and calling it a gate. kb.py is the ergonomic
-- surface; anyone typing `rekb.sh "UPSERT finding:x SET status='confirmed'"`
-- walked straight past it -- and README.md documents that as the primary
-- interface.
--
-- SurrealDB can enforce the promotion rule itself, at the datastore, against
-- the existing `cites` model, with no migration. Verified on 3.1.4 in a
-- throwaway database:
--
--   DEFINE FIELD status ON finding TYPE string
--     ASSERT $value != 'confirmed'
--         OR count(SELECT VALUE id FROM $this->cites->source
--                  WHERE strength IN ['reproduction','code']) > 0;
--
--   UPSERT finding:y SET status='confirmed'          -> REJECTED (no cite)
--   RELATE finding:y->cites->source:doc1 (attestation)
--   UPSERT finding:y SET status='confirmed'          -> REJECTED (too weak)
--   RELATE finding:y->cites->source:orc1 (reproduction)
--   UPSERT finding:y SET status='confirmed'          -> ACCEPTED
--   UPSERT finding:z SET status='inferred'           -> ACCEPTED (no cite needed)
--
-- Two things it does NOT do, which must be said or they will be assumed:
--   * ORDERING. The RELATE must precede the status write. The seeds are
--     UPSERT-then-RELATE, so applying them under this ASSERT needs a two-phase
--     pass (rows + edges first, statuses second).
--   * DRIFT. It fires on writes to `finding`. Demoting a SOURCE's strength
--     after the fact leaves the finding confirmed. It is a gate, not an
--     invariant -- a periodic audit is still required.
--
-- The ASSERT is NOT installed yet: 62 currently-`confirmed` rows cite no
-- reproduction- or code-grade SOURCE, so installing it today makes the seed
-- rebuild fail on all of them. That failure is correct and is the point; it
-- needs to be sequenced, not avoided.
-- ============================================================================
