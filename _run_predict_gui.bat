@echo off
rem ── MapleCast PREDICT LIVE build — user fightstick feel-check launcher ──
rem The live predict-drive rollback client. All 4 stages are compiled + headless-
rem validated GREEN, and the live capstone determinism gate PASSES (confirmed
rem timeline matches the server 112/112, mismatched=0, 60fps, no re-JOIN over
rem ~7000 frames). MAPLECAST_PREDICT_LIVE=1 turns ON the live predict-drive:
rem   - local input INSTANT (applied at the predicted head, ~0-frame latency)
rem   - remote predicted as repeat-last-confirmed; rollback+re-sim on mispredict
rem   - frame-stamped local input (server applies at the client's frame)
rem The default client (these vars unset) is completely untouched.
rem
rem YOU judge the FEEL: does local input react within ~1-2 frames of the stick
rem (not ~9), are rollbacks invisible in normal play, does it hold 60fps.
cd /d C:\Users\trist\projects\maplecast-flycast
set MAPLECAST_PLAYER_CLIENT=127.0.0.1
set MAPLECAST_LOCKSTEP=1
set MAPLECAST_HEADLESS_AUTOLOAD=1
set MAPLECAST_PREDICT=1
set MAPLECAST_PREDICT_LIVE=1
build\flycast.exe "C:\Users\trist\Downloads\Dreamcast Games\Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!]\Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi"
