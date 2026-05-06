#!/bin/bash
# ============================================================================
# MAPLECAST OPTION 6 LOOKUP-RENDERER SANDBOX — Parallel Deploy
#
# Usage:
#   ./deploy/scripts/deploy-lookup.sh <VPS_HOST>
#
# Example:
#   ./deploy/scripts/deploy-lookup.sh root@149.28.44.118
#
# WHAT THIS DEPLOYS
#   A second flycast instance ("flycast-lookup") that runs alongside the
#   production maplecast-headless service WITHOUT touching any of these
#   production paths:
#
#     /usr/local/bin/flycast              ← prod binary (untouched)
#     /etc/systemd/system/maplecast-headless.service   (untouched)
#     /etc/maplecast/headless.env         (untouched)
#     /opt/maplecast/                     (read-only access to ROM only)
#     /var/www/maplecast/                 (untouched)
#     ports 7100, 7200, 7201, 7211, etc.  (untouched)
#
# WHAT THIS CREATES
#     /usr/local/bin/flycast-lookup       ← new binary
#     /etc/systemd/system/maplecast-lookup.service       (new unit)
#     /etc/maplecast/lookup.env           (new env file)
#     /opt/maplecast-lookup/              (new top-level dir)
#     /var/log/maplecast-lookup/          (new log dir)
#     user maplecast-lookup               (new system user)
#     ports 7150 (input udp), 7250 (mirror ws), 7251 (control ws)
#
# BUILD STRATEGY
#   The headless build is Linux-only, so we BUILD ON THE VPS itself:
#     1. Push the feature branch to origin (operator does this beforehand).
#     2. Clone/pull the branch into /opt/maplecast-lookup/src on the VPS.
#     3. cmake -DMAPLECAST_HEADLESS=ON -DMAPLECAST_LOOKUP=ON.
#     4. Strip + install to /usr/local/bin/flycast-lookup.
#     5. ldd sanity check (must NOT pull libGL/libSDL/libX11/libvulkan/libcuda).
#
# PRE-REQS ON THE VPS
#   apt-get install -y build-essential cmake git ninja-build pkg-config \
#       libcurl4-openssl-dev libxdp-dev libbpf-dev libzstd-dev liblua5.4-dev \
#       libomp-dev libzip-dev zlib1g-dev libsdl2-dev libvulkan-dev
#   (libsdl2-dev/libvulkan-dev are needed by flycast's CMakeLists header
#   probes even for headless builds; the actual binary won't link them.)
#
# ROLLBACK
#   systemctl stop maplecast-lookup
#   systemctl disable maplecast-lookup
#   rm /etc/systemd/system/maplecast-lookup.service
#   rm /usr/local/bin/flycast-lookup
#   rm -rf /opt/maplecast-lookup /var/log/maplecast-lookup /etc/maplecast/lookup.env
#   userdel maplecast-lookup
#   systemctl daemon-reload
#
# Production is NEVER touched by this rollback.
# ============================================================================

set -euo pipefail

VPS_HOST="${1:?Usage: ./deploy-lookup.sh <VPS_HOST>}"
BRANCH="${BRANCH:-feat/option6-lookup-renderer}"
REPO_URL="${REPO_URL:-https://github.com/t3chnicallyinclined/maplecast-flycast.git}"

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "$SCRIPT_DIR/../.." &> /dev/null && pwd )"

echo "============================================"
echo "  MAPLECAST OPTION 6 LOOKUP-RENDERER DEPLOY"
echo "  Target host:    $VPS_HOST"
echo "  Source branch:  $BRANCH"
echo "  Source repo:    $REPO_URL"
echo ""
echo "  Production maplecast-headless will NOT be touched."
echo "  Parallel-instance ports: 7150/udp, 7250/tcp, 7251/tcp"
echo "============================================"
echo ""

# Pre-flight: confirm SSH works and confirm prod is healthy before we
# start. If we can't reach the VPS, fail loud here, not halfway through.
echo "[0/6] Pre-flight..."
ssh "$VPS_HOST" 'systemctl is-active maplecast-headless.service' \
    || { echo "ERROR: production maplecast-headless not active on $VPS_HOST"; exit 1; }
echo "  prod headless is active — proceeding."
echo ""

# Push the systemd unit + env from the local repo so we always deploy the
# version git knows about (no surprises from operator-edited copies).
echo "[1/6] Uploading systemd artifacts..."
ssh "$VPS_HOST" "mkdir -p /tmp/maplecast-lookup-deploy"
scp "$REPO_ROOT/deploy/systemd/maplecast-lookup.service" \
    "$VPS_HOST:/tmp/maplecast-lookup-deploy/maplecast-lookup.service"
scp "$REPO_ROOT/deploy/systemd/maplecast-lookup.env" \
    "$VPS_HOST:/tmp/maplecast-lookup-deploy/lookup.env"
echo "  uploaded."
echo ""

# Remote build + install. All commands wrapped in `set -euo pipefail` so
# any failure stops here, not silently mid-step.
echo "[2/6] Provisioning user + dirs on VPS..."
ssh "$VPS_HOST" bash -s <<'PROVISION_EOF'
set -euo pipefail

# Idempotent user creation. Home is /opt/maplecast-lookup (separate from
# /opt/maplecast which belongs to prod's `maplecast` user).
id maplecast-lookup >/dev/null 2>&1 || \
    useradd --system --home-dir /opt/maplecast-lookup \
            --shell /usr/sbin/nologin maplecast-lookup

# Add lookup user to the `maplecast` group so it can read the prod ROM
# (which is owned by maplecast:maplecast). The lookup user CANNOT write
# there because we don't grant +w on the prod ROM dir.
usermod -a -G maplecast maplecast-lookup || true

mkdir -p /opt/maplecast-lookup/{src,savestates,cfg,.local/share/flycast,.maplecast,visual_cache}
mkdir -p /var/log/maplecast-lookup
mkdir -p /etc/maplecast
chown -R maplecast-lookup:maplecast-lookup /opt/maplecast-lookup /var/log/maplecast-lookup
PROVISION_EOF
echo "  user maplecast-lookup + /opt/maplecast-lookup ready."
echo ""

echo "[3/6] Cloning + building feature branch on VPS (this is the slow step)..."
ssh "$VPS_HOST" bash -s -- "$REPO_URL" "$BRANCH" <<'BUILD_EOF'
set -euo pipefail
REPO_URL="$1"
BRANCH="$2"

cd /opt/maplecast-lookup/src

# Idempotent clone-or-pull. Uses /opt/maplecast-lookup/src/maplecast-flycast
# as the working tree. We do this as root, then hand off the binary alone
# to the unprivileged user — the source tree stays root-owned so an
# escaped flycast process can't tamper with future builds.
if [ ! -d maplecast-flycast/.git ]; then
    git clone "$REPO_URL" maplecast-flycast
fi
cd maplecast-flycast
git fetch origin
git checkout "$BRANCH"
git reset --hard "origin/$BRANCH"

cmake -B build-lookup \
      -DMAPLECAST_HEADLESS=ON \
      -DMAPLECAST_LOOKUP=ON \
      -DCMAKE_BUILD_TYPE=Release \
      -GNinja
cmake --build build-lookup --target flycast -- -j"$(nproc)"

# Sanity check — same posture as production's deploy-headless.sh.
if ldd build-lookup/flycast | grep -iE 'libGL|libEGL|libSDL|libX11|libGLX|libvulkan|libcuda'; then
    echo "ERROR: flycast-lookup pulls GPU/window libraries. MAPLECAST_HEADLESS gate broken."
    exit 1
fi

strip build-lookup/flycast
ls -la build-lookup/flycast
BUILD_EOF
echo "  built."
echo ""

echo "[4/6] Installing binary + systemd unit + env..."
ssh "$VPS_HOST" bash -s <<'INSTALL_EOF'
set -euo pipefail

install -m 0755 -o root -g root \
    /opt/maplecast-lookup/src/maplecast-flycast/build-lookup/flycast \
    /usr/local/bin/flycast-lookup

install -m 0644 -o root -g root \
    /tmp/maplecast-lookup-deploy/maplecast-lookup.service \
    /etc/systemd/system/maplecast-lookup.service

# Don't overwrite an existing env file — the operator may have edited it
# (e.g. enabled MAPLECAST_SCAN). Only install if missing.
if [ ! -f /etc/maplecast/lookup.env ]; then
    install -m 0640 -o root -g maplecast-lookup \
        /tmp/maplecast-lookup-deploy/lookup.env \
        /etc/maplecast/lookup.env
fi

rm -rf /tmp/maplecast-lookup-deploy

systemctl daemon-reload
systemctl enable maplecast-lookup.service
systemctl restart maplecast-lookup.service
INSTALL_EOF
echo "  installed."
echo ""

echo "[5/6] Verifying parallel-instance health..."
sleep 3
ssh "$VPS_HOST" bash -s <<'VERIFY_EOF'
set -e
echo "--- prod (untouched) ---"
systemctl is-active maplecast-headless.service && echo "  maplecast-headless: active"
echo ""
echo "--- lookup (new) ---"
systemctl is-active maplecast-lookup.service && echo "  maplecast-lookup: active"
echo ""
echo "--- listening sockets ---"
ss -ltnp 2>/dev/null | grep -E '7100|7150|7200|7250|7211|7251' || true
ss -lunp 2>/dev/null | grep -E '7100|7150' || true
echo ""
echo "--- last 10 log lines from lookup ---"
journalctl -u maplecast-lookup -n 10 --no-pager || true
VERIFY_EOF
echo ""

echo "[6/6] Done."
echo "============================================"
echo "  Tail logs:    ssh $VPS_HOST journalctl -u maplecast-lookup -f"
echo "  Stop sandbox: ssh $VPS_HOST systemctl stop maplecast-lookup"
echo "  Inspect TA stream from your laptop:"
echo "    ssh -L 7250:127.0.0.1:7250 $VPS_HOST"
echo "    then connect to ws://127.0.0.1:7250 with a debug client"
echo ""
echo "  Production maplecast-headless was not modified."
echo "============================================"
