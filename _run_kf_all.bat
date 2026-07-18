@echo off
rem ============================================================================
rem  ONE COMMAND: local keyframe/delta (kfdelta) render + input test.
rem  Starts the local kfdelta server, waits for it to boot, then the client.
rem  The client auto-connects to the CLOSEST server; with the local server up
rem  (0 ms) that is LOCAL RIG. If it lands on nobd-main instead, just click
rem  connect on 'local rig' in the client's Servers tab. Input auto-follows video.
rem ============================================================================
echo [1/2] starting local kfdelta server (MVC2 autoload ~16s)...
start "kfdelta server" cmd /c C:\Users\trist\projects\maplecast-flycast\_run_srv_tadict_kf.bat
timeout /t 16 /nobreak >nul
echo [2/2] starting client...  (in the panel: connect to LOCAL RIG if not already)
call C:\Users\trist\projects\maplecast-flycast\_run_native_kf.bat
