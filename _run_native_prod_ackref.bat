@echo off
rem Play on NYC PROD with the ACK-reference wire (thin + loss-tolerant).
rem MC_ACKREF=1 -> subscribe {mode:tdw2}; the server serves TDW2 to us and leaves
rem every other client (browsers, classic TDW1 natives) exactly as they were.
cd /d C:\Users\trist\projects\maplecast-flycast\native-client-tdw
set MAPLECAST_WS=ws://play.nobd.net:7200
set MC_CONNECT=0
set MC_NO_AUTO=1
set MC_TDW=players
set MC_ACKREF=1
set MAPLECAST_INPUT_HOST=play.nobd.net
set MAPLECAST_SLOT=0
set MC_FLEET_KEY=458d4990b770b30d4c1f737f24f8a4ea
set MC_STAGE=C:\Users\trist\projects\maplecast-flycast\atlas\stages\STG0B_ta.mcstg
set RUST_LOG=info
target\release\maplecast-native.exe > C:\Users\trist\projects\maplecast-flycast\_native_client.log 2>&1
