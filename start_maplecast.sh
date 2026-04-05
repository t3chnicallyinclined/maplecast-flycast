#!/bin/bash
echo "========================================"
echo "    MapleCast - Starting All"
echo "========================================"
echo

DIR="$(cd "$(dirname "$0")" && pwd)"
ROM="$HOME/roms/mvc2_us/Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi"

# Kill any existing MapleCast processes and free ports
EXISTING=$(pgrep -f "MAPLECAST=1.*flycast|telemetry\.py|http\.server.*--directory.*web" 2>/dev/null)
if [ -n "$EXISTING" ]; then
  echo "Killing existing MapleCast processes: $EXISTING"
  kill $EXISTING 2>/dev/null
  sleep 1
  kill -9 $EXISTING 2>/dev/null
fi
# Force-free ports in case of orphaned processes
fuser -k 7100/tcp 7200/tcp 7300/udp 8000/tcp 2>/dev/null
sleep 1

# Start telemetry server
echo "[1/3] Starting telemetry server..."
python3 "$DIR/web/telemetry.py" &
TELE_PID=$!
sleep 1

# Start web server
WEB_PORT="${MAPLECAST_WEB_PORT:-8000}"
echo "[2/3] Starting web server on http://localhost:$WEB_PORT ..."
python3 -m http.server "$WEB_PORT" --directory "$DIR/web" &
WEB_PID=$!
sleep 1

# Start Flycast with MapleCast input server
# Set MAPLECAST_JPEG=75 (or 1-100) for JPEG mode, unset for H.264
if [ -n "$MAPLECAST_JPEG" ]; then
  echo "[3/3] Starting Flycast server... JPEG mode (quality $MAPLECAST_JPEG)"
  export MAPLECAST_JPEG
else
  echo "[3/3] Starting Flycast server... H.264 mode"
fi
# Forward all MAPLECAST_* env vars to flycast
export MAPLECAST=1
export MAPLECAST_STREAM=1
[ -n "$MAPLECAST_MIRROR_SERVER" ] && export MAPLECAST_MIRROR_SERVER
[ -n "$MAPLECAST_MIRROR_CLIENT" ] && export MAPLECAST_MIRROR_CLIENT
"$DIR/build/flycast" "$ROM" &
FLY_PID=$!

echo
echo "========================================"
echo "  All services started!"
echo
echo "  Flycast:    MVC2 + WebRTC P2P (signaling on ws://localhost:7200)"
echo "  Web app:    http://localhost:$WEB_PORT"
echo "  Telemetry:  UDP:7300"
echo "  Gamepad:    UDP:7100 (input server)"
echo
echo "  PIDs: flycast=$FLY_PID web=$WEB_PID telemetry=$TELE_PID"
echo "  Press Ctrl+C to stop all"
echo "========================================"

cleanup() {
  echo "Stopping MapleCast..."
  kill $FLY_PID $WEB_PID $TELE_PID 2>/dev/null
  sleep 1
  kill -9 $FLY_PID $WEB_PID $TELE_PID 2>/dev/null
  echo "Stopped."
  exit
}
trap cleanup INT TERM
wait
