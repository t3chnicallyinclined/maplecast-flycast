@echo off
cd /d C:\Users\trist\projects\maplecast-flycast
set MAPLECAST_PLAYER_CLIENT=127.0.0.1
set MAPLECAST_LOCKSTEP=1
set MAPLECAST_LOCKSTEP_DEBUG=1
set MAPLECAST_HEADLESS_AUTOLOAD=1
set MAPLECAST_PREDICT=1
set MAPLECAST_PREDICT_LIVE=1
set MAPLECAST_PREDICT_LIVE_INJECT=1
set MAPLECAST_SUBHASH_LOG=1
build\flycast.exe "C:\roms\roms\mvc2.gdi" > C:\Users\trist\projects\maplecast-flycast\_cliINJ.log 2>&1
