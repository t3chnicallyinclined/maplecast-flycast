# CLAUDE.md — Rules and Context for AI Assistants on MapleCast

## CRITICAL DEPLOYMENT RULES — READ FIRST

### NEVER deploy to production without a backup
- **ALWAYS** use `./deploy/scripts/deploy-web.sh` for web files — it creates a timestamped backup
- **ALWAYS** use `./deploy/scripts/deploy-headless.sh` for the flycast server binary
- **NEVER** use raw `scp` to overwrite production files
- **NEVER** assume git matches production — production may be AHEAD of git
- **ALWAYS** verify with `md5sum` before and after deploying

### Deploy workflow
```bash
# Web files (king.html, JS modules):
./deploy/scripts/deploy-web.sh root@66.55.128.93

# Headless flycast server:
./deploy/scripts/deploy-headless.sh root@66.55.128.93

# Rollback (printed by deploy script):
ssh root@66.55.128.93 'rm -rf /var/www/maplecast && mv /var/www/maplecast-backup-YYYYMMDD-HHMMSS /var/www/maplecast'
```

### If production files were edited directly
Sync production → git BEFORE making any local changes:
```bash
scp root@66.55.128.93:/var/www/maplecast/king.html web/king.html
scp root@66.55.128.93:/var/www/maplecast/js/*.mjs web/js/
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

MapleCast turns Flycast (Dreamcast emulator) into a game streaming server. One MVC2 instance runs on a single 2-vCPU VPS with NO GPU. ~322 MB RAM, ~12% CPU. 60fps to `wss://nobd.net/ws`.

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
MAPLECAST_SERVER_HOST=66.55.128.93 \
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
