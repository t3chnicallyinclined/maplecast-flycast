@echo off
rem TDW over QUIC (roadmap D1, Phase 1) — same client as _run_native_tdw.bat but
rem the video wire rides the QUIC bridge (datagrams, no TCP head-of-line) instead
rem of the TCP WebSocket. Input stays on raw UDP:7100 (unchanged).
rem
rem Start order:
rem   1. _run_srv_tadict.bat   (flycast TDW server on :7200)
rem   2. _run_bridge.bat       (WS-subscribes :7200, serves QUIC on :7300)
rem   3. this bat
rem Watch _native_client.log for: "[quic] X Mbps · datagram loss Y% · worst inter-arrival Z ms"
cd /d C:\Users\trist\projects\maplecast-flycast\native-client-tdw
rem MC_QUIC=host:port for an explicit bridge; "1" = the default 127.0.0.1:7300.
set "MC_QUIC=127.0.0.1:7300"
set MAPLECAST_WS=ws://127.0.0.1:7200
set MAPLECAST_INPUT_HOST=
set MAPLECAST_SLOT=0
set MC_TDW=players
set MC_FLEET_KEY=458d4990b770b30d4c1f737f24f8a4ea
set MC_STAGE=C:\Users\trist\projects\maplecast-flycast\atlas\stages\STG0B_ta.mcstg
set RUST_LOG=info
target\release\maplecast-native.exe > C:\Users\trist\projects\maplecast-flycast\_native_client.log 2>&1
