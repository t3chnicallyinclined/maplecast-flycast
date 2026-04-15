# Deploying MapleCast

This is the public-facing deployment overview. It tells you how to build and run MapleCast / nobd.net's flycast variants on your own hardware. Operator-specific runbooks (live VPS credentials, admin panel internals, business roadmap) live in a separate private repo and are not published here.

## What you can build from this repo

| Variant | Build target | What it is |
|---|---|---|
| **headless flycast** (recommended for servers) | `cmake -DMAPLECAST_HEADLESS=ON -B build-headless && cmake --build build-headless` | CPU-only mirror server. No GPU, no SDL, no X11, no audio. ~26 MB stripped binary. Runs MVC2 + TA mirror streaming on a $5/month VPS. See [ARCHITECTURE.md "Mode 3: Headless"](ARCHITECTURE.md) for the design rationale. |
| **GPU flycast** (for local/cab play) | `cmake -B build && cmake --build build` | Standard flycast with full rendering. Used at a physical cab or for local LAN play with sub-millisecond input latency. |
| **WASM renderer** (browser viewer) | `cd packages/renderer && bash build.sh` | Standalone WebAssembly renderer that consumes the TA mirror stream and draws MVC2 in a browser canvas. See [WASM-BUILD-GUIDE.md](WASM-BUILD-GUIDE.md). |

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

# Full Production Setup (2026-04-15) — New Dedicated-CPU VPS

> This is the canonical recipe for bringing up nobd.net from scratch.
> Covers the full stack: flycast + relay + hub + nginx + SurrealDB +
> web pages + kernel tuning + capability grants. If you're deploying
> to a brand-new box, follow this end-to-end.

## Current production target

- **VPS**: `149.28.44.118` (dedicated AMD EPYC Genoa, 2 threads, 4 GB RAM, Ubuntu 24.04)
- **DNS**: `nobd.net` → `149.28.44.118`
- **Old VPS** (`66.55.128.93`): decommissioned on 2026-04-15

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
