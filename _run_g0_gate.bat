@echo off
rem G0 — the deterministic TDW loss gate (docs/TDW2-DESIGN.md).
rem  1. Capture a real TDW wire (full client handshake: request_sync + subscribe tdw).
rem  2. Replay it through the REAL decoder twice — clean vs induced datagram loss —
rem     and byte-diff the decoded TA per frame. Byte-exact TA == pixel-exact.
rem No GPU, no live server needed for step 2; fully deterministic, same result every run.
rem
rem Usage:  _run_g0_gate.bat [wss_url] [drop_every_N]
rem   default url = wss://play.nobd.net/ws (NYC main), default drop = 30 (~3.3%% loss)
setlocal
set "URL=%~1"
if "%URL%"=="" set "URL=wss://play.nobd.net/ws"
set "DROP=%~2"
if "%DROP%"=="" set "DROP=30"
set "CAP=%TEMP%\mc_g0.cap"

echo [1/2] capturing TDW wire from %URL% ...
cd /d C:\Users\trist\projects\maplecast-flycast\_bwlab
node cap2.mjs "%URL%" "%CAP%" 90
if errorlevel 1 ( echo capture failed — no decodable TDWS+streamStart & exit /b 1 )

echo.
echo [2/2] running the loss gate (drop every %DROP%th datagram) ...
cd /d C:\Users\trist\projects\maplecast-flycast\native-client-tdw
target\release\maplecast-native.exe gate "%CAP%" --drop %DROP%
echo.
echo (single-drop check:)
target\release\maplecast-native.exe gate "%CAP%" --drop-at 100
endlocal
