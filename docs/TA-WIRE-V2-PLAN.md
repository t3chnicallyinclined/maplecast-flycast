# TA-Wire v2 — sub-1 Mbps plan (measured basis)

> **STATUS 2026-07-09 (end of the build night): P0/P1/P1.5/P2 LIVE ON PROD and user-verified
> smooth in real play.** Prod env: ZSTREAM=1 LEVEL=9 SOA=1 TACANON=1 (PAGEGATE + STAGESTRIP
> present but OFF). Measured: ZCS2 ~1.1-1.2 Mbps in-match / 0.43 idle; legacy 5.18 Mbps
> (-24% for all viewers via the TACANON encoder fix). Phase 3a STAGESTRIP: built, byte-gated
> (strip-chain 1500/1500), 0.898 Mbps in-match on the rig — PARKED pending re-test on the
> now-clean transport + the 3b bake sweep. Client hardening shipped: WS-only page (WT
> datagrams corrupt stateful streams), structural frame whitelist, post-inverse frameSize
> belt (convention: frameSize EXCLUDES its own 4 bytes — two off-by-4 client bugs cost an
> evening; READ THE PRODUCER before writing wire checks). NEXT: worker-decode (supers FPS),
> 3a re-test, 3b bake sweep, 3c own-HUD.

> Basis: docs/render-state/06 (prior art) + 07 (compression lab, 2026-07-08 capture).
> Wire today: 6.875 Mbps in-match on feat/render-replica-live. Measured target stack:
> 0.93–0.79 Mbps @ ≤0.31 ms/frame. Zero image-quality loss at every phase; the
> determinism rig (MAPLECAST_DUMP_TA both ends, byte-compare) is the gate for every phase.

## Phase 0 — PAGEGATE: stop force-shipping memcmp-equal pages  [server-only]

- `MAPLECAST_PAGEGATE=measure`: wire UNCHANGED; counts forced-but-identical pages, logs
  every 600 frames (validates the lab's 56.9% live, plus asserts the bytes==shadow claim).
- `MAPLECAST_PAGEGATE=1`: a forced-dirty page ships only if its bytes differ from shadow
  (i.e. the forced bit can no longer force a memcmp-equal ship). Content changes always ship.
- Safety argument (verified 2026-07-08, see conversation + render-state/07): every shadow
  write site (init :491, ship :2210, freshSync reset :2325, request_sync reset :3518) is
  paired with the client receiving those exact bytes → shadow == client state → skipping
  equal pages cannot create staleness. The bitmap's protective role belonged to the
  pre-deterministic-wire detector (f9fca788a / d65cadccb era). Bitmap + hooks stay intact.
- Gate: determinism rig incl. scene transitions (char select / stage select) + fresh
  _bwlab capture showing the page-share drop. Default stays OFF until gated.
- Expected: 6.87 → ~6.1 Mbps alone; bigger win = halves client page-apply work; compounds
  with Phase 1. NOT load-bearing for the <1 Mbps target (streaming eats duplicates anyway).

## Phase 1 — ZCS2: streaming-zstd envelope  [the big lever, −72%]

- Server: persistent ZSTD_CStream, wlog=24, level 3 (env MAPLECAST_ZSTREAM_LEVEL), one
  flush per frame. New outer magic `ZCS2` (frame header carries a stream-epoch byte).
- Late-join: server resets the stream (new epoch) whenever it broadcasts a SYNC — the
  relay already triggers/caches SYNC for joiners, so a joiner gets [SYNC + epoch reset]
  and decodes from there. Belt: periodic reset every N frames (env, default 300 → measured
  1.03 Mbps; 0 = never, for the pure relay-owned-stream future).
- Consumers (the four-parser rule): frame-decoder.mjs (fzstd streaming or swap to the wasm
  MirrorDecompressor — verify fzstd wlog=24 streaming FIRST, it's the one unknown),
  king.html wasm + emulator.html wasm (ZSTD_DStream, zstd already linked), native
  clientReceive (same), relay (verbatim forward; streaming decode only for its inspection
  path — rust zstd crate supports it).
- Rollout: env-gated MAPLECAST_ZSTREAM=1 server-side; legacy ZCST path remains the default
  until every parser passes the rig; prod flips last.
- Expected: 6.87 → 1.93 Mbps (with P0: lower) at today's CPU (0.110 ms).

## Phase 2 — runSoA inside ZCS2 (v2 inner frame)

- Delta section becomes `nRuns | offsets[] (delta-coded) | lens[] | data` instead of
  interleaved 6-byte-header runs (1363 runs/frame, median 11 B → 39% of the section is
  headers). Pure reorder of data the server already holds; ship as part of the ZCS2 inner
  format so parsers migrate ONCE (bundle with Phase 1 client work).
- Then free level bump to z9 (0.31 ms measured). Expected: **0.93 Mbps**; optional
  VCACHE-refs variant → **0.79 Mbps**.

## Phase 1.5 — dead-byte canonicalization  [server-only, measured −17.3% on real play]
- MEASURED (stage_share.py, idle + 180s real-gameplay captures, _bwlab/STAGE-SHARE-REPORT.md):
  61.8% of within-stage churn (72% of ALL churn at idle) is bytes NO parser reads —
  padding bytes +24..+31 of 64B floating-color stage verts flip between engine
  staging-buffer scratch patterns. Zero parser-ignored byte ranges before the TA diff
  (canonicalize) → they stop churning forever. No wire change, no client change.
- Gate: four-parser read-set audit of the masked byte classes + determinism rig.

## Phase 3 — STAGE-STRIP: client-local stage render  [measured −49.0% on real play]
- MEASURED on the 180s real-gameplay capture (streaming z3 sim): as-is 3.505 Mbps →
  stage-stripped **1.789 (−49.0%)**; with canonicalization **1.615 (−53.9%)**. Spike
  seconds 9.3 → ~3.3 Mbps. Stage = ~123 KB/frame of buffer; x/y churn (camera) is real.
- Server: strip op-list stage polys from the mirrored TA (server already ta_parses);
  client renders the stage locally (native: gsta_stage path exists; browser: baked STG
  mesh + cam_mat M1·M2, proven 4.3e-5 px, re_kb/39). PREREQ: per-stage engine-TA bake
  sweep (only STG0B validated; stage_id→STGxx map incomplete — RENDER-STATE §2).
- **Translate-opcode PARKED by measurement:** 38.7% of stage frame-pairs are NOT a
  single screen-space translate (parallax layers at different depths; residuals up to
  780k px) — the stage needs the real 3D reproject, i.e. exactly the local stage render.

## Phase 4 (later) — WebTransport for latency tail.

## Standing gates per phase
1. MAPLECAST_DUMP_TA determinism rig, server+client, incl. scene transitions — 0 divergence.
2. Fresh _bwlab capture + decompose.py — the Mbps claim re-measured, not assumed.
3. webgpu-test.html visual A/B on the local rig (?ws=ws://localhost:7200).
4. [GPROF]/publish-time telemetry — server ms/frame within budget.
