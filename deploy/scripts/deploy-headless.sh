#!/bin/bash
# ============================================================================
# MAPLECAST HEADLESS FLYCAST — Deploy to VPS
#
# Usage:
#   ./deploy/scripts/deploy-headless.sh <VPS_HOST> [<ROM_PATH>]
#
# Example:
#   ./deploy/scripts/deploy-headless.sh root@66.55.128.93
#   ./deploy/scripts/deploy-headless.sh root@66.55.128.93 ~/roms/mvc2.gdi
#
# What this does:
#   1. Builds the headless flycast binary locally (cmake -DMAPLECAST_HEADLESS=ON)
#   2. Copies the binary + systemd unit + env file to the VPS
#   3. Installs the unit, creates the maplecast user, sets up dirs
#   4. Optionally uploads a ROM (second argument)
#   5. Starts maplecast-headless.service
#
# Prerequisites on the VPS:
#   apt-get install -y libcurl4 libxdp1 libbpf1 libgomp1 libzip4 zlib1g ca-certificates
#
# Prerequisites on your box:
#   Same as a normal flycast host build — CMake, a C++ compiler, libcurl-dev,
#   etc. See the "libfoo-dev" list in Dockerfile.headless.
#
# Environment overrides:
#   SSH_USER     ssh login user (default: parsed from VPS_HOST or "root")
#   REMOTE_DIR   install prefix on the VPS (default: /opt/maplecast)
#   BUILD_DIR    local build directory (default: build-headless)
# ============================================================================

set -euo pipefail

VPS_HOST="${1:?Usage: ./deploy-headless.sh <VPS_HOST> [ROM_PATH]}"
ROM_PATH="${2:-}"

REMOTE_DIR="${REMOTE_DIR:-/opt/maplecast}"
BUILD_DIR="${BUILD_DIR:-build-headless}"
SERVICE_NAME="maplecast-headless"

SCRIPT_DIR="$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
REPO_ROOT="$( cd -- "$SCRIPT_DIR/../.." &> /dev/null && pwd )"

echo "============================================"
echo "  MAPLECAST HEADLESS FLYCAST DEPLOY"
echo "  Target:       $VPS_HOST"
echo "  Install dir:  $REMOTE_DIR"
echo "  Build dir:    $BUILD_DIR"
if [ -n "$ROM_PATH" ]; then
  echo "  Uploading ROM: $ROM_PATH"
fi
echo "============================================"

# ─── Step 1: Build ───────────────────────────────────────────────
echo ""
echo "[1/5] Building headless binary locally..."
cd "$REPO_ROOT"
cmake -B "$BUILD_DIR" \
      -DMAPLECAST_HEADLESS=ON \
      -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -- -j"$(nproc)"
strip "$BUILD_DIR/flycast"

BINARY_SIZE=$(stat -c%s "$BUILD_DIR/flycast")
echo "Built $(du -h "$BUILD_DIR/flycast" | cut -f1) binary"

# Sanity check — must be GPU-library-free
echo ""
echo "[2/5] Verifying binary has no forbidden deps..."
if ldd "$BUILD_DIR/flycast" | grep -iE 'libGL|libEGL|libSDL|libX11|libGLX|libvulkan|libcuda'; then
  echo "ERROR: binary pulls GPU/window libraries. MAPLECAST_HEADLESS gate is broken."
  exit 1
fi
echo "  OK: no libGL, libSDL, libX11, libvulkan, libcuda"

# ─── Step 3: Upload ──────────────────────────────────────────────
echo ""
echo "[3/5] Uploading to $VPS_HOST..."

ssh "$VPS_HOST" "mkdir -p /tmp/maplecast-deploy"
scp "$BUILD_DIR/flycast" "$VPS_HOST:/tmp/maplecast-deploy/flycast"
scp "$REPO_ROOT/deploy/systemd/maplecast-headless.service" \
    "$VPS_HOST:/tmp/maplecast-deploy/maplecast-headless.service"
scp "$REPO_ROOT/deploy/systemd/maplecast-headless.env" \
    "$VPS_HOST:/tmp/maplecast-deploy/headless.env"

if [ -n "$ROM_PATH" ]; then
  if [ ! -f "$ROM_PATH" ]; then
    echo "ERROR: ROM file not found: $ROM_PATH"
    exit 1
  fi
  ROM_BASENAME=$(basename "$ROM_PATH")
  scp "$ROM_PATH" "$VPS_HOST:/tmp/maplecast-deploy/$ROM_BASENAME"
fi

# ─── Step 4: Install on VPS ──────────────────────────────────────
echo ""
echo "[4/5] Installing on VPS..."
ssh "$VPS_HOST" bash -s <<EOF
set -euo pipefail

# Create maplecast user (idempotent)
id maplecast >/dev/null 2>&1 || useradd --system --home-dir "$REMOTE_DIR" \
    --shell /usr/sbin/nologin maplecast

# Create dirs
mkdir -p "$REMOTE_DIR"/{savestates,cfg,roms} /var/log/maplecast /etc/maplecast
chown -R maplecast:maplecast "$REMOTE_DIR" /var/log/maplecast

# Install binary
install -m 0755 -o root -g root /tmp/maplecast-deploy/flycast /usr/local/bin/flycast

# Install systemd unit
install -m 0644 -o root -g root /tmp/maplecast-deploy/maplecast-headless.service \
    /etc/systemd/system/maplecast-headless.service

# Install env file (don't overwrite existing — admin may have customized)
if [ ! -f /etc/maplecast/headless.env ]; then
  install -m 0640 -o root -g maplecast /tmp/maplecast-deploy/headless.env \
      /etc/maplecast/headless.env
fi

# Install ROM if provided
if [ -f /tmp/maplecast-deploy/$(basename "${ROM_PATH:-/nonexistent}") ]; then
  install -m 0644 -o maplecast -g maplecast \
      "/tmp/maplecast-deploy/$(basename "${ROM_PATH:-/nonexistent}")" \
      "$REMOTE_DIR/roms/$(basename "${ROM_PATH:-/nonexistent}")"
fi

# Clean up staging
rm -rf /tmp/maplecast-deploy

# Reload + restart
systemctl daemon-reload
systemctl enable maplecast-headless.service
systemctl restart maplecast-headless.service
EOF

# ─── Step 5: Verify ──────────────────────────────────────────────
echo ""
echo "[5/5] Verifying..."
sleep 2
ssh "$VPS_HOST" "systemctl is-active maplecast-headless.service && \
                 systemctl status maplecast-headless.service --no-pager | head -20 && \
                 ss -ltnp 2>/dev/null | grep -E '7100|7200' || true"

echo ""
echo "============================================"
echo "  DEPLOY COMPLETE"
echo "  Check logs: ssh $VPS_HOST journalctl -u maplecast-headless -f"
echo "============================================"
