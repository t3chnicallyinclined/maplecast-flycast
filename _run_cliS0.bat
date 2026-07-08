@echo off
cd /d C:\Users\trist\projects\maplecast-flycast
set MAPLECAST_PLAYER_CLIENT=127.0.0.1
set MAPLECAST_LOCKSTEP=1
set MAPLECAST_LOCKSTEP_DEBUG=1
set MAPLECAST_HEADLESS_AUTOLOAD=1
set MAPLECAST_PREDICT=1
set MAPLECAST_PREDICT_STAGE0=300,5,25
build\flycast.exe "C:\roms\roms\mvc2.gdi" > C:\Users\trist\projects\maplecast-flycast\_cliS0.log 2>&1
