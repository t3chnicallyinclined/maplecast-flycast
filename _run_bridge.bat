@echo off
rem MapleCast TDW -> QUIC bridge (roadmap D1, Phase 1). Co-located with the local
rem flycast server: WS-subscribes the TDW players wire on :7200 and re-serves it
rem over QUIC on :7300 (TDW1 -> datagrams, SYNC/TDWS/oversized -> uni-streams).
rem
rem Test triangle:
rem   1. C:\Users\trist\projects\maplecast-flycast\_run_srv_tadict.bat   (flycast TDW server)
rem   2. this bat                                                        (the bridge)
rem   3. set MC_QUIC=1 && _run_native_tdw.bat                            (client over QUIC)
rem Watch the client log for: "[quic] X Mbps · datagram loss Y% · worst inter-arrival Z ms"
rem That loss-vs-inter-arrival split tells us if the impairment is LOSS or DELAY.
cd /d C:\Users\trist\projects\maplecast-flycast\quic-bridge
set MC_BRIDGE_LISTEN=0.0.0.0:7300
set MC_FLYCAST_WS=ws://127.0.0.1:7200
set RUST_LOG=info
target\release\maplecast-quic-bridge.exe
