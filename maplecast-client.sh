#!/bin/bash
# MapleCast Competitive Client — launches flycast + overlay side by side
#
# Usage: ./maplecast-client.sh [hub_url]
#
# The flycast game window takes 70% of screen width, the overlay panel
# takes the remaining 30% on the right side. Both auto-sized to your
# monitor resolution.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLYCAST="${SCRIPT_DIR}/build/flycast"
OVERLAY="${SCRIPT_DIR}/web/overlay.html"
HUB_URL="${1:-https://nobd.net/hub/api}"
CONTROL_PORT=7211

# Get screen dimensions
read SCREEN_W SCREEN_H < <(xdpyinfo 2>/dev/null | grep dimensions | awk '{print $2}' | tr 'x' ' ')
SCREEN_W=${SCREEN_W:-1920}
SCREEN_H=${SCREEN_H:-1080}

# Layout: 70% game, 30% overlay
GAME_W=$((SCREEN_W * 70 / 100))
GAME_H=$SCREEN_H
OVERLAY_W=$((SCREEN_W - GAME_W))

echo "[maplecast] Screen: ${SCREEN_W}x${SCREEN_H}"
echo "[maplecast] Game window: ${GAME_W}x${GAME_H} at (0,0)"
echo "[maplecast] Overlay: ${OVERLAY_W}x${SCREEN_H} at (${GAME_W},0)"
echo "[maplecast] Hub: ${HUB_URL}"
echo ""

# Kill any existing instances
killall flycast 2>/dev/null || true
sleep 0.5

# Launch flycast (mirror client)
MAPLECAST_MIRROR_CLIENT=1 \
MAPLECAST_HUB_URL="$HUB_URL" \
SDL_VIDEO_WINDOW_POS="0,0" \
"$FLYCAST" &
FLYCAST_PID=$!

# Wait for control WS to come up
echo "[maplecast] Waiting for flycast (PID $FLYCAST_PID)..."
for i in $(seq 1 30); do
    if nc -z localhost $CONTROL_PORT 2>/dev/null; then
        echo "[maplecast] Control WS ready on port $CONTROL_PORT"
        break
    fi
    sleep 0.5
done

# Resize flycast window to left portion
sleep 1
wmctrl -r Flycast -e "0,0,0,${GAME_W},${GAME_H}" 2>/dev/null || true

# Launch overlay in a chromium app window (frameless, no toolbar)
OVERLAY_URL="file://${OVERLAY}?port=${CONTROL_PORT}"
if command -v chromium-browser &>/dev/null; then
    chromium-browser --app="$OVERLAY_URL" \
        --window-size="${OVERLAY_W},${SCREEN_H}" \
        --window-position="${GAME_W},0" \
        --disable-infobars --no-first-run &
elif command -v google-chrome &>/dev/null; then
    google-chrome --app="$OVERLAY_URL" \
        --window-size="${OVERLAY_W},${SCREEN_H}" \
        --window-position="${GAME_W},0" \
        --disable-infobars --no-first-run &
elif command -v firefox &>/dev/null; then
    firefox --new-window "$OVERLAY_URL" &
else
    xdg-open "$OVERLAY_URL" &
fi

echo "[maplecast] Client running. Close flycast window to exit."
echo "[maplecast] Keybinds: F1-F3 HUD toggles | Backtick settings"
echo ""

# Wait for flycast to exit, then clean up
wait $FLYCAST_PID 2>/dev/null
echo "[maplecast] Flycast exited, cleaning up..."
killall chromium-browser 2>/dev/null || true
