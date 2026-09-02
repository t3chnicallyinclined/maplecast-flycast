-- ============================================================================
-- 95: make `docnote` a QUEUE WITH A DRAIN RATE, not an archive
--
-- 80 split 576 doc bullets out of `finding`. The re-ingest took that to 1,167.
-- The split was right and it left a second problem behind: an untriaged pile
-- that grows every time the ingest runs and shrinks never.
--
-- 900 of 1,167 carry doc_cue='confirmed'. That value is a CHECKMARK SCRAPED
-- FROM MARKDOWN -- docs_parse.py maps a tick to "confirmed" -- and it is the
-- exact value that, when it was called `status`, made 478 changelog ticks
-- indistinguishable from disassembly-backed results. It is deliberately named
-- doc_cue and deliberately not a status. THE NEXT TOOL THAT TREATS IT AS ONE
-- RECREATES THE ORIGINAL BUG. This file makes that harder by giving the table
-- a real triage state of its own.
--
--   triaged      bool  -- has a human/agent looked at this row?
--   disposition  why it left the queue: 'plan' | 'promoted' | 'duplicate' |
--                'not-re' | 'stale'
--   promoted_to  the finding id, when a row actually became a claim
--
-- FIRST DRAIN: 267 rows from 20 PLAN and DECISION documents. A plan is a
-- statement about the FUTURE -- it cannot be confirmed, only executed or
-- abandoned -- so no plan bullet will ever become a finding. Marking them
-- triaged/'plan' takes a fifth of the queue out of the way of the rows that
-- might genuinely be claims. Nothing is deleted: the text stays searchable and
-- the row keeps its source_file.
--
-- Apply: PYTHONIOENCODING=utf-8 python tools/re_kb/apply_seed.py \
--            tools/re_kb/95_docnote_queue.surql
-- ============================================================================
USE NS re DB kb;

-- Everything starts untriaged, explicitly, so the queue has a denominator.
UPDATE docnote SET triaged = false WHERE triaged = NONE;

-- Plan / decision / roadmap documents: never promotable to a finding.
UPDATE docnote SET triaged = true, disposition = 'plan'
  WHERE string::contains(string::uppercase(source_file ?? ''), 'PLAN')
     OR string::contains(string::uppercase(source_file ?? ''), 'DECISION')
     OR string::contains(string::uppercase(source_file ?? ''), 'ROADMAP');

-- The graph's own paperwork: a handover about the KB is not knowledge about
-- MVC2. docs_parse.py already excludes tools/re_kb, but rows ingested before
-- that exclusion are still here.
UPDATE docnote SET triaged = true, disposition = 'not-re'
  WHERE string::contains(source_file ?? '', 'HANDOVER')
     OR string::contains(source_file ?? '', 'CHANGELOG');
