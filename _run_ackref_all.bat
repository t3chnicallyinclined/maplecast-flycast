@echo off
rem ============================================================================
rem  ONE COMMAND: play on the ACK-reference (TDW2) protocol — thin ~2 Mbps AND
rem  loss-tolerant. Starts the local ACK-ref server, waits, then the client.
rem  The client renders TDW2 and ACKs its highest frame; the server references it.
rem  Connect to LOCAL RIG in the panel if it doesn't already. Input follows video.
rem ============================================================================
echo [1/2] starting local ACK-reference server (MVC2 autoload ~16s)...
start "ackref server" cmd /c C:\Users\trist\projects\maplecast-flycast\_run_srv_ackref.bat
timeout /t 16 /nobreak >nul
echo [2/2] starting client...  (connect to LOCAL RIG in the panel if not already)
call C:\Users\trist\projects\maplecast-flycast\_run_native_kf.bat
