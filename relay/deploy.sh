#!/bin/bash
# ============================================================================
# MAPLECAST RELAY — Deploy to Vultr VPS
#
# Usage:
#   ./deploy.sh <VPS_IP> <FLYCAST_HOST>
#
# Example:
#   ./deploy.sh 149.28.xxx.xxx 192.168.1.100
#
# What this does:
#   1. Cross-compiles for x86_64 Linux (musl static)
#   2. SCPs the binary to the VPS
#   3. Sets up systemd service
#   4. Starts the relay
#
# Prerequisites on your box:
#   rustup target add x86_64-unknown-linux-musl
#   sudo apt install musl-tools
# ============================================================================

set -euo pipefail

VPS_IP="${1:?Usage: ./deploy.sh <VPS_IP> <FLYCAST_HOST>}"
FLYCAST_HOST="${2:?Usage: ./deploy.sh <VPS_IP> <FLYCAST_HOST>}"
FLYCAST_PORT="${3:-7200}"
WS_LISTEN_PORT="${4:-7201}"
SSH_USER="${SSH_USER:-root}"

BINARY="maplecast-relay"
REMOTE_DIR="/opt/maplecast"
SERVICE_NAME="maplecast-relay"

echo "╔══════════════════════════════════════════════╗"
echo "║   MAPLECAST RELAY DEPLOY                      ║"
echo "║   Target: ${VPS_IP}                            "
echo "║   Upstream: ws://${FLYCAST_HOST}:${FLYCAST_PORT}"
echo "╚══════════════════════════════════════════════╝"

# Step 1: Build static binary
echo "[1/4] Building release binary (static musl)..."
cargo build --release --target x86_64-unknown-linux-musl 2>&1

BINARY_PATH="target/x86_64-unknown-linux-musl/release/${BINARY}"
if [ ! -f "$BINARY_PATH" ]; then
    echo "WARN: musl build failed, falling back to regular release build..."
    cargo build --release
    BINARY_PATH="target/release/${BINARY}"
fi

BINARY_SIZE=$(ls -lh "$BINARY_PATH" | awk '{print $5}')
echo "    Binary: ${BINARY_PATH} (${BINARY_SIZE})"

# Step 2: Upload binary
echo "[2/4] Uploading binary to ${VPS_IP}..."
ssh "${SSH_USER}@${VPS_IP}" "mkdir -p ${REMOTE_DIR}"
scp "$BINARY_PATH" "${SSH_USER}@${VPS_IP}:${REMOTE_DIR}/${BINARY}"
ssh "${SSH_USER}@${VPS_IP}" "chmod +x ${REMOTE_DIR}/${BINARY}"

# Step 3: Create systemd service
echo "[3/4] Installing systemd service..."
ssh "${SSH_USER}@${VPS_IP}" "cat > /etc/systemd/system/${SERVICE_NAME}.service << 'UNIT'
[Unit]
Description=MapleCast Relay — Zero Copy TA Stream Fanout
After=network.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=${REMOTE_DIR}/${BINARY} --ws-upstream ws://${FLYCAST_HOST}:${FLYCAST_PORT} --ws-listen 0.0.0.0:${WS_LISTEN_PORT} --max-clients 500
Restart=always
RestartSec=2
Environment=RUST_LOG=maplecast_relay=info

# Performance tuning
LimitNOFILE=65536
LimitMEMLOCK=infinity

# Security hardening
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadOnlyPaths=/
ReadWritePaths=${REMOTE_DIR}

[Install]
WantedBy=multi-user.target
UNIT"

# Step 4: Start service
echo "[4/4] Starting relay service..."
ssh "${SSH_USER}@${VPS_IP}" "systemctl daemon-reload && systemctl enable ${SERVICE_NAME} && systemctl restart ${SERVICE_NAME}"

# Verify
sleep 1
echo ""
echo "Service status:"
ssh "${SSH_USER}@${VPS_IP}" "systemctl status ${SERVICE_NAME} --no-pager -l" || true

echo ""
echo "════════════════════════════════════════════════"
echo "  RELAY LIVE at ws://${VPS_IP}:${WS_LISTEN_PORT}"
echo ""
echo "  Browser clients connect to:"
echo "    ws://${VPS_IP}:${WS_LISTEN_PORT}"
echo ""
echo "  View logs:"
echo "    ssh ${SSH_USER}@${VPS_IP} journalctl -u ${SERVICE_NAME} -f"
echo "════════════════════════════════════════════════"
