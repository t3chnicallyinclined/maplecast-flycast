@echo off
echo ╔══════════════════════════════════════╗
echo ║       MapleCast - Starting All       ║
echo ╚══════════════════════════════════════╝
echo.

:: Start telemetry server
echo [1/4] Starting telemetry server...
start "MapleCast Telemetry" cmd /k "cd /d C:\Users\trist\projects\maplecast-flycast\web && python telemetry.py"
timeout /t 1 >nul

:: Start WebSocket proxy
echo [2/4] Starting WebSocket proxy...
start "MapleCast Proxy" cmd /k "cd /d C:\Users\trist\projects\maplecast-flycast\web && python proxy.py"
timeout /t 1 >nul

:: Start web server
echo [3/4] Starting web server on http://localhost:3000 ...
start "MapleCast Web" cmd /k "cd /d C:\Users\trist\projects\maplecast-flycast\web && python -m http.server 3000"
timeout /t 1 >nul

:: Start Flycast
echo [4/4] Starting Flycast server...
set MAPLECAST=1
set MAPLECAST_STREAM=1
start "MapleCast Flycast" cmd /k "set MAPLECAST=1 && set MAPLECAST_STREAM=1 && C:\Users\trist\projects\maplecast-flycast\build\flycast.exe "C:\roms\mvc2_us\Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi""

echo.
echo ═══════════════════════════════════════
echo   All services started!
echo.
echo   Flycast:    MVC2 server (UDP:7100, TCP:7200)
echo   Proxy:      ws://localhost:8080
echo   Web app:    http://localhost:3000
echo   Telemetry:  UDP:7300 -^> telemetry.log
echo.
echo   Connect gamepad:
echo     python pc_gamepad_sender.py 127.0.0.1 7100
echo.
echo   Or connect W6100 stick (auto-sends to 192.168.1.63:7100)
echo ═══════════════════════════════════════
