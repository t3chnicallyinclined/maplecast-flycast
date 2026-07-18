@echo off
rem ONE-COMMAND QUIC test triangle (roadmap D1, Phase 1):
rem   flycast TDW server (:7200)  ->  QUIC bridge (:7300)  ->  native client
rem Each runs in its own window; the client runs here. Close this window's client
rem and the two spawned windows to stop everything.
echo [1/3] starting flycast TDW server...
start "MapleCast flycast server" cmd /c C:\Users\trist\projects\maplecast-flycast\_run_srv_tadict.bat
echo     waiting for the server to boot (autoload ~15s)...
timeout /t 16 /nobreak >nul
echo [2/3] starting QUIC bridge...
start "MapleCast QUIC bridge" cmd /c C:\Users\trist\projects\maplecast-flycast\_run_bridge.bat
timeout /t 3 /nobreak >nul
echo [3/3] starting client over QUIC...
call C:\Users\trist\projects\maplecast-flycast\_run_native_quic.bat
