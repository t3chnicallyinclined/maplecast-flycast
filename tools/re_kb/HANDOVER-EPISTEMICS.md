# Handover: re_kb epistemics work (2026-09-01)

Written for the next agent working in `maplecast-flycast`. Everything below was
measured today against the real files and the live graph. Where something is
**not** verified it says so.

---

## 1. State right now

| | |
|---|---|
| SurrealDB | **RUNNING** on `127.0.0.1:8001`, started by hand from the repo root. **It is session-bound and will die.** Nothing starts it at boot. |
| Store | `maplecast-flycast/re_kb_data/re_kb` — 146 MB RocksDB. **The only store on this machine.** |
| Binary | `C:\Users\trist\AppData\Local\SurrealDB\surreal.exe` v3.1.4, on PATH |
| Branch | `feat/executor-pool-spawn` |
| New files | **5, all untracked.** Nothing committed. |

Restart it with (must be run from the repo root — see trap 3):

```bash
cd /c/Users/trist/projects/maplecast-flycast
surreal start --user root --pass root --bind 127.0.0.1:8001 rocksdb:re_kb_data/re_kb
```

---

## 2. What was measured

### The graph has two populations mixed into one table

```
INGESTED (doc_*)   576 findings   478 confirmed   93 inferred
HAND-CURATED RE     62 findings    51 confirmed    0 inferred
```

`ingest/docs_parse.py` writes a `finding` row per doc bullet, so **90% of the
`finding` table is not RE knowledge**. Every aggregate over `finding` is
diluted by it. The live graph looks like a healthy 529:93 and is not — all 93
`inferred` rows are machine-generated. The hand-curated layer has **zero**.

The committed seeds say the same thing independently: **162 confirmed, 1
inferred, 15 open**.

### Evidence coverage (measured over the seeds)

```
122  confirmed WITH reproduction/code evidence   (rule satisfied)
 30  confirmed citing only weaker evidence       (violation)
 10  confirmed with NO cites edge at all         (violation)
     -> 25% of confirmed claims cannot show qualifying evidence

 44  findings whose TEXT records a dead end, but whose STATUS does not
     -- so no query can find them, and the next session re-walks them
```

The 30 weak-only claims are overwhelmingly **render fixes confirmed on a
metric** (`replica_live_stage_black_fix`, `carve_nonsquare_linear_fix`,
`gsta_stage_floor_cull_fix`, `gsta_cable_purple_engine_faithful`). A pixel
diff that improved is a measurement, not a mechanism. **That is the
`masks_only` population** — the false-win set `senior-re-generalist` exists to
catch, now findable by query.

### The seeds and the live store had diverged in BOTH directions

- **24 curated findings existed only in `re_kb_data/`** — which is gitignored,
  and which `README.md` calls "a rebuildable RocksDB store". It was not.
  Applying the documented rebuild would have destroyed them. **Recovered** to
  `78_recovered_live_only.surql` (24 findings + 58 edges).
- **174 seed findings have never been applied to the live DB.** The store's
  last write was Jun 13; seeds carry dates to Jul 18.

Neither store is complete. There is currently no single source of truth.

---

## 3. Files added (all untracked)

| file | what | verified? |
|---|---|---|
| `77_epistemics.surql` | Schema: declares `supersedes`/`corrects`/`fixes` (all three were **in use but never DEFINEd**), adds `approach --tried_on--> finding` with the outcome on the edge, backfills `source.strength`, documents the status vocabulary + promotion rule. | **NOT APPLIED.** See trap 1. |
| `kb.py` | The enforced write surface. `confirm()` requires a qualifying source; `propose()` has no `status` parameter; `record_attempt()` requires an outcome incl. `masks_only`; `query()` refuses write verbs. Plus `health()`, `dead_ends()`, `false_wins()`, `contradictions()`. | `health()` **runs against the live graph.** `contradictions()` is **untested** — expect to fix SurrealQL there. Write helpers untested. |
| `audit_seeds.py` | The health metric, parsed straight from the `.surql` files. Needs no server. | **Runs. Output above is real.** |
| `78_recovered_live_only.surql` | The 24 rescued findings + 58 edges. | Generated, well-formed, **not applied** (they are already live — this file exists so they survive a rebuild). |
| `_dump_live_only.py` | Regenerates the above. Re-run after any live-only writes. | **Runs.** |

Design rationale for all of it is in `resume/incident-kb-spec.html` sections
03, 04 and 07 — the status vocabulary, the typed-evidence ladder and the
promotion rule are ported from there.

---

## 4. Traps — these cost real time today

**1. `77_epistemics.surql` has NOT been applied, so `source.strength` is
`null` on all 138 sources.** Until it is applied, `kb.confirm()` rejects every
call, because it tests `strength in {reproduction, code}` and gets `None`.
Apply 77 first or nothing writes.

**2. A `cites` edge can land on a `routine`, not just a `source`.**

```
299  ->cites->source:
 55  ->cites->routine:
  5  ->cites->finding:
```

A `routine` id **is** its PC in the marvelous2 disassembly (`routine:loc_8c0344d4`),
so it is code-grade evidence and satisfies the promotion rule. I wrote two
separate checks that counted only `source` and both under-reported compliance
badly — once claiming 40% violations when the truth was 25%. If you write a
query about evidence, handle all three target tables.

**3. The datastore path is RELATIVE.** `rocksdb:re_kb_data/re_kb` resolves
against the current working directory. Start surreal from `mvc2-oracle/` and
SurrealDB does **not** error — it silently creates a new empty store there, on
the same port, from the same documented command. Every query then returns
nothing and it looks exactly like data loss. Always start from the repo root.

**4. `kind='memory'` does NOT mean somebody's recollection.** In this graph it
means `_marv_re/memory/pl_mem.asm` and `work.asm` — authoritative symbol
tables. It ranks as **code**, not attestation. A naive kind→strength mapping
mis-ranks 3 sources and will wrongly reject valid confirmations.

**5. `PYTHONIOENCODING=utf-8` is mandatory** for any script that prints KB
text. Windows stdout is cp1252 and the notes contain `→`, `—`, `’`. Without it
you get `UnicodeEncodeError` mid-run.

**6. `RELATE` is not idempotent** (pre-existing, in the README). Re-applying a
seed duplicates edges. Run `07_dedup_edges.surql` after any reseed.

**7. Three copies of the seeds exist.** `tools/re_kb` (87 files, current),
`mvc2-oracle/re_kb` (16 files, frozen Jun 13), `_handover_mvc2_re/re_kb` (83
files, Jul 12). All shared files are **byte-identical** — no fork, just
staleness. `mvc2-oracle` is missing ~69 seed files while its README bills the
directory as "the RE knowledge graph", and its numbering jumps 09→15→18 so the
truncation is invisible.

---

## 5. Pending work, in order

1. **Commit the five files.** The 24 recovered findings are the one thing that
   should not sit untracked.

2. **Apply `77_epistemics.surql`**, then `07_dedup_edges.surql`. This is what
   makes `kb.py` functional.

3. **Apply the seeds to bring the live DB current** (adds the 174 missing
   findings), then dedup again. Documented loop is in `README.md` under
   "Rebuild from scratch" — extend it past `09_facing_subgraph`, which is where
   it currently stops despite 76 files existing.

4. **Split `doc_*` into its own table.** They are not findings and they make
   every metric meaningless. This is a change to `ingest/docs_parse.py`.

5. **Backfill statuses — needs human judgment, do not automate:**
   - 44 findings whose text records a dead end → `ruled_out`
   - the 30 metric-only "fixes" → triage into genuine `confirmed` (find the
     code-level mechanism and cite it) or honest `masks_only`
   - the 10 with no provenance at all → cite or demote to `inferred`

6. **Pin the absolute datastore path** in both READMEs (trap 3), and sync
   `mvc2-oracle/re_kb` or add a note saying it is a subset.

7. **Not started:** the enforcement layer the whole exercise was aiming at —
   a `PreCompact` hook re-injecting the confirmed + ruled-out set, and a
   `PreToolUse` hook querying the KB before an agent re-derives an address.
   `.claude/` in this repo contains only `agents/` — there is no
   `settings.json` and no hook of any kind. Note also that of the five agent
   definitions, `re_kb` is mentioned 17× in `mvc2-sh4-re-expert.md`, 5× in
   `mvc2-sprite-render-expert.md`, once each in two others, and **zero times in
   `flycast-internals-expert.md`** — the renderer agent does not know the graph
   exists.

---

## 6. Commands

```bash
cd /c/Users/trist/projects/maplecast-flycast

# start the graph (session-bound)
surreal start --user root --pass root --bind 127.0.0.1:8001 rocksdb:re_kb_data/re_kb

# audit without a server, straight from the seeds
PYTHONIOENCODING=utf-8 python tools/re_kb/audit_seeds.py

# audit the live graph
PYTHONIOENCODING=utf-8 python tools/re_kb/kb.py

# apply the schema, then dedup
tools/re_kb/rekb.sh @tools/re_kb/77_epistemics.surql
tools/re_kb/rekb.sh @tools/re_kb/07_dedup_edges.surql

# re-check what is live-only after any direct writes
PYTHONIOENCODING=utf-8 python tools/re_kb/_dump_live_only.py
```

---

## 7. The through-line

Four separate instances of the same failure turned up today, and it is worth
naming because it will happen again:

- the status vocabulary drifted to 11 values against a README that declares 4
- `supersedes` / `corrects` / `fixes` were used 12 times and never declared
- "query the KB before re-deriving" is a README sentence with no hook behind it
- the seeds forked into three copies with nothing keeping them in sync

Each time the primitive was right and the enforcement was absent. The rules
were written as norms addressed to a model, not as gates in the path. That is
what `kb.py` changes, and it is why step 7 above matters more than steps 2–6.
