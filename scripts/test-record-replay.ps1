# scripts/test-record-replay.ps1
#
# End-to-end test of the .mcrec V3 record/replay flow.
# Demonstrates the full cycle: start headless flycast, send a
# record_start command via the control-WS at ws://127.0.0.1:7211,
# play for N seconds, stop, then optionally replay.
#
# Usage:
#   .\scripts\test-record-replay.ps1                       # record 10s, replay
#   .\scripts\test-record-replay.ps1 -RecordSeconds 30     # record 30s
#   .\scripts\test-record-replay.ps1 -SkipReplay           # only record
#   .\scripts\test-record-replay.ps1 -RomPath "C:\roms\..." # custom ROM
#
# Requires: build-headless-win/flycast.exe built (cmake --build ...).
# Output:   .mcrec file written to %TEMP%\maplecast-test.mcrec

param(
    [int]$RecordSeconds = 10,
    [int]$ReplaySeconds = 8,
    [switch]$SkipReplay,
    [string]$RomPath = "C:\roms\mvc2_us\Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi",
    [string]$Flycast = "build-headless-win\flycast.exe",
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

$env:MAPLECAST_MIRROR_SERVER = "1"
$env:MAPLECAST_HEADLESS_DISABLE_SYS_MISC_1 = "1"
Remove-Item env:MAPLECAST_REPLAY_IN -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_TEST_ROLLBACK -ErrorAction SilentlyContinue

$logRecord = Join-Path $env:TEMP "maplecast-test-record.log"
$proc = Start-Process -FilePath ".\$Flycast" -ArgumentList "`"$RomPath`"" `
    -RedirectStandardOutput $logRecord -RedirectStandardError "$logRecord.err" `
    -PassThru -NoNewWindow

# Wait for control-WS to be ready (~2s)
Start-Sleep -Seconds 3

# Send record_start
$startCmd = @{
    cmd      = "record_start"
    path     = $McrecPath
    p1_name  = "TestP1"
    p2_name  = "TestP2"
    reply_id = "rec1"
} | ConvertTo-Json -Compress

try {
    $reply = Send-WsCommand -Json $startCmd
    Write-Host "[test] record_start reply: $reply" -ForegroundColor Green
} catch {
    Write-Host "[test] FAIL: record_start: $_" -ForegroundColor Red
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

# Let the emulator run while recording
Write-Host "[test] recording for ${RecordSeconds}s..." -ForegroundColor Cyan
Start-Sleep -Seconds $RecordSeconds

# Send record_stop
$stopCmd = @{ cmd = "record_stop"; reply_id = "rec2" } | ConvertTo-Json -Compress
try {
    $reply = Send-WsCommand -Json $stopCmd
    Write-Host "[test] record_stop reply: $reply" -ForegroundColor Green
} catch {
    Write-Host "[test] FAIL: record_stop: $_" -ForegroundColor Red
}

# Stop flycast
Start-Sleep -Seconds 1
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

if (!(Test-Path $McrecPath)) {
    Write-Host "[test] FAIL: .mcrec not produced at $McrecPath" -ForegroundColor Red
    Write-Host "Last log lines:"
    Get-Content $logRecord -Tail 20
    exit 1
}

$size = (Get-Item $McrecPath).Length
Write-Host "[test] recorded $size bytes -> $McrecPath" -ForegroundColor Green

if ($SkipReplay) {
    Write-Host "[test] -SkipReplay set, exiting." -ForegroundColor Yellow
    exit 0
}

# ── Phase 2: replay ──
Write-Host ""
Write-Host "[test] Phase 2: replaying $McrecPath for ${ReplaySeconds}s..." -ForegroundColor Cyan

$env:MAPLECAST_REPLAY_IN = $McrecPath
$logReplay = Join-Path $env:TEMP "maplecast-test-replay.log"
$proc2 = Start-Process -FilePath ".\$Flycast" -ArgumentList "`"$RomPath`"" `
    -RedirectStandardOutput $logReplay -RedirectStandardError "$logReplay.err" `
    -PassThru -NoNewWindow
Start-Sleep -Seconds $ReplaySeconds
Stop-Process -Id $proc2.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Remove-Item env:MAPLECAST_REPLAY_IN

$replayLog = Get-Content $logReplay -ErrorAction SilentlyContinue
$opened   = $replayLog | Select-String "replay-reader.*embedded savestate"
$started  = $replayLog | Select-String "MCREC|playback"

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
