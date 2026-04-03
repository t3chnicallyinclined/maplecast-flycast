@echo off
echo ========================================
echo     MapleCast - Starting All
echo ========================================
echo.

:: Start telemetry server
echo [1/3] Starting telemetry server...
start "MapleCast Telemetry" cmd /k "cd /d C:\Users\trist\projects\maplecast-flycast\web && python telemetry.py"
timeout /t 1 >nul

:: Start web server
echo [2/3] Starting web server on http://localhost:3000 ...
start "MapleCast Web" cmd /k "cd /d C:\Users\trist\projects\maplecast-flycast\web && python -m http.server 3000"
timeout /t 1 >nul

:: Start Flycast (has built-in WebSocket server on port 7200 — NO PROXY NEEDED)
echo [3/3] Starting Flycast server...
start "MapleCast Flycast" cmd /k "set MAPLECAST=1 && set MAPLECAST_STREAM=1 && set MAPLECAST_JPEG=1 && C:\Users\trist\projects\maplecast-flycast\build\flycast.exe "C:\roms\mvc2_us\Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi""

echo.
echo ========================================
echo   All services started!
echo.
echo   Flycast:    MVC2 + WebSocket on ws://localhost:7200
echo   Web app:    http://localhost:3000
echo   Telemetry:  UDP:7300
echo   Gamepad:    UDP:7100
echo.
echo   NO PROXY NEEDED - browser connects directly to Flycast
echo ========================================
