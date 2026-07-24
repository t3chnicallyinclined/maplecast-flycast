@echo off
rem Play on NYC PROD over QUIC — the ack-reference wire on per-frame uni-streams.
rem A dropped packet delays ONE frame (~16ms) instead of TCP's ~200ms head-of-line
rem stall, so those "every couple seconds" spikes should be gone. Input still goes
rem direct to NYC (play.nobd.net:7100); video rides QUIC via the bridge on :7300.
cd /d C:\Users\trist\projects\maplecast-flycast\native-client-tdw
set MC_QUIC=play.nobd.net:7300
set MAPLECAST_WS=ws://play.nobd.net:7200
set MC_CONNECT=0
set MC_NO_AUTO=1
set MC_TDW=players
set MC_ACKREF=1
rem MC_PACE=N present-pacing buffer: hold N frames, release one per display refresh, so
rem bursty network arrival (p50 12ms / p95 41ms) becomes an EVEN display cadence (fixes
rem the "looks juddery" feel). Costs ~N frames of latency (~16ms each). 0=off. Try 1-3.
set MC_PACE=2
set MAPLECAST_INPUT_HOST=play.nobd.net
set MAPLECAST_SLOT=0
set MC_FLEET_KEY=458d4990b770b30d4c1f737f24f8a4ea
set MC_STAGE=C:\Users\trist\projects\maplecast-flycast\atlas\stages\STG0B_ta.mcstg
set RUST_LOG=info
target\release\maplecast-native.exe > C:\Users\trist\projects\maplecast-flycast\_native_client.log 2>&1
