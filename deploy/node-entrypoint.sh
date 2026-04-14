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

ROM_PATH="${1:-/data/mvc2.gdi}"

if [ ! -f "$ROM_PATH" ]; then
  echo "ERROR: ROM not found at $ROM_PATH"
  echo "Mount your ROM: docker run -v /path/to/mvc2.gdi:/data/mvc2.gdi:ro ..."
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
