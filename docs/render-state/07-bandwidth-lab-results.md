# RENDER-STATE appendix 07 — TA-wire compression lab: measured results (2026-07-08)

> Empirical companion to appendix 06 (prior-art survey). Every number below was MEASURED on a
> fresh 35.07 s in-match capture (2104 ZCST frames) from the local Windows headless rig.
> Re-runnable: `_bwlab\decompose.py / experiments.py / experiments2.py / experiments3.py`
> (corpus `_bwlab\cap.mirror.zcst` + `cap.gsta.mcrr` are ROM-derived — NEVER commit).
> Full report: `_bwlab\REPORT.md`.

## Headline: the wire measured 6.875 Mbps in-match on this branch (not the Apr-2026 ~4.1)

- ZCST frames: 30,135,897 B / 35.07 s = **6.875 Mbps** (avg 14,323 B/frame; steady 6.27–7.37).
- Side-channel msgs on the same socket (OBJS/OBJF/GSTA/TXTR/PALF): +1.08 Mbps → **8.10 total**.
- Likely cause of the delta vs the Apr anchor: the DMA force-dirty page shipping below (this
  branch ships pages "even if memcmp would say unchanged" — a workaround class the
  466d72d54-era wire had removed).

## Byte split (uncompressed 298,964,260 B = 68.20 Mbps raw; 0 B unaccounted)

| component | raw bytes | raw % | compressed alone (zstd-1) |
|---|---:|---:|---:|
| VRAM dirty pages (region 1) | 235,397,120 | **78.74%** | **3.393 Mbps** |
| TA delta run data | 26,949,650 | 9.01% | 2.707 Mbps (incl. headers) |
| TA delta run headers (6 B/run) | 16,917,136 | 5.66% | — |
| PVR/palette pages (region 3) | 14,008,320 | 4.69% | 0.307 Mbps |
| TA keyframes | 5,202,432 | 1.74% | 0.214 Mbps |
| headers/envelope/page-hdrs | 506,434 | 0.17% | 0.036 Mbps |

**REVISION of the "wire is 73% geometry" claim:** on this branch/capture, compressed cost is
~49% VRAM pages / ~39% TA delta. The dirty-page stream IS the biggest single cost — driven by:

- **56.9% of shipped pages are byte-identical to the last ship of the same slot** — source is the
  DMA force-dirty bitmap path (maplecast_mirror.cpp:2183–2205) shipping unconditionally.
- **82.8% of shipped pages are identical to SOME previously-shipped content** (only 10,446 unique
  page contents in 35 s) — exactly the redundancy the in-tree VCACHE mode (:1620–1630) exploits.
- TA delta structure: **1362.7 runs/frame, median run 11 B**; 96.8% of run data in runs ≤32 B, so
  39% of the delta section is 6-B run headers.
- Consecutive keyframes are **92.0% byte-identical**.

## Experiments (projected steady-state Mbps @60fps / server ms-per-frame)

| experiment | Mbps | ms/frame | verdict |
|---|---:|---:|---|
| re-zstd-1 per frame (sanity) | 6.870 | — | matches wire ✓ |
| per-frame z3 / z9 / z19 | 6.612 / 6.301 / 5.426 | 0.154 / 0.727 / 24.4 | z19 dead on CPU |
| trained dictionary (112,640 B) z1 / z3 | 4.728 / **3.756** | 0.101 / 0.144 | best per-frame-independent option |
| SoA 32-B byte-plane transpose | **7.907** | — | **WORSE — REJECTED with numbers** (the delta stream is fragmented runs, not clean parcel arrays; do not revisit without new evidence) |
| keyframe XOR-delta vs prev keyframe | −0.17 total | — | keyframes are only 0.28 Mbps — not worth wire complexity |
| prev-frame prefix dictionary z3 | 3.907 | 0.592 | superseded by streaming |
| **streaming zstd (shared window, flush/frame) z3 wlog=24** | **1.931** | **0.110** | **−72% at today's CPU cost** |
| streaming z9 wlog=24 | 1.471 | 0.472 | |
| hash-gate forced-dirty pages + per-frame z1 (today's wire, zero client change) | 6.131 | 0.100 | server-only quick win |
| hash-gate + runSoA + streaming z9 w24 | **0.931** | 0.306 | **UNDER 1 Mbps in budget** |
| vcache-refs + runSoA + streaming z9 w25 | **0.788** | 0.231 | best measured stack |

Late-join tax on the streaming window: reset every 60 frames → best stack becomes 1.505 Mbps;
reset every 300 → 1.026; no-reset requires per-client (or relay-owned) decompression streams —
the relay already decompresses for inspection, so relay-owned streams are the natural home.

## Top-3 recommendations (Mbps saved ÷ risk)

1. **Streaming zstd, shared window wlog=24, z3, flush per frame: 6.870 → 1.931 Mbps at 0.110 ms.**
   The cross-frame redundancy (82.8% dup pages, 92% identical keyframes, idle-loop TA periodicity)
   is invisible to per-frame contexts and free to a streaming one. Risk moderate: new envelope +
   all four parsers get a streaming decompressor + a late-join policy (reset cadence or per-client
   streams at the relay).
2. **Hash-gate the DMA force-dirty ships (do FIRST regardless): server-only, zero wire/client
   change** — one 8-B hash compare in serverPublish (~:2205). Kills 56.9% of page ships (47% of
   ALL raw bytes): 6.870 → 6.131 alone, compounds under streaming, halves client apply work.
   Near-zero risk → highest ratio measured. (Also: investigate WHY the force-dirty path is
   shipping at all — the deterministic wire supposedly retired it; probably re-added on this
   branch for rollback/predict.)
3. **runSoA delta serialization** (`nRuns | gap-offsets | lens | data` instead of interleaved
   6-B-header runs): 1.853 → 1.350 Mbps under streaming z3 AND cheaper CPU (0.079 → 0.061 ms);
   free level bump to z9 lands **0.931 / 0.788 Mbps at 0.31 / 0.23 ms**. Touches all four parsers
   but is a pure reorder of data the server already holds.

## Reconciliation with the prior-art stack (appendix 06)

- The "cheapest experiment" call (streaming context) was right — it is the single biggest lever.
- meshopt-style byte-plane transpose FAILED on this wire's delta stream (the runs are too
  fragmented) — the prior-art expectation did not survive contact with the data. It might still
  apply to a future quad-list wire (STAF-shaped), not to byte-run deltas.
- The semantic quad-delta ring (06 layer 1) remains the deepest lever for the TA-delta component,
  but the measured stack reaches <1 Mbps WITHOUT it — so it moves from "required" to "phase 2,
  only if <0.5 Mbps is wanted from the TA wire before switching tiers to GSTA/lockstep".
- Keyframe elimination (06 layer 2) demoted: measured cost only 0.28 Mbps.

Repro: `cd C:\Users\trist\projects\maplecast-flycast\_bwlab && python decompose.py && python experiments.py && python experiments2.py && python experiments3.py`
