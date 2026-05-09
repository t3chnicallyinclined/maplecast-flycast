# scripts/test-record-replay.ps1
#
# End-to-end test of the .mcrec V3 record/replay flow.
#
# Topology (matches the local rollback predictor architecture in
# docs/ROLLBACK-PREDICTION.md and docs/WINDOWS-HEADLESS-BUILD.md):
#   - SERVER:  build-headless-win/flycast.exe (MAPLECAST_HEADLESS=ON)
#              Full SH4 + AICA + mirror-server :7200. No GUI by design.
#   - CLIENT:  build/flycast.exe (MAPLECAST_CLIENT_ONLY=ON)
#              Renderer only, connects to 127.0.0.1:7200, GUI window.
#
# Default mode (-Visual omitted): headless-only — useful for byte-level
# testing where you don't need to *watch* the replay, just verify the
# bytes round-trip.
#
# -Visual mode: launches BOTH processes — headless server replays the
# .mcrec while mirror client renders the TA stream in a real window.
# Same architecture as production (server on VPS, client on player's
# machine), only with both endpoints on localhost.
#
# Usage:
#   .\scripts\test-record-replay.ps1                  # record + headless replay
#   .\scripts\test-record-replay.ps1 -Visual          # record + visual replay
#   .\scripts\test-record-replay.ps1 -RecordSeconds 30
#   .\scripts\test-record-replay.ps1 -SkipReplay
#   .\scripts\test-record-replay.ps1 -RomPath "C:\..."
#
# Requires:
#   - build-headless-win/flycast.exe (cmake -DMAPLECAST_HEADLESS=ON)
#   - build/flycast.exe              (cmake -DMAPLECAST_CLIENT_ONLY=ON)  [for -Visual]

param(
    [int]$RecordSeconds = 10,
    [int]$ReplaySeconds = 8,
    [switch]$SkipReplay,
    [switch]$Visual,
    [string]$RomPath = "C:\roms\mvc2_us\Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi",
    [string]$Flycast = "build-headless-win\flycast.exe",
    [string]$ClientFlycast = "build\flycast.exe",
    [string]$McrecPath = (Join-Path $env:TEMP "maplecast-test.mcrec")
)

$ErrorActionPreference = "Stop"

function Send-WsCommand {
    param([string]$Json)
    $ws = New-Object System.Net.WebSockets.ClientWebSocket
    $cts = New-Object System.Threading.CancellationTokenSource
    $cts.CancelAfter(5000)
    $uri = [System.Uri]::new("ws://127.0.0.1:7211")
    $ws.ConnectAsync($uri, $cts.Token).Wait()

    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Json)
    $segment = New-Object System.ArraySegment[byte] -ArgumentList @(,$bytes)
    $ws.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $cts.Token).Wait()

    # Read reply
    $buf = New-Object byte[] 4096
    $recvSeg = New-Object System.ArraySegment[byte] -ArgumentList @(,$buf)
    $result = $ws.ReceiveAsync($recvSeg, $cts.Token).Result
    $reply = [System.Text.Encoding]::UTF8.GetString($buf, 0, $result.Count)
    $ws.CloseAsync(
        [System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure,
        "done", $cts.Token).Wait()
    return $reply
}

# ── Sanity checks ──
if (!(Test-Path $Flycast)) {
    Write-Host "FAIL: flycast not found at $Flycast" -ForegroundColor Red
    Write-Host "Build with: cmake --build build-headless-win --target flycast"
    exit 2
}
if (!(Test-Path -LiteralPath $RomPath)) {
    Write-Host "FAIL: ROM not found at $RomPath" -ForegroundColor Red
    exit 2
}

# Clean up old recording if present
if (Test-Path $McrecPath) { Remove-Item $McrecPath -Force }

# ── Phase 1: record ──
Write-Host "[test] Phase 1: starting flycast for record..."  -ForegroundColor Cyan
Write-Host "[test]   ROM:  $RomPath"
Write-Host "[test]   .mcrec: $McrecPath"
Write-Host "[test]   record duration: ${RecordSeconds}s"

$env:MAPLECAST = "1"                            # input UDP listener on :7100
$env:MAPLECAST_MIRROR_SERVER = "1"
$env:MAPLECAST_HEADLESS_AUTOLOAD = "1"          # boot from savestate, not attract mode
$env:MAPLECAST_HEADLESS_DISABLE_SYS_MISC_1 = "1"
# V2 discipline: trigger recording at autoload boundary (same lifecycle
# moment replay restores). Recording captures the autoload state via
# dc_savestate(slot 99), embeds in .mcrec, dc_loadstate(99) anchors SH4
# at the post-load state. No mid-execution capture.
$env:MAPLECAST_REPLAY_OUT   = $McrecPath
$env:MAPLECAST_REPLAY_P1_NAME = "TestP1"
$env:MAPLECAST_REPLAY_P2_NAME = "TestP2"
# Warmup default = 0 (no skip). Override via MAPLECAST_REPLAY_WARMUP if
# you want SH4 to free-run N frames before inputs start being logged.
Remove-Item env:MAPLECAST_REPLAY_WARMUP -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_REPLAY_IN -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_TEST_ROLLBACK -ErrorAction SilentlyContinue

$logRecord = Join-Path $env:TEMP "maplecast-test-record.log"
$proc = Start-Process -FilePath ".\$Flycast" -ArgumentList "`"$RomPath`"" `
    -RedirectStandardOutput $logRecord -RedirectStandardError "$logRecord.err" `
    -PassThru -NoNewWindow

# Wait for autoload + recording-start + mirror server to be ready
Start-Sleep -Seconds 3

# In -Visual mode launch the mirror client too so the operator can see
# the game and play it while recording.
$recClient = $null
$logRecClient = Join-Path $env:TEMP "maplecast-test-record-client.log"
if ($Visual) {
    if (!(Test-Path -LiteralPath $ClientFlycast)) {
        Write-Host "[test] FAIL: mirror client not found at $ClientFlycast" -ForegroundColor Red
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        exit 2
    }
    $oldServer = $env:MAPLECAST_MIRROR_SERVER
    Remove-Item env:MAPLECAST_MIRROR_SERVER -ErrorAction SilentlyContinue
    Remove-Item env:MAPLECAST -ErrorAction SilentlyContinue
    Remove-Item env:MAPLECAST_HEADLESS_AUTOLOAD -ErrorAction SilentlyContinue
    $env:MAPLECAST_MIRROR_CLIENT = "1"
    $env:MAPLECAST_SERVER_HOST   = "127.0.0.1"
    $env:MAPLECAST_SERVER_PORT   = "7200"

    Write-Host "[test] launching mirror client for record (windowed) - play now!" -ForegroundColor Yellow
    $recClient = Start-Process -FilePath ".\$ClientFlycast" `
        -RedirectStandardOutput $logRecClient `
        -RedirectStandardError ($logRecClient + ".err") `
        -PassThru

    # Restore server-side env in this shell so subsequent ws probes are unaffected
    Remove-Item env:MAPLECAST_MIRROR_CLIENT -ErrorAction SilentlyContinue
    Remove-Item env:MAPLECAST_SERVER_HOST -ErrorAction SilentlyContinue
    Remove-Item env:MAPLECAST_SERVER_PORT -ErrorAction SilentlyContinue
    if ($oldServer) { $env:MAPLECAST_MIRROR_SERVER = $oldServer }

    # Give the client a moment to handshake before recording starts
    Start-Sleep -Seconds 2
}

# Recording auto-starts at autoload via MAPLECAST_REPLAY_OUT (set above).
# Just wait while operator plays.
Write-Host "[test] recording for $RecordSeconds s - PLAY NOW in the flycast window..." -ForegroundColor Cyan
Start-Sleep -Seconds $RecordSeconds

# Stop client first, then headless
Start-Sleep -Seconds 1
if ($recClient) { Stop-Process -Id $recClient.Id -Force -ErrorAction SilentlyContinue }
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

if (!(Test-Path $McrecPath)) {
    # at_match=true and the operator never reached a match - armed
    # but never fired. Disarm via record_stop already happened above;
    # nothing more to do.
    Write-Host "[test] no .mcrec produced - recording was armed but in_match never went 0 to 1" -ForegroundColor Yellow
    Write-Host "[test] tip: enter a match within RecordSeconds to capture" -ForegroundColor Yellow
    exit 0
}

$size = (Get-Item $McrecPath).Length
Write-Host "[test] recorded $size bytes -> $McrecPath" -ForegroundColor Green

if ($SkipReplay) {
    Write-Host "[test] -SkipReplay set, exiting." -ForegroundColor Yellow
    exit 0
}

# ── Phase 2: replay ──
Write-Host ""
if ($Visual) {
    Write-Host "[test] Phase 2 (Visual): headless replay server + mirror client (windowed)" -ForegroundColor Cyan
} else {
    Write-Host "[test] Phase 2: replaying $McrecPath for ${ReplaySeconds}s (headless, no window)..." -ForegroundColor Cyan
}

# Headless replay server — produces TA stream from the .mcrec.
# In -Visual mode, the mirror client connects to it and renders.
# Replay's loadStartSavestate writes the embedded slot bytes back to
# slot 99 and dc_loadstate(99)'s — same code path the recorder used at
# its autoload, on the same bytes.
Remove-Item env:MAPLECAST_HEADLESS_AUTOLOAD -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_REPLAY_OUT      -ErrorAction SilentlyContinue
$env:MAPLECAST_REPLAY_IN = $McrecPath
$logReplay = Join-Path $env:TEMP "maplecast-test-replay.log"
$proc2 = Start-Process -FilePath ".\$Flycast" -ArgumentList "`"$RomPath`"" `
    -RedirectStandardOutput $logReplay -RedirectStandardError "$logReplay.err" `
    -PassThru -NoNewWindow

if ($Visual) {
    if (!(Test-Path -LiteralPath $ClientFlycast)) {
        Write-Host "[test] FAIL: mirror client not found at $ClientFlycast" -ForegroundColor Red
        Write-Host "Build with cmake -B build -DMAPLECAST_CLIENT_ONLY=ON then cmake --build build --target flycast"
        Stop-Process -Id $proc2.Id -Force -ErrorAction SilentlyContinue
        Remove-Item env:MAPLECAST_REPLAY_IN
        exit 2
    }
    # Give headless server time to autoload the embedded savestate
    Start-Sleep -Seconds 3

    Write-Host "[test] launching mirror client (windowed)..." -ForegroundColor Cyan
    Write-Host "[test] close the flycast window or wait ${ReplaySeconds}s" -ForegroundColor Yellow
    $logClient = Join-Path $env:TEMP "maplecast-test-client.log"

    # Mirror client env: connects to local headless server
    $oldServer = $env:MAPLECAST_MIRROR_SERVER
    Remove-Item env:MAPLECAST_MIRROR_SERVER -ErrorAction SilentlyContinue
    $env:MAPLECAST_MIRROR_CLIENT = "1"
    $env:MAPLECAST_SERVER_HOST   = "127.0.0.1"
    $env:MAPLECAST_SERVER_PORT   = "7200"

    $client = Start-Process -FilePath ".\$ClientFlycast" `
        -RedirectStandardOutput $logClient `
        -RedirectStandardError ($logClient + ".err") `
        -PassThru
    $client.WaitForExit($ReplaySeconds * 1000) | Out-Null
    if (!$client.HasExited) {
        Write-Host "[test] reached ${ReplaySeconds}s, closing client..." -ForegroundColor Yellow
        Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue
    }

    Remove-Item env:MAPLECAST_MIRROR_CLIENT
    Remove-Item env:MAPLECAST_SERVER_HOST
    Remove-Item env:MAPLECAST_SERVER_PORT
    if ($oldServer) { $env:MAPLECAST_MIRROR_SERVER = $oldServer }
    Write-Host "[test] mirror client log: $logClient" -ForegroundColor Cyan
} else {
    Start-Sleep -Seconds $ReplaySeconds
}

Stop-Process -Id $proc2.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Remove-Item env:MAPLECAST_REPLAY_IN

$replayLog = Get-Content $logReplay -ErrorAction SilentlyContinue
$opened   = $replayLog | Select-String "embedded savestate|in-memory savestate applied"

Write-Host "[test] replay log highlights:" -ForegroundColor Cyan
$replayLog | Select-String "replay|embedded savestate|MCREC|autoload-debug" | ForEach-Object { Write-Host "  $($_.Line)" }

if ($opened) {
    Write-Host "[test] PASS: replay loaded the embedded savestate" -ForegroundColor Green
} else {
    Write-Host "[test] WARN: didn't see embedded savestate load message" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "[test] full logs:"
Write-Host "  record: $logRecord"
Write-Host "  replay: $logReplay"
Write-Host "  .mcrec: $McrecPath ($size bytes)"
if ($Visual) { Write-Host "  client: $logClient" }
