# Graphify Evaluation — maplecast-flycast

**Date:** 2026-06-27  **Status:** Research / paused
**Tool:** [safishamsi/graphify](https://github.com/safishamsi/graphify) (MIT, by Safi Shamsi)

> One of 5 per-repo notes from the same research session. Siblings (same filename,
> `docs/research/graphify-evaluation.md`) live in: **GP2040-CE, nobd-desktop,
> mvc2-oracle, mvc2-skin-studio**. "Shared verdict" is identical across all five.

---

## What graphify is (1-paragraph)

CLI that turns folders of **code + docs + PDFs + data** into a queryable knowledge graph.
**AST tier** (tree-sitter, local, free) for code structure; **semantic tier** (LLM, your
API key, costs tokens) for doc/concept edges. Outputs `graph.json` (committable),
`graph.html`, `GRAPH_REPORT.md`, and an **MCP server**
(`python -m graphify.serve graph.json`). Edges tagged `EXTRACTED`/`INFERRED`/`AMBIGUOUS`.
Install: `uv tool install graphifyy` then `graphify install`.

---

## This repo's assessment

**Complement only — do NOT duplicate `re_kb`.** This repo already contains the
canonical RE knowledge graph: `tools/re_kb/` (10 SurrealQL seed files, SurrealDB at
127.0.0.1:8001) where every finding has a `cites` edge to its source (marvelous2 / anotak
/ live Oracle) and a `CONFIRMED`/`INFERRED`/`OPEN` status. graphify's LLM-inferred RE
edges would be **lower confidence than these curated facts** — pointing it at the RE
content would only produce a noisier, redundant copy.

**Where graphify CAN help here (docs/prose lane, not facts):**
- The 53 markdown docs in `docs/` + 3 in `re-catalog/` are a good semantic-tier target
  (ARCHITECTURE.md 80KB, DEPLOYMENT.md 52KB, MVC2-MEMORY-MAP.md 40KB, plan/handoff docs).
- Surfacing the **shared MVC2 format vocabulary** (GFX1/GFX2, twiddle/LZSS, palette banks,
  `PLxx` IDs, char_struct offsets) that is re-documented in inline comments across
  mvc2-skin-studio / mvc2-skin-processor / this repo's `tools/` — graphify could flag
  "this format is described in N places" without touching `re_kb`.
- The two domain agent definitions (`.claude/agents/mvc2-sh4-re-expert.md`,
  `mvc2-sprite-render-expert.md`) cite 50+ findings — a queryable docs layer aids onboarding.

**CRITICAL scoping** — this is a ~900K-LOC vendored Flycast fork: ~9,600 C++ files,
~6,000 Python tool files, 758 markdown (most in `core/deps/` + node_modules).
**Never run `graphify .` at repo root.** Scope to: `docs/`, `re-catalog/`,
fork-specific `core/network/` + `core/marvelous2/`, and `tools/` (excluding generated
output). Treat the `marvelous2/` submodule as external linked knowledge.

Possible future integration: export `re_kb` as a *source* into a graphify docs graph
(don't replace it).

---

## Shared verdict (identical across all 5 repos)

| Constellation | Repos | Existing graph? | Verdict |
|---|---|---|---|
| **NOBD input-timing** | GP2040-CE, nobd-desktop, nobd-research, nobd-website, maplecast input-latch | **None** | **Strongest, cleanest win** |
| **MVC2 reverse-engineering** | mvc2-oracle, **maplecast-flycast**, mvc2-skin-studio, mvc2-skin-processor | **Yes — SurrealDB `re_kb`** | **Complement only — do NOT duplicate `re_kb`** |

**Risks:** scale blast radius (this repo is the worst offender — scope hard);
yet-another-store risk (re_kb + graphify); AST code-graph value modest; git-share muted.

**Overall:** docs/onboarding + cross-repo vocabulary layer here; the NOBD constellation
is where graphify earns adoption.

---

## Where we left off / next steps

1. Pilot on **nobd-desktop** first (smallest, validates output).
2. GP2040-CE docs only.
3. Cross-repo **NOBD** graph + MCP ← unique-value artifact.
4. **This repo, docs-only, scoped** (`docs/` + `re-catalog/` only) — shared-vocabulary
   view; keep `re_kb` authoritative.
5. Decide MCP-first after steps 1–2.

**Nothing installed/run yet.** Prereqs: `uv` + Python 3.10+.
