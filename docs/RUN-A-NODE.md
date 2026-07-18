# Run a MapleCast node

**Anyone with a copy of Marvel vs Capcom 2 can host a node.** A node runs the game
headless and streams it as a thin state wire; players near you get a faster match.
Registration is open — no approval, no account. Your node shows up on the
[live map](https://play.nobd.net/network.html) within seconds.

## What you need

- **A Linux box.** A cheap VPS is easiest (public IP, always on). A spare home
  machine works too — see *Reachability* below.
- **Docker.**
- **Your own MvC2 GDI.** It never leaves your box; the node only reports its
  SHA-256 so the map can show a "verified ROM" badge.
- **Open ports:** `7201/tcp` (the stream) and `7100/udp` (input).

## One command

```bash
docker run -d --name maplecast-node --restart unless-stopped \
  -p 7201:7201/tcp -p 7100:7100/udp \
  -v /path/to/mvc2.gdi:/data/mvc2.gdi:ro \
  -v maplecast-node:/opt/maplecast/.maplecast \
  -e MAPLECAST_NODE_NAME="my-node" \
  -e MAPLECAST_NODE_REGION="us-east" \
  ghcr.io/t3chnicallyinclined/maplecast-node:latest
```

That's it. The container:
1. boots flycast headless on your ROM,
2. generates a persistent node identity (kept in the `maplecast-node` volume),
3. registers with the hub at `nobd.net/hub`, geolocates by IP, and starts hosting.

Check `docker logs maplecast-node` — you'll see `Hub registration successful`,
then your node on the map.

### Build it yourself (instead of the prebuilt image)

```bash
git clone <this repo> && cd maplecast-flycast
docker build -f Dockerfile.node -t maplecast-node .
# then the same `docker run ...` with maplecast-node:latest
```

## Reachability

- **Native clients** (the desktop client) reach your node over UDP + WS as soon
  as `7201/tcp` and `7100/udp` are open to the internet.
- **Browser viewers** need `wss://` — i.e. a **public IP + a domain + TLS**. Put
  nginx (or Caddy) with a cert in front of `:7201`, and set
  `-e MAPLECAST_PUBLIC_RELAY_URL="wss://your-domain/ws"`. Without that, browsers
  can't connect but native clients still can.
- **Home box behind NAT:** forward `7201/tcp` + `7100/udp` on your router. Browser
  serving still needs the domain + TLS above.

## Config (all optional)

| Env | Default | What |
|---|---|---|
| `MAPLECAST_NODE_NAME` | `node-<hostname>` | Shown on the map |
| `MAPLECAST_NODE_REGION` | `auto` | e.g. `us-east`, `eu-west` |
| `MAPLECAST_PUBLIC_HOST` | auto-detected | Override your public IP |
| `MAPLECAST_PUBLIC_RELAY_URL` | — | `wss://…/ws` when behind TLS |
| `MAPLECAST_HUB_TOKEN` | auto-generated | Your node's ownership secret |

The token is generated on first run and persisted in the volume. It's what proves
*you* own your node — only that token can update or remove it. Keep the volume and
your node keeps its spot; lose it and you simply register a fresh node.

## How open, and how fair

Hosting is **open and casual** — you choose whose node you play on, and the ROM
badge tells you who's running the canonical game. Because a node is authoritative,
**ranked play stays on trusted nodes**; community/casual matches can run anywhere.
Registration is rate-limited (a handful of new nodes per IP per hour) to keep the
map clean.

*It's state, not video — your node streams the game as data and the browser draws
the pixels. Tiny bandwidth, byte-exact.*
