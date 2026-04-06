# VPS Infrastructure — nobd.net

**Host:** 66.55.128.93 (`flycast-relay`)
**OS:** Ubuntu 24.04.4 LTS, kernel 6.8.0-107-generic, x86_64
**Resources:** 2GB RAM, 47GB disk (19% used), 4.8GB swap
**SSH:** `ssh root@66.55.128.93`

---

## Services

| Service | Port | Binary | Description |
|---------|------|--------|-------------|
| **nginx** | 80, 443 | `/usr/sbin/nginx` | Reverse proxy + static file server |
| **maplecast-relay** | 7201 | `/opt/maplecast-relay` | WebSocket TA frame relay (Rust) |
| **SurrealDB** | 8000 | `/usr/local/bin/surreal` | Player database (auth, stats, ELO) |
| **sshd** | 22 | `/usr/sbin/sshd` | SSH access |

---

## 1. nginx

**Config:** `/etc/nginx/sites-enabled/maplecast`
**Web root:** `/var/www/maplecast/`
**SSL:** Let's Encrypt (auto-renew via certbot)
**Cert expires:** 2026-07-05

### Routes

| Path | Backend | Purpose |
|------|---------|---------|
| `/` | Static files | king.html, JS modules, WASM renderer |
| `/ws` | `127.0.0.1:7201` | WebSocket relay (binary frames + JSON) |
| `/db/*` | `127.0.0.1:8000` | SurrealDB HTTP API (with CORS) |
| `*.wasm` | Static | WASM files with correct MIME type |
| `*.mjs` | Static | ES module files |

### Common Operations

```bash
# Test config
nginx -t

# Reload (no downtime)
systemctl reload nginx

# View logs
journalctl -u nginx -f

# Edit config
nano /etc/nginx/sites-enabled/maplecast

# Renew SSL cert
certbot renew
```

### Key Config Details
- `proxy_buffering off` + `tcp_nodelay on` on /ws for lowest latency
- `proxy_read_timeout 86400` (24h) for long-lived WebSocket connections
- CORS headers on /db/ for browser SurrealDB access
- HTTP → HTTPS redirect managed by Certbot

---

## 2. MapleCast Relay

**Binary:** `/opt/maplecast-relay`
**Service:** `/etc/systemd/system/maplecast-relay.service`
**Upstream:** `ws://74.101.20.197:7200` (home server flycast)

### What It Does
Connects to the home flycast server via WebSocket, receives TA mirror frames (binary) and status messages (JSON), and fans them out to up to 500 browser clients. Zero-copy Rust relay.

### Configuration

```bash
# Edit service
nano /etc/systemd/system/maplecast-relay.service

# Key flags:
#   --ws-upstream ws://HOME_IP:7200    ← flycast server address
#   --ws-listen 0.0.0.0:7201          ← listen port for clients
#   --max-clients 500                 ← max concurrent viewers

# Apply changes
systemctl daemon-reload
systemctl restart maplecast-relay

# View logs
journalctl -u maplecast-relay -f

# Check status
systemctl status maplecast-relay
```

### Changing Home Server IP
If your home IP changes:
```bash
sed -i 's/74.101.20.197/NEW_IP/' /etc/systemd/system/maplecast-relay.service
systemctl daemon-reload
systemctl restart maplecast-relay
```

---

## 3. SurrealDB

**Version:** 3.1.0-nightly
**Binary:** `/usr/local/bin/surreal`
**Service:** `/etc/systemd/system/surrealdb.service`
**Data:** `/opt/maplecast/db/arcade/` (SurrealKV persistent storage)
**Schema:** `/opt/maplecast/schema.surql`

### Database Structure

| Table | Purpose |
|-------|---------|
| `player` | Username, argon2 password, ELO rating, W/L, streaks, combos |
| `match` | Full match records (chars, HP, combos, inputs, finish type) |
| `char_stats` | Per-player per-character stats |
| `team_stats` | Per-player per-team-composition stats |
| `h2h` | Head-to-head records between players |
| `badge` | Achievement definitions (combo monster, streak king, etc.) |
| `earned` | Player → badge relations |
| `stick` | Registered NOBD hardware sticks |
| `owns` | Player → stick relations |
| `played` | Player → match relations |
| `game_event` | Significant gameplay moments |

### Common Operations

```bash
# Query the database
curl -s -X POST \
  -H 'Accept: application/json' \
  -H 'Surreal-NS: maplecast' \
  -H 'Surreal-DB: arcade' \
  -H 'Authorization: Basic cm9vdDpyb290' \
  http://localhost:8000/sql \
  -d "SELECT username, rating, wins, losses FROM player ORDER BY rating DESC LIMIT 10"

# Import/reload schema
echo 'OPTION IMPORT;' | cat - /opt/maplecast/schema.surql > /tmp/import.surql
surreal import -e http://localhost:8000 --user root --pass root \
  --ns maplecast --db arcade /tmp/import.surql

# Backup database
cp -r /opt/maplecast/db/arcade /opt/maplecast/db/arcade.bak.$(date +%Y%m%d)

# Restart
systemctl restart surrealdb

# View logs
journalctl -u surrealdb -f

# Interactive SQL (via curl)
curl -X POST -H 'Surreal-NS: maplecast' -H 'Surreal-DB: arcade' \
  -H 'Authorization: Basic cm9vdDpyb290' \
  http://localhost:8000/sql -d "INFO FOR DB"
```

### Auth Credentials
- **DB Root:** `root` / `root` (Basic auth: `cm9vdDpyb290`)
- **Namespace:** `maplecast`
- **Database:** `arcade`

### Updating Schema
1. Edit `web/schema.surql` on dev machine
2. Copy to VPS: `scp web/schema.surql root@66.55.128.93:/opt/maplecast/schema.surql`
3. Import: see command above

---

## 4. Deploying Web Updates

```bash
# From dev machine — deploy all files
scp web/king.html root@66.55.128.93:/var/www/maplecast/
scp web/js/*.mjs root@66.55.128.93:/var/www/maplecast/js/
scp web/relay.js root@66.55.128.93:/var/www/maplecast/
scp web/renderer.mjs root@66.55.128.93:/var/www/maplecast/
scp web/renderer.wasm root@66.55.128.93:/var/www/maplecast/

# Or all at once
rsync -avz --include='*.html' --include='*.mjs' --include='*.js' \
  --include='*.wasm' --include='js/***' --exclude='*' \
  web/ root@66.55.128.93:/var/www/maplecast/
```

No nginx restart needed — static files are served immediately.

---

## 5. SSL Certificate

- **Provider:** Let's Encrypt (Certbot)
- **Domains:** nobd.net, www.nobd.net
- **Expires:** 2026-07-05
- **Auto-renew:** Yes (certbot timer)

```bash
# Check cert status
certbot certificates

# Force renew
certbot renew --force-renewal

# Check auto-renew timer
systemctl list-timers | grep certbot
```

---

## 6. Firewall / Ports

| Port | Protocol | Access | Service |
|------|----------|--------|---------|
| 22 | TCP | Public | SSH |
| 80 | TCP | Public | HTTP → HTTPS redirect |
| 443 | TCP | Public | HTTPS (nginx) |
| 7201 | TCP | localhost only (nginx proxies) | Relay WebSocket |
| 8000 | TCP | localhost only (nginx proxies) | SurrealDB |

Note: Ports 7201 and 8000 are bound to 0.0.0.0 but should be firewalled to localhost-only if not already. Browser clients access them through nginx proxy (`/ws` and `/db/`).

---

## 7. Monitoring

```bash
# All services status
systemctl status nginx maplecast-relay surrealdb

# Live relay connections
journalctl -u maplecast-relay -f | grep -i connect

# SurrealDB queries
journalctl -u surrealdb -f

# Disk usage
df -h /
du -sh /opt/maplecast/db/

# Memory
free -h

# Active connections
ss -s
```

---

## 8. Troubleshooting

**Relay can't connect to home server:**
- Check home IP hasn't changed: `curl ifconfig.me` on home machine
- Update relay: see section 2

**SurrealDB won't start:**
- Check logs: `journalctl -u surrealdb -n 50`
- Common: data directory permissions, corrupted DB
- Nuclear: `rm -rf /opt/maplecast/db/arcade && systemctl restart surrealdb` (loses all data)

**SSL cert expired:**
- `certbot renew`
- `systemctl reload nginx`

**WebSocket 502 errors:**
- Relay might be down: `systemctl status maplecast-relay`
- Restart: `systemctl restart maplecast-relay`

**Site not loading:**
- Check nginx: `nginx -t && systemctl status nginx`
- Check files exist: `ls -la /var/www/maplecast/`
