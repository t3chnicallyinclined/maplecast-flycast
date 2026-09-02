# `re_kb` — architecture and rationale

**Audience:** an architect deciding whether to trust, extend, or copy this design.
**Status:** as built on 2026-09-02. Every number below was measured, not estimated.
**Code:** `tools/re_kb/`. Operational quickstart is in `CLAUDE.md`; this file is the *why*.

---

## 1. What it is

A curated knowledge graph of MVC2 / SH-4 reverse-engineering results, with three
properties that ordinary documentation does not have:

1. **Every claim carries its evidence, and evidence is *graded*.** A claim cannot
   be marked `confirmed` unless it cites something strong enough.
2. **The rule is enforced by the datastore**, not by convention — including on the
   raw-SQL path.
3. **It records what was ruled OUT**, not only what is true.

It is retrieved automatically: a hook injects relevant facts into an agent's
context when a tool call names an MVC2 address or SH-4 PC.

```
findings 257   confirmed 184 · inferred 21 · open 17 · ruled_out 13 · superseded 12 · resolved 10
routine 4,811  calls 3,215   cell 111,796   attack 3,384   docnote 1,167
address 956    field 2,248   source 449     store ~150 MB
```

---

## 2. What problem it solves

An RE project accumulates two things that plain docs lose:

- **Hard-won negatives.** "We tried per-frame state injection into the live SH-4;
  it always corrupts engine-owned pointers." Nothing in a codebase records that,
  so it gets re-attempted.
- **Which claim is current.** The same question gets answered five times over six
  months. Without a supersession model, all five answers look equally live.

And it accumulates one specific pathology, which this project had badly:
**claims drift upward in confidence over time.** A measurement becomes "confirmed",
a plan becomes a fact, a doc checkmark becomes a result. Measured 2026-09-01,
before this work: **529 confirmed to 93 inferred** — and every one of the 93 was
machine-generated. The hand-curated layer was **51 confirmed, 0 inferred**.

That ratio is the signature of a promotion rule that isn't enforced.

---

## 3. The five design decisions

### 3.1 Evidence is typed *and ranked*

`source.kind` is descriptive (~30 values). `source.strength` is **ordinal**, and it
is what a rule can test:

| strength | meaning | can confirm alone? |
|---|---|---|
| `reproduction` | we made it happen again — a capture, a deterministic differ with pass/fail | **yes** |
| `code` | the mechanism is visible in source — a marvelous2 PC, a `pl_mem.asm` symbol, flycast source | **yes** |
| `metric` | a number showing the behaviour | no |
| `recurrence` | it stopped happening after the change | no |
| `attestation` | a doc, a third-party page, "I remember" | no |

**The promotion rule:** `inferred → confirmed` requires a cited source of
`reproduction` or `code` grade. Never a model, never consensus, never confidence.

The lineage is deliberate: this is the **Admiralty Code** (source reliability ×
information credibility) and **GRADE** from evidence-based medicine. A case report
cannot support what a randomised trial can, and a changelog tick cannot support
what a disassembly line can.

The discriminator that does the most work, and the one to copy: **frozen input +
pass/fail is a reproduction; a census over a live session is a metric.** Applying
that one line correctly re-graded 14 sources.

### 3.2 Outcomes live on edges, not on things

Stated twice in the schema because the project paid for the lesson twice:

```
approach --tried_on{outcome}--> finding      what was tried, and how it went
routine  --verified_by{verdict,scope}--> oraclerun    byte-exactness, and over WHAT
```

`routine.verified_byte_exact` as a boolean would have been **true** for the
object-pool spawn chain on 2026-07-21 and **false** on 2026-07-22 — same code,
different seed. "Byte-exact" is a property of *(function-set, seed, tick range)*,
never of a function. So the verdict lives on the edge to the **run** that produced
it, with its `scope`.

`tried_on.outcome` has four values, and the third is the point:

- `effective` — worked, mechanism understood
- `ineffective` — tried, did not help *(the most under-recorded and most useful)*
- **`masks_only`** — right output, mechanism **not** understood. A tuned constant,
  a fudged coordinate, a clamp hiding the real bug. Ship it if you must; it is not
  a fix.
- `unproven` — tried once, seemed to help. Correlation.

`masks_only` exists because this project has repeatedly declared false wins.

### 3.3 The gate is in the datastore, not the application

The original design said the rule "is not enforceable through `rekb.sh`" and put it
in a Python wrapper. **That was false**, and believing it meant shipping ergonomics
and calling them enforcement — `rekb.sh` hands out arbitrary SQL with root
credentials, and the README documents that as the *primary* interface.

```sql
DEFINE FIELD OVERWRITE status ON finding TYPE option<string>
  ASSERT $value = NONE OR (
    $value INSIDE ['open','inferred','confirmed','ruled_out','superseded','resolved']
    AND ( $value != 'confirmed'
       OR count(SELECT VALUE id FROM $this->cites->source
                WHERE strength IN ['reproduction','code']) > 0
       OR count(SELECT VALUE id FROM $this->cites->routine) > 0 ));
```

Verified on the bypass path: unbacked confirm **rejected**; `inferred` with no cite
**accepted**; bogus status **rejected**.

Two consequences an architect must know:

- **Ordering.** The ASSERT fires when `status` is written. Seeds are
  UPSERT-then-RELATE, so a *correct* claim would be rejected because its citation
  doesn't exist yet. `apply_seed.py` therefore does a **two-phase apply**: pass 1
  writes rows and edges with `status` stripped; pass 2 writes statuses. It is the
  only supported loader.
- **`option<string>`, not `string`.** Mandatory would reject a row mid-load and
  *lose the finding* to enforce a field pass 2 is about to set. The cost is real: a
  hand-written UPSERT that forgets a status is accepted, so `health()` reports
  `findings_with_NO_status`.

### 3.4 Bulk ingest may never write `finding`

Doc scraping, the disassembly call graph, anotak's attack tables and keyframes all
land in `docnote` / `routine` / `attack` / `cell` — **artifacts**, not claims.
Promotion happens one row at a time through `kb.confirm()`.

This is not fastidiousness. 576 markdown bullets were once **90 % of the `finding`
table**, carrying `status='confirmed'` inherited from doc checkmarks. Under this
rule, ~13,000 statements were ingested in one day and the finding count moved
**245 → 245**.

`docnote` is explicitly a **queue with a drain rate** (`triaged`, `disposition`,
`promoted_to`), not an archive — otherwise it grows on every ingest and shrinks
never.

### 3.5 Negative results are first-class and cheaper to record than positives

`ruled_out` is **deliberately not gated** by the ASSERT; only `confirmed` is.
Recording a dead end must never be harder than recording a success — that asymmetry
is exactly why negative results go unrecorded everywhere else.

---

## 4. How it is used

Three paths, in descending reliability:

**Automatic — hooks** (`.claude/settings.json`). This is the only path that
requires no prior knowledge of the graph.
- `PreToolUse` on `Bash|Grep|Read|Edit`: if the tool input names `0x8C……`,
  `loc_8c……` or `+0xNNN`, inject what is known — **including what is ruled out**.
- `PreCompact`: re-inject `confirmed` + `ruled_out` before the conversation is
  summarised, so settled facts survive compaction.

Both are **advisory by construction**: they never block a tool call and exit 0
silently on any failure. That has a corollary worth stating loudly — **silence
means "the graph isn't running", not "nothing is known"**. `tools/re_kb/start.sh`
first.

**Subagents** — `.claude/agents/*.md` charters. **Manual** — `rekb.sh` (read),
`kb.py` (write).

### What retrieval actually buys

Measured, for `0x8C2895E0`:

| | result |
|---|---|
| `grep -rl` across the repo | **46 files** to open and read |
| the graph | **4 confirmed findings** stating what it *is* |

The best of the four: *"the engine renders ONLY the char-base nodes present in the
slot table — it does NOT iterate the 6 char structs."* grep can show every line
mentioning the address; it cannot tell you that. And there is no text to grep for
an idea nobody wrote down as code — which is what the 13 `ruled_out` rows and 12
recorded attempts are.

---

## 5. What this is, in the literature

Not applied AI/ML: no model, no learning, no inference over data. It does not get
smarter; it **accumulates under a constraint**. It is an assembly of four
established pieces:

1. **Knowledge graph with provenance** (W3C PROV lineage).
2. **Graded evidence** — Admiralty Code / GRADE, as above.
3. **Truth maintenance system** (Doyle 1979; de Kleer's ATMS) — the closest single
   classical name. Beliefs held *with their justifications*; `supersedes`/`corrects`
   are belief revision; `ruled_out` is a retracted belief kept with its reason.
4. **Database integrity constraints** — the ASSERT is a `CHECK`.

**Be precise about the gap:** this is a TMS **without automatic propagation**. A
real JTMS demotes a belief when its justification is retracted. Here, demoting a
source's `strength` after a confirm leaves the finding confirmed — the gate is a
check at the door, not an invariant over time. Until propagation exists, the
substitute is `health()["DRIFT_confirmed_that_would_fail_the_gate_today"]`,
reported by default because an audit nobody runs is not a safeguard. **Currently 0.**

Where AI genuinely enters is **retrieval**: this is agent long-term memory /
context engineering. The unusual choice is a *curated symbolic graph with a write
gate* rather than a vector store — retrieval is exact (address → finding) and
precision is enforced at **write** time. RAG over embeddings cannot refuse to store
an unsupported claim; this can.

The piece with no good standard name is the one doing the most work: systematically
recording **negative results**. Science names only the failure to do it —
publication bias.

---

## 6. The failure mode this design is built against

Every serious defect found while building this was the same shape: **a check that
reports success by default.**

| what | how it failed |
|---|---|
| Seed application | SurrealDB parses a POSTed file as one script. An unbalanced quote swallows the following statements as string content, returns **200**, every block says `OK`. **58 findings had never reached the graph** while every apply looked clean. |
| `health()` | Its query errored on a NULL field; `_rows()` dropped failed blocks. It reported **zero** unbacked claims. True count: 37. |
| `apply_seed --dry-run` | Skipped `post()`, so it printed "0 failed" **without contacting the server**. |
| Integrity check | Scoped to `cites` only; two `about` edges pointed at nothing. |
| Edge dedup | Rebuilt `calls` from `(in,out)` alone, **destroying `via`** on 3,207 edges. |
| `contradictions()` | Returned 164 rows of empty arrays because `about` was overloaded. **A broken detector that answers is never re-examined.** |

The architectural response is not "be careful". It is:

- **errors raise** (`_rows`), **exit codes reflect failure** (`rekb.sh`),
  **dry-run says "NOT EXECUTED"**;
- **apply one statement per request**, so a malformed statement can only lose
  itself and the output names it;
- **report the unflattering number next to the flattering one** — `health()`
  returns unbacked-loose (0) *and* unbacked-strict (42), because they disagree and
  the gap is a real disagreement about what a routine citation proves.

A second recurring shape: **loose pattern-matching that produces confident
nonsense.** The original handover claimed "44 findings record a dead end" — a regex
firing on the word "failed". Re-measured strictly: 19, of which **17 were correctly
`confirmed`** (positive results that *refute* a competing hypothesis). Acting on
that number would have deleted real knowledge. The same trap recurred four more
times; each fix was a *narrowing* (case-sensitivity, anchoring to a statement's
opening, a size cap, requiring two tokens for an instruction).

---

## 7. Known limitations

| limitation | size | why not fixed |
|---|---|---|
| **A locator is not a citation.** 42 confirmed findings rest only on a `→cites→routine` edge, which proves the claim *mentions* a PC, not that it is *visible* there. | 42 | The fix (`SET at=…, shows=…`) needs someone reading the disassembly at each PC. 17 edges were lifted from prose; the rest cannot be generated without inventing evidence. |
| **No TMS propagation** (§5). | — | Needs an event or a scheduled audit; `health()` reports drift meanwhile. |
| **Ratio still not interpretable.** 184:21 confirmed:inferred cannot distinguish "the rule works" from "writes bypass it" until a post-gate cohort exists. | — | `via`/`wrote_at` now stamped by `kb.py`; the cohort query needs *time*, not code. |
| **`docnote` backlog.** | 891 untriaged | Needs judgement per row. 276 triaged; plan/decision docs dispositioned. |
| **Confirmed findings unreachable by the hook** (no `about` anchor). | 56 | They name no address, PC or offset that exists as a row. |

None is a blocker for use. The first is the largest honest debt.

---

## 8. If you are copying this design

Five things that carried their weight, in order:

1. **Put the gate in the datastore.** Everything else is advice.
2. **Rank evidence ordinally and write the promotion rule against the rank.** The
   frozen-input/live-census discriminator is where the judgement actually lives.
3. **Outcomes on edges.** A verdict without its scope is a claim the evidence does
   not support.
4. **Bulk ingest may never write claims.** One rule; it is what keeps the table
   honest under volume.
5. **Make negative results cheaper to record than positive ones.**

And one thing to expect: **most of the work is not modelling, it is discovering
that your checks pass when they should fail.** Budget for it.
