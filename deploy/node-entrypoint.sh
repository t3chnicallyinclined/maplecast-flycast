#!/bin/bash
# ============================================================================
# MAPLECAST NODE ENTRYPOINT — Starts flycast + relay in one container
#
# Usage: docker run ... maplecast-node:latest /data/mvc2.gdi
#
# Environment variables:
#   MAPLECAST_HUB_URL      — Hub API URL (enables distributed registration)
#   MAPLECAST_HUB_TOKEN    — Operator token for hub auth
#   MAPLECAST_NODE_NAME    — Human-readable node name
#   MAPLECAST_NODE_REGION  — Region identifier
#   MAPLECAST_PUBLIC_HOST  — Public hostname/IP (auto-detected if omitted)
#
# OVERKILL IS NECESSARY.
# ============================================================================

set -e

ROM_PATH="${1:-}"
[ -f "$ROM_PATH" ] || ROM_PATH=""
if [ -z "$ROM_PATH" ]; then
  # A .gdi is a MULTI-FILE image (index + track01.bin, track02.raw, ...), so the
  # whole folder must be mounted. Auto-detect the disc image in /data so the
  # filename doesn't matter.
  ROM_PATH="$(ls /data/*.gdi /data/*.chd /data/*.cdi 2>/dev/null | head -n1)"
fi
if [ -z "$ROM_PATH" ] || [ ! -f "$ROM_PATH" ]; then
  echo "ERROR: no disc image found in /data."
  echo "Mount the FOLDER holding your MvC2 .gdi AND its track files:"
  echo "  docker run -v /path/to/mvc2-folder:/data:ro ..."
  exit 1
fi

echo "╔══════════════════════════════════════════════╗"
echo "║   MAPLECAST NODE — DISTRIBUTED GAME SERVER     ║"
echo "║   OVERKILL IS NECESSARY                        ║"
echo "╚══════════════════════════════════════════════╝"

# Trap signals for clean shutdown
cleanup() {
  echo "Shutting down..."
  [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null
  [ -n "$FLYCAST_PID" ] && kill "$FLYCAST_PID" 2>/dev/null
  wait
  echo "Node stopped."
}
trap cleanup SIGTERM SIGINT

# Start flycast headless in background
echo "Starting flycast headless with ROM: $ROM_PATH"
/usr/local/bin/flycast "$ROM_PATH" &
FLYCAST_PID=$!

# Wait for flycast WS to be ready (port 7200)
echo "Waiting for flycast WS on port 7200..."
for i in $(seq 1 30); do
  if nc -z 127.0.0.1 7200 2>/dev/null; then
    echo "flycast ready."
    break
  fi
  if [ "$i" -eq 30 ]; then
    echo "ERROR: flycast WS not ready after 15s"
    kill "$FLYCAST_PID" 2>/dev/null
    exit 1
  fi
  sleep 0.5
done

# ── Hub registration: default to the public hub + a persisted per-node token ──
# Open registration (anyone with a ROM can host): the token is simply THIS node's
# ownership secret. Generate a random one on first run and persist it so the node
# keeps owning its spot on the map across restarts. Mount a volume at
# ~/.maplecast (/opt/maplecast/.maplecast) to keep the token + node id stable.
: "${MAPLECAST_HUB_URL:=https://nobd.net/hub/api}"
: "${MAPLECAST_NODE_NAME:=node-$(hostname)}"
TOKEN_FILE="$HOME/.maplecast/hub_token"
if [ -z "$MAPLECAST_HUB_TOKEN" ]; then
  mkdir -p "$(dirname "$TOKEN_FILE")"
  if [ -s "$TOKEN_FILE" ]; then
    MAPLECAST_HUB_TOKEN="$(cat "$TOKEN_FILE")"
  else
    MAPLECAST_HUB_TOKEN="$(head -c 24 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 32)"
    printf '%s' "$MAPLECAST_HUB_TOKEN" > "$TOKEN_FILE"
    echo "Generated a new node token (persisted at $TOKEN_FILE)."
  fi
fi
export MAPLECAST_HUB_URL MAPLECAST_HUB_TOKEN MAPLECAST_NODE_NAME

# Build relay args
RELAY_ARGS="--ws-upstream ws://127.0.0.1:7200 --no-webtransport"

if [ -n "$MAPLECAST_HUB_URL" ]; then
  RELAY_ARGS="$RELAY_ARGS --hub-register"
  RELAY_ARGS="$RELAY_ARGS --hub-url $MAPLECAST_HUB_URL"
  [ -n "$MAPLECAST_HUB_TOKEN" ] && RELAY_ARGS="$RELAY_ARGS --hub-token $MAPLECAST_HUB_TOKEN"
  [ -n "$MAPLECAST_NODE_NAME" ] && RELAY_ARGS="$RELAY_ARGS --node-name $MAPLECAST_NODE_NAME"
  [ -n "$MAPLECAST_NODE_REGION" ] && RELAY_ARGS="$RELAY_ARGS --node-region $MAPLECAST_NODE_REGION"
  [ -n "$MAPLECAST_PUBLIC_HOST" ] && RELAY_ARGS="$RELAY_ARGS --public-host $MAPLECAST_PUBLIC_HOST"
  echo "Hub registration enabled: $MAPLECAST_HUB_URL"
fi

# Start relay
echo "Starting relay: maplecast-relay $RELAY_ARGS"
/usr/local/bin/maplecast-relay $RELAY_ARGS &
RELAY_PID=$!

echo "Node is live. flycast=$FLYCAST_PID relay=$RELAY_PID"

# Wait for either process to exit
wait -n "$FLYCAST_PID" "$RELAY_PID"
EXIT_CODE=$?
echo "Process exited with code $EXIT_CODE"
cleanup
exit "$EXIT_CODE"
