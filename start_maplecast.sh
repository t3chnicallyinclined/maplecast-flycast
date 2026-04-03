#!/bin/bash
echo "========================================"
echo "    MapleCast - Starting All"
echo "========================================"
echo

DIR="$(cd "$(dirname "$0")" && pwd)"
ROM="$HOME/roms/mvc2_us/Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi"

# Kill any existing MapleCast processes
EXISTING=$(pgrep -f "MAPLECAST=1.*flycast|telemetry\.py|http\.server.*--directory.*web" 2>/dev/null)
if [ -n "$EXISTING" ]; then
  echo "Killing existing MapleCast processes: $EXISTING"
  kill $EXISTING 2>/dev/null
  sleep 1
  # Force kill stragglers
  kill -9 $EXISTING 2>/dev/null
fi

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
echo "[3/3] Starting Flycast server..."
MAPLECAST=1 MAPLECAST_STREAM=1 "$DIR/build/flycast" "$ROM" &
FLY_PID=$!

echo
echo "========================================"
echo "  All services started!"
echo
echo "  Flycast:    MVC2 + WebSocket on ws://localhost:7200"
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
