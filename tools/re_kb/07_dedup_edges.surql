-- ============================================================================
-- 07: dedup edge tables. RELATE is NOT idempotent; re-running seeds duplicates.
-- Graph arrows (->reads->) require edges created by RELATE (it maintains the
-- bidirectional graph refs); a plain CREATE on an edge table does NOT. So we
-- snapshot DISTINCT (in,out) pairs per table, wipe, then RELATE each pair back.
-- The arrow position needs a LITERAL table name, hence one block per table.
-- Run AFTER (re-)applying the seed files. Safe to run repeatedly.
-- ============================================================================
USE NS re DB kb;

LET $reads = (SELECT in, out FROM reads GROUP BY in, out);
DELETE reads;
FOR $p IN $reads { RELATE ($p.in)->reads->($p.out); };

LET $writes = (SELECT in, out FROM writes GROUP BY in, out);
DELETE writes;
FOR $p IN $writes { RELATE ($p.in)->writes->($p.out); };

-- `calls` carries via/resolved_by, so it must be deduped like tried_on:
-- rebuilding from (in, out) alone DESTROYS the properties. That happened on
-- 2026-09-01 -- 3,207 freshly-ingested edges lost `via` the moment this file
-- ran, which silently broke the indirect-reach audit the field exists for.
-- If you add a property to an edge table, its dedup block must group by it.
-- First drop PROPERTY-LESS duplicates: the seeds create `calls` with a bare
-- RELATE (no via), while ingest/marv_calls.py creates the same edge WITH via.
-- Grouping by (in,out,via) treats those as different edges and keeps both, so
-- the same call appears twice -- once usefully, once blank.
LET $withvia = (SELECT in, out FROM calls WHERE via != NONE GROUP BY in, out);
FOR $p IN $withvia {
  DELETE calls WHERE in = $p.in AND out = $p.out AND via = NONE;
};

LET $calls = (SELECT in, out, via, resolved_by FROM calls
              GROUP BY in, out, via, resolved_by);
DELETE calls;
FOR $p IN $calls {
  RELATE ($p.in)->calls->($p.out) SET via = $p.via, resolved_by = $p.resolved_by;
};

LET $lives_at = (SELECT in, out FROM lives_at GROUP BY in, out);
DELETE lives_at;
FOR $p IN $lives_at { RELATE ($p.in)->lives_at->($p.out); };

LET $maps_to = (SELECT in, out FROM maps_to GROUP BY in, out);
DELETE maps_to;
FOR $p IN $maps_to { RELATE ($p.in)->maps_to->($p.out); };

LET $about = (SELECT in, out FROM about GROUP BY in, out);
DELETE about;
FOR $p IN $about { RELATE ($p.in)->about->($p.out); };

LET $cites = (SELECT in, out FROM cites GROUP BY in, out);
DELETE cites;
FOR $p IN $cites { RELATE ($p.in)->cites->($p.out); };

LET $has_field = (SELECT in, out FROM has_field GROUP BY in, out);
DELETE has_field;
FOR $p IN $has_field { RELATE ($p.in)->has_field->($p.out); };

LET $instance_of = (SELECT in, out FROM instance_of GROUP BY in, out);
DELETE instance_of;
FOR $p IN $instance_of { RELATE ($p.in)->instance_of->($p.out); };

LET $owns = (SELECT in, out FROM owns GROUP BY in, out);
DELETE owns;
FOR $p IN $owns { RELATE ($p.in)->owns->($p.out); };

LET $part_of = (SELECT in, out FROM part_of GROUP BY in, out);
DELETE part_of;
FOR $p IN $part_of { RELATE ($p.in)->part_of->($p.out); };

LET $confirms = (SELECT in, out FROM confirms GROUP BY in, out);
DELETE confirms;
FOR $p IN $confirms { RELATE ($p.in)->confirms->($p.out); };

-- ---- the epistemics edges (added 2026-09-01) --------------------------------
-- 77_epistemics.surql DEFINEd supersedes / corrects / fixes -- they had been in
-- use since June without ever being declared -- but this dedup file was never
-- extended to cover them. RELATE is not idempotent, so every re-apply of the
-- seeds multiplied them: measured today, `supersedes` held 181 rows over ~10
-- distinct pairs (each duplicated 15-18x) and `corrects` held 15 rows over ONE
-- pair. A traversal then returns the same claim a dozen times and any count
-- over these edges is meaningless.
LET $supersedes = (SELECT in, out FROM supersedes GROUP BY in, out);
DELETE supersedes;
FOR $p IN $supersedes { RELATE ($p.in)->supersedes->($p.out); };

LET $corrects = (SELECT in, out FROM corrects GROUP BY in, out);
DELETE corrects;
FOR $p IN $corrects { RELATE ($p.in)->corrects->($p.out); };

LET $fixes = (SELECT in, out FROM fixes GROUP BY in, out);
DELETE fixes;
FOR $p IN $fixes { RELATE ($p.in)->fixes->($p.out); };

-- tried_on carries an OUTCOME on the edge, so two edges between the same pair
-- are not necessarily duplicates -- an approach can be tried twice with
-- different results. Dedup on (in, out, outcome), keeping the note and date of
-- one representative.
LET $tried_on = (SELECT in, out, outcome, note, date FROM tried_on
                 GROUP BY in, out, outcome, note, date);
DELETE tried_on;
FOR $p IN $tried_on {
  RELATE ($p.in)->tried_on->($p.out) SET outcome = $p.outcome, note = $p.note, date = $p.date;
};
