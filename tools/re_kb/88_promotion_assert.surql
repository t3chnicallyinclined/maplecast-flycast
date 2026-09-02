-- ============================================================================
-- 88: THE GATE -- enforce the promotion rule at the DATASTORE
--
-- 77 claimed this rule "is not enforceable through rekb.sh". That was FALSE,
-- and believing it is why the graph shipped a convention wrapper (kb.py) and
-- called it enforcement. rekb.sh hands out arbitrary SQL with root
-- credentials, and README.md documents that as the primary interface -- so
-- every rule lived exactly one one-liner away from irrelevance.
--
-- SurrealDB enforces it itself, against the EXISTING cites model, no
-- migration. A finding may only be `confirmed` if it cites either
--   * a source of reproduction- or code-grade, OR
--   * a routine (a routine id IS its PC in the marvelous2 disassembly)
--
-- The routine arm is a KNOWN COMPROMISE, not an oversight. That edge carries
-- no line and no excerpt, so it proves the claim MENTIONS a PC rather than
-- that it is VISIBLE there -- see the closing note in 87. 43 confirmed
-- findings currently rest on it. Excluding it would reject all 43 on a rule
-- nobody has yet done the reading to satisfy; including it still stops the
-- thing that actually happened, which was `confirmed` with NO evidence at all.
-- kb.health() reports both numbers so the compromise stays visible.
--
-- TWO THINGS THIS DOES NOT DO, which must be said or they will be assumed:
--
--   ORDERING. It fires when `status` is written. The seeds are UPSERT-then-
--   RELATE, so a status would land before its citation. apply_seed.py defers
--   every status to a second pass -- it is now the ONLY supported way to load
--   the seeds. `rekb.sh @file` on a seed that sets a status will be rejected,
--   correctly.
--
--   DRIFT. Demoting a source's `strength` AFTER a confirm leaves the finding
--   confirmed. This is a gate on writes, not an invariant over time. The
--   periodic audit is kb.health(); it is not optional.
--
-- Verified before installation: 0 confirmed findings fail this rule
-- (87 took that from 17 to 0), so the seed rebuild still completes.
--
-- Apply: tools/re_kb/rekb.sh @tools/re_kb/88_promotion_assert.surql
-- Remove: REMOVE FIELD status ON finding;
-- ============================================================================
USE NS re DB kb;

-- TYPE option<string>, not string. `string` makes the field MANDATORY, and
-- the two-phase apply deliberately writes rows with the status stripped in
-- pass 1 -- a CONTENT replace then leaves status NONE and SurrealDB rejects
-- the row with "Expected `string` but found `NONE`". The finding would be
-- lost to enforce a field that pass 2 is about to set. Optional here means
-- "transiently absent during a load", not "a finding may have no status":
-- kb.health() reports the distribution and a status-less finding shows up.
-- OVERWRITE, not define-if-absent: re-applying this file must be able to
-- TIGHTEN or LOOSEN the rule. Plain DEFINE FIELD errors with "the field
-- 'status' already exists", which on a re-run looks like the new rule was
-- installed when the OLD one is still in force -- a silent no-op on the
-- one statement that must never silently no-op.
DEFINE FIELD OVERWRITE status ON finding TYPE option<string>
  ASSERT
    $value = NONE
    OR (
    $value INSIDE ['open','inferred','confirmed','ruled_out','superseded','resolved']
    AND (
      $value != 'confirmed'
      OR count(SELECT VALUE id FROM $this->cites->source
               WHERE strength IN ['reproduction','code']) > 0
      OR count(SELECT VALUE id FROM $this->cites->routine) > 0
    ));
