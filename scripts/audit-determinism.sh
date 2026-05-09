#!/usr/bin/env bash
# Determinism gate — runs the rollback ring's F.2 byte-diff audit and
# exits non-zero if dc_serialize round-trip drifts from byte-perfect.
#
# Run this before any commit that touches:
#   - core/serialize.{h,cpp}
#   - core/hw/sh4/sh4_sched.{h,cpp}
#   - core/hw/sh4/sh4_mmr.cpp
#   - core/hw/pvr/{pvr,spg,Renderer_if,ta_vtx}.cpp
#   - core/hw/aica/* (AICA serialize)
#   - core/network/maplecast_rollback.{h,cpp}
#   - any subsystem with a *_serialize / *_deserialize function
#
# Or any merge from upstream flycast that touches the same files.
#
# Usage:
#   scripts/audit-determinism.sh [path-to-rom]
#
# Defaults to MVC2 at the standard dev path. Override the binary by
# setting FLYCAST=path/to/flycast.exe. ROM path defaults to MAPLECAST_AUDIT_ROM
# env var or the dev box default.
set -eu

ROM_DEFAULT_NIX="/c/roms/mvc2_us/Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi"
ROM_DEFAULT_WIN='C:\roms\mvc2_us\Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi'
ROM="${1:-${MAPLECAST_AUDIT_ROM:-$ROM_DEFAULT_WIN}}"
FLYCAST="${FLYCAST:-build-headless-win/flycast.exe}"
TIMEOUT_S="${TIMEOUT_S:-25}"
LOG="${LOG:-/tmp/dc_audit.log}"

# Verify ROM by trying both forms (Win32 flycast wants backslash paths,
# but the file might exist under either). Convert if needed.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # On Windows, flycast.exe expects backslash paths. If the user
        # passed a /c/foo style path, convert it.
        if [[ "$ROM" == /c/* ]] || [[ "$ROM" == /[a-zA-Z]/* ]]; then
            ROM=$(cygpath -w "$ROM" 2>/dev/null || echo "$ROM")
        fi
        ;;
esac

if [ ! -f "$FLYCAST" ]; then
    echo "FAIL: flycast binary not found at $FLYCAST"
    echo "Build with: cmake --build build-headless-win --target flycast"
    exit 2
fi
if [ ! -f "$ROM" ]; then
    echo "FAIL: ROM not found at $ROM"
    echo "Set MAPLECAST_AUDIT_ROM or pass path as arg 1"
    exit 2
fi

echo "[audit] flycast=$FLYCAST"
echo "[audit] rom=$ROM"
echo "[audit] timeout=${TIMEOUT_S}s"
rm -f "$LOG"

# Run flycast in background with audit env. The audit fires once around
# frame 301 (~5 seconds in) and then prints results. We give it
# TIMEOUT_S total before killing.
MAPLECAST_MIRROR_SERVER=1 \
MAPLECAST_ROLLBACK_RING=1 \
MAPLECAST_DC_AUDIT=300 \
MAPLECAST_DC_AUDIT_DEFERRED=1 \
MAPLECAST_HEADLESS_DISABLE_SYS_MISC_1=1 \
"$FLYCAST" "$ROM" >"$LOG" 2>&1 &
FLYCAST_PID=$!

# Wait until either the audit has reported or timeout
elapsed=0
while [ $elapsed -lt $TIMEOUT_S ]; do
    if grep -q "BYTE-PERFECT round-trip" "$LOG" 2>/dev/null; then
        break
    fi
    if grep -q "total differing bytes:" "$LOG" 2>/dev/null; then
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

# Kill the running emulator (it would otherwise run forever)
kill "$FLYCAST_PID" 2>/dev/null || true
sleep 1
kill -9 "$FLYCAST_PID" 2>/dev/null || true

# Parse result
if grep -q "BYTE-PERFECT round-trip" "$LOG"; then
    echo "[audit] PASS — byte-perfect rollback round-trip"
    grep -E "(total differing bytes|BYTE-PERFECT)" "$LOG"
    exit 0
fi

if grep -q "total differing bytes:" "$LOG"; then
    DRIFT=$(grep "total differing bytes:" "$LOG" | head -1)
    echo "[audit] FAIL — $DRIFT"
    echo
    echo "Drift details:"
    grep -E "(total differing|differing bytes|first diff)" "$LOG" | head -40
    echo
    echo "Full log: $LOG"
    exit 1
fi

echo "[audit] FAIL — audit never reported (process may have hung or crashed)"
echo "Last 40 log lines:"
tail -40 "$LOG"
exit 3
