-- ============================================================================
-- 89: the attempt subgraph -- what has been TRIED and did not work
--
-- 77 built `approach --tried_on--> finding` with an outcome on the edge, and
-- it has held TWO rows since. Meanwhile the operator's auto-memory records a
-- dozen abandoned approaches in prose, where no query reaches them.
--
-- This is the cheapest high-value content in the graph and the most expensive
-- to rediscover. A confirmed finding tells you what IS true; only this tells
-- you what was already tried and cost someone a week. `ineffective` needs the
-- lowest evidence bar of any outcome -- "we did it and measured no
-- improvement" -- and it cannot inflate the confirmed count, so it is exactly
-- where a backfill should start.
--
-- Each dead end gets a `ruled_out` finding (the thing eliminated) plus an
-- `approach` and a `tried_on` edge carrying the outcome. `ruled_out` is not
-- gated by the promotion ASSERT (88) -- only `confirmed` is -- because
-- recording a negative result must never be harder than recording a positive
-- one. That asymmetry is deliberate: it is why negative results go unrecorded
-- everywhere else.
--
-- Every row cites the auto-memory file it came from, so the provenance is
-- checkable rather than inherited.
--
-- Apply: PYTHONIOENCODING=utf-8 python tools/re_kb/apply_seed.py \
--            tools/re_kb/89_attempts_backfill.surql
-- ============================================================================
USE NS re DB kb;

UPSERT source:automemory_2026 SET
  kind='doc', strength='attestation',
  title='Operator auto-memory (~/.claude/projects/.../memory/*.md)',
  ref='memory/project_rollback_shelved.md, project_render_pipeline_state.md, project_a2_runahead_state.md, project_native_client_tauri.md, project_executor_rollback_integration.md',
  note='Attestation-grade by design: these are session notes, not captures. Enough to record that an approach was tried and abandoned; NOT enough to confirm a claim about MVC2.';

-- ---- 1. GGPO-style rollback (shelved 2026-05-09) ----------------------------
UPSERT finding:ruled_ggpo_rollback SET
  status='ruled_out',
  statement='GGPO-style rollback prediction for MapleCast (Phase 1 / A.4-A.6).',
  tried='Built to a working .mcrec V2 replay system, then re-examined against the architecture.',
  evidence='GGPO solves latency for PEER-TO-PEER netcode where each player runs a full local SH4. MapleCast is a central authoritative server + thin clients (one SH4 on the VPS; clients are renderers and input sinks) and end-to-end latency is already ~10ms. The premise does not hold. .mcrec replay + audit determinism were kept.',
  date='2026-05-09';
UPSERT approach:ggpo_rollback SET name='ggpo_rollback',
  how='Per-client local SH4 simulation with rollback/re-simulation on input mismatch, GGPO-style.';
RELATE approach:ggpo_rollback->tried_on->finding:ruled_ggpo_rollback
  SET outcome='ineffective', date='2026-05-09',
      note='Not a failure of implementation -- a mismatch of architecture. Shelved, gated MAPLECAST_ROLLBACK_RING=1.';
RELATE finding:ruled_ggpo_rollback->cites->source:automemory_2026;

-- ---- 2. pixel-shipping the wire (dead 2026-06-07) ---------------------------
UPSERT finding:ruled_pixel_shipping_wire SET
  status='ruled_out',
  statement='Shipping PIXELS (full TA mirror / STAF / VCACHE) as the wire format.',
  tried='Measured in-match across all three variants, including a content-addressed VRAM-page dedup (MAPLECAST_VCACHE).',
  evidence='All three land at 36-88 Mbps. VCACHE reconstructs VRAM BYTE-IDENTICALLY and still does not reduce the wire: zstd already collapses within-window page repeats, so an ~84%% dedup COUNT does not become bytes. The wire is ~73%% geometry + ~27%% textures and VCACHE only touches textures. There is a floor here that compression cannot pass. The pivot was reconstruct-from-state at ~0.05 Mbps, 700-1700x leaner.',
  date='2026-06-07';
UPSERT approach:vcache_vram_dedup SET name='vcache_vram_dedup',
  how='Content-addressed VRAM-page dedup on the mirror wire (MAPLECAST_VCACHE, maplecast_mirror.cpp).';
RELATE approach:vcache_vram_dedup->tried_on->finding:ruled_pixel_shipping_wire
  SET outcome='ineffective', date='2026-06-07',
      note='Renders pixel-perfect and saves no bandwidth. Same family as the VRAM diff -- do not revisit without a NEW reason why zstd would not already have collapsed it.';
RELATE finding:ruled_pixel_shipping_wire->cites->source:automemory_2026;

-- ---- 3. A2 run-ahead: server-side adaptive skip (wrong layer) ---------------
UPSERT finding:ruled_runahead_server_adaptive_skip SET
  status='ruled_out',
  statement='Fixing the run-ahead super-frame FPS drop by SERVER-side adaptive frame-skip / rewind-stall tuning.',
  tried='Many rounds of server instrumentation and a v2 adaptive gate keyed on lastPublishedTotalSize (>350KB).',
  evidence='Clean capture with a user genuinely supering: the SERVER holds a rock-steady 60fps average through real supers (publishes 600/600, RA-ADAPT delta10s=60.0) while the client shows wire_fps=27.8. Server sends 60, client shows 27 -- the sustained drop is DOWNSTREAM of the server tick. The whole server-side effort was aimed at the WRONG LAYER. The v2 gate also never fired, correctly, because those supers were light (<350KB). If run-ahead is revived the lever is CLIENT-side delivery smoothing.',
  date='2026-07-14';
UPSERT approach:runahead_server_adaptive_skip SET name='runahead_server_adaptive_skip',
  how='Detect heavy publishes server-side and skip/adapt the run-ahead rewind to avoid the stall.';
RELATE approach:runahead_server_adaptive_skip->tried_on->finding:ruled_runahead_server_adaptive_skip
  SET outcome='ineffective', date='2026-07-14',
      note='The instrumented symptom was real and the causal layer was wrong. Run-ahead itself is PROVEN +1 frame and is disarmed on prod, not deleted -- all of it is env-gated.';
RELATE finding:ruled_runahead_server_adaptive_skip->cites->source:automemory_2026;

-- ---- 4. flycast-native as the desktop client (abandoned) --------------------
UPSERT finding:ruled_flycast_native_client SET
  status='ruled_out',
  statement='Re-hosting the desktop client inside the flycast binary (flycast-native).',
  tried='Pursued as the native-client path before the fresh Rust/wgpu port.',
  evidence='Abandoned in favour of native-client/, which re-hosts the WORKING WebGPU renderer (same WGSL, rustls direct wss) and reached a running client on prod. Tauri was kept only as a fallback.',
  date='2026-07-13';
UPSERT approach:flycast_native_desktop SET name='flycast_native_desktop',
  how='Build the desktop client as a mode of the flycast binary rather than a separate renderer.';
RELATE approach:flycast_native_desktop->tried_on->finding:ruled_flycast_native_client
  SET outcome='ineffective', date='2026-07-13',
      note='Superseded rather than disproven -- the wgpu port reached a working client first and carries the renderer we already trust.';
RELATE finding:ruled_flycast_native_client->cites->source:automemory_2026;

-- ---- 5. the executor as a rollback re-sim engine (not a quick win) ----------
UPSERT finding:ruled_executor_for_rollback_resim SET
  status='ruled_out',
  statement='Using the transpiled SH4 executor as the re-simulation engine for client-side rollback.',
  tried='Scoped after the executor reached a 0-diff whole-tick reproduction.',
  evidence='Three blockers, none of them the executor being wrong: (1) roster-lock -- the SPL relocation is Storm/Cable-only; (2) a confHash residual INSIDE the hash; (3) the render-composition wall -- the executor owns mem_b only, while flycast render needs the whole SH-4 context. Verdict: an R&D track, not a jitter patch. The executor\'s real home is BROWSER prediction.',
  date='2026-07-25';
UPSERT approach:executor_drives_rollback_resim SET name='executor_drives_rollback_resim',
  how='Drive rollback re-simulation from the transpiled tick instead of a second flycast instance.';
RELATE approach:executor_drives_rollback_resim->tried_on->finding:ruled_executor_for_rollback_resim
  SET outcome='ineffective', date='2026-07-25',
      note='The 113-byte residual CLOSED (Test A 0-diff, 4f93dc647) and it STILL is not a quick win. Recording that distinction is the point: a component can be correct and still be the wrong lever.';
RELATE finding:ruled_executor_for_rollback_resim->cites->source:automemory_2026;
