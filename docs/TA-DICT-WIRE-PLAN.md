# TA-DICT-WIRE-PLAN.md — TDW1: content-addressed TA-block wire

**Status: CURRENT plan (2026-07-14). Phases 0, 1, and the Phase-2 equality gate COMPLETE — all gates PASS (see §3). Fully isolated: env-gated default OFF, new magics only, nothing deployed to prod. Next: browser decoder + render cut-over (Phase 3).**
**Prereq reading:** docs/RENDER-STATE.md (ledger), docs/TA-WIRE-V2-PLAN.md (ZCS2, the envelope this reuses), `_bwlab/REPORT.md` (all baseline numbers).

---

## 0. What this is and the measured basis

Replace the **positional byte-delta** TA section of the wire with **content-addressed block references**: every 32/64-byte TA parameter block is stored once in a server↔client dictionary; each frame ships only the ordered list of dictionary ids (+ any never-seen-before blocks). Geometry is byte-exact **by construction** — the client reassembles the engine's own TA stream, in emit order, and renders it through the existing full-TA path. No render_frame, no sprite machine, no splice descriptor, no STM2 body state on this client class.

Measured basis (2026-07-14, scripts `_bwlab/ta_dict_histogram.py` / `ta_dict_canon.py` / `ta_dictwire_sim.py`, corpora `_bwlab/cap.mirror.zcst` 2104 frames in-match, `_bwlab/cap_prod_play.mirror.zcst` 5348 frames prod play):

| fact | value |
|---|---|
| TA grammar (canon-masked, 89 s prod play) | 1 control + 116 poly-global + 302 sprite-global + 4,704 poly-vertex + 23,684 sprite-vertex distinct blocks |
| sprite global params | PCW/ISP/TSP/base/offs each take exactly **1** value; only TCW varies (302 texture addrs); **8** distinct UV blocks |
| positions | quantized on the native 384×224 lattice (X steps 5/3 px, Y steps 480/224 px) |
| params per frame | ~2,700, near-constant (2,674–2,810) |
| content repeat | 95.16 % of blocks exist in the previous frame, **99.82 %** in some prior frame |
| dict growth | 28,807 blocks / ~1.7 MB in 89 s; new-block rate decays 1,553/s → 63/s |
| **dict-wire, spec-exact codec, mask ON** | **0.106 Mbps** prod play / 0.234 Mbps in-match, median 181 B/frame, p95 559 B, max 31 KB (scene change) — streaming zstd-3 flush-per-frame (`ta_dictwire_decode.py`, 2026-07-14) |
| **dict-wire, spec-exact codec, mask OFF** | **0.357 Mbps** prod play (dict 213,868 blocks / 12.85 MB) / 0.653 Mbps in-match — wire bytes **bit-identical to engine TA**, zero mask risk |
| today's TA-delta share | 2.71 Mbps (per-frame ZCST, REPORT.md §1) |
| GSTA+OBJS+OBJF side-channels, same 35 s corpus | ≈ 0.99 Mbps |

What it does **not** replace: the texture channel. VRAM dirty pages + PVR/palette pages ship exactly as today (Phase 5 thins them via the already-measured vcache/bodytex-local arcs). Effects/super **textures** stay on the wire — they are sim-composited into Effect Poly scratch and exist in neither ROM nor savestate. The client needs **no ROM** in this design: the dictionary IS the geometry; textures arrive as pages.

Per the 2026-07-14 decision: **the client wire carries zero game-state semantics.** Hitboxes/meters/match data are read server-side (gamestate.cpp `readAllDrawn`, slot table `0x8C2895E0`) and go to the database (Phase 6) — GSTA/OBJS/OBJF/STM2 all retire from this client class.

---

## 1. Exactly how this differs from what we send today

### 1a. Legacy ZCST delta frame (inner, uncompressed — parsed at `maplecast_mirror.cpp:1571-1658` client-side, built at `:2569-2634`; layout comment `:2031-2033`)

```
off 0   frameSize        u32
off 4   frameNum         u32
off 8   pvr_snapshot     64 B (16×u32)
off 72  taSize           u32
off 76  deltaPayloadSize u32
off 80  TA section:
          if deltaPayloadSize == taSize → raw TA bytes (keyframe)
          else → runs { u32 byteOffset, u16 runLen, runLen bytes } … 0xFFFFFFFF terminator
then    checksum u32
then    dirtyPageCount u32   (0xFFFFFFFF = VCACHE marker, then real count; entries grow a 9 B ref header)
then    page entries { regionId u8, pageIdx u32, 4096 B }  × N
then    optional STM2 state trailer: … dataLen u32 + "STM2"   (WIREMON parse `:3369-3375`)
```

The TA delta is **positional**: a run says "at byte offset O, replace runLen bytes". Any content shift (a sprite param appearing/disappearing upstream in the buffer) re-addresses everything after it — this is exactly the measured CHARSTRIP inflation (stripping body quads shifted every remaining TA byte; docs/RENDER-ARCHITECTURE-CHECKPOINT-2026-07-11.md).

### 1b. ZCS2 streaming envelope (built at `:3290-3358`; spec comment `:472-486`)

```
'ZCS2'(4)  epoch u8  flags u8  innerSize u32 LE
[camera 132 B if flags bit3] [vframe u32 if bit5] [orderRuns if bit6] [seq u16, bit7]
+ one streaming-zstd chunk (persistent CStream, wlog24, flush per frame) → yields ONE legacy inner frame (1a)
```

Flag bits — **all 8 allocated**: bit0 streamStart, bit1 runSoA, bit2 stagestrip, bit3 camera, bit4 charstrip, bit5 vframe, bit6 splice-order runs, bit7 seq. This is why TDW gets its own magic, not a ZCS2 flag.

### 1c. /replica-live (:7212) + STM2 (docs/RENDER-REPLICA-RECORDING-FORMAT.md)

Ships **RAM state regions** (the *cause*); the client re-executes transpiled render code (render_frame.wasm) to regenerate geometry. Byte-exact for bodies, but: super floor ~70 KB/frame (efxtmpl/rectab = 84 % of a super's wire volume — docs/HANDOFF-WIRE-THINNING-2026-07-11.md), ~6 ms wasm decode, read-set completeness is a standing risk class, and it needs the vframe-pairing machinery (bit5) because body TA is regenerated out-of-band.

### 1d. TDW1 (this plan)

Ships the **effect** — the TA blocks themselves — but by content identity instead of buffer position:

| property | ZCST/ZCS2 delta (today) | STM2+render_frame (today) | **TDW1 (planned)** |
|---|---|---|---|
| TA encoding | positional byte-runs vs prev frame | not shipped; regenerated from state | ordered dict refs; new content ships once, ever |
| content shift (strip, scene change) | re-addresses everything after it | n/a | refs change, dictionary unaffected |
| super cost | cold pages + TA churn | +70 KB/frame state floor | refs only (~0 marginal); cold *pages* unchanged |
| compositing order | exact (raw stream) | needs splice descriptor (bit6) + vframe pairing (bit5) | exact (raw stream, emit order) |
| client CPU | delta-apply + parse | ~6 ms wasm + pairing | memcpy concat + parse (cheapest) |
| garble risk classes | none (ground truth) | read-set/reconstruction classes | none (ground truth by construction) |
| measured TA-side wire | 2.71 Mbps (ZCST) / less under ZCS2 window | 0.33–0.36 Mbps steady, spikes | **0.106–0.236 Mbps** |

---

## 2. Wire spec (exact)

Two new message types. Legacy ZCST and ZCS2 remain **byte-for-byte unchanged** at every phase — king.html, emulator.html, existing browsers, and the native client are untouched until they opt in. The relay needs **no change** through Phase 3: unknown magics classify as `Critical` = never dropped, strictly in-order (`relay/src/protocol.rs:133-138, :216`; `relay/src/fanout.rs:448`) — exactly the delivery guarantee a dictionary stream needs.

### 2a. `TDW1` — per-frame message

```
off 0   'TDW1'            4 B magic
off 4   dictEpoch         u8    — bumps on dictionary reset; client hard-resyncs on change
off 5   flags             u8    — bit0 zstd streamStart
                                  bit1 canonMasked (TACANON dead-byte map applied)
                                  bit2 fullFrame   (checksum+pages section present — Phase 3)
                                  bit3–7 reserved, must be 0
off 6   seq               u16 LE — per-epoch counter; gap ⇒ desync ⇒ wait for streamStart (ZCS2 discipline, `maplecast_mirror.cpp:3313-3319`)
off 8   innerSize         u32 LE
off 12  streaming-zstd chunk (own persistent ZSTD_CStream, wlog24, level 3, ZSTD_e_flush per frame — clone of the ZCS2 compressor discipline `:472-486`)
```

Inner payload (uncompressed, `innerSize` bytes):

```
off 0   frameNum   u32   (server publish counter — same value as legacy inner off 4)
off 4   vframe     u32   (game frame counter 0x8C3496B0, STARTRENDER-stamped like ZCS2 bit5)
off 8   taSize     u32   (reassembled TA buffer size; sanity check, must be Σ block lens)
off 12  nBlocks    u32
off 16  newSection u32   (byte length of the newBlocks section, INCLUDING length prefixes)
off 20  refs       nBlocks × u32 LE   (dictionary ids, emit order)
then    newBlocks  repeat { u8 len (32 or 64), len bytes }   — first-appearance order
[flags bit2 only:] checksum u32 + dirtyPageCount u32 + page entries — byte-identical
                   layout to legacy (1a), so the existing page-apply code is reused verbatim
```

**Decoder algorithm (normative):** iterate `refs` in order; if `id == dict.length`, pop the next block from `newBlocks` and append it to the dict, then emit it; if `id < dict.length`, emit `dict[id]`; if `id > dict.length` → desync. Concatenation of emitted blocks = the frame's TA buffer. *(Deliberate deviation from `ta_dictwire_sim.py`: the u8 length prefix on new blocks. The sim relied on FSM replay to infer lengths; the prefix removes that coupling for ~63 B/s cost.)*

### 2b. `TDWS` — dictionary snapshot (late join / epoch reset)

One-shot, **own outer envelope**: `'TDWS'(4) + uncompressedSize u32 LE + zstd blob`.

> ⚠️ **LESSON (2026-07-14, hit live):** never introduce a new inner type under the `ZCST` outer. Every legacy consumer decompresses every ZCST message and pattern-matches the inner — and `web/js/render-worker.mjs:397` routes any ZCST with usize > 1 MB **as a compressed SYNC**. The first TDWS implementation rode the ZCST outer; a 12.7 MB snapshot was swallowed as a VRAM/PVR SYNC → garbled characters in the browser. New message types get a new OUTER magic, full stop (unknown outer magics are dropped by magic checks + the frameSize belt everywhere, and the relay passes them as Critical).

Inner (zstd-compressed):

```
'TDWS'(4)  dictEpoch u8  pad u8×3
blockCount u32   totalSectionBytes u32
repeat { u8 len, len bytes } × blockCount   — dictionary in id order (id = index)
```

Sent: to each client on join (alongside SYNC), and broadcast on every dictEpoch bump. Expected size: ~1.7 MB raw at the 89 s mark, zstd-compressed one-shot (same class as today's ~540 KB SYNC).

### 2c. Dictionary lifecycle

- Ids are insertion-order, dense, never reused within an epoch. The dict **persists across zstd stream restarts** (streamStart only resets the compressor window, not the dict).
- Caps: `MAPLECAST_TADICT_MAXBLOCKS` (default 1,048,576) and `MAPLECAST_TADICT_MAXMB` (default 64). On breach: `dictEpoch++`, clear, broadcast TDWS(empty-or-rebuilt), set streamStart. Measured decay (63 new/s falling) says a real session should never hit the cap; the cap is the runaway backstop.
- Server lookup: `XXH64(block) → id` hash map keyed on the full block bytes (hash collision ⇒ memcmp confirm). ~2,700 lookups/frame; budget ≤ 200 µs/frame, measured in Phase 1 stats.
- Canon mask: when `MAPLECAST_TACANON=1` the wire TA copy is already dead-byte-zeroed **before** the encoder sees it (`taCanonicalize`, `maplecast_mirror.cpp:514-572` — the identical FSM/ranges the lab scripts used; parser-read-set validated 11/11 md5). TDW sets flags bit1 accordingly. The dict-wire itself is mask-agnostic: it content-addresses whatever bytes the chain carries. The 0.106 number is the masked variant; the unmasked variant is measured in Phase 0 (expected worse — 171,391 vs 44,197 distinct blocks on the 35 s corpus).

---

## 3. Phases and gates (order + risk, no time estimates)

### Phase 0 — offline round-trip proof + unmasked numbers — ✅ COMPLETE 2026-07-14

`_bwlab/ta_dictwire_decode.py`: full encoder+decoder pair implementing §2a exactly (length-prefixed new blocks). The G0-B canon reference is an independent killRange-style implementation, cross-checking the walker's block masking.

- **G0-A (exactness, mask OFF): PASS** — 5348/5348 (prod play) + 2104/2104 (in-match) frames byte-exact vs the original TA buffer.
- **G0-B (exactness, mask ON): PASS** — 5348/5348 + 2104/2104 frames byte-exact vs the independent canon reference.
- **G0-C (numbers):**

| corpus | mask OFF (bit-identical bytes) | mask ON (TACANON map) |
|---|---|---|
| prod play 89.1 s | **0.357 Mbps**, median 512 B/frame, dict 213,868 blk / 12.85 MB | **0.106 Mbps**, median 181 B/frame, dict 28,807 blk / 1.74 MB |
| in-match 35.1 s | **0.653 Mbps**, median 1078 B/frame, dict 171,391 blk / 9.53 MB | **0.234 Mbps**, median 420 B/frame, dict 44,197 blk / 2.69 MB |

**Consequence for the plan:** ~~the mask is an OPTIMIZATION, not a prerequisite~~ — **OVERTURNED BY LIVE PLAY 2026-07-14 (same day):** under real input-driven fighting, unmasked dead-byte scratch churn ran ~1,800 new blocks/frame, blew the 1M-block dict cap every ~30–60 s (epoch storm), and pushed the wire to 4.5–6.5 Mbps. The offline corpora undersampled this failure mode. **TACANON=2 is REQUIRED for TDW** (`_run_srv_tadict.bat` sets it): live masked run = dead bytes 14.7 KB/frame zeroed (10.2 % of buffer), dict converged at ~67 K blocks / 3.9 MB with new-content windows hitting **0 B/frame**, wire **0.09–0.13 Mbps**, zero cap hits. Byte-equality is unaffected (both legs encode the same canon'd copy); the 20,000-frame live gate PASSed spanning the unmasked storm, epoch resets, AND a server restart+TDWS resync — 0 mismatches. TACANON's own pixel-neutrality remains parser-validated (11/11 md5) + visually confirmed in play; the formal frozen-frame pixel gate stays a G3-a item.

### Phase 1 — server shadow encoder (`MAPLECAST_TADICT=1`) — ✅ COMPLETE 2026-07-14

Landed in `core/network/maplecast_mirror.cpp`: `tadict::` namespace (encoder, FNV64-keyed intern, TDWS builder, own ZSTD_CStream wlog24/lvl3) directly after `taCanonicalize` (whose FSM the walker mirrors); publish hook after the TA checksum write in `serverPublish` (encodes `_taBuf[cur]`, the post-TACANON legacy wire copy — strip-config-independent); TDWS+restart pendings wired to BOTH SYNC points (forced/periodic SYNC + `client_request_sync`). Launch script `_run_srv_tadict.bat`.

- **G1 measured (local rig, attract, unmasked):** warm wire **0.29–0.43 Mbps** (647→610 B/frame), converging on the offline 0.357 figure from a 0.707 cold-start; dict 13.7 MB after ~10 min of attract scene churn. **enc = 1.25–1.36 ms/frame — MISSES the ≤200 µs target** (byte-wise FNV over ~144 KB + map churn); within the 16.7 ms frame budget so acceptable as a shadow leg, but optimize before any prod-default (XXH64 from the linked zstd, arena reserve) — tracked in §5.
- TDWS for a 224,290-block/12.77 MB dict = **1.60 MB on the wire** (one-shot, join-triggered only — same class as SYNC).

### Phase 2 — live byte-equality proof (PAGEGATE pattern) — ✅ EQUALITY GATE COMPLETE 2026-07-14

Done via a standalone Python WS client (`_bwlab/tadict_gate_live.py`) instead of the browser — zero web-file changes, fully automatable. It decodes BOTH wires (legacy ZCST chain exactly like the production client, and TDW1/TDWS per §2) and byte-compares per frameNum.

- **G2-a (from-start client): PASS — 3000/3000 frames, 0 mismatches, at 59 fps realtime.**
- **G2-b (mid-run joiner): PASS — 1200/1200 frames, 0 mismatches**, after receiving the 224,290-block TDWS and syncing at the paired streamStart. Join path proven live.
- **TDWS envelope regression (found live 2026-07-14, fixed same day):** the first TDWS rode the ZCST outer → browsers garbled (see the §2b lesson). Fixed to the own `TDWS` outer magic; **both gates re-run PASS on the fixed binary** (3000/3000 + joiner 1200/1200 with a 224,051-block/12.76 MB snapshot).
- **G2 remaining before cut-over:** an in-match leg including a triple super (attract-only so far), and a ≥10k-frame soak. The browser decoder itself moves to Phase 3 (it is the render cut-over's first half).
- **IN-MATCH LEG RUN (2026-07-14 night, supervised live play) — TWO-REGIME RESULT:** byte-equality held (30,000/30,000 PASS through six dict-cap epoch resets), but the masked wire is **two-regime**: static/repeat content 0.02–0.05 Mbps (dict froze at 744,125 blocks, new=0 B/frame for minutes) vs **active play 3–4.7 Mbps** (~30–78 KB/frame new masked blocks, cap cycling every 60–90 s). The offline corpora undersold live in-match churn ~20–50× in both mask modes — leading suspect: camera pan/zoom re-scales every sprite-vertex f32 corner per frame, so a whole-BLOCK dictionary cannot reuse vertex blocks even though positions are lattice-quantized. **Consequence: the whole-block dictionary (v1) does not beat the shipping wire for players in motion; the next design is SPLIT-STREAM coding** — dictionary for the finite fields (PCW/ISP/TSP/TCW headers, UV blocks: proven tiny vocabulary) + a separate lattice-indexed/delta stream for vertex corners. Measure offline first (extend `ta_dictwire_decode.py` with a corner-split variant on a NEW capture taken from live local play, since the existing corpora are unrepresentative of live churn).

### Phase 3 — render cut-over (`MAPLECAST_TADICT=2`, client `?wire=tadict`)

- Server: TDW1 gains flags bit2 — inner carries checksum + the **page section byte-identical to legacy** (reuse the existing page serializer), making TDW1 a complete standalone frame. STM2 trailer NOT included. Legacy + ZCS2 broadcasts continue unchanged for everyone else.
- Client `?wire=tadict`: render entirely from the TDW chain — reassembled full TA → existing `_injectFrame` full-frame path, pages → existing D.vram/pvrRegs apply. No STM2, no replica-live socket, no splice, no vframe pairing (TA and pages ride the same message).
- **G3-a (pixels):** standing frozen-frame A/B rig (RENDER-STATE.md §4: savestate freeze via control WS 7211 + capture + differ) — TDW client vs TA-mirror ground truth, **0 px diff**, on: idle stage, in-match bodies, hit-flash, and a super leg. Numbers, never impressions.
- **G3-b (wire):** WIREMON-measured live Mbps for a full match incl. triple super. Expectations to verify, not assume: steady TA-side ≈ 0.1–0.25 Mbps; **no 6 Mbps super spike from state** (the 84 % STM2 floor is gone by construction); cold super *pages* still spike — record the page-only spike as the Phase 5 target.
- **G3-c (latency):** existing E2E probe unchanged or better vs ZCS2 render (decode is a concat; expect ≤ ZCS2's worker cost).

### Phase 4 — late-join + lifecycle hardening

- TDWS re-broadcast on every SYNC; dictEpoch reset drill (force-cap in a test build → verify clean client resync); reconnect storm behavior on the local rig; seq-gap desync → resync-at-streamStart verified by fault injection.
- Relay (first relay touch, optional): shed legacy ZCST **and** ZCS2 for TDW subscribers, mirroring the existing ZCS2-subscriber shedding (`relay/src/fanout.rs:794-796`). Until then TDW browsers on prod simply also receive ZCS2 (dead weight ~1–3 Mbps, acceptable for testing).
- **G4:** joiner mid-match renders correctly within one SYNC period; no epoch desync over a 1-hour soak; dict size curve logged for the soak (validates the 89 s decay extrapolation).

### Phase 5 — texture-channel thinning (orthogonal, pre-measured)

The TA side is now ~0.1 Mbps; the wire is pages. Apply the already-measured stack, in order of proof: vcache-refs (0.788–1.026 Mbps whole-wire, REPORT.md §2-3), then body-bank page drop {82,83,88,89} for `bodytex=local` clients (carve proven 0-defect, 21,312/21,312 tiles). Palette pages always ship (skins). Gate: same frozen-frame pixel rig per step.

### Phase 6 — semantics wedge → database (separate arc, unblocked by this plan)

Client wire carries zero semantics. Server-side per-frame reader (gamestate.cpp `readAllDrawn` on the slot table `0x8C2895E0`/`0x8C287DE0`; fields per docs/MVC2-WIRE-GAP-ANALYSIS.md) → SurrealDB now, NATS fanout per docs/MATCH-DATA-PLATFORM.md later. GSTA/OBJS/OBJF sockets and the STM2 trailer retire for TDW clients; hitbox viewer/mods consume the DB/control-WS, not the render wire.

---

## 3b. PANEL VERDICT 2026-07-14 (four-expert audit, all claims cited in the session record)

**The user's recollection was correct:** "always-updating memory defeats the cache" killed or degraded FOUR prior attempts — Option-6 Pivot A whole-TA-buffer dedup measured **0% hit rate** ("per-frame timer + HUD + animation tick make every TA buffer unique even on stationary frames", docs/OPTION6-MASTER-PLAN.md); TA staging scratch was measured word-by-word (bytes +24..+31 of 64B stage verts flipping between two patterns on 22% of idle frames, _bwlab/STAGE-SHARE-REPORT.md §3) → fixed by TACANON (−17.3% real play); the DMA force-dirty page storm (56.9% byte-identical re-ships, _bwlab/REPORT.md §1) → fixed by PAGEGATE. TDW runs behind both fixes — same bet as Pivot A at 4,500× finer granularity, which is exactly why 0% became 95%+ reuse.

**Today's 95% non-body churn is NOT scratch** (cross-confirmed): the TA is regenerated into the SH4 store queues every frame (`bank12.loc_8c124812`); the stage is **92.8% of the buffer** (~2,400 verts ≈ 133 KB/frame, one 1,448-vert strip — STAGE-SHARE-REPORT §2) and every vertex is ftrv-projected through M1·M2 rebuilt per frame from camera state (re_kb/26b, un-projection proof 0.000 px) — camera still ⇒ bitwise-identical blocks (measured new=0 B/frame); camera pan/zoom ⇒ every stage vertex block content-new. These are parser-READ f32s: **no mask can ever touch them.** Corollaries: HUD meter fill is GEOMETRY (width from meter/144.0, finite quantized vocabulary — re_kb/57/58); Effect Poly 0x0CED0000 is **NOT per-frame scratch** (241 models loaded once, embedded literal TCWs — re_kb/64; this plan's §0 phrasing "sim-composited into Effect Poly scratch" is loose — the runtime-composed thing is the VRAM staging band + efxtmpl/rectab arenas); one bounded live-byte scratch leak exists beyond the mask (stale-palsel TCW carry, re_kb/62). The corpora lied because `cap_prod_play` is **provably idle** (timer frozen at 99, camera frozen, buttons 0xFFFF for 90 s — STAGE-SHARE-REPORT §0).

**Architecture consensus:** players-only TDW wire + client-local re-projected stage + state-drawn HUD is a **convergence of already-built pieces**, not a new design: stage-local is built + offline-gated (−49% measured on real play; camera model 4.3e-5 px; bake round-trip 3.6e-6 px; gaps: 1/17 stages baked, live camera-motion A/B never witnessed); synthetic HUD exists twice (native byte-matched per-quad vs the HUDQ oracle re_kb/58/59; browser approximate); **effects are the weakest pillar** (sprite machine A/B-rejected re_kb/74, 3D machine unimplemented, pre-bake hybrid unbuilt — effects' TA churn is their genuinely-moving vertex corners; their textures are page-channel). The old STAGESTRIP=0 verdict was wire-specific (static stage ≈ free under the zstd positional window) and DOES NOT TRANSFER to TDW.

**Riskiest unknown:** the decomposition of the 95% churn between stage / HUD / effects — nowhere in the record. **Agreed test order (each step can kill the plan before the next costs anything):**
1. Fresh **verified-active** live capture (`node _bwlab/cap_wss.mjs ws://127.0.0.1:7200 _bwlab/cap_liveplay.mirror.zcst 180` during real play; verify activity via the GSTA side-channel — the idle-corpus lesson) with movement, a super, a tag.
2. Offline churn decomposition: classify each NEW dictionary block by TCW/list class (body {82,83,88,89} / stage {9fc00,a0000} / HUD bands {0x9be00,0x80000,0x9de00..0x9e900} re_kb/67 / remainder=effects) + read-vs-unread byte attribution (mask-extension check) + stage-blocks-minus-positions re-lookup (camera-reprojection proof).
3. Offline players-only TDW sim → the real endgame Mbps number before any server code; corner-split (split-stream) sim on the same capture as the fallback design.
4. Live stage-local A/B on the local rig (existing code: MAPLECAST_STAGESTRIP=1 + ZCS2 bit3 camera) — closes the never-witnessed camera-motion item, frozen-frame pixel-gated.
5. Browser/native own-HUD leg (full OP+TR HUD strip + synthesis from GSTA fields; note: name letters ride TR para5 — a players-only filter drops them, the synthetic HUD must cover them).
6. Effects decision ONLY from #2's numbers (keep-on-wire vs pre-baked hybrid on the worst super).

### §3b RESULTS — verified-active capture MEASURED 2026-07-14 (steps 1–3 complete)

Capture: `_bwlab/cap_liveplay.mirror.zcst` (10,582 frames / 176.4 s, supervised real play, TACANON=2 + per-frame camera). Analysis: `_bwlab/ta_liveplay_analysis.py`.

- **[1] ACTIVE verified:** camera changed on 72.6 % of frame-pairs, 7,271 distinct matrices. (The idle-corpus mistake is structurally prevented now.)
- **[2] Churn decomposition (the riskiest unknown — CLOSED):** of 760.6 MB total new dictionary content: **stage 89.6 %**, effects/otherTex 3.9 %, **body 3.8 %**, HUD 2.7 %. Effects are NOT load-bearing; the stage utterly dominates.
- **[3] Whole-block dict on real play: 4.181 Mbps** (median 10.2 KB/frame) — v1 verdict confirmed on a proper corpus.
- **[4] PLAYERS-ONLY dict wire: 0.347 Mbps** (body+control blocks, dict 452 K blocks) — the endgame's TA-side cost, measured.
- **[5] Reprojection test — the prediction-coding go signal: 98.5 % of ALL new vertex blocks are position-only variants of known geometry.** Stage **100.0 %**, body **100.0 %**, HUD 93.6 %, otherTex 81.2 %. The churn is POSITIONS, not content — camera-compensated prediction/split-stream coding attacks ~all of it, and the "camera re-scales f32 corners" suspect is now CONFIRMED, not inferred.

**Decision consequence:** two viable endgames, both now measured-based: (a) players-only wire (0.347 Mbps TA) + local stage + state HUD + effects-on-wire (3.9 % churn = cheap); (b) full-scene prediction-residual coding (positions predicted server+client from the per-frame camera; residual ≈ 0 for stage/body) — keeps EVERYTHING byte-exact on the wire with no local-stage/HUD pillars at all. Next lab step: prediction-residual codec sim on this same capture to price (b); then choose.

### OPEN DEFECT (2026-07-14, user-reported vs ground-truth knowledge): local floor DISAPPEARS during supers
On the v4 players wire + local stage, the floor vanishes during super flashes — the user confirms the real game keeps it visible. Suspects, in test order: (1) stage.rs drops a whole STRIP when any vertex fails its sanity/w guard (browser culls per-TRIANGLE and keeps ground-plane tris with far off-screen corners — the 2026-07-11 floor-fix note); super cameras may push the floor's near corners past a guard. (2) The super dim overlay (untextured TR quad, now carried) may render with wrong blend over the locally-drawn floor. Diagnose with a same-instant A/B vs the full-mirror client during a super; fix per-triangle culling first — it is the known browser-parity gap.

### Endgame decision — (b) priced, (a) CHOSEN (2026-07-14, `_bwlab/ta_predict_sim.py`)

Prediction-residual sim on the same capture: **2.348 Mbps** (temporal-copy + camera-reprojected stage with 2 px fallback protocol). The camera model itself is validated — the proven column-major M1·M2 transform (tools/bake_stage_from_ta.py, round-trip 1e-13 px) predicts well-paired stage verts at **0.23 px median cross-frame** — but two engineering gaps keep (b) expensive in simulation: (i) occurrence-index instance pairing drifts when strips cull under camera motion (only 52.5 % inliers; 24.5 % of stage verts fell back to raw), and (ii) sub-pixel-accurate prediction is still not BIT-exact, so XOR residuals are mantissa noise, not zeros (69.6 % zero bytes). Production could close (ii) by running flycast's exact ftrv on both ends against RAM-true world verts, and (i) with content-based correspondence — but neither is provable from this capture, and (a) needs neither.

**The closing insight: (a) sidesteps (b)'s hard problem entirely.** The same proven reprojection math that must be *bit-exact* for (b) only has to be *visually exact* for (a) — the local-stage client RENDERS the stage from baked world geometry + the per-frame camera (0.23 px median error is invisible as pixels, unacceptable as bytes). Byte-exactness is kept only where it is cheap and proven: the players, at a measured **0.347 Mbps**. **DECISION: proceed with endgame (a)** — players-only TDW wire + client-local re-projected stage + state-drawn HUD + effects/pages on wire. Build order = §3b steps 4→6 (live stage-local A/B first).

## 4. Touch-point table

| # | file | what changes | phase |
|---|---|---|---|
| 1 | `_bwlab/ta_dictwire_decode.py` (new) | spec-exact encoder+decoder, gates G0-A/B/C | 0 |
| 2 | `core/network/maplecast_mirror.cpp` | TADICT encoder + dict + TDWS send; hook after `:3358`; stats; env plumbing | 1,3 |
| 3 | `web/webgpu-test.html` (worker `:418-538` + fallback) | TDWS/TDW1 decode, `?tadictgate=1`, `?wire=tadict` render path | 2,3 |
| 4 | `relay/src/fanout.rs` | (optional) TDW-subscriber shedding | 4 |
| 5 | native client (`maplecast_mirror.cpp` GSTA client path `:6958-6997` area) | TDW decode — after browser is proven | post-4 |
| 6 | `core/network/gamestate.cpp` + DB bridge | semantics wedge | 6 |

Four-parser rule status: TDW1/TDWS are **new** message types; no existing parser's input changes. Parsers gaining TDW support, in order: server encoder (2), browser (3), native client (5). king.html WASM + emulator.html stay legacy until explicitly migrated.

Env vars: `MAPLECAST_TADICT` (0/1 shadow/2 full-frame), `MAPLECAST_TADICT_MAXBLOCKS` (1048576), `MAPLECAST_TADICT_MAXMB` (64), `MAPLECAST_TADICT_RESET` (0). Interacts with: `MAPLECAST_TACANON` (sets bit1; encoder is agnostic), `MAPLECAST_ZSTREAM` (independent stream), STAGESTRIP/CHARSTRIP (ignored by the TDW chain — it encodes the unstripped legacy copy).

## 5. Known-unproven list (kept honest)

1. **Dict growth beyond 89 s** — designed caps + epoch reset; measured only to 89 s (masked decay 63/s; unmasked dict is ~7× larger, 12.85 MB/89 s). G4 soak closes this; it is the main open question for the mask-OFF default.
2. ~~Unmasked-variant cost~~ — **MEASURED 2026-07-14 (G0-C): 0.357/0.653 Mbps.** Mask demoted to optional optimization; Phases 1–3 run mask OFF.
3. **TACANON pixel-neutrality end-to-end** — now OFF the critical path (mask-OFF default is bit-identical). Only relevant when the ≈3× mask optimization is attempted; gate it then with the frozen-frame pixel rig.
4. **Server CPU** — MEASURED 2026-07-14: **1.25–1.36 ms/frame** (not the estimated ≤200 µs). Shadow-acceptable (budget 16.7 ms) but must be optimized before prod-default: switch FNV64→XXH64 (already linked with zstd), pre-reserve arena/refs, skip re-hash of unchanged prefix (92 % of the buffer is frame-to-frame identical — a prefix/suffix trim before the walk should cut most of it).
5. **Super page spike magnitude on the TDW wire** — G3-b records it; it becomes the Phase 5 target, not a TDW problem.

## 6. Test commands

```bash
# Reproduce the measured basis (both corpora):
python c:\Users\trist\projects\maplecast-flycast\_bwlab\ta_dictwire_sim.py c:\Users\trist\projects\maplecast-flycast\_bwlab\cap_prod_play.mirror.zcst
python c:\Users\trist\projects\maplecast-flycast\_bwlab\ta_dictwire_sim.py c:\Users\trist\projects\maplecast-flycast\_bwlab\cap.mirror.zcst

# Phase 0 gate (once _bwlab/ta_dictwire_decode.py lands):
python c:\Users\trist\projects\maplecast-flycast\_bwlab\ta_dictwire_decode.py --gate c:\Users\trist\projects\maplecast-flycast\_bwlab\cap_prod_play.mirror.zcst
python c:\Users\trist\projects\maplecast-flycast\_bwlab\ta_dictwire_decode.py --gate c:\Users\trist\projects\maplecast-flycast\_bwlab\cap.mirror.zcst

# Live equality gate (local rig, fully isolated from prod):
#   1. start the gate client FIRST (it retry-connects):
python c:\Users\trist\projects\maplecast-flycast\_bwlab\tadict_gate_live.py --frames 3000
#   2. then launch the server:
c:\Users\trist\projects\maplecast-flycast\_run_srv_tadict.bat
#   server stats: findstr TADICT c:\Users\trist\projects\maplecast-flycast\_srv_tadict.log
#   a SECOND gate run while the server is up exercises the mid-run joiner (TDWS) path.
```
