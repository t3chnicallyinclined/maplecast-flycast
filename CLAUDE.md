# CLAUDE.md — Rules and Context for AI Assistants on MapleCast

## WHERE PRODUCTION IS (read before any deploy)

**Prod = rise3, `15.204.141.58`** (OVH RISE, North America; hostname `ns1012691`) since the
2026-09-01 nobd.net cutover. `ssh -i ~/.ssh/ovh_maplecast ubuntu@15.204.141.58` (passwordless
sudo; **not** `root@`). Live there: unit `maplecast-flycast.service` running
`/home/ubuntu/src/maplecast-flycast/build-headless/flycast`, env `/etc/maplecast/headless.env`,
web root `/var/www/maplecast`, ports 7200/7201/7203 public. `maplecast-headless.service` is
**masked** on rise3 - the unit name changed.

Retired boxes - never deploy to these: `66.55.128.93` (decommissioned 2026-04-15),
`149.28.44.118` (Vultr `flycast-inputserver-nyc`; served nobd.net until 2026-09-01, still
powered on, no longer the origin - DNS failback lever is forgily-creations
`scripts/nobd_dns_flip.sh vultr`), `65.109.77.178` (dev0ps, Hetzner - forgily rollback standby
only, runs no maplecast).

**Server architecture is documented in ONE place:** forgily-creations
`plans/rise3_handover.md` section 0 "CURRENT ARCHITECTURE" (synced copy `~/HANDOVER.md` on
rise3). Do not restate those facts in this repo - restated facts are how they drift.

## CRITICAL DEPLOYMENT RULES - READ FIRST

### NEVER deploy to production without a backup
- **ALWAYS** use `./deploy/scripts/deploy-web.sh` for web files — it creates a timestamped backup
- **ALWAYS** use `./deploy/scripts/deploy-headless.sh` for the flycast server binary
- **NEVER** use raw `scp` to overwrite production files
- **NEVER** assume git matches production — production may be AHEAD of git
- **ALWAYS** verify with `md5sum` before and after deploying

### Deploy workflow
```bash
# Web files (king.html, JS modules):
./deploy/scripts/deploy-web.sh ubuntu@15.204.141.58     # rise3 (needs sudo for /var/www)

# Headless flycast server:
./deploy/scripts/deploy-headless.sh ubuntu@15.204.141.58   # see caveat in the script header

# Rollback (printed by deploy script):
ssh ubuntu@15.204.141.58 'sudo rm -rf /var/www/maplecast && sudo mv /var/www/maplecast-backup-YYYYMMDD-HHMMSS /var/www/maplecast'
```

### If production files were edited directly
Sync production → git BEFORE making any local changes:
```bash
scp ubuntu@15.204.141.58:/var/www/maplecast/king.html web/king.html
scp ubuntu@15.204.141.58:/var/www/maplecast/js/*.mjs web/js/
git add web/ && git commit -m "sync: pull production web files from VPS"
```

### NEVER commit ROMs or disc images
- MVC2 and all Dreamcast/Naomi ROMs are **copyrighted**. Committing one is a DMCA event and pollutes git history permanently — `git filter-repo` across the full history is the only fix.
- ROM paths live **outside** the repo: production is `/opt/maplecast/roms/mvc2.gdi`, dev ROMs stay wherever the user keeps them locally. NEVER copy a ROM into the working tree "just to test."
- `.gitignore` already blocks `*.gdi *.cdi *.chd *.iso *.cue *.nrg *.mdf *.mds *.ccd` and `roms/ ROMs/ rom/` folders. If you need a new ROM-adjacent path, add it to `.gitignore` BEFORE placing any file there.
- Before `git add -A` or `git add .`, run `git status` and eyeball it. If uncertain whether a file is ROM-derived (sprite rips, texture dumps, palette extracts, audio samples), the answer is **don't commit**.
- Exception: `tests/files/test_gdis/` contains upstream flycast parser fixtures (dummy bytes, predates our rule). Leave it alone — don't add new files there.

### NEVER commit savestates, VMU, NVRAM, or cartridge saves
- Savestates are derived from ROM execution — treat them like ROMs. Flycast formats: `*.state` (savestate), `*.sav` (cartridge), `*.eeprom` (JVS/Naomi EEPROM), `*.nvmem`/`*.nvmem2` (NVRAM flash). All are gitignored.
- Exception: `resources/flash/*.nvmem.zip` are upstream flycast BIOS flash defaults (public, shipped by flycast itself). Leave tracked.
- **Don't commit symlinks into `savestates/` or `roms/`.** Git stores the target path as a string, which bakes an absolute host path (`/home/tris/...`) into history forever. Useless to anyone else, and signals sloppiness. An incident on 2026-04-14 tracked `web/mvc2.state` as a symlink for ~6 months before removal.
- In-RAM sync snapshots (state-sync / replica client) are ephemeral by design — they should never hit disk inside the repo.

### What happened on 2026-04-10
An AI assistant scp'd the git version of king.html to production, overwriting the real production version which had SurrealDB live subscriptions, live arcade panel, player cards, and other features not yet in git. The site broke for users. Recovery required searching all commits. **This MUST NOT happen again.**

---

## Architecture Overview

> **Domain topology (2026-07-14 migration).** The game front end + this node's
> streams moved to **play.nobd.net**. **nobd.net** is now the NOBD-ZERO marketing
> board (Next.js standalone) that ALSO proxies the game control plane on the same
> box — `/hub/api`, `/ws`, `/play`, `/audio`, `/db`, `/replica-live`, ... — so the
> distributed node network + native/desktop clients keep working. Every `*.html`
> game page 301s from nobd.net → play.nobd.net; `zero.nobd.net` 301s → nobd.net.
> The hub URL deliberately stays `nobd.net/hub/api`. Details: `docs/DEPLOYMENT.md`
> (DNS section). Same prod box (**rise3** `15.204.141.58` since 2026-09-01); routing is
> nginx `server_name` on rise3's host nginx at **:8081**, behind the k8s ingress that owns
> 80/443 there.

MapleCast turns Flycast (Dreamcast emulator) into a game streaming server. One MVC2 instance runs on a single 2-vCPU VPS with NO GPU. ~322 MB RAM, ~12% CPU. 60fps to `wss://play.nobd.net/ws`.

**As of 2026-04-14 there's a fifth pillar — the Distributed Input Server Network.** Anyone can run an "input server" (flycast headless + the relay binary). A central hub on nobd.net does discovery + matchmaking. Native clients auto-pick the lowest-RTT server via UDP probing. Browsers connect through nginx-TLS-fronted endpoints. The hub is **NEVER** in the gameplay hot path. **Read `docs/ARCHITECTURE.md` "Pillar 5: Distributed Input Server Network" before touching any hub/node/native-client code.** Companion vision doc: `docs/COMPETITIVE-CLIENT.md`.

### Three flycast build variants — ALWAYS disambiguate
| Variant | Build | What it is |
|---------|-------|------------|
| **flycast server** | `cmake -DMAPLECAST_HEADLESS=ON` | Headless, authoritative, on VPS. No GPU/SDL/X11. ~26 MB stripped. |
| **flycast client** | `MAPLECAST_MIRROR_CLIENT=1` env var | Native TA stream viewer + UDP input sink. No local SH4. |
| **flycast wasm** | `packages/renderer/build.sh` | Browser renderer. 831 KB .wasm. No ROM, no CPU, TA parser only. |

### System topology
```
nobd.net VPS:
  flycast headless (:7210 loopback) → relay (:7201) → nginx (:443 /ws) → browsers
  input server (:7100/udp) ← NOBD sticks, browser gamepads, native clients
  SurrealDB (:8000 loopback) → nginx /db proxy
  control WS (:7211 loopback only)
```

### Key ports
| Port | Service | Access |
|------|---------|--------|
| 7100/udp | Input server | Public |
| 7101/udp | Tape publisher | Public |
| 7102/tcp | State sync | Public |
| 7200/tcp | Mirror WS (flycast direct) | Loopback → relay |
| 7201/tcp | Relay WS | Public via nginx /ws |
| 7211/tcp | Control WS | Loopback ONLY |
| 8000/tcp | SurrealDB | Loopback, proxied via /db |

### Production paths
| What | Where |
|------|-------|
| Flycast binary | `/usr/local/bin/flycast` |
| ROM | `/opt/maplecast/roms/mvc2.gdi` |
| Systemd unit | `maplecast-headless.service` |
| Web files | `/var/www/maplecast/` |
| SurrealDB data | `/var/lib/surrealdb/data.db` |
| Relay binary | deployed via `relay/deploy.sh` |

---

## Wire Format — TA Mirror Stream

### Compressed envelope
`ZCST(4 bytes) + uncompressedSize(u32 LE) + zstd_blob(N)`

Magic: `0x5A 0x43 0x53 0x54` ("ZCST" ASCII). **THE CRITICAL BYTE-ORDER LANDMINE** — wire is LE bytes, load as `0x5453435A`.

### Delta frame (uncompressed)
```
frameSize(4) + frameNum(4) + pvr_snapshot[16×4](64) +
taSize(4) + deltaPayloadSize(4) + [TA delta data] +
checksum(4) + dirtyPageCount(4) +
[regionId(1) + pageIdx(4) + pageData(4096)] × N
```

### SYNC frame
```
"SYNC"(4) + vramSize(4) + vram[8MB] + pvrSize(4) + pvr[32KB]
```

### Region IDs for dirty pages
- 1 = VRAM (textures)
- 3 = PVR registers (palette, fog, hardware state)

### Six regression bugs — NEVER reintroduce
1. `DecodedFrame::pages` must be `std::vector`, NOT fixed array (scene transitions ship 100-200+ pages)
2. TA delta `runLen` MUST clamp to 65535 BEFORE gap-merge
3. Diff loop snapshots live→shadow ONCE per dirty page
4. `_decoded` overwrite race — merge previous frame's pages into new frame
5. PVR atomic snapshot at top of `serverPublish()`
6. `_decodedMtx` mutex on producer/consumer

### Always update all parsers together
Changes to the wire format must update ALL FOUR parsers:
- `maplecast_mirror.cpp` (C++ server/client)
- `packages/renderer/src/wasm_bridge.cpp` (king.html WASM)
- `core/network/maplecast_wasm_bridge.cpp` (emulator.html)
- `relay/src/protocol.rs` (Rust relay)

---

## MVC2 Memory Map — Key Addresses

### Character structs (all in page 616, 0x8C268000)
| Slot | Base Address | Stride |
|------|-------------|--------|
| P1C1 | `0x8C268340` | 0x5A4 |
| P2C1 | `0x8C2688E4` | |
| P1C2 | `0x8C268E88` | |
| P2C2 | `0x8C26942C` | |
| P1C3 | `0x8C2699D0` | |
| P2C3 | `0x8C269F74` | |

### Per-character struct offsets
```
+0x000  active (u8)          +0x001  character_id (u8)
+0x034  pos_x (float)        +0x038  pos_y (float)
+0x0E0  screen_x (float)     +0x0E4  screen_y (float)
+0x110  facing (u8)          +0x142  anim_timer (u16)
+0x144  sprite_id (u16)      +0x1D0  animation_state (u16)
+0x420  health (u8)          +0x424  red_health (u8)
+0x52D  palette (u8)
```

### Global game state (page 649, 0x8C289000)
```
0x8C289621  match_sub_state
0x8C289624  in_match flag
0x8C28962B  round_counter
0x8C289630  game timer
0x8C289638  stage_id
0x8C289646  p1_meter_fill (u16)
0x8C289670  p1_combo (u16)
0x8C3496B0  frame_counter (u32)
```

---

## MVC2 Skin System

### PVR palette bank formula
```
bank = 16 × (char_pair + 1) + (8 × player_side)
```

| Slot | Bank | PVR Entry Range |
|------|------|-----------------|
| P1C1 | 16 | 256-271 |
| P2C1 | 24 | 384-399 |
| P1C2 | 32 | 512-527 |
| P2C2 | 40 | 640-655 |
| P1C3 | 48 | 768-783 |
| P2C3 | 56 | 896-911 |

### Palette format
ARGB4444 little-endian. 16 colors per palette. 32 bytes per palette. Index 0 = transparent.

### SurrealDB skin storage
- Namespace: `maplecast`, Database: `mvc2`
- Table: `skin` — 5,202 community skins with author credits
- Fields: `char_id, char_name, char_dir, author, hash, palette_hex, colors[], credit, source`
- Source: https://github.com/karttoon/mvc2-skins

### How skins work on headless server
- Headless PVR palette RAM is always empty (norend mode)
- `pvr_WriteReg` writes populate otherwise-empty entries
- PVR dirty page diff (region 3) ships palette to all viewers
- `applyPaletteOverrides()` runs every frame in `serverPublish` before diff scan
- Palette overrides upsert by startIndex (no duplicates)

---

## Input Latch Architecture

Two policies per slot, runtime-toggled:

**LatencyFirst (default):** Reads latest packet atomically. Zero added latency. Best for NOBD sticks (12 KHz).

**ConsistencyFirst (opt-in):** Drains accumulator preserving every button edge. Guard window (500µs default) defers near-boundary arrivals. Adds ≤1 frame latency. Best for browser gamepads (60-250 Hz).

Atomic layout: `[buttons:16][lt:8][rt:8][seq:32]` — 64-bit packed, single atomic store.

---

## Native Mirror Client (flycast client)

### How to run
```bash
MAPLECAST_MIRROR_CLIENT=1 \
MAPLECAST_SERVER_HOST=15.204.141.58 \
MAPLECAST_SERVER_PORT=7201 \
./build/flycast
```

### Architecture
- TA stream from relay → native OpenGL renderer (no SH4)
- Input sink: SDL `ButtonListener` → UDP `sendto` to server:7100
- Analog triggers: 120Hz poll thread reads `lt[]/rt[]` directly
- Client-side palette override: write to local PALETTE_RAM before Render
- E2E latency: 10ms avg (0.6 frames over public internet)

### HTML settings dashboard
- `web/client-settings.html` connects to `ws://localhost:7211`
- Config get/set, telemetry, controller mapping, E2E latency probe
- Opened via Back/Select button on gamepad or gear icon click

---

## MVC2-AI Training Dataset Exporter (input-side ML)

The **`mvc2-ai`** repo (sibling `../mvc2-ai`) is the input/decision-side ML layer — models that
GENERATE inputs (behavior cloning now; RL later). Clean split: **ML emits inputs; the exact sim
(this repo) processes them.** It consumes a per-frame dataset the headless server produces.
Design: `docs/DATASET-EXPORTER-DESIGN.md`; field catalog: `../mvc2-ai/docs/DATASET-FIELDS.md`.

### The exporter (server side, this repo)
- **`serverPublish` state tap** (`maplecast_mirror.cpp`, before the no-subscriber early-return)
  reads guest RAM each frame → a **`.mctele`** stream: one 8964-byte blob/frame of 6 char structs
  (`0x8C268340`), globals (`0x8C2895E0`), camera (`0x8C26A520`), **both players' latched
  `Input_DEC` (`0x8C2681DC`)**, and `frame_counter` (`0x8C3496B0`). The `.mctele` is
  SELF-CONTAINED (state + inputs) — no `.mcrec` needed for the dataset.
- **Read-only, determinism-PROVEN** (`MAPLECAST_DETLOG` gate: 1116/1116 game-state hashes
  identical tap on vs off), ~0 CPU, on the publish thread (off the input→sim latency path).
- **Gated on `in_match` (`0x8C289624`)** — records only real match frames, never idle.
- Writer + runtime toggle in `replay_writer.{h,cpp}`: `setDatasetRecording(on,dir)`,
  `datasetRecordingActive()`, `beginStateStream`/`appendState`/`closeStateStream`.
- **Gotcha (in DATASET-FIELDS.md):** the `.mcrec` input log writes a neutral-duplicate entry
  per frame — per-frame input must be the UNION of presses, not last-write-wins. `Input_DEC`
  in the `.mctele` avoids this (it's the clean latched value).

### Admin toggle — OFF by default (no continuous recording)
- Control-WS command **`dataset_record {on[,dir]}`** (`maplecast_control_ws.cpp`) flips the
  runtime flag; the tap responds live. The **server-side admin** surface is the **Mod Command
  Center** (`mod_bridge.mjs`, systemd `maplecast-modbridge`, node :9099 behind nginx
  `/modcmd/`, key-gated): its `dataset_record` action forwards to the loopback control WS
  (:7211). A **"Dataset Recording"** toggle lives in that panel (NOT in the client's
  `client-settings.html` — recording is a server admin action, since the authoritative server
  owns both inputs). OFF unless an operator flips it per training session. No `dir` → recorder
  uses `recordings/` (prod `WorkingDirectory=/opt/maplecast` → `/opt/maplecast/recordings`).

### R2 offload + training flow
- `deploy/scripts/r2-sync-recordings.sh` (cron on prod, root) uploads `.mctele` to Cloudflare
  R2 bucket **`mvc2-dataset`** (rclone remote `r2`, endpoint
  `da520a87698c3b96f1a0652b01a039c9.r2.cloudflarestorage.com`, **`no_check_bucket=true`** — a
  bucket-scoped token can't CreateBucket), then prunes local. The 3090 rig pulls via
  `../mvc2-ai/tools/r2-pull-dataset.sh` and trains offline (PyTorch/CUDA → ONNX; NEVER torch in
  the input-server hot path). Reward = DIAMBRA-standard damage-dealt-minus-taken (health-delta).
- Milestone A (input-only predictor) already beats the naive "repeat-last-input" baseline
  **+10.4pp on decision frames** (`../mvc2-ai/src/mvc2_ai/milestone_a.py`). Next: the
  state-conditioned model, once real 2-human matches accumulate.

### Deploy state (2026-07-18) — HISTORICAL, pre-cutover box
- **Was live on the then-prod box** (149.28.44.118) as `/usr/local/bin/flycast` md5 `eacf3077…` (backup
  `flycast.bak-20260718-225110`), built from branch **`deploy/exporter-prod`** (node-console tip
  + the 5 `(dataset)` commits — the exporter itself is on `feat/dataset-exporter`). Exporter
  GATED OFF; the `dataset_record` toggle is confirmed live on prod.
- **Operator console (PRIMARY):** the **Training Console** at `nobd.net/training?key=<TRAIN_KEY>` —
  a DEDICATED standalone service (`tools/training-console/training_server.mjs`, systemd
  `maplecast-training`, node :9097 behind nginx `/training/`, its OWN key `MC_TRAIN_KEY` ≠ the mod
  key). Deliberately separate from the Mod Command Center. Full ops: recording control (3-state
  badge OFF/ARMED/RECORDING + Start/Stop), live monitor (both players' state + decoded Input_DEC,
  session frames/size/rate, **decision-frame rate** = the data-quality signal), **session library**
  (every local `.mctele` with exact frame count parsed from the `MCTELE01` header + R2 upload
  status), **storage/R2** (local vs uploaded vs pending, R2 bucket totals via `rclone lsjson`
  cached, last R2 sync time), and a training-status tile (reads `training-status.json`). Reaches the
  loopback control WS (7211) for state + `dataset_record`; RAM never touches the public net. Page =
  `web/training.html`. **No binary rebuild** — pure service + static page.
- **Legacy/secondary:** the Mod Command Center still exposes a **Dataset Recording** toggle and a
  `nobd.net/modcmd/monitor?key=<MOD_KEY>` page (`web/recmon.html`, bridge GET `/monitor`) — same
  control WS, mod-key-gated. The Training Console supersedes it as the home for this.
- **Whole fleet is recordable (2026-07-19).** All 5 distributed nodes (atl/dfw/ord/sea/ewr) now run
  ONE unified flycast build (TDW2 thin-wire + exporter, md5 `787d9b81`) — **build fleet binaries
  `-DMAPLECAST_PORTABLE=ON`** (Skylake nodes have no AVX-512; a `-march=native` build SIGILLs them —
  see the Vultr deploy memory). Each node has R2 sync (`recordings/<node>/` prefix) and a per-node
  recording agent: `tools/training-console/noderec_agent.py` (python3, loopback :9098) fronted by
  the node's **Caddy** `/noderec/*`, key-gated (`MC_NODE_KEY`), talking to that node's local control
  WS (:7211). The Training Console's Nodes panel has **per-node Start/Stop** (`/api/node-record` fans
  out: self→local control WS, remote→its `/noderec` agent). Deliberately NOT hub-relayed (that would
  need a fleet rebuild of relay+hub). systemd `maplecast-noderec` per node. All gated OFF.

## The RE knowledge graph (`tools/re_kb`) — READ BEFORE RE-DERIVING

Every MVC2 address, struct offset, SH4 routine and reverse-engineering finding
this project has established lives in a queryable graph, **including what has
been RULED OUT**. Ask it before you re-derive an address or re-walk a dead end.

```bash
tools/re_kb/start.sh                       # start it (idempotent; needs the repo root)
tools/re_kb/start.sh --status              # up? how many findings?

tools/re_kb/rekb.sh "SELECT id, statement FROM finding WHERE status='ruled_out';"
tools/re_kb/rekb.sh "SELECT * FROM routine:loc_8c0344d4;"
tools/re_kb/rekb.sh "SELECT record::id(in) AS caller, via FROM calls WHERE record::id(out)='loc_8c0344d4';"
```

**It is not automatic unless the server is running.** Two hooks
(`.claude/settings.json`) inject known facts when a tool call names an address
or PC, and re-inject settled facts before compaction — but both exit silently
when the graph is down, so silence means "not running", not "nothing known".
`tools/re_kb/start.sh` first.

**Writing to it — never with raw SQL.** Use `tools/re_kb/kb.py`:

```python
kb.propose(slug, statement, about='address:...')   # always lands as `inferred`
kb.confirm(slug, source='...')                     # REQUIRES reproduction- or code-grade evidence
kb.rule_out(slug, statement, tried=..., evidence=...)
kb.record_attempt(approach, on=slug, outcome='ineffective'|'masks_only'|...)
kb.health()                                        # the audit
```

`confirm()` refuses an attestation-grade source on purpose. **A pixel diff that
improved is a measurement, not a mechanism** — if the output is right and you
cannot say why, that is `record_attempt(..., outcome='masks_only')`, not a
confirmed finding. This project has repeatedly declared false wins; that
outcome value exists to catch them.

**Bulk ingest may never write `finding`.** Doc/asset ingestion goes to
`docnote` / `attack` / `routine` — artifacts, not claims. Promotion happens one
row at a time through `kb.confirm()`. (576 markdown bullets were once 90% of
the `finding` table, carrying `status='confirmed'` inherited from doc
checkmarks.)

Full docs: `tools/re_kb/README.md`. Rules and evidence ladder:
`tools/re_kb/77_epistemics.surql`.

## Code Guidelines

- After 2-3 failed builds, stop and ask — don't burn time on retries
- For player-facing tradeoffs, ship both policies behind a runtime gate
- Run `MAPLECAST_DUMP_TA=1` determinism rig at end of phase, not after every step
- The build is the per-step check

## Per-frame budget
- 16.67ms at 60fps
- Server publish: ~80µs compress, ~1µs palette override
- Client render: ~700-1200µs depending on resolution
- E2E: ~10ms button-to-pixel over public internet

## RE METHOD (locked 2026-09-03) — read `../mvc-live-skins-quarters/docs/RE-METHOD.md` first
1. Port the SH4 annotations to the Steam binary by function matching.
2. Seed with unique constants, then propagate along the call graph.
3. Translate globals through the block map before comparing reference sets.
4. Tag confirmed versus inferred, and store the pairs as edges in the knowledge graph (`re_kb`, seeds in `tools/re_kb/NN_*.surql`, apply with `tools/re_kb/apply_seed.py`).
Query the graph before deriving; derive before capturing; cite an address for every claim; say UNKNOWN rather than guess.
