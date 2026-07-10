# HANDOFF — Dark band CLOSED + HUD/stage/panel arc (2026-07-09 → 07-10)

> Continues `docs/HANDOFF-SATELLITE-READSET-2026-07-09.md`. This session CLOSED the
> re_kb/51 "dark band" residual (BOTH mechanisms), fixed the live HUD (stage-bake
> contamination), fixed two splice/pairing bugs, and rebuilt the debug cockpit with
> evidence-grade tooling. Everything on `feat/render-replica-live`, deployed to prod
> `149.28.44.118`. **Read `docs/RENDER-STATE.md` + `tools/re_kb/README.md` first.**

## TL;DR — the five fixes shipped (all live, all gated)

1. **re_kb/66 — blank-record desc slots** (`745751f6b`). `rebuild_tile_grid` skipped
   sel==0xFF blank GFX2 records; the engine allocates them formula-derived tiledesc
   slots (PROVEN: shipped slot `[20 00 00 01]` == formula on GFX1[0xFF] sw=4 sh=4) and
   suppresses only the draw (pen still accumulates). The `continue` compacted the slice
   one slot left → every later record read its neighbor's count/m → +1 over-emission +
   a 3-part strip drawn 51-180px low (burst dark bands on P2C1).
2. **re_kb/68 — wide-part COLUMN-PAIR-MAJOR tile order** (`491c36f35`) — **the ORIGINAL
   re_kb/51 mechanism.** Engine desc builder emits wide parts as
   `[col-pair][2-row band][col-in-pair][row]` (a 2-col macro-column through all bands,
   then the next pair); our rebuild was row-band-major. Identical for cols≤2 OR rows≤2
   (all normal chars) — 2×2-block-swaps 4×4+ parts (Sentinel 128×128 sel 124: tiles
   20↔24/21↔25/22↔26/23↔27 every frame = the flashing black blocks + holes).
   Proven against the SHIPPED desc (`_tx_desc_order` MATCH=YES post-fix).
3. **re_kb/67 — stage-bake HUD contamination** (`2e4429249`). STG0B_ta.json carried
   **68/72 meshes of FROZEN bake-time HUD** (tcw 809be00 bars/font, 8080000,
   809dexx-809e9xx portraits) — census-proven z-identical twins of the live wire HUD
   groups. Prepended every frame → depth writes at the live HUD's own z-planes
   (0.0052..0.0141) → live HUD strict-Greater REJECTED against its stale doppelgänger =
   "incomplete HUD"; those depths also sat over the char tiles (z≈0.0092) = cape
   interference. Asset filtered 72→4 meshes (deck+backdrop) + `bake_stage_from_ta.py`
   now drops `HUD_TEX_WORDS` at bake time. **stage-client fetches now cache-busted**
   (`?v=hudpurge1`, `64609abbc`) — lazy fetches ignore hard-refresh!
4. **Cable-vanish — body-merge pass bookkeeping** (`92bf7f824`). `_bodyMerge` grew
   `g.translucent` but never extended `renderPasses.tr_count` (unlike `_stageMerge`);
   honoring wire passes truncated the merged tail = appended bodies undrawn. Fixed at
   the merge + renderer-side Math.max clamp of the final pass (no future merge can
   silently drop polys).
5. **Body-pairing drift** (`5f66d5641`). The two-socket (ZCS2 wire vs /replica-live)
   vframe ring was 16 deep (~267ms) with exact-match only → after minutes of socket
   drift, permanent pairMiss → latest-wins fallback = WRONG-POSE bodies (progressive
   whole-sprite garble, "perfect on fresh connect"). Now 120 deep (~2s) + nearest-match
   (usable ≤90f). Wire panel shows `pair Ne/Nn/Nm vfΔ` live. Measured after fix:
   7052e/313n/0m vfΔ0 over 2 min.

Also: **debug panel rebuilt** (7 collapsible groups; ZCS2/WTCH overlays moved off the
sidebar; duplicate dbg_singlePass/CustomBG dead-UI removed; singlePass default FALSE —
its old checked-default merged wire render passes), **knob persistence** (💾 Save /
localStorage, `_applyKnobs()`), **state census** (one-shot: per (list, src=stage|wire|body,
depthFunc, depthWrite, blend, TCW) with z-ranges + knobs echo — THE tool that cracked
the stage-bake case).

## The evidence pipeline (use this method — it closed everything)

capture_break.mjs (local, wss://nobd.net/replica-live, non-disruptive) → `.mcrr`
+ frame-exact ASMTRACE window (awk by vframe) → `_tx_detect.mjs` (facing-aware corner
pairing, per-node vframe-shift search, swap detector, count-mismatch report) →
`_tx_dump.mjs` (side-by-side node-frame + per-quad sels) → `_tx_rec_dump.mjs` (GFX2/desc
RAM hexdump per frame) → `_tx_desc_order.mjs` (shipped vs rebuilt desc order per record).
WALKDBG pattern: instrument gen_walker.c prints (backup → edit → emcc node build →
restore), run on `_extract_ram.mjs` output.

**Gates on file:** band2.mcrr (1273 gameplay frames) 5664/5664 node-frames clean @6px;
band4.mcrr (2400 frames incl Sentinel) 4800/4800 clean, 9600 transpositions → 0.
Captures + asm windows in `tools/render-replica-poc/` (band2/band4 + asm_band2/4.txt).

### Diff-tooling gotchas (cost hours — don't re-hit)
- **ASMTRACE anchor = quad LEFT-edge X or RIGHT-edge X by facing** (Y = quad max-y
  always). Compare corner-aware or every moving part shows a phantom width×(5/3) offset
  (`_diff_sweep` has this artifact; `_tx_detect` is corner-aware).
- **ASMTRACE dies silently when /dev/shm fills** (mid-line truncation; ~75 min of play
  = 1.77GB). `df /dev/shm` before trusting a missing trace window.
- **ASMTRACE was force-armed by a systemd DROP-IN** —
  `/etc/systemd/system/maplecast-headless.service.d/asmtrace.conf` (now moved to
  `/root/asmtrace.conf.disabled-20260710`). headless.env toggles were no-ops all along.
  It's a PRESENCE-check env (=0 still arms). NOW fully off; re-arm = restore the
  drop-in + daemon-reload + restart.
- Browser: lazy JS fetches (stage JSON) ignore hard-refresh — always ?v= them.

## Prod state (149.28.44.118)

- Wasm: `render_frame.{mjs,wasm}` `?v=widecarve1` (has re_kb/66 + re_kb/68 + the
  0x85xxx cull removal). Backups: `render_frame.wasm.bak-{blankslot,widecarve}`.
- Web: `webgpu-test.html` (panel/census/knobs/pairing), `pvr2-renderer.mjs ?v=9`
  (per-list knobs + census + final-pass clamp), `stage-client.mjs ?v=3`,
  `test-atlas/stages/STG0B_ta.json` (4 meshes; `.bak-hudpurge` = original).
- ASMTRACE OFF (drop-in removed, env commented, log 0, tmpfs 14%). Cron truncate guard
  REMOVED (was `*/15 truncate mc_assembly.log`).
- ZCS2 default wire (`?legacy=1` opts out). Bandwidth measured this session: 0.39 Mbps.

## OPEN — the remaining residual + next steps

**Periodic one-frame glitch (~every few seconds, SUB-epoch cadence — user-verified NOT
epoch-correlated).** Bodies pop/garble for a frame then recover. Leads, in order:
1. **Body strip-filter bank set vs arena parity**: `_bodyApplyFrame` keeps only TCW
   banks `{0x82,0x83,0x88,0x89}` (webgpu-test.html ~line 700); the engine's rectab
   resolution FLIPS with arena_base parity (16↔400) and a census (dump 2) showed
   `2a084xxx` (bank 0x84!) body quads — frames whose tiles resolve to the other-parity
   banks would be DROPPED for that frame = 1-frame holes. Audit the full bank set the
   engine uses (capture TCW-bank histogram across frames) and widen/parity-fix the set.
2. Near-pair clustering (4.3% of frames pair at vfΔ1 — a stale-pose frame during fast
   motion). Check if glitch frames == near-pair frames (add a flash counter/log).
3. On-change GFX tail application timing (BODY.gfxN ticks vs glitch moments).
Method: same as re_kb/68 — capture (re-arm ASMTRACE via the drop-in), gate, and if
geometry is clean through glitch frames, it's client-side compositing (bank filter).

Then (carried over): efxtmpl region port to prod (re_kb/50, low priority — supers look
good), stage_id→STGxx map for the other 16 stages (only STG0B baked; rebake now safe
with the HUD filter), Phase 2a pixel-side (G1-G9), lockstep/predict arc.

## Commit trail (this session, oldest first)

`745751f6b` re_kb/66 blank-slot + _tx tools · `e38f9b49e` per-list knobs + census ·
`268b7d2e9` census z-ranges · `8c8fe0e2f` panel reorg + singlePass default ·
`da4dacc0d` knob persistence · `92bf7f824` body-merge pass fix + src= tags ·
`64609abbc` stage fetch cache-bust · `2e4429249` re_kb/67 stage-bake HUD purge ·
`491c36f35` re_kb/68 col-pair order · `5f66d5641` pairing drift ring.
re_kb: `66_blank_record_desc_slot` · `67_stage_bake_hud_contamination` ·
`68_wide_part_colpair_order` (+ finding:gsta_body_part_transposition RESOLVED).
