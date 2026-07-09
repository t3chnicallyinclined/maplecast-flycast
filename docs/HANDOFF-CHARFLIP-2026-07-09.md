# HANDOFF — Character Flip / TA-Wire v2 (2026-07-09)

> Complete session handover: the character flip (strip chars from the ZCS2 wire, redraw
> client-side) went from plan (render-state/09) to LIVE ON PROD in one arc, plus the VCACHE
> join-gap fix. Everything below is committed on `feat/render-replica-live`
> (HEAD `140e603c3` at write time) and deployed unless marked otherwise.
> Read `docs/RENDER-STATE.md` first if you're new; this doc is the delta on top of it.

## TL;DR — where things stand

- **Prod (149.28.44.118)** runs the char-strip live: `MAPLECAST_CHARSTRIP=1` in
  `/etc/maplecast/headless.env` (alongside ZSTREAM/SOA/TACANON/STAGESTRIP/PAGEGATE/VCACHE).
  In-match ZCS2 wire measured **0.21–0.37 Mbps** on the rig with both strips on.
- **Client** (`web/webgpu-test.html`): characters draw from `render_frame.wasm` fed by the
  `/replica-live` state socket, spliced back into the wire frame at the engine's exact TR
  positions. `?bodylocal=1` pre-seeds the feed (recommended); auto-starts on the first
  char-stripped frame otherwise.
- **Remaining visible defect**: per-tile "confetti" on characters during action bursts.
  ROOT CAUSE (measured, see §5): live two-socket ARRIVAL-ORDER race — wire frame can beat
  its matching state frame; one frame of geometry then samples the other staging bank's
  tiles (the engine double-buffers sprite staging per frame). The fix arc is IN FLIGHT:
  `?bodytex=local` (render_frame geometry + body_decoder textures from local disc GFX),
  which makes the race structurally impossible AND is the prerequisite for dropping the
  char texture pages (the measured **70% of fight bytes**).

## 1. Wire format changes (ZCS2 envelope, all live on prod)

Header: `'ZCS2'(4) + epoch(1) + flags(1) + innerSize(4) + [blocks…] + zstd chunk`.
Flags (bit → meaning → payload appended in this order after the 10B header):

| bit | value | meaning | payload |
|-----|-------|---------|---------|
| 0 | 1 | stream start (epoch begin) | — |
| 1 | 2 | SoA delta section | — |
| 2 | 4 | STAGE-stripped frame | — |
| 3 | 8 | camera block present | 132B: stage_id u32 + M2 16f + M1 16f |
| 4 | 16 | CHAR-stripped frame | — |
| 5 | 32 | vframe stamp | u32 game frame counter (0x8C3496B0) |
| 6 | 64 | TR run-order descriptor | u8 nRuns + nRuns × {u8 cls, u16 count} |
| 7 | 128 | per-epoch seq (2026-07-09) | u16 msg counter, resets 0 at stream-start. Deterministic mid-epoch drop detection: a lost msg leaves the epoch byte unchanged, so it previously surfaced only as zstd garbage in soaInverse. Client desyncs on the first gap. Parsers: webgpu-test (main+worker), zcs2-test, `tools/render-replica-poc/zcs2_seq_gate.mjs` (synthetic gate, run anywhere) |

- **Strip set** = TCW addr 4K blocks `{0x82,0x83,0x88,0x89}` (TR-list para5 only; para4
  NEVER stripped). Blocks 84/85/86/8a (assists/effect classes render_frame doesn't emit)
  stay on the wire. Env `MAPLECAST_CHARSTRIP=1` (block set) or `lo-hi` hex for a range;
  `measure` = histogram-only mode (CSV `_charstrip_hist.csv`).
- **Sticky flags**: bits 2/4 (and camera/vframe presence) key on the in-match hysteresis
  gate, NOT per-frame stripDone — `taSize==0` skip-frames used to flap them and force
  client chain-resets every render-skip.
- **Descriptor rules (DERIVED from the renderer, don't re-learn these)**:
  - cls: 0=stripped char sprite run, 1=kept TR sprite, 2=kept TR para4 poly. Counts are
    PARAMS, recorded at each param's **FIRST VERTEX** (flycast/ta-parser drop zero-vertex
    params — the engine emits a vertexless leading TR poly param every frame).
  - Client splice keys on **param identity** (`ta-parser.mjs?v=2` stamps `.param`;
    endStrip clones inherit it) because one param can parse into N strip-polys.
- **`{"type":"request_sync"}`** (mirror WS text, rate-limited 2s): any viewer can request a
  fresh SYNC broadcast (= epoch reset + VCACHE reseed). The page sends it on connect and on
  VCACHE ref-miss. This closed the VCACHE join gap (relay serves CACHED SYNC without
  telling the server; joiners' hash caches start empty → silently stale pages).
- **VCACHE reseed** is join/SYNC-driven (`_vcacheReseedPending` at both SYNC sites) + a
  5-min fallback (`VCACHE_RESEED_FRAMES=18000`). The old 10s timer was the super-spike bug.
- **VCACHE sentinel is now in ALL parsers** (parser-lockstep rule): frame-decoder.mjs,
  relay protocol.rs, native client (BOTH clientReceive + SHM paths in maplecast_mirror.cpp),
  both wasm bridges. Note: prod king.html does not parse the TA wire at all.
- **Relay fanout rebuilt (2026-07-09)**: per-client SendQueue with type-aware backpressure —
  ZCS2/state/audio NEVER dropped, legacy deltas drop-oldest over an 8MB budget (then the relay
  auto-sends request_sync on the client's behalf), a SYNC entering the queue evicts every queued
  legacy delta + older SYNC. Targeted SYNC delivery: clients that speak request_sync get ONLY
  snapshots they asked for (another client's join can't stall your socket). Zero-copy Bytes end
  to end; broadcast channel 16→1024. Root cause it kills: tokio broadcast `Lagged` silently
  dropped ~0.25s of messages whenever a multi-MB SYNC crossed a slow socket, corrupting that
  client's ZCS2 epoch — the "perfect until the sync hits, then background gone + garble" loop.
  Policy pinned by unit tests in fanout.rs (`send_queue_tests`).

## 2. Client architecture (webgpu-test.html)

- **Body feed**: `/replica-live` WS → msg1 = ZCST-zstd MCRR prefix (seeds 16MB `BODY.ram`),
  then FRMx per frame (dyn regions + on-change GFX tail; **frames can be ZCST-wrapped
  too** — the silent-drop bug). `render_frame.wasm` re-emits the body TA; output filtered
  to the strip block set (out-of-set quads stay on the wire; drawing them = double-draw).
- **Pairing**: wire frame stamped S pairs with body ring entry **S−1** (capture-proven:
  the TA lags the state counter by one; staging banks alternate per frame so ±1 = foreign
  tiles). 16-deep vframe-keyed ring; panel shows `pair N/M`.
- **Merge**: `_bodyMerge(g, wantVf, ord)` splices body polys into `g.translucent` at the
  descriptor positions; self-verifying (distinct-param count match, else append fallback +
  `spl` counter). Runs after `_stageMerge`, before fillBGP skip.
- **Renderer facts** (read from source, don't re-derive): page defaults
  `singlePass + noSort` → passes rebuilt to full lengths, full-list SUBMISSION ORDER is the
  compositing rule; z low-byte residuals in the body TA are inert in this mode.
  `pp.texObj` on a poly entry bypasses TextureManager (added for the sprite bridge).
- **Experimental `?bodysrc=sprite`** (default `wasm`): sprite-machine draw-list bridge.
  NOT the drawer — measured 20px mean divergence (see §4). Kept for A/B.

## 3. Standing gates & tools (all in `_bwlab/`, run from `tools/render-replica-poc/`)

| tool | what it proves |
|------|----------------|
| `zcs2_verify.mjs ws://127.0.0.1:7200 30` | byte gate: ZCST/ZCS2 pairing + stripped-chain md5 vs a JS strip twin (block set + descriptor offsets current) |
| `cap_userplay.mjs [secs]` | dual-socket raw capture (wire + replica) → `_cap_userplay/cap.bin` (ROM-derived, never commit) |
| `garble_diff.mjs` | frame-by-frame: engine char quads (legacy chain ground truth) vs render_frame re-emit; found the off-by-one + slot coverage |
| `order_gate.mjs` | strip+descriptor+splice (the page's exact algorithm) vs engine TR sequence; proves the splice law offline |
| `body_feed_smoke.mjs ws://127.0.0.1:7212` | replica feed + wasm + TAParser pipeline smoke |
| `sprite_bridge_smoke.mjs` | sprite bridge structure + position-match vs engine quads |
| `decompose_live.mjs wss://nobd.net/ws 120` | wire composition split (TA vs pages full/ref) |
| `ta_para5_scan.mjs` | live TR-para5 TCW census (strip-set evidence) |

Local rig: `_run_srv_charstrip.bat` (CHARSTRIP=1 + full v2 env, autoload match,
log `_srv_charstrip.log`). Kill flycast.exe before rebuilding (LNK1104).

## 4. Measured facts ledger (sources in the commits)

- Fight-wire composition (120s prod): **71.6% first-time texture pages, 27.6% TA
  geometry**, refs+headers <1%. VCACHE dedup works (744MB saved/15min server-side).
- Strip set evidence: ALL in-match TR-para5 TCWs ∈ blocks 82–8A; all pcw `a2000009`;
  nothing above y=150; FONT/HUD textures outside the staging area.
- render_frame vs engine (capture): geometry byte-exact at pairing S−1 except benign
  fields (offset-color byte param+20..23, EOS pcw bit28, z float low-bytes). Coverage:
  blocks 82/88 99.6%, 83/89 97%, 84/85/86/8a 0–21% (assists/effects — hence kept on wire).
  INTERNAL emit order diverges from engine in 177/1359 covered frames (next walker lever).
- Engine TR order interleaves S and K runs (`P1 S43 K5 S16 K23 S27 P12`) — why append-at-
  end garbled compositing and the descriptor exists.
- Sprite bridge (`?bodysrc=sprite`): structural PASS, positions diverge — windowed mean
  |d| 20px; PL2A feetDy +21.8px (12% ≤8px) vs PL17 +2.2px (25.7%). PROVENANCE: the 5/5
  byte-exact closure (189544592) validated the **body_decoder texel layer**, not the
  sprite-client draw-list path. Two pipelines, different validation status.
- Replica feed integrity (capture): 0 vframe gaps, 99.98% S−1 availability — the live
  confetti is arrival ORDER, not loss.
- MVC2 wire order per frame: ZCST → GSTA → OBJS (state 1 frame stale at TA-decode time).

## 5. Open items, ranked

1. **IN FLIGHT — `?bodytex=local`** (expert agent running at handoff): body_decoder
   textures (local disc GFX via applyLocalGfx, same RAM image as the geometry) attached to
   render_frame quads via `pp.texObj`. Kills the confetti race structurally. Gate:
   per-quad texel match rate vs wire staging bytes on the user-match capture
   (`_bwlab/bodytex_gate.mjs` expected). THEN: server page-gates the char staging VRAM
   blocks — the 70% win.
2. render_frame walker emit-order fidelity (177/1359 frames) + the 3% coverage leak in
   blocks 83/89 during heavy action.
3. Sprite-machine draw-list anchors (PL2A-class +21.8px) + satellite strip-set attribution
   + live-palette recolor in the pvr2 leg — promotes `?bodysrc=sprite` when closed.
4. Blocks 84/85/86/8a stay on wire (bandwidth give-back) until a drawer covers assists.
5. Black stage (parked by user decision): suspect = stage-client.mjs v2 getTexture wrap
   shim from the ModNao port; strip stays ON regardless.
6. zcs2_verify pair-gate shows `paired 0` under VCACHE in some runs (its legacy parser
   pairing predates the sentinel) — tool gap only; strip-chain gate unaffected.

## 6. Resuming from another machine

```bash
git clone https://github.com/t3chnicallyinclined/maplecast-flycast.git
cd maplecast-flycast && git checkout feat/render-replica-live
```
- Windows headless build: `_build_headless.bat` → `build-headless-win\flycast.exe`
  (ROM expected at `C:\roms\roms\mvc2.gdi` by the rig bats — adjust paths).
- Node tools run from `tools/render-replica-poc/` (has `ws` in node_modules).
- Captures/atlases are gitignored (ROM-derived) — re-capture with `cap_userplay.mjs`.
- Prod access: `root@149.28.44.118` (key auth). Flycast source `/opt/maplecast/src`
  (branch `prod-zcs2-shadow`, patched via `git apply` of local diffs — NEVER checkout
  branch files there). Deploy pattern per `reference_prod_web_deploy` memory: file backup
  + surgical scp + md5 verify both ends. Binary: build in `/opt/maplecast/src/build-headless`,
  `install -m755` to `/usr/local/bin/flycast`, `systemctl restart maplecast-headless`.
- Prod rollback inventory (in `/usr/local/bin/`): `flycast.bak-reqsync-*`, `-charstrip-*`,
  `-vfstamp-*`, `-garblefix-*`, `-ordfix-*`, `-fvfix-*`; web backups `*.bak-charflip`,
  `*.bak-reqsync`, `*.bak-spritebridge` in `/var/www/maplecast/`. Char strip off =
  remove `MAPLECAST_CHARSTRIP=1` from `/etc/maplecast/headless.env` + restart (env backup
  `headless.env.bak-charstrip`).

## 7. Commit trail (this arc, oldest first)

`6ebee5506` VCACHE join-driven reseed · `b85e43c5e` request_sync + sentinel in all parsers ·
`b56332d10` CHARSTRIP A+B · `8f5108654` A/B in-match gate · `e18575791` vframe stamp ·
`0770877a2` off-by-one + block set + sticky flags · `bc73e86a8` order descriptor ·
`cf63f32db` splice law (param identity + first vertex) · `6c3374a1a` sprite bridge (exp) ·
`140e603c3` predict WIP committed-as-found.
