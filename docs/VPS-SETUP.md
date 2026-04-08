# VPS Infrastructure — nobd.net

**Host:** 66.55.128.93 (`flycast-relay`)
**OS:** Ubuntu 24.04.4 LTS, kernel 6.8.0-107-generic, x86_64
**Resources:** 2GB RAM, 47GB disk, 4.8GB swap
**SSH:** `ssh root@66.55.128.93`

> **As of 2026-04-08: THE ENTIRE nobd.net STACK RUNS HERE.** Flycast
> (headless compile-out, no GPU libs) runs as `maplecast-headless.service`
> on this box, the relay consumes from `ws://127.0.0.1:7210`, nginx
> terminates TLS at `wss://nobd.net/ws`, and SurrealDB stores player
> state. The home box is no longer in the production path — home
> bandwidth and uptime are no longer relevant to nobd.net viewers.
>
> Total footprint of the streaming stack (flycast + relay): ~322 MB RAM,
> ~12% of 2 vCPU. Massive headroom on this instance.

---

## Services

| Service | Port | Binary | Description |
|---------|------|--------|-------------|
| **nginx** | 80, 443 | `/usr/sbin/nginx` | Reverse proxy + static file server |
| **maplecast-headless** | 7100/udp, 7210/tcp (loopback) | `/usr/local/bin/flycast` | **Headless flycast server — the game** (no GPU libs, compile-out build) |
| **maplecast-relay** | 7201 | `/opt/maplecast-relay` | WebSocket TA frame relay (Rust), upstream `127.0.0.1:7210` |
| **SurrealDB** | 8000 (loopback) | `/usr/local/bin/surreal` | Player database (auth, stats, ELO) |
| **sshd** | 22 | `/usr/sbin/sshd` | SSH access |

**Critical invariant:** `maplecast-headless` listens on `127.0.0.1:7210`
(loopback only — not exposed to the internet). The relay on the same box
is the only thing that can reach it. Outside traffic hits the relay at
`:7201` via nginx → `/ws`, and the relay proxies state back to flycast
as needed.

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
**Upstream:** `ws://127.0.0.1:7210` (same-VPS headless flycast — since 2026-04-08)

### What It Does
Connects as a WebSocket client to the **same-box** headless flycast
on `127.0.0.1:7210`, receives zstd-compressed TA mirror frames (binary)
and status messages (JSON), and fans them out to up to 500 browser
clients on `:7201`. Maintains a SYNC cache so late joiners get an
instant initial state. ZCST-aware: decompresses for inspection only,
forwards original compressed bytes downstream (zero re-encode).

### Configuration

```bash
# Edit service
nano /etc/systemd/system/maplecast-relay.service

# Key flags (current live config):
#   --ws-upstream ws://127.0.0.1:7210   ← VPS-local headless flycast
#   --ws-listen 0.0.0.0:7201           ← listen port for browser clients
#   --http-listen 127.0.0.1:7202       ← /metrics, /health, /api, /turn-cred
#   --max-clients 500

# Apply changes
systemctl daemon-reload
systemctl restart maplecast-relay

# View logs
journalctl -u maplecast-relay -f

# Check status
systemctl status maplecast-relay
```

### Pre-2026-04-08 history (for context)

Before the headless deploy, the relay's `--ws-upstream` pointed at a
home IP (`ws://74.101.20.197:7200`) over the public internet. Every
frame for every spectator paid the home→VPS internet hop (~10-40ms
round trip depending on upload quality, plus variable jitter).

The 2026-04-08 migration replaced the upstream with `ws://127.0.0.1:7210`.
Frames now travel 0 internet hops between flycast and relay — they're
same-process-neighbors on the VPS. Viewer latency went from
"player↔VPS + VPS↔home" to just "player↔VPS".

If you ever need to point the relay back at a remote flycast (e.g. for
local dev where flycast runs on your laptop and the relay still lives
on the VPS), swap the upstream:
```bash
sed -i 's|ws://127\.0\.0\.1:7210|ws://YOUR_IP:7200|' /etc/systemd/system/maplecast-relay.service
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

### /overlord admin role bootstrap

The `/overlord` admin panel (see WORKSTREAM-OVERLORD) gates write
endpoints with the `admin` bool on the `player` table. The field
defaults to `false` so newly-registered users can never accidentally
gain admin access. The first admin (you) has to be flagged manually
with one SQL statement.

**Adding the field** (once, after deploying the schema change):

```bash
ssh root@66.55.128.93 'curl -sS -u root:nobd_arcade_2026 \
  -H "Surreal-NS: maplecast" -H "Surreal-DB: arcade" \
  -H "Accept: application/json" \
  --data "DEFINE FIELD admin ON player TYPE bool DEFAULT false;" \
  http://127.0.0.1:8000/sql'
```

Expected response: `[{"result":null,"status":"OK", ...}]`

**Backfill existing accounts** (DEFAULT only applies to new records,
existing accounts get `admin = NONE` until you set it):

```bash
ssh root@66.55.128.93 'curl -sS -u root:nobd_arcade_2026 \
  -H "Surreal-NS: maplecast" -H "Surreal-DB: arcade" \
  -H "Accept: application/json" \
  --data "UPDATE player SET admin = false WHERE admin = NONE;" \
  http://127.0.0.1:8000/sql'
```

**Flag your account as admin:**

```bash
ssh root@66.55.128.93 'curl -sS -u root:nobd_arcade_2026 \
  -H "Surreal-NS: maplecast" -H "Surreal-DB: arcade" \
  -H "Accept: application/json" \
  --data "UPDATE player SET admin = true WHERE username = '"'"'trisdog'"'"';" \
  http://127.0.0.1:8000/sql'
```

**Verify the state:**

```bash
ssh root@66.55.128.93 'curl -sS -u root:nobd_arcade_2026 \
  -H "Surreal-NS: maplecast" -H "Surreal-DB: arcade" \
  -H "Accept: application/json" \
  --data "SELECT username, admin FROM player;" \
  http://127.0.0.1:8000/sql' | python3 -m json.tool
```

Expected: every player has either `admin: true` or `admin: false` —
**never `None`**. None will fail-deny correctly in `check_admin()` but
`false` is more obvious-correct.

**Removing admin from a compromised/rotated account:**

```bash
ssh root@66.55.128.93 'curl -sS -u root:nobd_arcade_2026 \
  -H "Surreal-NS: maplecast" -H "Surreal-DB: arcade" \
  -H "Accept: application/json" \
  --data "UPDATE player SET admin = false WHERE username = '"'"'oldname'"'"';" \
  http://127.0.0.1:8000/sql'
```

There is **no secondary admin path** — no shared secret, no
emergency-override env var. If you lose access to your admin account,
SSH into the VPS and re-flag yourself or a different account directly
in SurrealDB. This is by design: every admin write goes through
`check_admin()` which validates against this single field, so there
is exactly one trust boundary.

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

**Relay shows `upstream disconnected` / can't connect to flycast:**
- `systemctl status maplecast-headless` — is flycast actually running?
- `ss -ltnp | grep 7210` — is flycast listening on the loopback port?
- `journalctl -u maplecast-headless -n 50` — any SH4 JIT / boot errors?
- If flycast is down, `systemctl restart maplecast-headless` — relay
  will auto-reconnect within a few seconds.
- If flycast is up but relay still can't reach it, check the relay's
  ExecStart: it should be `--ws-upstream ws://127.0.0.1:7210` (not a
  home IP — that's pre-2026-04-08 configuration).

**Headless flycast keeps crashing on startup:**
- `journalctl -u maplecast-headless -n 100` — look for the real error
- Common: ROM file missing or unreadable by the `maplecast` user →
  check `ls -l /opt/maplecast/roms/mvc2.gdi` and that ownership is
  `maplecast:maplecast`
- If the crash is `gui_newFrame` SIGSEGV, the binary is the wrong
  build — you need the compile-out binary (from `build-headless/`),
  not a GPU build. Check `ldd /usr/local/bin/flycast` returns no
  `libGL`/`libSDL`/`libX11`.

**Flycast boots but REIOS fires instead of loading the savestate:**
- The boot log shows `REIOS: Booting up` with no
  `N[SAVESTATE]: Loaded state` line before it.
- This means flycast couldn't find the savestate file at the path
  dictated by `emu.cfg` + `SavestateSlot`.
- See "The flycast config + data layout" subsection below for the
  full path chain. The TL;DR: the file must be at
  `/opt/maplecast/.local/share/flycast/mvc2_<slot>.state` where
  `<slot>` matches the `Dreamcast.SavestateSlot` value in
  `/opt/maplecast/.config/flycast/emu.cfg`.

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

---

## 9. Headless Flycast Server — `maplecast-headless.service`

**STATUS: LIVE IN PRODUCTION as of 2026-04-08.** This is the authoritative
MVC2 emulator instance for nobd.net. The relay consumes from it over
localhost (`ws://127.0.0.1:7210`), the home box is not in the loop.

**Binary:** `/usr/local/bin/flycast` — compile-out build with **zero**
libGL/libSDL/libX11/libvulkan/libcuda/libpulse/libasound/libao linkage.
26 MB stripped. Verify with
`ldd /usr/local/bin/flycast | grep -iE 'libGL|libSDL|libX11|libvulkan'`
— should return empty. If it doesn't, the wrong binary is installed.

**Service:** `/etc/systemd/system/maplecast-headless.service`
**Env file:** `/etc/maplecast/headless.env` (operator-editable; not
overwritten on redeploy)
**User:** `maplecast` (system user, `/opt/maplecast` home,
`/usr/sbin/nologin` shell)
**Working directory:** `/opt/maplecast`
**ROM path:** `/opt/maplecast/roms/mvc2.gdi` (+ `track01.bin`, `track02.raw`,
`track03.bin` in the same dir)
**Ports:** `127.0.0.1:7210/tcp` (TA mirror WS, loopback only),
`0.0.0.0:7100/udp` (input server)
**Resources:** `MemoryMax=1G`, `TasksMax=64`, `CPUQuota=200%` —
headroom is massive (live usage is ~301 MB RSS, ~24% of one core)

### The flycast config + data layout (CRITICAL — easy to get wrong)

Flycast's auto-load-savestate path is determined by a chain that's
easy to misread. On the VPS, the chain is:

1. Flycast reads **`/opt/maplecast/.config/flycast/emu.cfg`** on
   startup (the `maplecast` user's `$XDG_CONFIG_HOME/flycast/`).
2. That config contains:
   ```
   Dreamcast.AutoLoadState = yes
   Dreamcast.SavestatePath =                    # empty → fall back to user_data_dir
   Dreamcast.SavestateSlot = 1                  # load slot 1 at boot
   ```
3. With `SavestatePath` empty, flycast falls back to `user_data_dir`,
   which on Linux is `$HOME/.local/share/flycast/`. For the
   `maplecast` user that's **`/opt/maplecast/.local/share/flycast/`**.
4. The savestate filename is `<rom-basename>_<slot>.state`. With
   `mvc2.gdi` as the ROM and slot 1, that's **`mvc2_1.state`**.
5. So the auto-loaded file is
   **`/opt/maplecast/.local/share/flycast/mvc2_1.state`**.

**Do NOT put the savestate in `/opt/maplecast/savestates/`.** That
path is a leftover from the initial 2026-04-08 deploy before we
synced the local flycast config over — flycast never looks there.
The old files in that dir are kept as backups only (look for
`.apr3-backup` suffixes).

Full VPS layout of flycast-facing data:

```
/opt/maplecast/
├── .config/flycast/
│   └── emu.cfg                             ← flycast config, synced from ~/.config/flycast/emu.cfg
├── .local/share/flycast/                   ← flycast user_data_dir (reads savestates, BIOS, VMU, NVRAM here)
│   ├── dc_boot.bin                         ← 2 MB  Dreamcast BIOS
│   ├── dc_flash.bin                        ← 128 KB DC flash ROM
│   ├── dc_nvmem.bin                        ← 128 KB DC persistent state (your dreamcast settings)
│   ├── mvc2_1.state                        ← 7.7 MB **THE AUTO-LOADED SAVESTATE (slot 1)**
│   └── T1212N_vmu_save_A1.bin              ← 128 KB MVC2 VMU save (rankings, options)
├── roms/
│   ├── mvc2.gdi                            ← 87 B text manifest
│   ├── track01.bin                         ← 690 KB
│   ├── track02.raw                         ← 1.2 MB
│   └── track03.bin                         ← 1.2 GB ROM data
└── savestates/                             ← LEGACY dir from 2026-04-08 deploy, flycast ignores it
    ├── mvc2.state.apr3-backup              ← original stale save I shipped first
    └── mvc2_1.state                        ← backup copy of slot 1 (not loaded)
```

### Syncing flycast config + data from your dev box

To push a new savestate, new emu.cfg, updated BIOS, or new VMU state
to the VPS:

```bash
# From the repo root on your dev box:

# 1. Upload savestate (slot 1 is the live auto-load slot)
scp "savestates/Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!]_1.state" \
    root@66.55.128.93:/tmp/new-state.state

# 2. Upload config (optional — only if you want to overwrite)
scp /home/tris/.config/flycast/emu.cfg \
    root@66.55.128.93:/tmp/new-emu.cfg

# 3. Install + restart
ssh root@66.55.128.93 bash <<'EOF'
install -m 0644 -o maplecast -g maplecast \
    /tmp/new-state.state \
    /opt/maplecast/.local/share/flycast/mvc2_1.state

# For config: patch the paths BEFORE installing so we don't overwrite
# with a path that points at your home dir
sed -e 's|^Dreamcast.SavestatePath = .*|Dreamcast.SavestatePath = |' \
    -e 's|^Dreamcast.SavestateSlot = .*|Dreamcast.SavestateSlot = 1|' \
    -e 's|^UploadCrashLogs = .*|UploadCrashLogs = no|' \
    -e 's|^DiscordPresence = .*|DiscordPresence = no|' \
    /tmp/new-emu.cfg > /opt/maplecast/.config/flycast/emu.cfg
chown maplecast:maplecast /opt/maplecast/.config/flycast/emu.cfg

rm /tmp/new-state.state /tmp/new-emu.cfg
systemctl restart maplecast-headless

# Verify the savestate actually loaded (look for "[SAVESTATE]: Loaded")
sleep 3
journalctl -u maplecast-headless --no-pager -n 30 | grep -E "SAVESTATE|REIOS"
EOF
```

**Expected verification output:**
```
N[SAVESTATE]: Loaded state ver 853 from /opt/maplecast/.local/share/flycast/mvc2_1.state size 27785327
```

If you see `REIOS: Booting up` INSTEAD of the SAVESTATE line, the
savestate wasn't found. Double-check:
- Path is `/opt/maplecast/.local/share/flycast/mvc2_1.state` (NOT
  `/opt/maplecast/savestates/...`)
- Filename basename matches the ROM basename (`mvc2.gdi` → `mvc2_*.state`)
- `SavestateSlot` in the VPS config matches the `_N` suffix on the
  savestate file
- File is owned by `maplecast:maplecast` and readable

### Current live configuration

```bash
systemctl cat maplecast-headless | grep -E '^(Environment|ExecStart)='
```

Should show (with `MAPLECAST_SERVER_PORT=7210` — NOT 7200, that's the
old default in the unit file that gets patched per-deploy):

```
Environment=MAPLECAST=1
Environment=MAPLECAST_MIRROR_SERVER=1
Environment=MAPLECAST_HEADLESS=1
Environment=MAPLECAST_PORT=7100
Environment=MAPLECAST_SERVER_PORT=7210
Environment=MAPLECAST_ROM=/opt/maplecast/roms/mvc2.gdi
ExecStart=/usr/local/bin/flycast ${MAPLECAST_ROM}
```

### Redeploying the binary

When you change flycast code and want to push a new binary to the VPS,
from the repo root on your dev box:

```bash
cmake --build build-headless -- -j$(nproc)
./deploy/scripts/deploy-headless.sh root@66.55.128.93
```

The deploy script:

1. Rebuilds locally with the existing `build-headless/` cmake config
2. Runs `ldd` sanity check on the fresh binary — bails loudly if
   `libGL`/`libSDL`/`libX11`/`libvulkan`/`libcuda` shows up (means the
   CMake gate broke; don't deploy)
3. SCPs the binary + systemd unit + env file to `/tmp/headless-deploy/`
4. Installs the binary to `/usr/local/bin/flycast` and restarts the
   service
5. Verifies `systemctl is-active` + port listening

**ROM and savestate are NOT re-uploaded by the deploy script** — they
live on the VPS and don't change between deploys. To update them, see
the "Syncing flycast config + data from your dev box" subsection
below, which has the correct paths (hint: savestates live under
`/opt/maplecast/.local/share/flycast/`, NOT `/opt/maplecast/savestates/`
— the latter is a legacy stub from the initial deploy).

### Installed runtime dependencies

```bash
# Apt packages required (Ubuntu 24.04 Noble — uses t64 variants):
dpkg -s libcurl4t64 libxdp1 libbpf1 libgomp1 libzip4t64 zlib1g ca-certificates
```

All are installed. `libxdp1` + `libbpf1` were added during the
2026-04-08 deploy; the rest were already present for other services.
XDP fails silently to plain UDP on kernels that don't support it, so
even if libxdp is missing the input server still works on standard UDP.

### How flycast and the relay talk (same-box loopback)

```
maplecast-headless (pid ~68450)
     │
     │ bind 127.0.0.1:7210
     │ TA mirror WS publish
     │
     ▼
maplecast-relay (pid ~68620)
     │ --ws-upstream ws://127.0.0.1:7210
     │ connects as WS client on startup, auto-reconnects on drop
     │ fanouts to 0.0.0.0:7201
     │
     ▼
nginx /ws → 127.0.0.1:7201 → public wss://nobd.net/ws
```

When you restart `maplecast-headless`, the relay reconnects within
a second or two (seen: 42 ms reconnection on a clean restart).
Existing browser clients on the relay side drop their WS briefly
and reconnect via the usual reconnect logic in `web/js/ws-connection.mjs`.

### Verification

```bash
ssh root@66.55.128.93

# Service alive?
systemctl status maplecast-headless

# Ports listening?
ss -ltnp | grep -E '7100|7210'

# Logs (follow live)
journalctl -u maplecast-headless -f

# Frame count over 5s (needs python3 + websockets on the box)
python3 -c "
import asyncio, time, websockets
async def main():
    n=0; b=0
    async with websockets.connect('ws://127.0.0.1:7210', max_size=None) as ws:
        end = time.time() + 5
        while time.time() < end:
            try: m = await asyncio.wait_for(ws.recv(), timeout=1.0)
            except asyncio.TimeoutError: break
            if isinstance(m, bytes): n += 1; b += len(m)
    print(f'{n} frames, {b} bytes, {n/5:.1f} fps')
asyncio.run(main())
"
# Expected: ~60 fps sustained
```

### Common issues

**`ldd` check in deploy script fails:**
Your local build still has USE_OPENGL on somehow. Reconfigure:
`rm -rf build-headless && cmake -B build-headless -DMAPLECAST_HEADLESS=ON ..`

**`systemctl status` shows exit 139 (SIGSEGV):**
Check `journalctl -u maplecast-headless -n 50` — if it's in `gui_newFrame`,
the Phase 4 compile-time gate fix isn't in your binary. Rebuild from
commit `93ceeff9d` or later on the `headless-server` branch.

**No frames on `:7210` but port is listening:**
Likely the ROM failed to load. Check logs for `[BOOT]` errors. Common
causes: ROM not at `/opt/maplecast/roms/mvc2.gdi` or wrong permissions
(should be readable by the `maplecast` user, so owned
`maplecast:maplecast` or world-readable).

**Relay shows `upstream disconnected` repeatedly:**
Flycast is probably not listening. `systemctl status maplecast-headless`
— if it's dead, check its journal. If the relay upstream is pointing
at something other than `ws://127.0.0.1:7210` (e.g. still the old home
IP from before 2026-04-08), update the ExecStart in
`/etc/systemd/system/maplecast-relay.service` per section 2.

**AF_XDP fallback warning in logs:**
Harmless. XDP requires kernel NIC driver support; on most VPS instances
the NIC is virtual and XDP falls back to plain UDP on `:7100`. Both
paths work.
