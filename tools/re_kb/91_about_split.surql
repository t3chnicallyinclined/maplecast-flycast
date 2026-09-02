-- ============================================================================
-- 91: split `about` -- it meant two different things at once
--
-- `about` was carrying BOTH
--     "this claim is about that ENTITY"   (finding -> address/routine/field)
-- and "this claim relates to that CLAIM"  (finding -> finding)
--
-- 161 of 806 edges (20%) were the second kind. That overload is why
-- kb.contradictions() could not work: the query groups claims BY the thing
-- they are about, so grouping over a mixed edge groups claims by other claims
-- and returns 164 rows of mostly-empty arrays. A broken detector is worse than
-- no detector -- it answers, so nobody looks again.
--
-- The finding->finding edges become `relates_to`. Deliberately GENERIC: some
-- are refinements, some are open questions hanging off a confirmed finding,
-- some are reconciliations. Classifying them further without reading each one
-- would be inventing structure. `supersedes` / `corrects` already exist for
-- the cases where the relationship IS known, and 13 of these pairs already
-- carry one -- those keep both edges, which is correct: "relates to" and
-- "supersedes" are different facts.
--
-- LET-scoped: apply WHOLE, never statement-by-statement.
-- Apply: tools/re_kb/rekb.sh @tools/re_kb/91_about_split.surql
-- ============================================================================
USE NS re DB kb;

DEFINE TABLE IF NOT EXISTS relates_to SCHEMALESS
  COMMENT 'finding -> finding. A generic association: refines, reconciles, raises a question about. NOT a claim that one replaces the other -- that is `supersedes`, and `corrects` for the narrower case. Split out of `about` on 2026-09-01, which had been carrying both meanings and breaking contradictions().';

LET $ff = (SELECT in, out FROM about WHERE record::tb(out) = 'finding' GROUP BY in, out);
FOR $p IN $ff { RELATE ($p.in)->relates_to->($p.out); };
DELETE about WHERE record::tb(out) = 'finding';
