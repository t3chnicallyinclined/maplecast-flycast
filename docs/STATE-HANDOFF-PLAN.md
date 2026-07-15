# Live State Hand-off Between Servers (Server Transfer)

**Goal (user, 2026-07-15):** switching servers in the client should carry the
*game* with you — save the current state, push it to the destination server,
continue flawlessly. Today a switch just shows the destination's own
independent game (perceived as a "reset").

## Why this is guaranteed to work here — the three load-bearing facts

1. **SH4 emulation is byte-deterministic across machines AND OSes**
   (validated 2026-05-07). A state saved on node A and resumed on node B
   continues bit-identically.
2. **The fleet is md5-identical since 2026-07-15** (`188c6f13…` on all 6
   nodes) with an identical ROM (`rom_hash 396548fe…` on every hub entry).
   Savestate formats are build-sensitive; identical binaries erase that
   entire compatibility class. *The clone fleet is the prerequisite that
   makes state hand-off trivially safe.*
3. **PROVEN LIVE 2026-07-15 (v0, zero code changes):** saved sea's game via
   control WS (`savestate_save` slot 7, 7.2MB file), copied the file to ewr's
   boot slot, restarted ewr. ewr's MVC2 frame counter (`0x8C3496B0`) read
   **116,320** right after boot vs sea's **115,262 at save** — the 1,058-frame
   gap is exactly the ~17s boot+read lag at 60fps. A normal ewr reboot starts
   near 0. ewr ran sea's exact game, deterministically continued.
   (ewr's original boot file backed up + restored after the demo.)

## The one landmine — read before building v1

**`savestate_load` on a RUNNING headless server is KNOWN BROKEN and
hard-disabled** (`maplecast_control_ws.cpp:314-345`): `dc_loadstate()` mid-run
bounces through `Emulator::loadstate()` and silently kills the emu thread
(verified on the VPS 2026-04-08; ARCHITECTURE.md "never use emu.loadstate()
for live resync").

**The working primitive already exists:** the A2 run-ahead rollback ring does
in-memory `dc_serialize`/`dc_deserialize` at frame boundaries ON the emu
thread **every frame** (STATEVF 1177/1177). The migration receiver must use
that path — deserialize from a memory buffer at the next frame boundary —
followed by `maplecast_mirror::requestSyncBroadcast()` (ARCHITECTURE.md bug
#8: mirror shadows must realign or clients graft deltas onto stale bases).

## Architecture

### v0 — file hand-off via boot slot (WORKS TODAY, proven)
`ctrl_ws savestate_save` on A → scp state file → dest's autoload slot
(`/opt/maplecast/.local/share/flycast/mvc2.state`) → restart dest service.
Gap: dest restart ≈ 15-20s. Good for admin moves/rescues, not for players.

### v1 — in-band push, sub-second (the real feature)
```
client F2 panel "transfer ▶ <server>"
  → MIGR{dest} on the :7200 WS (client→server messages: new tiny handler)
A: frame boundary → dc_serialize to memory (~2.6ms measured by run-ahead)
   → zstd (7.2MB file already zstd-ish; wire ~5-8MB)
   → push to B :7200 as a WS *client*: STPU{fleet_key, rom_hash, blob}
B: validates fleet_key + rom_hash (+ binary version tag)
   → emu-thread frame boundary: in-memory dc_deserialize   [run-ahead primitive,
     NOT Emulator::loadstate — see landmine]
   → requestSyncBroadcast()  → ACK to A
A: → client REDIRECT{B}  → existing switch machinery fires
     (reset_for_new_server, TDWS seed from B, input retarget)
```
Freeze budget: serialize 3ms + compress ~100ms + DC-to-DC transfer <1s +
deserialize ~5ms + client TDWS seed. **Expected blackout ≈ 1s.**
Auth: `MAPLECAST_FLEET_KEY` env, shared across nodes; reject pushes without it
(anyone can reach :7200 — the key is mandatory, plus a size cap).

### v2 — zero-freeze warm standby (future; also the failover story)
Feed the destination the input tape live (lockstep replication, 2.4KB/s —
.mcrec + state-sync machinery exists) so B runs a hot replica; "transfer"
becomes an instant wire flip. Same machinery = tournament fair-frame tier and
node-failure recovery.

## Client work (native-client-tdw, some already landed 2026-07-15)
- Input follows the wire — hardened: retarget no longer latches on a failed
  UDP connect (retries), resets RTT EMA + forces a fresh state packet, and the
  F2 Servers panel now always shows `input -> host:7100`, with a loud orange
  **⚠ input PINNED** warning when `MAPLECAST_INPUT_HOST` is set (the pin was
  the prime suspect for "wire switched but input didn't": old launcher bats
  `set` it and cmd sessions keep env vars).
- v1 additions: "transfer ▶" button per server row; handle REDIRECT.

## Connection info for the hub/lobby (research 2026-07-15)

What the established genres show, mapped to what MapleCast already measures:

| Convention (source game) | Ours today | Hub/lobby plan |
|---|---|---|
| Ping to server (every FPS/browser) | UDP :7100 RTT probe (F2 panel, 2s) | per-server row, color-coded |
| Jitter / ping stability graph (CS2 "Network Quality") | — | keep rolling min/max/stddev of the :7100 probe |
| Packet loss %, per direction (CS2 shows srv→cli and cli→srv lines) | input seq gaps known server-side; TDW1 seq gaps client-side | count + show both directions |
| Tick / update rate (CS2 64-tick, missed ticks) | 60fps wire; wire_fps vs render fps already in F1 | show "60/60" style health |
| Interpolation/buffer delay (net_graph lerp; OW IND) | jitter-buffer toggle (+~1 frame) | show buffer depth in frames |
| Rollback/delay frames (SF6 shows delay bumps by RTT bracket; GGST bars) | run-ahead parked; MVC2 internal lag = 1 frame fixed | show "delay: 1F (game) + transport" |
| **Press→present ms** | **E2EP probe, 10-15ms measured** | *nobody else shows this — make it the headline stat* |
| Server browser columns (Source/BF: name, map, players/max, ping, region) | hub /nodes: name, region, spectators, status, rom_hash | add: game-in-progress (in_match @0x8C289624), player slots free (FCFS), TDW warm dict size, version/md5 |
| Wi-Fi indicator (SF6/FGC lobbies) | — | flag high-jitter connections |
| Region auto-pick lowest RTT (Valorant/consoles) | native client already RTT-ranks | "best server" auto-select + manual override |

FGC-specific lesson (SF6 criticism): added delay frames re-derived from RTT
brackets mid-match and never lowered again reads as unfair — if we ever add
adaptive delay, display it and let it recover.

Sources: [CS2 net graph guide](https://steamcommunity.com/sharedfiles/filedetails/?id=3652618149),
[CS2 jitter/loss displays](https://community.skin.club/en/articles/how-to-fix-net-jitter-cs2),
[packet-loss diagnostics](https://openpacketloss.com/gaming-packet-loss),
[SF6 rollback review (infil)](https://words.infil.net/w04-sf6review-p5.html),
[rollback netcode 101](https://litetheironman.medium.com/street-fighter-v-and-rollback-netcode-101-8921a1e8a1c6),
[tickrate primer](https://speedtesthq.com/guides/gaming/what-is-server-tickrate),
[server browser conventions](https://www.gametracker.com/search/).

## v1 BUILT + GATED (2026-07-15 evening)

Implemented in `maplecast_ws_server.cpp` (migration section at the bottom) +
`emulator.cpp` (apply slot) + `native-client-tdw` (transfer ▶ button, reply
handling). **Gate: two instances on one Windows box (A :7200, B :7205);
marker 0x07000000 planted in A's frame counter via ram_write; client-style
"migrate" JSON to A → B's counter read exactly 0x07000000 after "APPLIED
27785526 B state", both processes healthy, B's game advancing.** ACK 0.1s on
loopback; capture 27.8MB → 10.1MB wire.

### The three thread-contract lessons (each cost a live gate round)
1. `rend_start_rollback()` (the lockstep-client guard) HANGS a headless
   server — it waits on a vramRollback signal only a completed render sets.
   Use the A2 recipe: `rend_wait_render_idle()`.
2. The serverPublish site is the RENDER thread. Applying there swaps RAM
   under a RUNNING SH4 → `verify()` abort 0x80000003 seconds after "APPLIED".
   Capture (dc_serialize) from there is fine — same path as MCSV/control-WS
   saves.
3. A loop-top emu-thread hook never fires: `runInternal()` does NOT return
   per frame on a plain server. The working pattern (4th member of the
   existing family: rollback rewind, oracle-probe reload, readtrace flip):
   STPU receipt arms `raArmStepStop()` → vblank Stop()s the SH4 at the next
   true frame boundary → emu loop applies with the SH4 PAUSED →
   `Start()+continue`.

### Multi-instance servers (same box) — proven by the gate rig
One flycast process = one SH4 = one game. N games per box = N processes with
per-instance ports, ALL already env-tunable: `MAPLECAST_PORT` (input UDP),
`MAPLECAST_SERVER_PORT` (mirror WS; audio = +3), `MAPLECAST_CONTROL_PORT`.
`_run_srv_tadict_b.bat` = the reference second instance (7105/7205/7215).
Client directory entries carry `host:port` input hosts for non-7100 instances.
Capacity math: prod instance ≈ 12% CPU + ~322MB → a 2-vCPU box comfortably
runs 2-4 instances; lobby "rooms" = instances the hub registers per node.

### INTERNET GATE PASSED + FLEET ROLLED (2026-07-15 ~22:00)
**Local Windows desktop → prod main (nobd.net:7200): "migrated" in 0.5s,
main's frame counter read exactly the locally-planted marker 0x0070700b,
APPLIED 27785454 B, service healthy.** Fleet uniform on `728d091ca55b`
(all 5 edges + main, active, NRestarts=0, MAPLECAST_FLEET_KEY armed).

### The edge-OOM saga (lesson 4 — three kills on sea)
955MB edges idle with flycast ~755MB resident and ~50-70MB available; the
receive/apply burst OOM-KILLED sea three times DESPITE 3GB of free swap
(allocation rate outruns reclaim; kill lands seconds after "APPLIED" or even
at receipt). Fixes layered in: 32MB ctx + early blob free (insufficient) →
`migEagerInit()` boot-reserved arenas (insufficient — the box has no true
headroom) → **memory guards: receiver rejects STPU below 150MB MemAvailable,
source rejects a migrate below 250MB; the player stays put with a clear
error.** Consequence: **the current 1GB edges decline migrations by design;
they need 2GB instances to participate** (or a flycast-footprint diet —
resident is ~443MB anon + 278MB shmem, worth its own audit).

### Remaining
- v1.1: post-apply ack (today the sender acks on receipt — an apply-side
  failure after ack still reports "migrated"), rom_hash in the STPU header.
- Edge participation: upsize to 2GB (user/billing decision) or footprint diet.
- v2 warm standby rides the relay/fan-out arc (SYSTEM-MODEL.md §5).
