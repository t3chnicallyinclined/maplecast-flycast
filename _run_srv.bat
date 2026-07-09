@echo off
cd /d C:\Users\trist\projects\maplecast-flycast
set MAPLECAST=1
set MAPLECAST_MIRROR_SERVER=1
set MAPLECAST_HEADLESS_AUTOLOAD=1
set MAPLECAST_LOCKSTEP=1
set MAPLECAST_LOCKSTEP_INTERVAL=6
rem NOTE: 6 (dense) for the predict-live confirmed-hash gate; production default is 60.
set MAPLECAST_LOCKSTEP_DEBUG=1
build-headless-win\flycast.exe "C:\roms\roms\mvc2.gdi" > C:\Users\trist\projects\maplecast-flycast\_srv.log 2>&1
