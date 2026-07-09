@echo off
rem TA-Wire v2 rig server: PAGEGATE=1 + ZCS2 shadow stream (dual-emit).
cd /d C:\Users\trist\projects\maplecast-flycast
set MAPLECAST=1
set MAPLECAST_MIRROR_SERVER=1
set MAPLECAST_HEADLESS_AUTOLOAD=1
set MAPLECAST_REPLICA_LIVE=1
set MAPLECAST_PAGEGATE=1
set MAPLECAST_ZSTREAM=1
set MAPLECAST_TACANON=1
set MAPLECAST_ZSTREAM_SOA=1
set MAPLECAST_STAGESTRIP=1
set MAPLECAST_VCACHE=1
set MAPLECAST_CHARSTRIP=measure
set MAPLECAST_ZSTREAM_LEVEL=9
build-headless-win\flycast.exe "C:\roms\roms\mvc2.gdi" > C:\Users\trist\projects\maplecast-flycast\_srv_charmeasure.log 2>&1
