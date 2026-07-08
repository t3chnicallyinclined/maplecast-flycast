@echo off
cd /d C:\Users\trist\projects\maplecast-flycast
set MAPLECAST_PLAYER_CLIENT=127.0.0.1
set MAPLECAST_LOCKSTEP=1
set MAPLECAST_LOCKSTEP_DEBUG=1
set MAPLECAST_HEADLESS_AUTOLOAD=1
set MAPLECAST_PREDICT=1
set MAPLECAST_PREDICT_STAGEC=300,8,20
build\flycast.exe "C:\roms\roms\mvc2.gdi" > C:\Users\trist\projects\maplecast-flycast\_cliSC.log 2>&1
