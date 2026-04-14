#!/usr/bin/env bash
# ============================================================================
# test-network.sh — Distributed node network deployment + local node
#
# Real-world test setup:
#   1. VPS hosts the hub (public via nginx /hub/api)
#   2. VPS relay registers itself as Node #1
#   3. Your local machine runs Node #2 via Docker, pointing at VPS hub
#   4. Dashboard at https://nobd.net/network.html shows both
#
# Usage:
#   ./deploy/scripts/test-network.sh deploy-hub <user@vps>
#       Build hub locally, scp to VPS, install systemd unit
#
#   ./deploy/scripts/test-network.sh deploy-relay <user@vps>
#       Build relay locally (with hub_client), scp to VPS, restart service
#
#   ./deploy/scripts/test-network.sh deploy-nginx <user@vps>
#       Show nginx config snippet to add for /hub/api proxy
#
#   ./deploy/scripts/test-network.sh deploy-dashboard <user@vps>
#       Copy web/network.html to /var/www/maplecast/
#
#   ./deploy/scripts/test-network.sh local-node
#       Start a node on this machine (Docker), registered with VPS hub
#       Requires: ROM=/path/to/mvc2.gdi, HUB_TOKEN=<operator-token>
#
#   ./deploy/scripts/test-network.sh status
#       curl the VPS hub and show what's registered
#
#   ./deploy/scripts/test-network.sh stop-local
#       Stop the local Docker node
#
# Environment:
#   VPS_HUB_URL=https://nobd.net/hub/api    Public hub URL (default)
#   ROM=/path/to/mvc2.gdi                   For local-node command
#   HUB_TOKEN=<operator-token>              For local-node command
#   NODE_NAME=local-dev                     Optional, default "local-dev"
# ============================================================================

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NODE_CONTAINER="maplecast-local-node"
DOCKER_IMAGE="ghcr.io/t3chnicallyinclined/maplecast-node:latest"

VPS_HUB_URL="${VPS_HUB_URL:-https://nobd.net/hub/api}"
NODE_NAME="${NODE_NAME:-local-dev}"

# ── Pretty output ─────────────────────────────────────────────────────
c_reset='\033[0m'; c_cyan='\033[36m'; c_green='\033[32m'; c_yellow='\033[33m'; c_red='\033[31m'
say()  { printf "${c_cyan}[test-net]${c_reset} %s\n" "$*"; }
ok()   { printf "${c_green}[ok]${c_reset} %s\n" "$*"; }
warn() { printf "${c_yellow}[!]${c_reset} %s\n" "$*"; }
die()  { printf "${c_red}[err]${c_reset} %s\n" "$*" >&2; exit 1; }

# ────────────────────────────────────────────────────────────────────
# VPS DEPLOY: HUB
# ────────────────────────────────────────────────────────────────────

cmd_deploy_hub() {
  local target="${1:-}"
  [[ -n "$target" ]] || die "Usage: $0 deploy-hub <user@vps>"

  say "Building hub release binary..."
  (cd "$REPO/hub" && cargo build --release)

  local bin="$REPO/hub/target/release/maplecast-hub"
  [[ -x "$bin" ]] || die "Build failed — binary not found"

  say "Uploading hub binary to $target..."
  scp "$bin" "$target:/tmp/maplecast-hub"

  say "Installing on VPS..."
  ssh "$target" "bash -se" <<'REMOTE'
set -e
sudo install -m 0755 /tmp/maplecast-hub /usr/local/bin/maplecast-hub
rm -f /tmp/maplecast-hub

# Install systemd unit if missing
if [ ! -f /etc/systemd/system/maplecast-hub.service ]; then
  echo "[remote] systemd unit missing — please scp deploy/systemd/maplecast-hub.service first"
  exit 1
fi

# Create env file with bootstrap operator if missing
if [ ! -f /etc/maplecast/hub.env ]; then
  sudo mkdir -p /etc/maplecast
  TOKEN=$(openssl rand -hex 32)
  sudo tee /etc/maplecast/hub.env > /dev/null <<EOF
MAPLECAST_HUB_BOOTSTRAP_OPERATOR=admin
MAPLECAST_HUB_BOOTSTRAP_TOKEN=$TOKEN
EOF
  sudo chmod 600 /etc/maplecast/hub.env
  echo "[remote] Created /etc/maplecast/hub.env with new operator token"
  echo "[remote] TOKEN: $TOKEN"
  echo "[remote] Save this — you need it for node registration"
fi

sudo systemctl daemon-reload
sudo systemctl enable --now maplecast-hub.service
sleep 1
sudo systemctl is-active maplecast-hub.service && echo "[remote] hub is active"
REMOTE

  say "Uploading systemd unit..."
  scp "$REPO/deploy/systemd/maplecast-hub.service" "$target:/tmp/maplecast-hub.service"
  ssh "$target" "sudo install -m 0644 /tmp/maplecast-hub.service /etc/systemd/system/ && rm /tmp/maplecast-hub.service && sudo systemctl daemon-reload && sudo systemctl restart maplecast-hub.service"

  ok "Hub deployed. SSH in and check: sudo cat /etc/maplecast/hub.env"
  echo "  Save the bootstrap token — local nodes need it."
}

# ────────────────────────────────────────────────────────────────────
# VPS DEPLOY: RELAY (with hub_client)
# ────────────────────────────────────────────────────────────────────

cmd_deploy_relay() {
  local target="${1:-}"
  [[ -n "$target" ]] || die "Usage: $0 deploy-relay <user@vps>"

  say "Building relay release binary..."
  (cd "$REPO/relay" && cargo build --release)

  local bin="$REPO/relay/target/release/maplecast-relay"
  [[ -x "$bin" ]] || die "Build failed — binary not found"

  say "Uploading relay binary to $target..."
  scp "$bin" "$target:/tmp/maplecast-relay"

  say "Installing on VPS + restarting..."
  ssh "$target" "bash -se" <<'REMOTE'
set -e
sudo install -m 0755 /tmp/maplecast-relay /usr/local/bin/maplecast-relay
rm -f /tmp/maplecast-relay
sudo systemctl restart maplecast-relay.service 2>/dev/null || \
  echo "[remote] No maplecast-relay.service — start it however you currently run the relay"
REMOTE

  ok "Relay deployed."
  echo
  echo "To enable hub registration, add these env vars to the relay's environment:"
  echo "  MAPLECAST_HUB_URL=http://127.0.0.1:7220/hub/api"
  echo "  MAPLECAST_HUB_TOKEN=<bootstrap-token-from-hub.env>"
  echo "  MAPLECAST_NODE_NAME=nobd-main"
  echo "  MAPLECAST_NODE_REGION=us-east"
  echo "  MAPLECAST_PUBLIC_HOST=nobd.net"
}

# ────────────────────────────────────────────────────────────────────
# NGINX SNIPPET
# ────────────────────────────────────────────────────────────────────

cmd_deploy_nginx() {
  cat <<'EOF'
Add this to your nginx server block on the VPS, then reload nginx:

  # Hub API — distributed node registry
  location /hub/api/ {
      proxy_pass http://127.0.0.1:7220/hub/api/;
      proxy_http_version 1.1;
      proxy_set_header Host $host;
      proxy_set_header X-Real-IP $remote_addr;
      proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
      proxy_set_header X-Forwarded-Proto $scheme;
  }

After saving:
  sudo nginx -t && sudo systemctl reload nginx

Verify:
  curl https://nobd.net/hub/api/nodes
EOF
}

# ────────────────────────────────────────────────────────────────────
# VPS DEPLOY: DASHBOARD HTML
# ────────────────────────────────────────────────────────────────────

cmd_deploy_dashboard() {
  local target="${1:-}"
  [[ -n "$target" ]] || die "Usage: $0 deploy-dashboard <user@vps>"

  say "Uploading network.html to $target..."
  scp "$REPO/web/network.html" "$target:/tmp/network.html"
  ssh "$target" "sudo install -m 0644 /tmp/network.html /var/www/maplecast/network.html && rm /tmp/network.html"

  ok "Dashboard live at: https://nobd.net/network.html"
}

# ────────────────────────────────────────────────────────────────────
# LOCAL: RUN A NODE
# ────────────────────────────────────────────────────────────────────

cmd_local_node() {
  [[ -n "${ROM:-}" ]] || die "ROM=/path/to/mvc2.gdi required"
  [[ -f "$ROM" ]] || die "ROM not found: $ROM"
  [[ -n "${HUB_TOKEN:-}" ]] || die "HUB_TOKEN=<operator-token> required"
  command -v docker >/dev/null || die "Docker not installed"

  if docker ps -a --format '{{.Names}}' | grep -q "^${NODE_CONTAINER}$"; then
    say "Removing existing local node container..."
    docker rm -f "$NODE_CONTAINER" >/dev/null
  fi

  say "Pulling latest image..."
  docker pull "$DOCKER_IMAGE"

  # Mount the ROM's directory so .gdi can find its track*.bin sidecars
  ROM_DIR="$(cd "$(dirname "$ROM")" && pwd)"
  ROM_FILE="$(basename "$ROM")"

  say "Starting local node, registering with $VPS_HUB_URL..."
  say "  Mounting $ROM_DIR → /data (readonly)"
  say "  ROM file: $ROM_FILE"
  # --shm-size=256m: flycast needs ~168MB in /dev/shm for the TA mirror
  # ring buffer (RingHeader + 32MB BRAIN + 128MB RING). Default 64MB
  # SIGBUSes the SH4 process the moment it touches the mapping.
  docker run -d --name "$NODE_CONTAINER" --net=host --shm-size=256m \
    -v "$ROM_DIR:/data:ro" \
    -e MAPLECAST_HUB_URL="$VPS_HUB_URL" \
    -e MAPLECAST_HUB_TOKEN="$HUB_TOKEN" \
    -e MAPLECAST_NODE_NAME="$NODE_NAME" \
    -e MAPLECAST_NODE_REGION="local" \
    "$DOCKER_IMAGE" \
    "/data/$ROM_FILE"

  ok "Local node started."
  echo "  Tail logs:    docker logs -f $NODE_CONTAINER"
  echo "  Verify:       $0 status"
  echo "  Dashboard:    https://nobd.net/network.html"
  echo "  Stop:         $0 stop-local"
}

cmd_stop_local() {
  if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${NODE_CONTAINER}$"; then
    say "Stopping local node..."
    docker rm -f "$NODE_CONTAINER" >/dev/null
    ok "Stopped."
  else
    warn "No local node running."
  fi
}

# ────────────────────────────────────────────────────────────────────
# STATUS
# ────────────────────────────────────────────────────────────────────

cmd_status() {
  echo
  say "Local Docker node:"
  if docker ps --format '{{.Names}}\t{{.Status}}' 2>/dev/null | grep "^${NODE_CONTAINER}"; then :; else
    warn "  not running"
  fi

  echo
  say "VPS hub status (${VPS_HUB_URL}):"
  if ! curl -sf "$VPS_HUB_URL/dashboard/stats" -o /tmp/_hub_stats.json; then
    die "Hub unreachable at $VPS_HUB_URL"
  fi
  python3 -m json.tool < /tmp/_hub_stats.json
  rm -f /tmp/_hub_stats.json

  echo
  say "Registered nodes:"
  curl -s "$VPS_HUB_URL/nodes" | python3 -c '
import json, sys
data = json.load(sys.stdin)
nodes = data.get("nodes", [])
if not nodes:
    print("  (none registered yet)")
else:
    print(f"  {\"node_id\":36}  {\"name\":18}  {\"status\":10}  {\"location\"}")
    print("  " + "-"*90)
    for n in nodes:
        geo = n.get("geo") or {}
        loc = f"{geo.get(\"city\",\"?\")}, {geo.get(\"country\",\"?\")}" if geo else "?"
        print(f"  {n[\"node_id\"]:36}  {n[\"name\"]:18}  {n[\"status\"]:10}  {loc}")
'
  echo
  echo "Dashboard: https://nobd.net/network.html"
}

# ────────────────────────────────────────────────────────────────────
# DISPATCH
# ────────────────────────────────────────────────────────────────────

case "${1:-}" in
  deploy-hub)        shift; cmd_deploy_hub "$@" ;;
  deploy-relay)      shift; cmd_deploy_relay "$@" ;;
  deploy-nginx)      cmd_deploy_nginx ;;
  deploy-dashboard)  shift; cmd_deploy_dashboard "$@" ;;
  local-node)        cmd_local_node ;;
  stop-local)        cmd_stop_local ;;
  status)            cmd_status ;;
  *)
    cat <<EOF
Usage: $0 <command> [args]

VPS setup (run these IN ORDER, once):
  $0 deploy-hub user@nobd.net           # build + install hub binary + systemd
  $0 deploy-nginx                       # prints nginx config to add manually
  $0 deploy-relay user@nobd.net         # build + install new relay (with hub_client)
  $0 deploy-dashboard user@nobd.net     # copy web/network.html to /var/www/maplecast/

After deploy-hub, ssh in and grab the bootstrap token:
  ssh user@nobd.net 'sudo cat /etc/maplecast/hub.env'

Then add hub vars to the VPS relay's env file (manual step) and restart the relay
so the VPS itself becomes Node #1.

Local node (any time after VPS is set up):
  ROM=~/roms/mvc2.gdi HUB_TOKEN=<token> $0 local-node

Verification:
  $0 status                             # curl VPS hub, show registered nodes
  $0 stop-local                         # stop the local Docker node
EOF
    exit 1
    ;;
esac
