@echo off
rem MapleCast ACK-reference (TDW2) server — thin (~2 Mbps) AND loss-tolerant.
rem Encodes each frame vs the frame the client last ACKed, so a dropped packet
rem just skips a frame (never a freeze). Start this, then _run_native_kf.bat.
cd /d C:\Users\trist\projects\maplecast-flycast
set MAPLECAST=1
set MAPLECAST_MIRROR_SERVER=1
set MAPLECAST_HEADLESS_AUTOLOAD=1
set MAPLECAST_TADICT=1
set MAPLECAST_TDW_ONLY=1
set MAPLECAST_TACANON=2
set MAPLECAST_TADICT_PLAYERS=1
set MAPLECAST_TDW_SPLITPOS=1
set MAPLECAST_FLEET_KEY=458d4990b770b30d4c1f737f24f8a4ea
set MAPLECAST_TDW_ACKREF=1
build-headless-win\flycast.exe "C:\roms\roms\mvc2.gdi" > C:\Users\trist\projects\maplecast-flycast\_srv_kf.log 2>&1
