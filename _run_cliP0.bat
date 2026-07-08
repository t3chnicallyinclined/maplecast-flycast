@echo off
cd /d C:\Users\trist\projects\maplecast-flycast
set MAPLECAST_PLAYER_CLIENT=127.0.0.1
set MAPLECAST_LOCKSTEP=1
set MAPLECAST_LOCKSTEP_DEBUG=1
set MAPLECAST_HEADLESS_AUTOLOAD=1
set MAPLECAST_PREDICT=1
set MAPLECAST_PREDICT_GATE0=300,5,25
rem Fast+memwatch validated byte-exact (7/7) but crude global-protect tanks fps;
rem the reconcile loop must use maplecast_rollback's per-frame protect+getPages cycle.
rem set MAPLECAST_PREDICT_FASTRESTORE=1
rem set MAPLECAST_PREDICT_MEMWATCH=1
set MAPLECAST_DISABLE_MEMWATCH=1
build\flycast.exe "C:\roms\roms\mvc2.gdi" > C:\Users\trist\projects\maplecast-flycast\_cliP0.log 2>&1
