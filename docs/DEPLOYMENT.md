# Deploying MapleCast

This is the public-facing deployment overview. It tells you how to build and run MapleCast / nobd.net's flycast variants on your own hardware. Operator-specific runbooks (live VPS credentials, admin panel internals, business roadmap) live in a separate private repo and are not published here.

## What you can build from this repo

| Variant | Build target | What it is |
|---|---|---|
| **headless flycast** (recommended for servers) | `cmake -DMAPLECAST_HEADLESS=ON -B build-headless && cmake --build build-headless` | CPU-only mirror server. No GPU, no SDL, no X11, no audio. ~26 MB stripped binary on Linux, ~9 MB on Windows. Runs MVC2 + TA mirror streaming on a $5/month VPS. **Identical binary architecture on Linux and Windows** — Linux deploys to a VPS, Windows runs locally as the rollback predictor for sub-RTT input feel. See [ARCHITECTURE.md "Mode 3: Headless"](ARCHITECTURE.md) for the design rationale. |
| **GPU flycast** (for local/cab play) | `cmake -B build && cmake --build build` | Standard flycast with full rendering. Used at a physical cab or for local LAN play with sub-millisecond input latency. |
| **Windows mirror client** (native desktop spectator/player) | `cmake -DMAPLECAST_CLIENT_ONLY=ON -B build` (with vcpkg toolchain for libcurl) | Native Windows `flycast.exe` that connects to a remote MapleCast server (e.g. nobd.net) and renders the TA mirror stream pixel-perfect. No NVENC, no WebRTC, no DX9, no OpenSSL. See [WINDOWS-CLIENT-BUILD.md](WINDOWS-CLIENT-BUILD.md) for the full setup recipe. |
| **WASM renderer** (browser viewer) | `cd packages/renderer && bash build.sh` | Standalone WebAssembly renderer that consumes the TA mirror stream and draws MVC2 in a browser canvas. See [WASM-BUILD-GUIDE.md](WASM-BUILD-GUIDE.md). |

### Local rollback-predictor topology (Phase 1 of rollback prediction)

For **competitive players** who want sub-RTT input latency, the headless flycast can also run **on the player's own machine** as a deterministic predictor. The architecture is identical to the server-side deployment — same binary, same WS protocol, same TA mirror format — only the network path is different (loopback instead of WAN):

```
┌──────────────────────────┐    ws://127.0.0.1:7200    ┌──────────────────────┐
│ headless flycast         │  ─────── TA stream ──────► │ flycast mirror client│
│ (MAPLECAST_HEADLESS=ON,  │   UDP :7100 input ◄──────  │ (renderer only)      │
│  Windows or Linux)       │                            │                      │
└──────────────────────────┘                            └──────────────────────┘
```

Launch on Windows:
```powershell
$env:MAPLECAST=1
$env:MAPLECAST_MIRROR_SERVER=1
$env:MAPLECAST_HEADLESS=1
$env:MAPLECAST_PORT=7100
$env:MAPLECAST_SERVER_PORT=7200
$env:MAPLECAST_HEADLESS_AUTOLOAD=1   # if you have a savestate to autoload
.\build-headless-win\flycast.exe "<rom-path>"

# In a second terminal, connect the mirror client to localhost
$env:MAPLECAST_MIRROR_CLIENT=1
$env:MAPLECAST_SERVER_HOST="127.0.0.1"
$env:MAPLECAST_SERVER_PORT="7200"
.\build\flycast.exe
```

Same architecture supports three deployment modes:
- **Hub-hosted server**: prod operator runs headless on a VPS, browsers + mirror clients connect remotely
- **Decentralized node**: any operator runs headless on their own box, advertises via the hub
- **Local rollback predictor**: player runs headless on their own machine, mirror client connects to localhost

## Architecture overview

Read [ARCHITECTURE.md](ARCHITECTURE.md) for the full mental model. Short version:

1. A flycast instance runs MVC2 and emits a deterministic byte-perfect stream of raw GPU commands (TA buffers) + VRAM page diffs over a WebSocket.
2. A Rust relay fans the stream out to N browser clients.
3. Browsers run the WASM renderer to draw the stream pixel-perfect at 60 fps.
4. Players send gamepad input back over the same WebSocket connection (or via direct UDP for hardware sticks).

## Runbook for your own deployment

The high-level recipe:

1. **Build the headless binary** (see table above).
2. **Install the systemd unit** from `deploy/systemd/maplecast-headless.service` (template provided in this repo).
3. **Drop your MVC2 ROM** at the path the unit expects (default `/opt/maplecast/roms/mvc2.gdi`).
4. **Optionally**: install the [Rust relay](../relay/) for browser fan-out.
5. **Optionally**: serve the static `web/` directory under nginx for the King of Marvel UI.

The deploy script at `deploy/scripts/deploy-headless.sh` automates the binary build + install + systemd dance against a remote host you have SSH access to. Read it before running it — it does an `ldd` sanity check to make sure your build doesn't accidentally link `libGL`/`libSDL`/etc.

## Deploy scripts

| Script | What it deploys | Safety |
|--------|----------------|--------|
| `deploy/scripts/deploy-headless.sh <HOST>` | Headless flycast binary + systemd unit | ldd sanity check, strip, restart |
| `deploy/scripts/deploy-web.sh <HOST>` | king.html + JS modules to /var/www/maplecast/ | **Creates timestamped backup**, shows diff, confirms before deploy, prints rollback command |

### Web deploy workflow

**CRITICAL: always edit locally, commit to git, THEN deploy. Never edit production directly.**

```bash
# 1. Edit locally
vim web/king.html

# 2. Commit
git add web/ && git commit -m "feat: description"

# 3. Deploy (creates backup, asks confirmation)
./deploy/scripts/deploy-web.sh root@66.55.128.93

# 4. Rollback if needed (command printed by deploy script)
ssh root@66.55.128.93 'rm -rf /var/www/maplecast && mv /var/www/maplecast-backup-YYYYMMDD-HHMMSS /var/www/maplecast'
```

### Syncing production → git

If someone edited production files directly (via scp), sync them back to git BEFORE making any changes:

```bash
scp root@66.55.128.93:/var/www/maplecast/king.html web/king.html
scp root@66.55.128.93:/var/www/maplecast/js/*.mjs web/js/
git add web/ && git commit -m "sync: pull production web files from VPS"
```

---

## Standing up a browser-PLAYABLE server (env + ports checklist)

> Hard-won during the 2026-07-12 run-ahead feel-test standup. A server that
> *renders* in the browser and a server you can actually *play* are NOT the same
> thing — the input path has its own gate that fails **silently**.

### Required env flags

`MAPLECAST_MIRROR_SERVER=1` starts the **video** (mirror wire :7200). It does
**NOT** start the **input** server. Miss the input flag and the browser joins,
logs `P1 JOINED`, renders perfectly — and every button press is silently
dropped, then the player is idle-kicked after 5 min. You will chase the stick,
the Gamepad API, the proxy, and the join handshake before you find it.

| Flag | Starts | Symptom if missing |
|------|--------|--------------------|
| `MAPLECAST=1` | **Input server (UDP :7100/:7101)** + audio + telemetry (`emulator.cpp:1365`) | Join + render work, **all input silently dropped → idle-kick**. THE one people miss. |
| `MAPLECAST_MIRROR_SERVER=1` | Mirror wire + control WS (:7200) | No video wire at all |
| `MAPLECAST_HEADLESS_AUTOLOAD=1` | Auto-loads `mvc2.state` next to the ROM | Boots to BIOS/menu, not in a match |
| `MAPLECAST_REPLICA_LIVE=1` | replica-live GSTA seed (:7212) | render_frame client has no body seed |
| `MAPLECAST_STATE_MERGE=1` | folds per-frame body state onto the main wire (STM2) | bodies don't update / two-socket skew |
| `MAPLECAST_RUNAHEAD=1` | arms the run-ahead 3-leg cycle (optional) | (feature off) |

**Minimum browser-playable set:** `MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 MAPLECAST_HEADLESS_AUTOLOAD=1 MAPLECAST_REPLICA_LIVE=1 MAPLECAST_STATE_MERGE=1`.
The local rig's `start_maplecast.bat` sets `MAPLECAST=1` — copy from it, not from a mirror-only launcher.

### Port map (browser-playable headless)

| Port | Proto | Role | Bind |
|------|-------|------|------|
| 7100 / 7101 | UDP | Input server + tape publisher (**needs `MAPLECAST=1`**) | `*` |
| 7200 | TCP | **Unified** mirror wire **+** control WS — browser join AND gamepad input both land here | `*` |
| 7203 | TCP | Audio WS | `*` |
| 7211 | TCP | Control WS (config/telemetry) | loopback |
| 7212 | TCP | replica-live GSTA seed | loopback |

Input flow end-to-end: browser `/play` WS → `:7200` `onMessage` (binary 4-byte frame) → tagged with the player's slot → **UDP `:7100`** → input latch → SH4. If `:7100` isn't bound, that last hop is a silent drop.

### Client join reality (`webgpu-test.html`)

- It does **NOT** auto-join. There is a **🎮 JOIN P1** button (`joinP1.onclick`). Press a stick button first (Gamepad API needs interaction to register the pad), then click JOIN P1.
- `sessionId` is unique per page-load. **Ghost eviction** fires when a *second* join arrives with the same display name, so a second tab / reload race / mashing JOIN kicks the first. **One tab, one join.**
- Served over HTTPS the client runs prod "relay mode" and also loads the social stack (queue.mjs, SurrealDB `/db/rpc`, chat). Without SurrealDB those error/retry loudly in the console — **cosmetic, ignore them.**

### No-sudo / firewalled box → public endpoint (caddy + cloudflared)

For a box with **no root** and **no openable inbound port** (e.g. a Hetzner
dedicated box whose firewall you can't touch). A closed inbound port is a closed
door — so go **outbound**:

```
browser ──HTTPS──▶ Cloudflare edge ──▶ cloudflared (outbound tunnel, no inbound port)
                                          │
                                          ▼
                                   caddy :8088 (userspace path-router, no root)
                                     /ws           → localhost:7200
                                     /play         → localhost:7200
                                     /replica-live → localhost:7212
                                     /audio        → localhost:7203
                                     /             → serve web/ (static)
                                          │
                                          ▼
                                   headless flycast (the sim)
```

- Over the cloudflared HTTPS URL the client is in relay mode → all paths (`/ws /play /replica-live /audio`) resolve to the same host → **no per-path URL override needed** (exactly how prod nginx works).
- **Run every long-lived daemon in `tmux`.** caddy / cloudflared / flycast started over a one-shot `ssh` command die when the session closes — `caddy start`, `nohup`, and `setsid` all failed to survive here; detached tmux sessions persist. Use `tmux new-session -d -s NAME '/abs/path/script.sh'` (absolute paths — `~` does not expand inside the tmux command).
- **Latency:** Cloudflare adds only ~10–15 ms over the direct RTT (its edge sits next to the box). The transport is already UDP-input + `TCP_NODELAY`-wire, so there's no cheaper transport knob. XDP/DPDK is the wrong layer for a propagation-dominated budget — it's a jitter/throughput play on the prod input path (see the Phase-2 note in the fresh-box steps below), not a mean-latency win.
- Quick tunnel (`cloudflared tunnel --url http://localhost:8088`) gives an **ephemeral** URL; for a stable `sub.nobd.net` use a **named tunnel** (needs a Cloudflare API token scoped to the zone's DNS).

---

# Full Production Setup (2026-04-15) — New Dedicated-CPU VPS

> This is the canonical recipe for bringing up nobd.net from scratch.
> Covers the full stack: flycast + relay + hub + nginx + SurrealDB +
> web pages + kernel tuning + capability grants. If you're deploying
> to a brand-new box, follow this end-to-end.

## Current production target

- **VPS**: `149.28.44.118` (dedicated AMD EPYC Genoa, 2 threads, 4 GB RAM, Ubuntu 24.04)
- **DNS**: `nobd.net` → `149.28.44.118`
- **Old VPS** (`66.55.128.93`): decommissioned on 2026-04-15
- **Co-tenant (non-MapleCast):** the **NOBD Discord bots** also run on this box as systemd units
  (`nobd-oracle`, `nobd-roles`), fully isolated under `/opt/nobd-oracle/` with their own venv. They
  don't touch the flycast/relay/hub/SurrealDB stack — leave them alone (and they leave us alone).
  Owned by the `mvc2-oracle` repo's `discord-expert` agent; healthcheck `/opt/nobd-oracle/status.sh`.

## Component layout

```
Ports:
  7100/udp   — flycast input server (public)
  7101/udp   — input tape publisher (public, for player-clients)
  7102/tcp   — state sync (public)
  7210/tcp   — flycast WS (loopback only, relay upstream)
  7201/tcp   — relay WS downstream (proxied via nginx /ws)
  7202/tcp   — relay HTTP (loopback, proxied via nginx /api, /overlord/api, /turn-cred)
  7203/tcp   — audio WS (loopback, proxied via nginx /audio)
  7211/tcp   — control WS (loopback ONLY — admin/overlord)
  7220/tcp   — hub API (loopback, proxied via nginx /hub/api)
  8000/tcp   — SurrealDB (loopback, proxied via nginx /db)
  443/tcp    — nginx HTTPS (public)
  443/udp    — relay WebTransport/QUIC (public)

Binaries:
  /usr/local/bin/flycast                     (headless flycast)
  /usr/local/bin/maplecast-hub               (hub service)
  /opt/maplecast/maplecast-relay             (relay, runs as maplecast user)

Systemd units:
  maplecast-headless.service                 (flycast)
  maplecast-relay.service                    (relay)
  maplecast-hub.service                      (hub — new)
  + drop-ins in /etc/systemd/system/<service>.service.d/

Data paths:
  /opt/maplecast/.maplecast/node_id          (relay UUID — persistent)
  /opt/maplecast/roms/mvc2.gdi               (ROM + track*.bin sidecars)
  /opt/maplecast/savestates/                 (flycast savestates)
  /var/lib/maplecast/replays/                (hub replay storage)
  /var/lib/surrealdb/                        (SurrealDB data)
  /var/www/maplecast/                        (web files)
  /etc/maplecast/headless.env                (flycast env: ROM path, MAPLECAST_*)
  /etc/maplecast/hub.env                     (hub bootstrap operator token)
```

## Deploy order (fresh box)

### Step 1 — prerequisites

```bash
ssh root@<HOST>
apt-get update && apt-get install -y \
    nginx certbot python3-certbot-nginx \
    libcurl4 libxdp1 libbpf1 libgomp1 libzip4 zlib1g \
    ca-certificates openssl python3
```

Create the `maplecast` system user (owns /opt/maplecast + relay process):

```bash
useradd --system --home /opt/maplecast --shell /usr/sbin/nologin maplecast
mkdir -p /opt/maplecast/{roms,savestates,cfg,.local/share/flycast,.maplecast}
mkdir -p /var/lib/maplecast/replays /var/www/maplecast
chown -R maplecast:maplecast /opt/maplecast /var/lib/maplecast
```

### Step 2 — build binaries locally + upload

On your dev machine:

```bash
cd ~/projects/maplecast-flycast
cmake -B build-headless -DMAPLECAST_HEADLESS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-headless --target flycast
strip build-headless/flycast

(cd hub && cargo build --release)
(cd relay && cargo build --release)

# Upload
scp build-headless/flycast          root@<HOST>:/tmp/flycast-new
scp hub/target/release/maplecast-hub root@<HOST>:/tmp/hub-new
scp relay/target/release/maplecast-relay root@<HOST>:/tmp/relay-new
```

Install:

```bash
ssh root@<HOST>
install -m 0755 /tmp/flycast-new /usr/local/bin/flycast
install -m 0755 /tmp/hub-new     /usr/local/bin/maplecast-hub
install -m 0755 /tmp/relay-new   /opt/maplecast/maplecast-relay
chown maplecast:maplecast /opt/maplecast/maplecast-relay
rm /tmp/{flycast,hub,relay}-new
```

### Step 3 — upload ROM + track files

ROMs are NEVER in git. Upload from your local ROM dir:

```bash
scp /path/to/mvc2.gdi /path/to/track*.bin /path/to/track*.raw \
    root@<HOST>:/opt/maplecast/roms/
```

Ensure `MAPLECAST_ROM` in `/etc/maplecast/headless.env` matches the .gdi path.

### Step 4 — hub bootstrap + operator token

```bash
ssh root@<HOST>
mkdir -p /etc/maplecast
TOKEN=$(openssl rand -hex 32)
cat > /etc/maplecast/hub.env <<EOF
MAPLECAST_HUB_BOOTSTRAP_OPERATOR=admin
MAPLECAST_HUB_BOOTSTRAP_TOKEN=$TOKEN
EOF
chmod 600 /etc/maplecast/hub.env
chown root:maplecast /etc/maplecast/hub.env
echo "Admin token: $TOKEN"   # save this for community operators + local testing
```

### Step 5 — systemd units + drop-ins

**Main units** (from the repo, scp'd over):
- `deploy/systemd/maplecast-headless.service` → `/etc/systemd/system/maplecast-headless.service`
- `deploy/systemd/maplecast-hub.service` → `/etc/systemd/system/maplecast-hub.service`
- `maplecast-relay.service` (not in the repo — operator-created):

```ini
[Unit]
Description=MapleCast Relay — Zero Copy TA Stream Fanout + WebTransport
After=network.target

[Service]
Type=simple
User=maplecast
Group=maplecast
ExecStart=/opt/maplecast/maplecast-relay \
  --ws-upstream ws://127.0.0.1:7210 --ws-listen 0.0.0.0:7201 \
  --http-listen 127.0.0.1:7202 --max-clients 500 \
  --wt-listen 0.0.0.0:443 \
  --tls-cert /etc/letsencrypt/live/nobd.net/fullchain.pem \
  --tls-key  /etc/letsencrypt/live/nobd.net/privkey.pem
Environment=TURN_SECRET=<generate with openssl rand -hex 32>
Environment=NOBD_DB_URL=http://127.0.0.1:8000
Environment=NOBD_DB_NS=maplecast
Environment=NOBD_DB_DATABASE=arcade
Environment=NOBD_DB_USER=root
Environment=NOBD_DB_PASS=<your-surrealdb-password>
Environment=RUST_LOG=maplecast_relay=info
AmbientCapabilities=CAP_NET_BIND_SERVICE
CapabilityBoundingSet=CAP_NET_BIND_SERVICE
Restart=always
RestartSec=2
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

**Drop-ins** (critical — they wire hub registration + capabilities for optimizations):

`/etc/systemd/system/maplecast-relay.service.d/hub.conf`:

```ini
# Distributed node network registration + ultra-low-latency capability grants.
[Service]
Environment=MAPLECAST_HUB_URL=http://127.0.0.1:7220/hub/api
Environment=MAPLECAST_HUB_TOKEN=<admin-token-from-step-4>
Environment=MAPLECAST_NODE_NAME=nobd-main
Environment=MAPLECAST_NODE_REGION=us-east
Environment=MAPLECAST_PUBLIC_HOST=nobd.net
# nginx terminates TLS; proxy wss://nobd.net/{ws,play,audio} to internal.
# Without these overrides the hub stores ws://nobd.net:7201/ws which gets
# mixed-content-blocked from the https:// dashboard.
Environment=MAPLECAST_PUBLIC_RELAY_URL=wss://nobd.net/ws
Environment=MAPLECAST_PUBLIC_CONTROL_URL=wss://nobd.net/play
Environment=MAPLECAST_PUBLIC_AUDIO_URL=wss://nobd.net/audio
# ROM hash reporting — needs the same path flycast opens
Environment=MAPLECAST_ROM=/opt/maplecast/roms/mvc2.gdi
# Phase 2 ultra-low-latency: SCHED_FIFO + XDP + MEMLOCK for AF_XDP UMEM
AmbientCapabilities=CAP_NET_BIND_SERVICE CAP_NET_ADMIN CAP_BPF CAP_SYS_NICE
CapabilityBoundingSet=CAP_NET_BIND_SERVICE CAP_NET_ADMIN CAP_BPF CAP_SYS_NICE
```

`/etc/systemd/system/maplecast-headless.service.d/xdp.conf`:

```ini
# AF_XDP zero-copy input ingress + SCHED_FIFO on the hot threads
[Service]
AmbientCapabilities=CAP_NET_ADMIN CAP_BPF CAP_SYS_ADMIN CAP_SYS_NICE CAP_IPC_LOCK
CapabilityBoundingSet=CAP_NET_ADMIN CAP_BPF CAP_SYS_ADMIN CAP_SYS_NICE CAP_IPC_LOCK
LimitMEMLOCK=infinity
# Opt-in to the XDP code path in flycast
Environment=MAPLECAST_XDP=1
```

`/etc/systemd/system/maplecast-hub.service.d/replays.conf`:

```ini
# Hub writes uploaded .mcrec files here. Systemd unit has
# ProtectSystem=strict by default; this grants the write path.
[Service]
ReadWritePaths=/var/lib/maplecast
```

Enable everything:

```bash
systemctl daemon-reload
systemctl enable --now maplecast-hub maplecast-headless maplecast-relay
```

### Step 6 — nginx + TLS

Use certbot for Let's Encrypt (one-time):

```bash
certbot --nginx -d nobd.net -d www.nobd.net
```

Edit `/etc/nginx/sites-enabled/maplecast` — the full production config is
long; the critical locations you MUST have inside the main HTTPS server
block are:

```nginx
# Relay WS (broadcast TA frames)
location /ws {
    proxy_pass http://127.0.0.1:7201/;
    proxy_http_version 1.1;
    proxy_buffering off; tcp_nodelay on;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header Host $host;
    proxy_read_timeout 86400;
}

# Flycast direct control WS (player gamepad input)
location /play { proxy_pass http://127.0.0.1:7210; <WS upgrade headers> }

# Audio
location /audio { proxy_pass http://127.0.0.1:7203; <WS upgrade headers> }

# Relay HTTP (overlord + /api/join/signin/leave + /turn-cred + /metrics)
location ^~ /overlord/api/ { proxy_pass http://127.0.0.1:7202; client_max_body_size 64M; }
location ^~ /api/          { proxy_pass http://127.0.0.1:7202; }
location = /turn-cred      { proxy_pass http://127.0.0.1:7202/turn-cred; }

# Hub API (Phase 0+: distributed node registry + matchmaking + replay storage)
location ^~ /hub/api/ {
    client_max_body_size 64M;   # .mcrec uploads are 7-10 MB
    proxy_pass http://127.0.0.1:7220;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_read_timeout 30;
}

# SurrealDB
location /db/ { proxy_pass http://127.0.0.1:8000/; <WS upgrade headers> }
```

Reload: `nginx -t && systemctl reload nginx`

### Step 7 — ultra-low-latency kernel tuning

`/etc/sysctl.d/99-maplecast-lowlat.conf`:

```conf
# Spin-poll socket recv before sleeping — saves 10-100µs
net.core.busy_poll = 50
net.core.busy_read = 50

# Bigger socket buffers — absorb burst from 12kHz NOBD sticks
net.core.rmem_default = 1048576
net.core.rmem_max = 16777216
net.core.wmem_default = 1048576
net.core.wmem_max = 16777216

# UDP memory pool — let UDP buffer more under burst
net.ipv4.udp_rmem_min = 16384
net.ipv4.udp_wmem_min = 16384
net.ipv4.udp_mem = 262144 524288 1048576

# Bigger NIC backlog
net.core.netdev_max_backlog = 10000
net.core.netdev_budget = 600

# TCP latency knobs for relay WS path
net.ipv4.tcp_low_latency = 1
net.ipv4.tcp_fastopen = 3

# Let SCHED_FIFO threads actually get priority
kernel.sched_autogroup_enabled = 0
kernel.timer_migration = 0

# Hugepages for flycast's 168MB TA mirror shm + AF_XDP UMEM
vm.nr_hugepages = 256
```

Apply + mount hugetlbfs:

```bash
sysctl -p /etc/sysctl.d/99-maplecast-lowlat.conf
mountpoint -q /dev/hugepages || mount -t hugetlbfs nodev /dev/hugepages
```

Verify:

```bash
sysctl net.core.busy_poll net.core.rmem_max kernel.sched_autogroup_enabled vm.nr_hugepages
# Expected: all match the .conf
```

### Step 8 — NIC tuning (best-effort)

```bash
IFACE=$(ip route get 8.8.8.8 | awk '{print $5; exit}')
ethtool -g $IFACE         # show max ring sizes
ethtool -G $IFACE rx 256  # bump to max (virtio cap)
ethtool -C $IFACE rx-usecs 0              # disable interrupt coalescing (if supported)
ethtool -K $IFACE gro off lro off         # no aggregation — lowest latency
# Pin NIC IRQs to core 0, leaving other core for flycast
for irq in $(grep "$IFACE" /proc/interrupts | awk -F: '{print $1}' | tr -d ' '); do
    echo 1 > /proc/irq/$irq/smp_affinity
done
```

Virtio_net has limited ethtool support — some toggles no-op. Acceptable.

### Step 9 — deploy web files

```bash
cd ~/projects/maplecast-flycast
./deploy/scripts/deploy-web.sh root@<HOST>
```

This backs up + copies: `king.html`, `js/*.mjs`, `overlord/*`, `network.html`,
`replays.html`, `spectate.html`, `skin-picker.html`, `client-settings.html`.

### Step 10 — verification

```bash
# Hub is up + VPS registered itself
curl -s https://<HOST>/hub/api/input-servers | python3 -m json.tool

# ROM hash verified
curl -s https://<HOST>/hub/api/input-servers | \
  python3 -c "import json,sys; [print(n['name'], n['rom_hash'][:16]) for n in json.load(sys.stdin)['nodes']]"

# Dashboard loads
curl -sI https://<HOST>/network.html | head -2
curl -sI https://<HOST>/replays.html | head -2
curl -sI https://<HOST>/spectate.html | head -2

# Replay round-trip
./deploy/scripts/test-all.sh --hub https://<HOST>/hub/api
```

Expected logs on the VPS:

```bash
journalctl -u maplecast-relay -n 20 --no-pager | grep -i "ROM hash\|Hub registration"
# ROM hash (SHA-256): 396548fe53f9...  (MVC2 US v1.001)
# Hub registration successful — input server ... is live
```

---

## Common gotchas (all hit during the 2026-04-15 deploy — don't repeat)

1. **Backup files in `/etc/nginx/sites-enabled/`** — nginx loads EVERY file
   in sites-enabled; a `maplecast.bak-*` file there causes "duplicate
   default server" errors. Move backups to `/root/` or `/etc/nginx/backup/`.

2. **`tcp_nodelay` is NOT a sysctl** — it's a per-socket setsockopt only.
   Don't put it in `/etc/sysctl.d/`, it'll error.

3. **nginx `client_max_body_size` defaults to 1MB** — .mcrec uploads are
   7-10MB. Add `client_max_body_size 64M;` to the `/hub/api/` block or
   you'll get HTTP 413 Request Entity Too Large.

4. **Hub `ProtectSystem=strict` blocks writes** to `/var/lib/maplecast/`
   without an explicit `ReadWritePaths=` drop-in. The replay upload
   endpoint returns "write failed" silently until this is granted.

5. **Relay runs with stripped capabilities** — `CapabilityBoundingSet=CAP_NET_BIND_SERVICE`
   alone removes `CAP_DAC_OVERRIDE`, so even root can't write to dirs it
   doesn't own. The `~/.maplecast/` dir (for persistent `node_id`) must
   be owned by the running user. Default service runs as `maplecast` →
   `chown -R maplecast:maplecast /opt/maplecast/.maplecast`.

6. **`MAPLECAST_ROM` env var must be in the relay drop-in** (not just the
   flycast unit). The relay reads it to compute the ROM SHA-256 for the
   verification badge. Without it, the hub stores `rom_hash: "unknown"`
   and the dashboard shows "unknown" instead of ✓ verified.

7. **Virtio_net gotcha** — AF_XDP works but only in **copy mode**, not
   zero-copy (hardware limitation). Still faster than plain recvfrom.
   If you get a physical NIC (ENA, ixgbe, mlx5), flip the relay env to
   `MAPLECAST_XDP_ZEROCOPY=1` for true zero-copy.

8. **`systemctl stop <svc>` before binary swap** — `install` over a
   running binary returns `Text file busy`. Stop first, install, start.

9. **Hub in-memory store is volatile** — restarting the hub wipes all
   registered nodes. The relay's `hub_client.rs` auto-re-registers on
   404 heartbeat, so it recovers transparently — just wait 10s.

10. **DNS cutover** — `nobd.net` A record must point at the new VPS
    BEFORE you try to issue Let's Encrypt certs via certbot HTTP-01
    challenge. Otherwise the challenge fails.

11. **MVC2 attract-mode SH4 reset crashes flycast at ~75 seconds**
    (SIGSEGV in `bm_GetCode` at addr=`0xA0000000`). flycast's dynarec
    can't handle the SH4 soft-reset MVC2 does at the end of its
    attract loop. Works equally badly with `Dynarec.Enabled=yes`,
    `Dynarec.SafeMode=yes`, and `Dynarec.Enabled=no` (interpreter
    delays it by ~10s but still crashes). **Workaround:**
    (a) Drop a savestate at `/opt/maplecast/.local/share/flycast/<rom-basename>.state`
        — for `/opt/maplecast/roms/mvc2.gdi` that's `mvc2.state`
    (b) Add `Environment=MAPLECAST_HEADLESS_AUTOLOAD=1` via a systemd
        drop-in for `maplecast-headless.service`
    The flycast-side config `Dreamcast.AutoLoadState = yes` in emu.cfg
    is silently ignored by the headless build (separate flycast bug),
    so the env-var escape hatch is required. Once the savestate loads,
    the game boots straight to a stable screen (title/char-select) and
    never reaches the reset code path — stable indefinitely.

    Verification in journal:
    ```
    [autoload-debug] MAPLECAST_HEADLESS_AUTOLOAD=1 — forcing load
    Loaded state ver 853 from /opt/maplecast/.local/share/flycast/mvc2.state size 27785327
    ```

## Deploy scripts reference

| Script | What it deploys |
|--------|-----------------|
| `deploy/scripts/deploy-headless.sh <HOST>` | flycast headless binary + systemd |
| `deploy/scripts/deploy-web.sh <HOST>` | all web/*.html + js/*.mjs + overlord |
| `deploy/scripts/test-network.sh local-node` | Docker-based local node (for joining as community server) |
| `deploy/scripts/test-all.sh [--hub URL]` | End-to-end CLI test harness |
| `relay/deploy.sh <HOST>` | **CAREFUL** — overwrites the entire relay systemd unit. Prefer manual `scp + install + systemctl restart`. |

Operator token for joining community nodes (regenerate if rotated):

```bash
ssh root@<HOST> 'cat /etc/maplecast/hub.env | grep TOKEN'
```

---

## What's NOT in this repo

- **The /overlord admin panel internals** — the admin panel exists at https://nobd.net/overlord but its endpoint map and auth flow are not published.
- **PYQU (putyourquarterup.com) product/business roadmap** — that's a separate product layer being developed alongside MapleCast.

If you're an authorized operator who needs the private docs, contact the maintainer. If you're a contributor who wants to set up your own deployment, this file plus the public docs in `docs/` should be enough — open an issue if anything is missing.
