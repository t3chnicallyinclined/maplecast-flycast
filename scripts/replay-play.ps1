# scripts/replay-play.ps1 — open a .mcrec file and play it back in a
# fresh flycast (headless server + mirror client window).
#
# Usage:
#   .\scripts\replay-play.ps1 -McrecPath "C:\path\to\file.mcrec"
#   .\scripts\replay-play.ps1                  # uses most-recent .mcrec in default dir
#
# Spawns its own flycast pair (headless server + windowed client). The
# active flycast you might be playing in is unaffected — replay runs in
# a separate process.

param(
    [string]$McrecPath = "",
    [string]$RomPath = "C:\roms\mvc2_us\Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi",
    [int]$DurationSeconds = 0   # 0 = wait until client window closed
)

$repoRoot     = "C:\Users\trist\projects\maplecast-flycast"
$serverExe    = Join-Path $repoRoot "build-headless-win\flycast.exe"
$clientExe    = Join-Path $repoRoot "build\flycast.exe"

if ([string]::IsNullOrWhiteSpace($McrecPath)) {
    $dir = Join-Path $env:USERPROFILE "Documents\MapleCastReplays"
    if (!(Test-Path -LiteralPath $dir)) {
        Write-Host "No replay directory at $dir — pass -McrecPath to specify a file." -ForegroundColor Yellow
        exit 2
    }
    $latest = Get-ChildItem -Path $dir -Filter "*.mcrec" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $latest) {
        Write-Host "No .mcrec files found in $dir" -ForegroundColor Yellow
        exit 2
    }
    $McrecPath = $latest.FullName
    Write-Host "Using most-recent: $McrecPath"
}

if (!(Test-Path -LiteralPath $McrecPath)) {
    Write-Host "FAIL: .mcrec not found at $McrecPath" -ForegroundColor Red
    exit 2
}

# Stop any flycast already running (replay needs ports 7100/7200/7211 free).
Get-Process -Name "flycast" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

# ── Headless replay server ──
$serverLog = Join-Path $env:TEMP "replay-play-server.log"
if (Test-Path $serverLog) { Remove-Item $serverLog -Force }

$env:MAPLECAST                          = "1"
$env:MAPLECAST_MIRROR_SERVER            = "1"
$env:MAPLECAST_HEADLESS_DISABLE_SYS_MISC_1 = "1"
$env:MAPLECAST_REPLAY_IN                = $McrecPath
Remove-Item env:MAPLECAST_REPLAY_OUT      -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_HEADLESS_AUTOLOAD -ErrorAction SilentlyContinue

$server = Start-Process -FilePath $serverExe -ArgumentList "`"$RomPath`"" `
    -RedirectStandardOutput $serverLog -RedirectStandardError "$serverLog.err" `
    -PassThru -NoNewWindow
Write-Host "[replay-play] server PID: $($server.Id)"

Start-Sleep -Seconds 3   # let the headless server load the embedded savestate

# ── Mirror client (windowed) ──
$clientLog = Join-Path $env:TEMP "replay-play-client.log"
Remove-Item env:MAPLECAST_MIRROR_SERVER -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST              -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_REPLAY_IN    -ErrorAction SilentlyContinue
$env:MAPLECAST_MIRROR_CLIENT = "1"
$env:MAPLECAST_SERVER_HOST   = "127.0.0.1"
$env:MAPLECAST_SERVER_PORT   = "7200"

$client = Start-Process -FilePath $clientExe `
    -RedirectStandardOutput $clientLog `
    -RedirectStandardError ($clientLog + ".err") `
    -PassThru
Write-Host "[replay-play] client PID: $($client.Id) — playing $McrecPath"

if ($DurationSeconds -gt 0) {
    Start-Sleep -Seconds $DurationSeconds
} else {
    Write-Host "[replay-play] close the flycast window when done"
    $client.WaitForExit()
}

Stop-Process -Id $client.Id -Force -ErrorAction SilentlyContinue
Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
Write-Host "[replay-play] done"
