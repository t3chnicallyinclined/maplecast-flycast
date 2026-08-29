# STAGE-SHARE-REPORT — stage/background share of TA delta churn (measured)

> Input: `_bwlab/cap_prod_play.mirror.zcst` (85,863,072 B, prod wire wss://nobd.net/ws,
> 2026-07-08). 5391 ZCST + 5391 ZCS2 frames, frameNum 69317..74707 = 89.85 s, 2 SYNC,
> 2 MCSV (19.2 MB join savestates, excluded), side-channel GSTA/PALF/OBJS/OBJF/TXTR
> 3.29 MB = 0.29 Mbps.
> Re-runnable: `python _bwlab\stage_share.py` (full pipeline + counterfactual legs),
> `python _bwlab\decompose.py _bwlab\cap_prod_play.mirror.zcst` (baseline),
> `cd tools\render-replica-poc && node ..\..\_bwlab\validate_js_parse.mjs` +
> `python _bwlab\validate_py_parse.py` (cross-validation),
> `python _bwlab\debug_stage_churn.py <k>` (per-frame churn word dump).
> Full console output: `_bwlab/stage_share_full_run.txt`; per-frame data:
> `_bwlab/stage_share_frames.json`.

## ⚠️ 0. Capture condition — this is NOT moving gameplay (measured, not assumed)

Decoding the GSTA side-channel (all 5391 msgs): `in_match=1`, `stage_id=11`, but
**game timer frozen at 99, camera frozen at (960.0, 95.0), every char pos_x/pos_y/health
bitwise constant, p1_buttons = p2_buttons = 0xFFFF (no input) for the entire 90 s**, while
`frame_counter` increments 1:1 with wire frames (76985→81785 over 4800 frames). The match
is loaded and running but IDLE (chars cycling idle-anim sprite_ids only).

Consequences:
- **The "ZCS2 spikes to 4+ Mbps during gameplay" claim is not present in this capture**:
  ZCS2 per-second = min 0.389 / p50 0.955 / mean 1.027 / p95 1.509 / **max 1.643 Mbps**.
- **Camera-motion stage churn is untestable on this input**: 0 of 90 seconds are
  "moving" by the tasked definition (stage changed bytes > 20 % of stage span; actual
  max ≈ 2.7 %), and the stage vertex positions are **bitwise static across all 5257
  consecutive pairs** (single-(dx,dy) fit: dx=dy=0, residual 0.0000 px, 100 % of pairs).
- Everything below is therefore the **idle-match floor** of the wire — and it still
  produced a decisive, actionable finding (§3). **Re-capture during actual play before
  sizing the camera-motion component.**

## 1. Baseline (task 1) — decompose.py

| wire | Mbps | note |
|---|---:|---|
| ZCST legacy delta frames | **4.488** | avg 9350 B/frame; per-second flat 3.39–5.20 |
| ZCS2 streaming shadow | **1.037** | 11,643,259 B; per-second 0.39–1.64 |
| all msgs incl 19.2 MB MCSV joins | 7.634 | MCSV = 2 one-off join savestates |

Uncompressed split: VRAM pages 86.56 % raw (51.8 Mbps-raw — dies in the streaming
window), TA delta run data 4.52 % + run headers 3.51 %, PVR pages 3.28 %, keyframes 1.94 %.
Delta runs: 742.8 runs/frame, median 8 B. Consecutive keyframes 91.8 % identical.

**The ZCS2 "spikes" are epoch resets, not content:** resets occur every 300 frames
(seconds 4, 9, 14, …, 89 — 18 total), and the top-10 ZCS2 seconds are exactly reset
seconds (1.39–1.64 Mbps vs ~0.95 p50). The late-join reset cadence, not the scene, sets
this capture's peak rate.

## 2. TA composition + per-class churn (tasks 2–3)

Parser cross-validation: reconstruction + span parse were validated against the real
client code — 11 sampled frames, **md5(taBuffer) and vertexCount identical** to
`FrameDecoder`+`TAParser` (web/webgpu), and the rebuilt delta payloads are **byte-exact
vs the captured wire for 5257/5257 delta frames** (exact port of the server run-builder,
maplecast_mirror.cpp:2094–2142, incl. ≤8 B gap-merge + 65535 clamp + keyframe-every-60).

Composition (constant all capture; taSize ≈ 143.4 KB):

| class | B/frame | polys/f | verts/f | % of TA |
|---|---:|---:|---:|---:|
| opaque list = stage | 133,056 (constant) | 73 | 2,400 (one 1448-vert strip) | **92.8 %** |
| TR sprites (chars/HUD) | 7,905 | 82.3 | 329.4 | 5.5 % |
| TR polys | 2,112 | 11 | 48 | 1.5 % |
| PT / modvol / control | 256 | 3 | 0 | 0.2 % |

17 distinct opaque-list TCWs, all present in 100 % of frames (texture set fully static).

Raw byte-diff churn, 5257 pairs, 22,013,218 changed B (245 KB/s):

| class | changed B | share | B/frame | of which parser-IGNORED |
|---|---:|---:|---:|---:|
| opaque (stage) | 14,711,532 | **66.83 %** | 2,798 | **98.8 %** |
| TR sprite | 4,688,012 | 21.30 % | 892 | 25.2 % |
| TR poly | 2,396,171 | 10.89 % | 456 | 0.1 % |
| others | 217,503 | 0.99 % | 42 | 59 % |
| **total** | 22,013,218 | 100 % | 4,187 | **72.0 %** |

## 3. Root cause of the stage churn: DEAD BYTES, not geometry

Within-stage field attribution (every changed stage byte classified by TA record layout):

| field | changed B | share |
|---|---:|---:|
| **DEAD (TA-ignored bytes)** | 14,527,876 | **98.75 %** |
| param face-color floats (stage lighting anim) | 181,344 | 1.23 % |
| x / y / z / uv / vertex colors | **0** | 0.00 % |

The stage mesh is 64-byte textured floating-color vertices; **bytes +24..+31 of each are
ignored by the TA per HW spec** (no parser reads them: ta-parser.mjs reads colors at
+32/+48; flycast native likewise). MVC2's TA staging memory leaves engine scratch there,
and it **flips between two byte patterns** (e.g. `3f003f00 3f000000` ↔ `3c175533
4427eaab`) whenever the engine alternates staging buffers — an ~11,649 B burst on
1,156/5,257 frames (22 %), verified with `debug_stage_churn.py`. Dead churn by group
(bytes over capture): v64pad (64B-vert padding) 14,490,136; sprite-vert ignored word
872,549; sprite-param DMA words 311,011; EOL reserved 107,983; 32B-param unused
face-color region 61,343. Dead-maskable bytes ≈ 14,584 B/frame of the 143 KB buffer.

## 4. Counterfactual wire (task 4) — streaming zstd z3 wlog=24, flush/frame, resets replicated

Legs share the pipeline; only the TA section differs (checksum + dirty pages kept
as-captured). Leg B (rebuilt runs, unstripped) equals leg A to the byte — the harness is
exact.

| leg | Mbps | vs A | what it costs to build |
|---|---:|---:|---|
| ZCS2 as measured on wire | 1.037 | — | (incl. 10 B/frame envelope) |
| A original frames | 1.015 | — | anchor |
| C stage spans spliced out | 0.879 | **−13.4 %** | client-side stage renderer + late-join stage bootstrap + wire format change |
| **D dead-byte canonicalization only** | 0.834 | **−17.8 %** | server-only: zero parser-ignored bytes before diff; NO wire format change, NO client change |
| E = D + stage strip | 0.777 | −23.4 % | both |
| translate-opcode variant | — | n/a | **carries zero information here**: stage verts are bitwise static (5257/5257), so a per-frame (dx,dy) explains 0 B of the churn (which is dead bytes + lighting colors). Skipped as a leg; its ceiling is leg C by construction. |

Reset-second peaks: A 1.35–1.59 → C 1.05–1.26 → D 1.11–1.31 → E **0.93–1.11 Mbps**.
Uncompressed benefit of D: kills 72.0 % of all changed TA bytes (also halves run count →
less client apply work; helps the legacy ZCST wire too, which this capture still ships at
4.488 Mbps).

## 5. Verdict

1. **Stage-stripping is NOT the right Phase 3 on this evidence.** On the only capture we
   have, it saves 13.4 % — less than dead-byte canonicalization (17.8 %), while costing a
   client stage renderer, late-join stage bootstrapping, and a wire change. The premise
   ("camera motion re-ships the stage every frame") is falsified for idle-match: the
   stage geometry never moved a bit in 90 s, and the game was provably idle — so the
   gameplay premise itself is **unmeasured**, not disproven.
2. **Do dead-byte canonicalization first** (new Phase 0.5 of TA-WIRE-V2-PLAN): server
   zeroes TA-ignored bytes (64B-vert padding, sprite ignored word, sprite-param DMA words,
   EOL reserved, unused param face-color regions for colType 0/1) before the shadow diff.
   Server-only, no format change. Gate: (a) audit the four parsers' read-sets over the
   masked byte classes (js verified here by construction; native/wasm follow the HW spec;
   relay forwards verbatim); (b) the determinism rig must compare masked-vs-masked (the
   wire stops being byte-equal to raw engine TA at ignored bytes — by design).
3. **A translate opcode wins nothing** on any capture where stage verts don't move, and
   can never beat stripping when they do (strip ⊇ translate). Park it.
4. **Re-capture during real human gameplay** (verify via GSTA: timer ticking, buttons
   changing, camera moving) and re-run `stage_share.py` unchanged. Only that capture can
   size the camera-motion stage component and test the 4+ Mbps spike claim. In THIS
   capture the ZCS2 peaks are the 300-frame epoch resets (1.64 Mbps max) — if prod spikes
   to 4+, first suspect scene transitions/SYNC-adjacent resets, not the stage.
