@echo off
rem Local rig server WITH state-wire v2 (keyframe/delta dirty-diff, FRM2) on the
rem replica-live wire (7212). Same as _run_srv_gsta.bat + MAPLECAST_STATEWIRE_V2=1.
rem Connect web/render-replica/replay.html live to ws://127.0.0.1:7212 (or ?live=).
cd /d C:\Users\trist\projects\maplecast-flycast
set MAPLECAST=1
set MAPLECAST_MIRROR_SERVER=1
set MAPLECAST_HEADLESS_AUTOLOAD=1
set MAPLECAST_REPLICA_LIVE=1
set MAPLECAST_STATEWIRE_V2=1
build-headless-win\flycast.exe "C:\roms\roms\mvc2.gdi" > C:\Users\trist\projects\maplecast-flycast\_srv_statewire_v2.log 2>&1
