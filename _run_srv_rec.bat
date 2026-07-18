@echo off
rem ============================================================================
rem MapleCast -- AI DATASET recording server.
rem
rem Identical to _run_srv.bat (your authoritative 2-player headless sim) PLUS
rem continuous per-match .mcrec recording into the mvc2-ai dataset dir. Every
rem match you play becomes ONE .mcrec = both players' inputs, frame-indexed,
rem deterministic (savestate + input log). State is regenerated offline by
rem replaying the file, so nothing extra is needed during play.
rem
rem Connect your two players exactly as you normally do; just launch THIS
rem instead of _run_srv.bat. Recordings land in:
rem   C:\Users\trist\projects\mvc2-ai\data\recordings
rem ============================================================================
cd /d C:\Users\trist\projects\maplecast-flycast
set MAPLECAST=1
set MAPLECAST_MIRROR_SERVER=1
set MAPLECAST_HEADLESS_AUTOLOAD=1
set MAPLECAST_LOCKSTEP=1
set MAPLECAST_LOCKSTEP_INTERVAL=6
set MAPLECAST_LOCKSTEP_DEBUG=1
set MAPLECAST_SUBHASH_LOG=1
rem --- dataset recording ---
rem Optional FIRST arg = the class label = the sub-folder the session records into.
rem   _run_srv_rec.bat                 -> data\recordings\matches\        (real-match imitation data)
rem   _run_srv_rec.bat magneto_rom     -> data\recordings\magneto_rom\    (labeled combo/drill)
rem   _run_srv_rec.bat storm_tj_left   -> data\recordings\storm_tj_left\  (labeled setup)
rem Switch what you're drilling = relaunch with a different label. The folder IS the label.
set LABEL=%1
if "%LABEL%"=="" set LABEL=matches
set MAPLECAST_RECORD_MATCHES=1
set MAPLECAST_RECORD_STATE=1
set MAPLECAST_RECORDINGS_DIR=C:\Users\trist\projects\mvc2-ai\data\recordings\%LABEL%
set MAPLECAST_RECORD_RETENTION_DAYS=90
if not exist "%MAPLECAST_RECORDINGS_DIR%" mkdir "%MAPLECAST_RECORDINGS_DIR%"
echo [rec] session label = %LABEL%
echo [rec] recording to  = %MAPLECAST_RECORDINGS_DIR%
build-headless-win\flycast.exe "C:\roms\roms\mvc2.gdi" > C:\Users\trist\projects\maplecast-flycast\_srv_rec.log 2>&1
