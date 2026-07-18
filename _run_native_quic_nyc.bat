@echo off
rem TDW over QUIC from NYC MAIN (roadmap D1, Phase 1 — the REAL internet test).
rem Server + bridge already run on main; you only run this client. Video rides
rem QUIC datagrams from main's bridge (:7300 UDP); input goes to main :7100.
rem
rem Watch _native_client.log for the line that answers everything:
rem   [quic] X Mbps · datagram loss Y% · worst inter-arrival Z ms
rem   loss high  -> packet loss -> GGPO-style redundancy is the fix
rem   loss ~0, inter-arrival spiky -> delay/bunching -> jitter buffer is the fix
rem   both low -> QUIC alone killed the 61ms (no TCP head-of-line)
cd /d C:\Users\trist\projects\maplecast-flycast\native-client-tdw
rem MC_QUIC must be an IP:port (the QUIC client dials it directly). Main = 149.28.44.118.
set "MC_QUIC=149.28.44.118:7300"
rem Input to main (video comes over QUIC, input still raw UDP:7100).
set MAPLECAST_INPUT_HOST=play.nobd.net
set MAPLECAST_SLOT=0
set MC_TDW=players
set MC_FLEET_KEY=458d4990b770b30d4c1f737f24f8a4ea
set MC_STAGE=C:\Users\trist\projects\maplecast-flycast\atlas\stages\STG0B_ta.mcstg
set RUST_LOG=info
target\release\maplecast-native.exe > C:\Users\trist\projects\maplecast-flycast\_native_client.log 2>&1
