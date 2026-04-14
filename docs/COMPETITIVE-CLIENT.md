# MapleCast Competitive Client — The Best Native Client for Online MVC2

> "Frame-perfect, ultra-low-latency, lossless spectating, tournament-grade.
> The competitive standard for Marvel vs. Capcom 2 online play."

---

## Vision

A single native binary (`flycast --client`) that is simultaneously:
- **The lowest-latency way to play MVC2 online** — sub-10ms input-to-pixel over public internet
- **The highest-quality way to spectate** — pixel-perfect, native-resolution, zero compression artifacts
- **The canonical replay viewer** — frame-perfect, scrub-able, analyzable
- **A tournament-grade tool** — anti-cheat verifiable, signed match logs, broadcast-ready output

Built on the existing flycast headless server + TA mirror wire format. No new emulator. No new renderer. Just a native client wrapper that knows about the distributed input server network.

---

## The Killer Feature: Lossless Spectating

The TA mirror wire format is **GPU draw commands**, not pixels. Every spectator re-renders locally at their native resolution. Implications:

| Property | Twitch / YouTube | MapleCast Native Spectator |
|----------|-----------------|----------------------------|
| Resolution | Encoder limit (1080p free, 4K paid) | **Your monitor's native, up to 8K** |
| Compression artifacts | H.264/H.265, blocky in motion | **None — ever** |
| Bitrate per stream | 6 Mbps for 1080p60 | **~4 Mbps** for arbitrary resolution |
| Multi-view (4 matches) | 24 Mbps, 4 decoders | **16 Mbps**, 4 renderers, all 4K |
| Frame stepping | Keyframe-only | **Every frame is a keyframe** |
| Zoom / inspect | Pixelated | **Vector-clean re-render** |
| Replay storage | ~3 GB/hour at 1080p60 | **~1.8 GB/hour at any resolution** |

**This is unique to TA streaming.** No other fighting game spectator pipeline can do this. Tournament archives at 4K/120fps that are smaller than 1080p Twitch VODs.

---

## Terminology

Throughout the codebase, UI, and docs:

| Old | New (user-facing) |
|-----|-------------------|
| node | **input server** (or "server" in shorthand) |
| nodes registered | **input servers online** |
| node_id | input server id |
| network dashboard | **input server map** |
| local node | **my input server** / **local input server** |

Internal Rust types (`Node`, `node_id` field) stay as-is — they're encapsulated. User-facing surface (HTML, log messages, env var aliases) all says "input server."

---

## Architecture: The Whole Picture

```
                    ┌──────────────────────────────────────┐
                    │ HUB (nobd.net) — discovery only       │
                    │ - Lists input servers + GeoIP         │
                    │ - Min-max fairness matchmaking        │
                    │ - Match metadata, replay registry     │
                    │ - User profiles (cloud config sync)   │
                    └──────────┬───────────────────┬───────┘
                       discovery│           replay │ registry
                                │                  │
   ┌────────────────────────────┴──────────────────┴─────────────┐
   │                                                                │
   ▼                                                                ▼
┌───────────────────────────────────┐    ┌──────────────────────────┐
│ NATIVE CLIENT (flycast --client)  │    │ INPUT SERVER (anyone)    │
│                                    │    │                           │
│ ┌────────────────────────────────┐│    │ flycast headless          │
│ │ Hub auto-discovery             ││    │  + relay (with hub_client)│
│ │ UDP-ping probing               ││    │  + nginx (optional, TLS)  │
│ │ Multi-server hot standby       ││    │                           │
│ └────────────────────────────────┘│    │ Ports:                    │
│ ┌────────────────────────────────┐│    │   7100/udp - input        │
│ │ Network: parallel sockets      ││────┤   7201/tcp - TA frames    │
│ │ - QUIC for TA frames           ││    │   7213/tcp - audio        │
│ │ - UDP for input (redundant)    ││────┤   7220/tcp - hub API      │
│ │ - UDP for audio (separate)     ││    │     (relay only)          │
│ │ - TCP for telemetry            ││    │                           │
│ └────────────────────────────────┘│    └──────────────────────────┘
│ ┌────────────────────────────────┐│
│ │ Input: raw event device        ││  Modes:
│ │ - SCHED_FIFO 1000Hz thread     ││    --client    play (default)
│ │ - NOBD stick at 12KHz native   ││    --spectate  watch
│ │ - Lock-free SPSC ring          ││    --replay    file playback
│ └────────────────────────────────┘│    --multi N   N-way grid
│ ┌────────────────────────────────┐│
│ │ Render: native OpenGL/Vulkan   ││
│ │ - Mailbox vsync                ││
│ │ - VRR (G-Sync/FreeSync)        ││
│ │ - Direct fullscreen exclusive  ││
│ │ - Predictive frame rendering   ││
│ └────────────────────────────────┘│
│ ┌────────────────────────────────┐│
│ │ HUD: always-on overlay         ││
│ │ - Latency breakdown            ││
│ │ - Network grade per server     ││
│ │ - Frame data, hitboxes (toggle)││
│ │ - Input display                ││
│ └────────────────────────────────┘│
│ ┌────────────────────────────────┐│
│ │ Replay: lossless local record  ││
│ │ - Full TA stream + input log   ││
│ │ - .mcrec file format           ││
│ │ - Upload to nobd.net           ││
│ └────────────────────────────────┘│
└───────────────────────────────────┘
```

---

## Network Layer — Multi-Path, Redundant, Dedicated

### Sockets (one purpose each, no contention)

| Socket | Transport | Purpose | Optimization |
|--------|-----------|---------|--------------|
| Input (primary) | UDP `:7100` | Gamepad → server | SO_BUSY_POLL, SCHED_FIFO sender, send 2× per packet |
| Input (standby) | UDP `:7100` to backup server | Failover | Hot, idle but warm |
| TA frames | QUIC/WebTransport `:7201` | Server → client | Multiplexed streams, no HOL |
| TA frames (fallback) | WS `:7201` | If QUIC blocked | Auto-failover after 3s |
| Audio | UDP `:7213` (or QUIC datagram) | Server → client | Separate from video, no HOL between |
| Telemetry | TCP `:7202` | Stats, control | Low priority |

### Hub-Aware Discovery

On startup (or when user clicks "Find Best Server"):

1. `GET https://hub/api/input-servers/nearby` (GeoIP pre-filter)
2. For each candidate:
   - Open UDP socket to `:7100`, send 5 probe packets (4-byte sentinel) at 200ms intervals
   - Server echoes them back (new endpoint: probe ACK on input port)
   - Measure RTT — this is the **TRUE input-path latency**, not WS RTT
3. Rank by RTT (or hub does min-max for matched players)
4. Connect to winner; keep #2 as hot standby

### Input Redundancy

Every gamepad UDP packet sent twice:
- First send: T+0
- Second send: T+1ms (different network jitter window)
- Server dedups by sequence number
- Loss tolerance approaches 100% for single-packet drops
- Bandwidth: 4 bytes × 12,000 pps × 2 = **96 KB/s** (negligible)

### Multi-Server Hot Standby

- Open input UDP to top-2 servers
- Send only to primary, but keep secondary socket bound
- If primary RTT spikes >50ms or no ACKs for 100ms, **instant failover** (no reconnect time)
- Re-probe every 5 minutes; promote standby if it overtakes primary

### Predictive Input Pacing

Server broadcasts `t_next_latch_us` in status JSON (already exists). Native client:
- Aligns gamepad polling to fire 1ms before server vblank
- Sends UDP packet to land 0.5ms before vblank
- Cuts input lag from "random within 16ms vblank window" to "deterministic 0.5ms before latch"
- **2-4ms latency reduction, free**

### Linux-Specific Optimizations

| Feature | Win | Requires |
|---------|-----|----------|
| `io_uring` for batched send/recv | -10µs per syscall | Linux 5.1+ |
| `AF_XDP` for input ingress | -100µs (zero-copy) | CAP_NET_ADMIN, CAP_BPF |
| `SO_BUSY_POLL` | -10µs | sysctl tunable |
| `SO_PRIORITY` (TC_PRIO_INTERACTIVE) | kernel-level QoS | optional |
| `SCHED_FIFO` on net thread | no preemption | CAP_SYS_NICE |
| CPU pin (taskset) | better cache locality | optional |

All gracefully degrade if capabilities missing.

---

## Input Layer — Bypass Everything

### Raw Input Devices

Bypass SDL (which adds ~1-3ms via its event queue). Read directly:
- **Linux**: `/dev/input/event*` via libevdev
- **macOS**: HIDAPI / IOHIDManager
- **Windows**: Raw Input API

Auto-detect controller type, fall back to SDL only if unrecognized.

### NOBD Stick First-Class

The NOBD stick already exists with native UDP at 12KHz. Native client:
- If user's stick has registered identity, **send DIRECTLY from stick to server** (zero client involvement) — already works on cab
- Client-side gamepad path is for browser-style controllers (PS4, Xbox, etc.)

### Threading Model

```
┌─────────────────────────────────────────────────────────────┐
│ Thread: net_send  | SCHED_FIFO | core 0 | UDP send only      │
│ Thread: net_recv  | SCHED_FIFO | core 1 | UDP/QUIC recv only │
│ Thread: input     | SCHED_FIFO | core 2 | 1000Hz polling     │
│ Thread: render    | SCHED_FIFO | core 3 | TA parse + GL      │
│ Thread: audio     | SCHED_RR   | shared | low-latency output │
│ Thread: ui/main   | normal     | shared | menus, HUD updates │
└─────────────────────────────────────────────────────────────┘
```

Lock-free SPSC ring buffers between input ↔ net_send and net_recv ↔ render.

### Per-Input Timestamps

Every input packet carries a microsecond timestamp from `CLOCK_MONOTONIC_RAW`. Server echoes back the TA frame number that consumed it. Client computes true E2E latency:

```
E2E = (frame_present_ts - input_press_ts)
    = (1) gamepad poll → (2) UDP send → (3) server latch → 
      (4) frame render → (5) wire → (6) client render → (7) display
```

Display this in HUD. Live, per-input, never lies.

---

## Render Layer — Frame-Perfect Display

### Present Mode Selection

- **Mailbox (default)** — always present the freshest frame, drop stale ones. Lowest input-to-photons latency.
- **VRR (G-Sync / FreeSync)** — display refreshes when frame is ready, no judder
- **Immediate (low-latency)** — no vsync, tearing visible, **lowest possible latency** for tournament play
- **FIFO** — traditional vsync, frame-paced, smooth but +1 frame lag
- **Triple-buffer** — fallback for old hardware

User selects per-machine. Default: Mailbox + VRR if supported.

### Direct Fullscreen Exclusive

Bypass the compositor (KWin, Mutter, DWM, Quartz):
- **Linux**: DRM/KMS direct mode (saves ~5-15ms vs Wayland compositor)
- **macOS**: `kCGDisplayCaptureFlags` (saves ~5-10ms vs Quartz)
- **Windows**: Independent flip mode in DXGI (saves ~5ms vs DWM)

Toggle: "Tournament fullscreen mode" — one keypress.

### Predictive Rendering

Server frame phase telemetry tells client when next frame WILL arrive. Client:
- Pre-allocates render targets for next frame
- Begins WebGL/Vulkan command buffer recording
- When frame arrives, just submits the prepared buffer
- Saves ~0.5-1ms per frame

### Resolution Independence

The TA stream contains GPU commands at the native Dreamcast resolution (640×480). The client renderer:
- Default: 4K downscale-to-window (4K supersampled looks gorgeous)
- Custom: any resolution
- Multi-monitor: split a single match across monitors? (just for fun)

---

## Diagnostic HUD — The Competitive Overlay

Always-on, configurable, toggleable per element. Default position: bottom-left.

```
┌───────────────────────────────────────┐
│ ⚡ NETWORK                              │
│   Server: nobd-main (Piscataway, NJ)   │
│   RTT:    12.3ms ±0.4ms (stddev 0.18)  │
│   Loss:   0.0% (last 60s)              │
│   Jitter: 0.4ms p99                    │
│   Grade:  S                            │
├───────────────────────────────────────┤
│ ⏱ LATENCY                              │
│   Input:  1.2ms (poll → wire)          │
│   Frame:  8.4ms (wire → pixel)         │
│   Total:  21.9ms (button → display)    │
│   Last 60 frames: ▁▁▂▁▂▂▁▂▁▁▂▁▁▂▁     │
├───────────────────────────────────────┤
│ 🎮 INPUT                                │
│   ←   ↗ ↑   X Y Z                      │
│   ↙ ↓ ↘   A B C                        │
│   (last 30 frames)                     │
├───────────────────────────────────────┤
│ 🥊 FRAME DATA (toggle: F1)             │
│   P1: Storm  | Hyper Standby  +12      │
│   P2: Magnet | Tempest         -18 (lose)│
└───────────────────────────────────────┘
```

### HUD Elements (each toggle-able)

| Element | Toggle | Default |
|---------|--------|---------|
| Latency breakdown | F1 | ON |
| Network stats | F2 | ON |
| Input display | F3 | OFF (training mode auto-on) |
| Frame data (advantage on hit/block) | F4 | OFF |
| Hitbox / hurtbox | F5 | OFF (training mode toggle) |
| FPS counter | F6 | ON |
| Match clock | F7 | OFF |
| Server grade live | F8 | ON |

### Network Grading

Per-server, recomputed every 1s:

| Grade | Criteria |
|-------|----------|
| **S** | RTT <15ms, jitter <0.5ms, loss 0% |
| **A** | RTT <30ms, jitter <1ms, loss <0.1% |
| **B** | RTT <60ms, jitter <2ms, loss <0.5% |
| **C** | RTT <100ms, jitter <5ms, loss <2% |
| **F** | Any worse — show warning, suggest reconnect |

---

## Replay Layer — Lossless Local Recording

### File Format: `.mcrec`

```
[MCREC HEADER]
  magic:        "MCREC\0\0\0"
  version:      1
  match_id:     UUID
  game:         "MVC2"
  rom_hash:     SHA-256
  server_id:    UUID of input server
  start_ts:     ISO 8601
  duration_us:  total length
  p1_name:      string
  p2_name:      string
  p1_chars:     [3 ids]
  p2_chars:     [3 ids]
  winner:       0|1|null

[INDEX]
  every 60 frames: byte offset → frame number
  → enables instant scrubbing without scanning

[INPUT LOG]
  per-frame: [frame_num, p1_input_64bit, p2_input_64bit]
  ~24 bytes/frame × 60 fps × 60 sec = 86 KB/min

[TA STREAM]
  full compressed wire format, byte-perfect
  ~30 MB/min at 4 Mbps

[SIGNATURE]
  HMAC-SHA256 over entire file
  signed by input server's key
  proves match outcome without trust
```

**Bandwidth:** ~30 MB/min ≈ 1.8 GB/hour at unlimited resolution. (Twitch 1080p VOD: 3 GB/hour.)

### Playback Modes

```bash
flycast --replay match.mcrec                  # play at 1× speed
flycast --replay match.mcrec --speed 0.5      # half speed
flycast --replay match.mcrec --frame 12345    # jump to frame
flycast --replay match.mcrec --inputs         # show input display
flycast --replay match.mcrec --hitboxes       # show hitboxes
flycast --replay match.mcrec --analyze        # frame-data overlay
```

Scrub via keyboard (← →) or seek bar in UI.

### Replay Sharing

- One-click upload to nobd.net (HTTP POST to hub `/replays`)
- Hub stores in S3-like bucket, returns short URL
- Browser playback via WASM (re-uses king.html renderer)
- Embed-able in forums / Discord / etc.

---

## Spectator Modes

### Single-View Spectate

```bash
flycast --spectate <match_id>
flycast --spectate <input_server_id>:current  # current match on a server
flycast --spectate any                         # any active match
```

Pixel-perfect, native resolution, audio. Same code path as playing minus the input plumbing.

### Multi-View Grid

```bash
flycast --spectate-grid 4    # 2x2 grid of 4 active matches
flycast --spectate-grid 9    # 3x3 grid
flycast --spectate-grid 16   # 4x4 grid (yes, this is silly)
```

Each tile = independent renderer instance, all driven from the hub's "active matches" list. Users click a tile to focus / fullscreen it.

### Tournament Producer Mode

For someone running a tournament stream:

- Multi-view of bracket-relevant matches
- Manual focus switching (keyboard hotkeys)
- Picture-in-picture commentary cam input
- OBS-friendly window output (no overlays in capture)
- Manual graphic overlay (fighter portraits, scores)

---

## Tournament-Grade Features

### ROM Hash Verification

Before connecting:
- Server reports SHA-256 of its loaded ROM
- Client checks against known-good list (from hub)
- Display badge: ✅ Verified MVC2 v1.001 / ⚠️ Custom ROM hack / ❌ Unknown
- Refuse connection if user opts in to "verified-only" mode

### Match Signing

- Each input server has an ED25519 keypair (generated on first start, persisted)
- Public key registered with hub during registration
- Match outcome + final RAM state hashed
- Server signs the hash
- Client verifies with the public key from hub
- **Signed match logs prove outcomes without trusting the operator**

### Latency Parity (Optional)

Some tournament rules: if one player has 5ms and the other 50ms, artificially delay better player. Toggle:

```
Latency Parity: [ON] [OFF]
```

When ON, server adds frame delay to the lower-RTT player to match the higher one. Both players see the same E2E latency. Required for some events.

### Stream Output Mode

For broadcasting to Twitch / YouTube:

- Clean fullscreen, no overlays in main window
- Optional separate window for stats overlay (OBS source)
- HDMI output to capture card if available
- Latency-tuned audio for sync with capture

### Pause / Scroll Lock

Hotkey-based common-rule pauses:
- F11 / Print Screen / etc. → "Pause requested" packet to server
- Server confirms, both players see countdown, game pauses
- Untoggleable while in a tournament-locked match (operator config)

### Anti-Cheat Hooks

- Verify TA stream against deterministic baseline (per-frame hash)
- Flag suspicious inputs (impossible reaction times, robotic patterns)
- Optional input recording for post-match audit
- Server can submit suspicion reports to hub
- Hub aggregates per-player; community can review

---

## UX & System Integration

### Cloud Config Sync

Sign in with nobd.net account → keybinds, audio settings, video settings, server preferences sync via SurrealDB. Same setup on any machine.

### Region Preference

- "Always pick US-East servers"
- "Avoid TURN-relayed nodes"  
- "Prefer servers run by [operator name]"
- Whitelist / blacklist specific operators

### Friend List & Direct Challenge

- Browse online friends (pulled from nobd.net)
- "Challenge to a match" — both clients negotiate a server, queue together

### Bracket Integration

- Import from start.gg / Challonge via API
- Auto-queue against scheduled opponent at scheduled time
- Report results back to bracket

### Display Calibration

One-time tool: client flashes screen, measures monitor's actual response time via webcam (or photodiode if user has one). Stores baseline. HUD's "E2E latency" then includes display in the math.

### Tournament Mode

One-click toggle:
- All overlays off
- Keybind locked
- Cloud sync paused (no mid-match config changes)
- Auto-record replay
- Match signing enforced
- Stream output enabled

### Auto-Update

- Code-signed binaries
- Background download
- Apply on next launch
- Rollback to previous if startup fails

---

## Hub Extensions Required

To support all this, the hub gains a few endpoints:

| Endpoint | Purpose |
|----------|---------|
| `GET /api/input-servers` | (renamed from /nodes) |
| `GET /api/input-servers/nearby` | (renamed) |
| `POST /api/input-servers/:id/probe` | server-side probe ACK config |
| `GET /api/matches/active` | for spectator multi-view |
| `GET /api/matches/:id/info` | metadata for joining a spectator |
| `POST /api/replays` | upload replay |
| `GET /api/replays/:id` | download replay |
| `GET /api/users/:id/config` | cloud config sync (read) |
| `POST /api/users/:id/config` | cloud config sync (write) |
| `GET /api/operators/:id` | reputation, signed key, badges |

---

## File-by-File Implementation Map

### Phase 1 — Hub-Aware Native Client (foundation)

| File | Change |
|------|--------|
| `core/network/maplecast_mirror.cpp` | Native client mode reads `MAPLECAST_HUB_URL`; on startup fetches `/input-servers/nearby` via libcurl, UDP-pings each, picks best |
| `core/network/maplecast_input_server.cpp` | Add probe-ACK responder: when receiving 4-byte sentinel `[0xPP, seq, 0, 0]`, echo back with server timestamp |
| `relay/src/hub_client.rs` | `--public-relay-url` already added; ensure URLs propagate to nearby query response |
| `hub/src/api.rs` | Add `/api/input-servers/nearby` (alias for /nodes/nearby) |
| `web/network.html` | Rename "nodes" → "input servers" throughout UI |
| `docs/COMPETITIVE-CLIENT.md` | This document |

**Outcome:** `MAPLECAST_MIRROR_CLIENT=1 MAPLECAST_HUB_URL=https://nobd.net/hub/api ./flycast` — auto-discovers, picks best server, plays.

### Phase 2 — Multi-Socket + Input Redundancy

| File | Change |
|------|--------|
| `core/network/maplecast_mirror.cpp` | Add hot-standby UDP socket to backup server; failover logic |
| `core/network/maplecast_mirror.cpp` | Send each input UDP packet twice (T+0, T+1ms); add sequence number for server-side dedup |
| `core/network/maplecast_input_server.cpp` | Server-side input dedup by sequence number |
| `core/network/maplecast_mirror.cpp` | SCHED_FIFO + CPU pin on net thread (with capability check) |

**Outcome:** Sub-1% effective input loss even on lossy WiFi. Sub-50ms failover when primary server hiccups.

### Phase 3 — Diagnostic HUD

| File | Change |
|------|--------|
| `core/rend/imgui_overlay.cpp` (new) | Always-on overlay with latency / network / input |
| `core/network/maplecast_mirror.cpp` | Per-input RTT tracking via timestamp echo |
| `core/network/maplecast_input_server.cpp` | Echo TA frame number that consumed each input |
| `core/network/maplecast_mirror.cpp` | Network grading (S/A/B/C/F) computation |
| `core/rend/hitbox_overlay.cpp` (new) | Read DC RAM at known character struct offsets, render hitboxes |
| `core/rend/inputs_overlay.cpp` (new) | Show recent gamepad inputs at screen edge |

**Outcome:** Always-on competitive HUD. Player can diagnose latency issues themselves.

### Phase 4 — Replay Recording

| File | Change |
|------|--------|
| `core/network/replay_writer.cpp` (new) | Record TA stream + input log to `.mcrec` |
| `core/network/replay_reader.cpp` (new) | Playback `.mcrec` files |
| `core/network/maplecast_mirror.cpp` | `--record output.mcrec` flag, `--replay input.mcrec` mode |
| `hub/src/api.rs` | `POST /api/replays` upload endpoint, S3-style storage |
| `web/replay.html` (new) | Browser-based replay viewer (re-uses WASM renderer) |

**Outcome:** Lossless local recording. Upload + share. Browser playback at native quality.

### Phase 5 — Spectator + Tournament

| File | Change |
|------|--------|
| `core/network/maplecast_mirror.cpp` | `--spectate <match_id>` and `--spectate-grid N` modes |
| `core/network/match_signer.cpp` (new) | ED25519 key generation, match signing |
| `relay/src/hub_client.rs` | Send public key during registration |
| `core/network/maplecast_mirror.cpp` | Verify match signatures on completion |
| `hub/src/api.rs` | Operator reputation, signed key registry |
| `core/rend/imgui_tournament.cpp` (new) | Tournament Mode UI (one-click toggle), ROM verification badge |
| `core/network/maplecast_mirror.cpp` | Latency parity option (server-side delay configurable) |

**Outcome:** Tournament-ready. Verifiable matches. Multi-view spectating.

### Phase 6 — UX Polish

| File | Change |
|------|--------|
| `core/cloud_config.cpp` (new) | SurrealDB sync of user settings |
| `core/network/maplecast_mirror.cpp` | Region preference, operator filter |
| `core/friends.cpp` (new) | Friend list, direct challenge |
| `core/bracket_sync.cpp` (new) | start.gg / Challonge integration |
| `core/display_cal.cpp` (new) | Display response time calibration |
| `core/auto_update.cpp` (new) | Code-signed background updater |

---

## Implementation Phases (Ordered, Time-Estimated)

| # | Phase | Outcome | Estimated Effort |
|---|-------|---------|------------------|
| 1 | Hub-aware native client | Auto-pick best server | 1 day |
| 2 | Terminology rename | UI says "input servers" everywhere | 0.5 day |
| 3 | Multi-socket + input redundancy | Loss-tolerant, failover | 2 days |
| 4 | Diagnostic HUD | Always-on competitive overlay | 2 days |
| 5 | Replay recording (.mcrec) | Lossless local capture | 2 days |
| 6 | Replay playback + sharing | Upload, share, embed | 2 days |
| 7 | Spectator mode (single + grid) | Multi-view tournaments | 2 days |
| 8 | Tournament Mode + signing | Verifiable matches, ROM check | 3 days |
| 9 | Cloud config sync | Same setup any machine | 1 day |
| 10 | Friend list + challenges | Social layer | 1 day |
| 11 | Bracket integration | start.gg auto-queue | 2 days |
| 12 | Display calibration | Real E2E measurement | 1 day |
| 13 | Auto-update | Silent, signed updates | 2 days |

**Total: ~21 development days. Single binary at the end. Best MVC2 client ever made.**

---

## What Ships First (the One-Pass Plan)

The user asked for "one pass" execution. The minimum lovable competitive client = phases 1-7:

1. ✅ **Phase 1** — Hub-aware native client (auto-discovery, UDP probing)
2. ✅ **Phase 2** — Rename to "input server" everywhere user-facing
3. ✅ **Phase 3** — Multi-socket + input redundancy (loss tolerance, failover)
4. ✅ **Phase 4** — Diagnostic HUD (latency + network + input + grade)
5. ✅ **Phase 5** — Replay recording (`.mcrec` format)
6. ✅ **Phase 6** — Replay playback + sharing (upload to nobd.net)
7. ✅ **Phase 7** — Spectator mode (single + grid)

**~10 days of focused work for a tournament-grade client.** Phases 8-13 are quality-of-life that can ship incrementally after.

---

## The Killer Community Feature: Combo Trainer

Falls naturally out of deterministic replays. The combo trainer turns this from "the best client" into "the best **platform** for learning MVC2."

**How it works:**
1. Player A executes a sick combo, hits "save combo" → tiny `.mccombo` file (~5-50 KB, just inputs)
2. Uploads to nobd.net's community combo library
3. Player B browses combos, picks one to learn
4. Training mode loads it:
   - Game plays the combo on screen (deterministic re-emulation)
   - **Note Highway** at bottom of screen: scrolling buttons fall toward a "hit zone"
   - Player B has to hit each button as it crosses the line — Guitar Hero / DDR style
   - Score: PERFECT / GREAT / GOOD / MISS per input, with 5/10/20-frame timing windows
   - Loop forever, slow-mo (50-110%), shadow-overlay mode
5. Combos taggable: character, difficulty (1-10), situation (corner/midscreen/anti-air), source player

**Note Highway UI:**
```
                ┌────────────────────────────────┐
                │  GAME RENDERING (top 80%)       │
                │  Storm executing combo...       │
                ├────────────────────────────────┤
                │  ↘  →   X   Z   ↘+B   ↘  →    │  ← scrolling left
                │ ──────────╆HIT ZONE━━━━━━━━    │
                └────────────────────────────────┘
SCORE: 4/7 PERFECT  STREAK: 4  ACCURACY: 95.2%
```

**Why this wins:**
- **Infinite community-generated content** — every player adds combos
- **Network effect** — more combos → more learners → more creators
- **Pro replays become learning material instantly** — sick combo from EVO → training session within hours
- **Lowers execution barrier** — visual note-highway makes hard combos approachable
- **No other fighting game has this as a first-class community feature**

A Storm player wants to learn Magneto's ROM combo? Browse → pick → learn. Hours saved. Skill gap closed. The MVC2 community grows.

---

## Why This Wins

- **Latency**: 3-10ms E2E over good internet. Same as the cab.
- **Quality**: Native-resolution lossless rendering. Spectators see better-than-Twitch.
- **Reliability**: Multi-socket redundancy + failover. WiFi tolerable.
- **Trust**: Signed matches, verified ROMs, transparent operator info.
- **Ease**: One binary. Auto-discover. Zero config.
- **Community**: Anyone runs an input server. No central bottleneck.

The combination has never existed in fighting games. Fightcade is rollback-only. Console online has central servers. Discord-streamed matches are blurry. **MapleCast Native Client = the new standard.**
</thinking>
