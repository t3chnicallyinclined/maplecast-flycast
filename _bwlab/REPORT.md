# TA-Mirror Wire Bandwidth Lab — Measured Decomposition + Compression Experiments

Date: 2026-07-08. Branch `feat/render-replica-live`.
Corpus: `_bwlab/cap.mirror.zcst` — 2104 ZCST delta frames + 1 SYNC, frameNum 1..2104 = 35.07 s @60fps,
captured live from the local headless GSTA server (`_run_srv_gsta.bat`, ws://127.0.0.1:7200, in-match autoload savestate)
via `tools/render-replica-poc/_cap_persist.mjs --hard 40 --after 35`.

All numbers are measurements run on this corpus (scripts in this directory, re-runnable):
- `decompose.py` — wire decomposition, run histogram, keyframe diff
- `experiments.py` — rounds a-g (levels, dict, SoA, keyframe-delta, prev-frame dict, streaming)
- `experiments2.py` / `experiments3.py` — hash-gate, vcache-refs, runSoA, stacked combos, reset variants

Projection formula everywhere: **Mbps = (total compressed bytes / 2104 frames) x 60 fps x 8 / 1e6**.
ms/frame = wall time of the compression step / 2104 (libzstd C time; python transform overhead noted separately).

## 0. Headline: the wire is 6.875 Mbps, not ~4.1

| stream | measured |
|---|---|
| ZCST TA delta frames (the mirror wire proper) | **30,135,897 B / 35.07 s = 6.875 Mbps**, avg 14,323 B/frame |
| all mirror WS msgs (incl. GSTA/OBJS/OBJF/PALF/TXTR side-channels) | 35,493,857 B = 8.097 Mbps |
| per-second spread (s00-s34) | min 6.274, max 7.366 Mbps — flat, no bursts |
| keyframes (36 @ every 60 frames) | 1,245,735 B = 0.284 Mbps (avg 34,604 B compressed) |
| non-key deltas (2068) | 28,890,162 B = 6.591 Mbps (avg 13,970 B) |
| SYNC (1) | 540,498 B one-shot |

Side-channels on the same socket (not part of ZCST): OBJF 1,941,992 B + OBJS 1,601,144 B + GSTA 799,520 B + TXTR 371,478 B + PALF 33,664 B = **1.08 Mbps** on top of the 6.875.

## 1. Uncompressed byte split (where the bytes come from)

Total inner (uncompressed) = 298,964,260 B = 68.20 Mbps raw. Parse check: 0 bytes unaccounted.

| component | bytes | % of raw | raw Mbps | compressed alone (zstd-1) |
|---|---:|---:|---:|---:|
| (c) VRAM dirty pages (region 1) | 235,397,120 | **78.74%** | 53.703 | **3.393 Mbps** |
| (a) TA delta run data (non-key) | 26,949,650 | 9.01% | 6.148 | 2.707 Mbps (incl. run headers) |
| — TA delta run overhead (6 B/run + term) | 16,917,136 | 5.66% | 3.859 | (in above) |
| (d) PVR/palette dirty pages (region 3) | 14,008,320 | 4.69% | 3.196 | 0.307 Mbps |
| (b) TA keyframes (full buffer) | 5,202,432 | 1.74% | 1.187 | 0.214 Mbps |
| — page headers (5 B/page) | 304,450 | 0.10% | 0.069 | — |
| (e) fixed frame headers (88 B/frame) | 185,152 | 0.06% | 0.042 | 0.036 Mbps |
| — ZCST envelope (8 B/frame) | 16,832 | 0.01% | 0.004 | — |

Component attribution of the compressed 6.87 Mbps: **VRAM pages 3.39 + TA delta 2.71 + PVR pages 0.31 + keyframes 0.21 + headers 0.04** (separately-compressed, sums to 6.66; the 0.2 gap is shared-context benefit).

### TA delta fragmentation (why the delta costs 2.7 Mbps for 0.77 MB/s of data)
2,818,144 runs over 2068 delta frames = **1362.7 runs/frame, median run = 11 B, mean 9.6 B, max 2684 B**.
96.8% of run data lives in runs <=32 B. Per delta frame: ~13 KB of data carries ~8.2 KB of 6-byte run headers — 39% of the delta section is addressing overhead.

| run size | runs | bytes | % of run data |
|---|---:|---:|---:|
| 1-8 | 1,095,313 | 5,670,261 | 21.0% |
| 9-32 | 1,713,483 | 20,430,880 | 75.8% |
| 33-128 | 8,697 | 361,160 | 1.3% |
| 129-512 | 268 | 90,240 | 0.3% |
| 513-2048 | 372 | 371,570 | 1.4% |
| 2049-8192 | 11 | 25,539 | 0.1% |

### Keyframe redundancy
36 keyframes, sizes 142,144-147,904 B (mean 144,512). Consecutive keyframes are **92.0% byte-identical** (avg 132,211 identical of 143,713 overlap).

### Dirty-page redundancy (the big one)
60,890 pages shipped (~27.3 VRAM + 1.6 PVR pages/frame).
- **34,627 (56.9%) are byte-identical to the last content shipped for that same page slot.** The producer memcmps live vs shadow (maplecast_mirror.cpp:2205), so these come from the DMA force-dirty bitmap, which ships "even if memcmp would say it's unchanged" (comment at :2183-2185) — the game re-DMAs identical texture bytes every frame.
- **50,444 (82.8%) are byte-identical to SOME page content already shipped** (content-addressed). Only 10,446 unique page contents exist in the whole 35 s capture. (Exactly what the existing VCACHE mode at :1620-1630 / :2213-2228 exploits.)

## 2. Compression experiments (same 2104 inner frames)

Baseline sanity: re-zstd-1 per frame = 6.870 Mbps vs wire-as-captured 6.875 Mbps -> parser + methodology validated.

| # | experiment | Mbps @60fps | server ms/frame | notes |
|---|---|---:|---:|---|
| a | zstd-1 per frame (today) | 6.870 | 0.111 | baseline |
| b | zstd-3 per frame | 6.612 | 0.154 | |
| b | zstd-9 per frame | 6.301 | 0.727 | |
| b | zstd-19 per frame | 5.426 | 24.420 | blows the 1 ms budget 24x — dead |
| c | zstd-1 + trained dict (112,640 B, 200 samples) | 4.728 | 0.101 | dict trained on this corpus (self-test bias) |
| c | zstd-3 + trained dict | **3.756** | 0.144 | best per-frame-independent option |
| d | SoA byte-plane (32 B parcels) + zstd-1 | 7.907 | 0.111+0.15 | **WORSE than baseline — rejected** |
| d | SoA byte-plane + zstd-3 | 7.835 | 0.172+0.15 | TA parcels not fixed-record; transpose destroys locality |
| e | keyframe as XOR-delta vs prev keyframe (zstd-3) | kf share 0.284 -> 0.034 | 0.19/kf | 24,760 -> 4,105 B/kf; only 0.17 Mbps total — minor |
| f | zstd-1 + prev-frame raw dict | 4.422 | 0.298 | |
| f | zstd-3 + prev-frame raw dict | 3.907 | 0.592 | superseded by (g): cheaper AND better |
| g | streaming zstd-1, shared window, flush/frame | 3.831 | 0.105 | |
| g | streaming zstd-3, flush/frame | 2.723 | 0.125 | |
| g | streaming zstd-3 wlog=24 (16 MB window) | **1.931** | 0.110 | -72% vs baseline, same CPU |
| g | streaming zstd-3 wlog=26 | 1.810 | 0.109 | window past 32 MB ~ flat |
| i | streaming zstd-9 wlog=24 | 1.471 | 0.472 | |
| i | streaming zstd-12 wlog=24 | 1.408 | 1.263 | over budget |

### Stacked wire changes (round 3)

hash-gate = server skips forced-dirty pages whose 8-B hash == last-shipped hash for that slot (no wire change).
runSoA = delta section re-serialized as `nRuns | gap-offsets[] | lens[] | data[]` (gap = offset - prev run end).
vcache-refs = content-addressed 13-B page refs for any already-shipped content — the existing VCACHE wire.

| experiment | Mbps | zstd ms/frame |
|---|---:|---:|
| hash-gate + per-frame zstd-1 (today's wire, server-only fix) | 6.131 | 0.100 |
| hash-gate + streaming zstd-3 wlog=24 | 1.853 | 0.079 |
| hash-gate + runSoA + streaming zstd-3 wlog=24 | 1.350 | 0.061 |
| hash-gate + runSoA + streaming zstd-6 wlog=24 | 1.037 | 0.222 |
| hash-gate + runSoA + streaming zstd-9 wlog=24 | **0.931** | 0.306 |
| vcache-refs + runSoA + streaming zstd-3 wlog=24 | 1.211 | 0.062 |
| vcache-refs + runSoA + streaming zstd-9 wlog=24 | 0.837 | 0.231 |
| vcache-refs + runSoA + streaming zstd-9 wlog=25 | **0.788** | 0.231 |

### Late-join tax (streaming needs a stream-start for new viewers)
Context reset every N frames gives join points every N/60 s:

| variant | no reset | reset@300 (5 s) | reset@60 (1 s) |
|---|---:|---:|---:|
| streaming zstd-3 wlog=24 (alone) | 1.931 | — | 2.529 |
| hash-gate + runSoA + strm z9 w24 | 0.931 | 1.079 | 1.619 |
| vcache-refs + runSoA + strm z9 w24 | 0.837 | **1.026** | 1.505 |

(The vcache page cache is app-level, so it survives resets — why vcache beats hash-gate under reset.)
Alternative to resets: per-client ZSTD_CStream on server (0.23-0.31 ms/frame/client) or relay-owned per-client streams while the server sends one no-reset stream.

Timing caveats: transform costs from Python (runSoA 0.86 ms, dedup 0.23 ms) are upper bounds — in C++ the server already holds the run list before serialization (runSoA ~ free) and hashing ~29x4 KB pages ~ 30 us (xxhash). zstd ms/frame is libzstd C time and transfers directly.

## 3. Top-3 recommended wire changes (ranked by Mbps saved / risk)

**1. Streaming zstd with shared window (flush-per-frame), wlog=24, level 3.**
6.870 -> 1.931 Mbps no-reset (-4.94 Mbps, -72%) at 0.110 ms/frame — CHEAPER than today's 0.111. The single change that captures the cross-frame redundancy (82.8% duplicate page contents, 92% identical keyframes) that per-frame contexts can never see. Risk: moderate — new envelope magic, all four parsers switch to a streaming decompressor (16 MB client window), late-join needs reset@N (reset@60 -> 2.529, reset@300 -> ~1.4) or per-client/relay-owned streams. Best absolute saving of any single change.

**2. Hash-gate the DMA force-dirty page ships (server-only, zero wire change, zero client change).**
The force-dirty bitmap ships 34,627 pages (56.9% of all pages, 142 MB raw = 47% of ALL raw bytes) whose content is byte-identical to what the client already holds for that slot. Keep an 8-B hash per shipped slot; on forced-dirty, skip if unchanged. 6.870 -> 6.131 Mbps (-0.74) alone, compounds under streaming (1.931 -> 1.853), and roughly halves client decompress/apply work. Risk: near-zero — one hash-compare in serverPublish() (maplecast_mirror.cpp ~:2205); correctness identical by construction (client state == last-shipped content). Highest ratio measured. Do this first regardless.

**3. runSoA delta serialization (gap-offsets | lens | data) + level bump once streaming exists.**
1363 runs/frame x 6 B headers = 39% of the delta section; splitting the run list into three homogeneous arrays with gap-encoded offsets takes streaming zstd-3 wlog=24 from 1.853 -> 1.350 Mbps (-0.50) and REDUCES CPU (0.079 -> 0.061 ms). Risk: low-moderate — pure serialization reorder of data the server already has, but touches all four parsers. With the freed CPU, bumping the streaming level to 9 lands **0.931 Mbps (hash-gate) / 0.837 (vcache-refs) at 0.31/0.23 ms/frame — under the 1 Mbps target within the 1 ms budget.**

Rejected with numbers: 32-B-parcel byte-plane SoA transpose (WORSE: 7.9 vs 6.9), zstd-19 per frame (24.4 ms), keyframe XOR-delta as a priority (keyframes are only 0.28 Mbps; still a free -0.17 if touching that path). Fallback if streaming is unacceptable: trained shared dictionary (ship ~110 KB dict once) keeps per-frame-independent messages at 3.756 Mbps / 0.144 ms.
