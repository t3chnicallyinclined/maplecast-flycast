# Build brief: the overseer agent

For an agent picking this up cold. Written 2026-09-01. Rungs 0/1 are built and
measured; everything past that is design, clearly marked.

---

## 1. What this is

An agent that watches what a *working* agent produces and answers one question:

> Does the knowledge graph already know something about what you are touching?

It is **not** a supervisor and should never be described as one. A second model
is not more reliable than the first. What it has is a **different vantage
point**: the worker is inside the task and cannot see that it has tried three
variants of the same approach or probed the fourth field in the same struct.
Pattern-over-time is invisible from inside the loop and obvious from outside.

### The classical name

This is a **truth maintenance system** (Doyle, 1979; later assumption-based
TMS). Maintain beliefs, track the justification for each, retract when the
justification fails. `finding` + `cites` + `supersedes` is exactly that shape.
Worth reading the lineage before inventing more of it.

### Why it exists here

Long RE sessions drift. The worker loses track of what it has, starts guessing,
and re-walks dead ends. The human currently catches this by watching and
reminding, which works and does not scale.

### The design constraint that matters most

**The overseer may emit only retrieved rows with provenance and status. It has
no channel for opinion, by construction.**

Not a prompt instruction, an interface property. A confident wrong overseer is
worse than no overseer, because its output arrives framed as oversight and the
worker weights it above its own reasoning. `overseer.render()` can only format
KB rows. Keep it that way.

---

## 2. Architecture: rungs, not layers

Each rung is more expensive than the one before, and **most traffic never
reaches the expensive ones**. This is the same funnel as the autoseo pipeline:
the cheap thing decides whether to wake the expensive thing.

| Rung | What | Cost | Status |
|---|---|---|---|
| **0** | Regex-extract exact identifiers, index lookup | ~0 | **BUILT** (`overseer.py`) |
| **1** | Graph walk from the hit: `about`, `supersedes`, `tried_on` | ~0 | **BUILT** (partial) |
| **2** | Vector similarity over the 440 `note` fields | ~0 after model load | designed, not built |
| **3** | Small local model: is this output a *claim* or narration? | low | designed, not built |
| **4** | RLM: iterative query-write-observe against the graph | high | escalation only |

### Rung 0 is the one people skip and it carries most of the value

RE talk is unusually precise. When a worker is about to re-derive an address it
nearly always *says* the address: `0x3F24`, `loc_8c0344d4`, `blk+0x2eeb0`,
`pack_gen.rs:903`. A regex plus a hash lookup beats a vector here, because the
domain vocabulary is exact.

**Measured:** 9.2% of assistant turns carry at least one identifier.

### Rung 1 is why a graph beats a vector store

Vector search can say *this is similar*. Only the graph can say *this was
superseded on 2026-06-29 and here is what replaced it*. For an overseer that
distinction is the whole product.

### Rung 2 is for symptom-shaped input, and it is now justified by measurement

90.8% of turns carry no identifier. "The sprite is shifted sideways by about a
part width" has nothing to look up. That is what the 440 notes and 171
statements (~108k tokens of expert English) are for.

**Run the embedding locally.** ONNX Runtime, in-process, CPU, no network, no
per-call cost. The pattern already exists in `autoseo` (Silero VAD, AST, SCRFD
all run this way).

⚠ **The embeddings must match.** Whatever embeds the KB notes must be the same
model that embeds worker output. Forgily's `character_generation_flow.md` flags
this as CRITICAL for its own novelty check: different embedding sources put
vectors in different semantic spaces and comparisons become meaningless. Same
trap, different system.

### Rung 4: where an RLM actually belongs

Not the foundation. Rungs 0-1 are retrieval, not reasoning, and the measurement
says they already carry the value at zero cost.

An RLM earns its place on **escalation only**: a hit came back ambiguous, or the
worker is clearly stuck, and something needs to iteratively query the graph
(`plan → query → observe → refine`) rather than do one lookup. Bound it with a
step budget, and keep intermediate results out of the main context — only the
final answer with node ids as citations comes back. That property is what makes
it scale, and it is the same reason Book Scholar works.

---

## 3. Storage: it depends which repo this lands in

**If it stays in maplecast-flycast:** stay on SurrealDB. The KB is already there
(638 findings, 349 cite edges, 146MB RocksDB) and SurrealDB indexes vectors
natively (HNSW / MTREE), so graph and embeddings live in one store and a hybrid
query is a single statement. Adding a second store buys a sync problem, and this
project has a live example of what divergence costs (24 findings existed only in
the gitignored RocksDB store; 174 committed findings had never been applied to
it).

**If it lands in `vidkosha_cortex`: use Helix, which is already the memory
fabric there.** That repo is a Rust agent OS built on Helix AI Fabric (vector
graph, provenance-ready) with local vLLM embeddings on `bge-m3`, a specialist
router, and JetStream multi-agent fanout. Rungs 2 and 3 of this design are
already solved there; do not rebuild them.

House rule either way, from Retro Receipts: *nothing derived is allowed to
become authoritative.* One store, or a derived store that can be rebuilt.

## 4. What is built, and what it measured

```
tools/re_kb/overseer.py      rung 0/1. extract() → lookup() → check() → render()
tools/re_kb/replay_eval.py   offline evaluation against past transcripts
```

### The evaluation method is the important part

Do **not** evaluate this by running it live and forming an impression. This
project has declared false wins before; that is why `senior-re-generalist` and
`gsta-verification-harness` exist.

Evaluate offline against ground truth that already exists:

* agent transcripts (`~/.claude/projects/c--Users-trist-projects-maplecast-flycast/`,
  5 sessions, 168MB, ~30k records) carry an ISO `timestamp` on every record
* KB findings carry a `date`

So for any past turn: **did the worker touch something the graph already knew at
that moment?** Findings dated on or after the turn are excluded, so the overseer
is never credited with knowledge that did not exist yet. Deterministic,
repeatable, no model needed to produce the number.

### Results (full corpus, 2026-09-01)

```
13,544 assistant turns scanned
 1,249 carried >=1 identifier                    9.2%
 1,026 unique identifiers

   613 / 1,249 would fire                       49.1%
    33 cited ruled_out / superseded / masks_only

fires by status: 593 confirmed · 284 inferred · 129 resolved · 83 open · 33 superseded
latency: 36.4 ms/identifier · 37.4s for the whole corpus
```

### Read of those numbers

**The default fire policy is wrong.** 613 fires would be muted within a day.
Firing on `confirmed` is mostly noise — the worker is usually building *on* that
row, not about to re-derive it.

**Corrected policy:** fire on `ruled_out` / `superseded` / `masks_only` always,
`confirmed` never, `inferred` only when the output looks like an assertion. That
gives **33 fires in 13,544 turns, one per ~410**. Rare enough to be read every
time, frequent enough to be worth building.

**The ceiling is the KB's vocabulary, not the overseer.** All 33 come from
`superseded`. There are **zero `ruled_out` rows** because that status does not
exist in the data yet — the dead ends are prose inside `note` fields (19
candidates identified). Landing the backfill in `83_status_triage.surql` raises
the ceiling directly. **Do that before building rung 2.**

---

## 5. Still unproven — the P0 gate

**Nothing has been hand-labelled.** A fire means the worker touched something the
graph knew; that is sometimes a re-derivation about to happen and sometimes
correct building-on. Only labelling separates them.

> Pull the 33 actionable fires with surrounding transcript context and read them.
> If almost none are genuine re-walks, **stop**. The problem is not worth tooling.

This is the P0 gate from `resume/incident-kb-spec.html` §09, and it applies
unchanged: *"did anyone actually reach for it? If not, stop here — the problem
isn't tooling."*

Labelling needs someone who knows what the session was trying to do. It is not
an agent task.

---

## 6. Research worth reading

| Thread | Why |
|---|---|
| **Truth maintenance systems** (Doyle 1979; ATMS, de Kleer 1986) | The classical form of this exact idea: beliefs, justifications, retraction |
| **Graphiti** (Zep) | Closest working system — bi-temporal knowledge graph for agent memory, facts carry valid-from/valid-until |
| **GraphRAG** (Microsoft) | Retrieval over a graph rather than a flat corpus; community summarisation |
| **Mem0 / Letta (MemGPT)** | Agent memory layers; useful mainly as a comparison of what they *don't* do (no status, no provenance) |
| **W3C PROV** | A standard provenance model, if `cites` ever needs to be interoperable |
| **LLM-as-judge failure modes** | Directly relevant to why a model may propose but never promote |

The gap in all the shipped agent-memory products is the same one this design
closes: **they store what was said, not what is believed and how strongly.** No
status vocabulary, no evidence ranking, no first-class negative results. That is
the contribution here, and it comes from `incident-kb-spec.html` rather than from
any of them.

---

## 7. Build order for whoever picks this up

1. **Apply `83_status_triage.surql`** (backfill `ruled_out`). Precondition — the
   overseer has nothing valuable to say without it.
2. **Hand-label the 33.** Gate. If they are not genuine re-walks, stop.
3. **Fix the fire policy** in `overseer.check()` to the corrected one above.
4. **Index the lookup.** 36.4ms is a `CONTAINS` scan; per-turn use needs an index
   or the per-session cache `replay_eval.py` already demonstrates.
5. **Rung 2**: local ONNX embedder, vectors into SurrealDB, hybrid query. Re-run
   `replay_eval.py` and see whether coverage moves past 9.2%.
6. **Wire it live**: a `PreToolUse` hook, not a prompt instruction. The whole
   point is that it cannot be forgotten.
7. **Only then** consider rung 3/4.

---

## 8. The thing to understand before touching any of it

Four separate instances of one failure turned up while building this:

* the status vocabulary drifted to 11 values against a README declaring 4
* `supersedes` / `corrects` / `fixes` were used 12 times and never declared
* "query the KB before re-deriving" is a README sentence with no hook behind it
* `senior-re-generalist` — which *is* this overseer — says "Use PROACTIVELY",
  which is an instruction to a model to remember, i.e. elective

Every time, the primitive was right and the enforcement was absent. Rules were
written as norms addressed to a model instead of as gates in the path.

**The overseer is only worth building if it is wired to a hook.** As another
agent someone has to remember to invoke, it already exists and it already does
not fire.
