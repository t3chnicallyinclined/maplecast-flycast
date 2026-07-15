# TDW GOLD STANDARD — architecture report, gate audit, decommission plan

**Status: CURRENT (2026-07-15). TDW is the default/gold-standard wire.**
Companion: docs/TA-DICT-WIRE-PLAN.md (the measured campaign record that produced this).

---

## 1. Executive summary

TDW (TA Dictionary Wire) is a single-protocol game-streaming wire built and
user-verified 2026-07-14/15. One message type per frame carries everything:

```
TDW1 (per frame):  'TDW1' dictEpoch flags seq innerSize + streaming-zstd chunk
  inner: frameNum, vframe, taSize, nBlocks, newSection
         [camera 132B: stage_id+M2+M1]            (envelope bit3, always on)
         [pvr regs 64B]                           (bit5, always on)
         refs (u32 × nBlocks) + new blocks        (content-addressed dictionary)
         [page section, legacy layout verbatim]   (bit4: VRAM/palette textures)
         [E2EP 32B self-locating tail]            (press→present probe)
TDWS (join/epoch): dictionary snapshot, own outer magic + zstd
SYNC (join only):  legacy one-shot VRAM/PVR seed (the last legacy remnant)
```

**Measured (local rig, user-verified):**
| metric | value |
|---|---|
| press→present | **16–20 ms** (== the game's own 1-frame internal floor; old mirror ~22 ms) |
| wire, steady fighting | 0.02–0.36 Mbps (TA side) / ~0.7 Mbps warmup incl. pages |
| wire, super peaks | ~1.2 Mbps (first-seen effect geometry; repeats get cheaper) |
| byte-equality record | ~95,000 live frames, 0 mismatches (gate mode) |
| socket composition (TDW_ONLY) | TDW1 + one 542 KB SYNC at connect + 28 B TDWS — nothing else |
| client render | ~324 fps present, 60 fps wire, 0 drops; stage local (40 KB bake, reprojected per frame) |

Client renders: fighters+satellites+effects+HUD+menus from dictionary refs,
stage locally re-projected through the in-band camera (guard-volume clipped),
textures from the in-band page section. Server keep-rule v4: drop ONLY the
stage-allowlist geometry (the measured 89.6 % of dictionary churn).

## 2. Gate audit

### 2a. Server — the TDW gold set (required for the shipping mode)
| env | role |
|---|---|
| MAPLECAST=1, MAPLECAST_MIRROR_SERVER=1, MAPLECAST_HEADLESS_AUTOLOAD=1 | base headless server |
| **MAPLECAST_TADICT=1** | the TDW encoder |
| **MAPLECAST_TADICT_PLAYERS=1** | keep-rule v4 (stage stripped; everything else rides) |
| **MAPLECAST_TACANON=2** | dead-byte mask — REQUIRED (live-proven; unmasked play blows the dict cap) |
| **MAPLECAST_TDW_ONLY=1** | decommission mode: legacy/ZCS2/GSTA/OBJS/EFCT/TXTR broadcasts OFF |
| MAPLECAST_E2E_PROBE=1 | E2EP tail (keep on: 32 B/frame, zstd-trivial) |
| MAPLECAST_TADICT_MAXBLOCKS / _MAXMB / _RESET | dict caps (defaults fine) |

### 2b. Server — legacy-client legs (KEEP CODE, off in gold mode; needed until browser/prod clients speak TDW)
ZSTREAM(+_LEVEL/_RESET/_SOA/_CAM), VCACHE(+_MEASURE), STAGESTRIP, CHARSTRIP,
STATE_MERGE, REPLICA_LIVE, PAGEGATE, NOSYNC/SYNC tunables, USE_RELAY, HUB_URL.
These serve king.html/webgpu-test/replica clients and the gate/render dev modes
of the native client. Retirement condition: TDW decoder in the browser + prod
relay TDW support.

### 2c. Server — RE/diagnostic instrumentation (~150 of the 189 flags; candidates for a compile-time GOLD profile)
CHARQ(+_EMIT), ORACLE_PROBE, FRAME_ORACLE_HOOK, TILEDESC_*, WALKSNAP_VERIFY,
SUBHASH_LOG, IDXTAB_CHARSNAP, BODY_PARITY_PIN, POLY3D, HUD_DIAG, TAEFF,
STAFMEASURE, DUMP_TA/_DIR/_ON_SUPER/_FX_TEX, DUMP_GSTA_VRAM, WATCH(+_BASE/_LEN),
QDIAG, HITDIFF-family, VERIFY_DC, TEST_ROLLBACK, PREDICT_DECISIVE,
RA_DIRTY_* (run-ahead arc), … These are the RE campaign's instruments — all
default-OFF (zero hot-path cost beyond a static bool check), but they are code
surface. **Recommendation: a CMake option `MAPLECAST_GOLD=ON` that compiles the
diagnostic families out entirely** — smaller binary, zero accidental-enable
risk, and the audit line between product and lab becomes enforceable.

### 2d. Client (native-client-tdw) — 13 flags, all sane
| flag | default | note |
|---|---|---|
| MC_TDW | players (via bat) | gold mode; gate/render = dev (need TDW_ONLY off server-side) |
| MC_STAGE | bat sets STG0B | local stage bake path |
| MC_HUD | off | synth HUD reserved for a future ultra-thin tier |
| MC_WINDOW / persisted file | 640×480 remembered | display |
| MC_PLAYERS_ONLY | off | old draw-filter experiment — REMOVE at merge |
| MAPLECAST_WS / _INPUT_HOST / _INPUT_PORT / _SLOT | prod defaults | ⚠ input defaults to nobd.net — bat must override for local |
| MAPLECAST_REPLICA | prod default | replica thread retries pointlessly in gold mode — see decommission list |
| MAPLECAST_JITTER / _TELEMETRY_LOG / _STAGECACHE | off | optional |

## 3. Latency / spike / bandwidth decommission list

| # | item | cost | status / action |
|---|---|---|---|
| 1 | Legacy ZCST broadcast (compress + send) | ~0.2 ms/frame CPU + 10.7 Mbps | **DONE — gated by TDW_ONLY** |
| 2 | ZCS2 leg (compress + send) | ~0.7 ms/frame + 7.4 Mbps | **DONE — env off in gold bat** |
| 3 | GSTA/PALF/WTCH/OBJS/OBJF side channels | ~0.4 Mbps + per-frame RAM walks | **DONE — gated (incl. the readGameState walk)** |
| 4 | EFCT/TXTR block (ran a whole extra ta_parse per frame!) | ms-class per frame | **DONE — gated; measurable latency win** |
| 5 | Client double-apply (ZCS2 chain + TDW pages) | ~1–2 ms/frame client | **DONE — ZCS2 not even zstd-decoded now** |
| 6 | Periodic 60 s resilience SYNC | 542 KB spike + client VRAM overwrite hitch every 60 s | **KEEP for now** — it heals diff-loop torn pages (a real, documented drift class that the TDW page channel shares). Tunable via PERIODIC_SYNC_FRAMES; revisit after a soak shows torn-page frequency. |
| 7 | TDWS on join at large dict (multi-MB) | join-time spike only | acceptable; shrinks to ~nothing with persistent client caches (hash-ID protocol, queued) |
| 8 | Dict cap epoch reset (re-warm burst) | rare with mask (0 cap hits in user sessions) | monitored via [TADICT]; caps configurable |
| 9 | Client replica-socket retry loop | log noise + periodic connect syscalls | TODO: skip spawn when MC_TDW=players |
| 10 | STATE_MERGE STM2 capture attempt per frame | ~0 when REPLICA_LIVE off | fine; dies with legacy legs |
| 11 | TDW encoder | 250 µs (players) – 890 µs (with pages) per frame | fine; XXH64 + arena-reserve queued if it ever matters |
| 12 | Super first-seen effect geometry (~1.2 Mbps peaks) | inherent (novel content) | char-select prefetch + persistent dict (queued) turn repeat supers ~free |

## 4. Repo / structure recommendation

**Do not split into a separate repo yet.** The server IS flycast (core/network/
compiled into the emulator); the protocol changed 6 times in 24 hours and will
keep co-evolving until the browser decoder + prod shadow land. A split today
buys cross-repo friction, not cleanliness. Instead, in order:

1. **Merge the client**: native-client-tdw/ → native-client/ as the default
   mode (MERGE-NOTES.md written; coordinate with the active native-client
   agent; delete the clone after).
2. **One protocol spec file**: extract §1's as-built wire spec into
   docs/TDW-PROTOCOL.md as the normative reference (plan doc stays the
   campaign record). All four future parsers (server, native, browser, relay)
   cite it.
3. **MAPLECAST_GOLD CMake profile**: compile out the §2c diagnostic families;
   the gold server binary = base + TDW + the legacy legs (still env-off).
4. **Split at prod-shadow time** (the natural seam, if still desired):
   `maplecast-client` (the Rust crate — it is ALREADY standalone: wgpu + WS +
   40 KB stage bakes; no flycast dependency) and keep the server as the
   flycast fork it inherently is. The client repo is the one that makes sense
   to open up first.

## 5. Standing next-actions queue
1. D1 (drones ground blink) + D2 (snowball streaks) — pixel-gate session
   (needs a wgpu screenshot readback in the client for the A/B differ).
2. Persistent caches: content-hash dictionary IDs → disk-persistent dict +
   page cache; char-select prefetch. Kills join spikes and repeat-super costs.
3. 17-stage bake sweep + per-stage strip allowlist (control-WS stage forcing
   can automate captures).
4. Browser TDW decoder (webgpu-test worker) → prod relay TDW class → prod
   shadow deploy (deploy-discipline, all default-OFF).
5. Long soak review ([TADICT] log accumulates while the rig runs).
6. Game-wedge repro (pause→character-change→loading hang; suspected
   autoload-savestate/GD-ROM state; pre-existing).
