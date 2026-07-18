@echo off
rem Native TDW client -> the LOCAL kfdelta server (127.0.0.1:7200). The client
rem auto-detects the keyframe/delta wire (flag bit7) and renders it byte-exact.
rem Start _run_srv_tadict_kf.bat FIRST, then this. Watch the window render the match.
cd /d C:\Users\trist\projects\maplecast-flycast\native-client-tdw
set MAPLECAST_WS=ws://127.0.0.1:7200
rem connect straight to the LOCAL rig (servers[0]=MAPLECAST_WS); skip the auto-closest
rem probe (which excludes local) so we don't get bounced to a remote fleet node.
set MC_CONNECT=0
set MC_NO_AUTO=1
set MC_TDW=players
set MAPLECAST_INPUT_HOST=127.0.0.1
set MAPLECAST_SLOT=0
set MC_FLEET_KEY=458d4990b770b30d4c1f737f24f8a4ea
set MC_STAGE=C:\Users\trist\projects\maplecast-flycast\atlas\stages\STG0B_ta.mcstg
set RUST_LOG=info
target\release\maplecast-native.exe > C:\Users\trist\projects\maplecast-flycast\_native_client.log 2>&1
