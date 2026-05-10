# scripts/local-play.ps1 -- launch the local-play topology in two windows.
#
# Window 1: headless server (its own cmd window, console output visible there)
# Window 2: mirror client  (its own flycast window + AllocConsole alongside)
#
# This script just opens both, waits, then cleans up when you close the
# client. It does not try to merge their output -- look at each window
# directly.
#
# Usage:
#   & "C:\Users\trist\projects\maplecast-flycast\scripts\local-play.ps1"
#
# Override:
#   -RomPath full path to a .gdi

param(
    [string]$RomPath = "C:\roms\mvc2_us\Marvel vs. Capcom 2 v1.001 (2000)(Capcom)(US)[!].gdi"
)

$repoRoot  = "C:\Users\trist\projects\maplecast-flycast"
$serverExe = Join-Path $repoRoot "build-headless-win\flycast.exe"
$clientExe = Join-Path $repoRoot "build\flycast.exe"

if (!(Test-Path -LiteralPath $serverExe)) { Write-Host "FAIL: missing $serverExe" -ForegroundColor Red; exit 2 }
if (!(Test-Path -LiteralPath $clientExe)) { Write-Host "FAIL: missing $clientExe" -ForegroundColor Red; exit 2 }
if (!(Test-Path -LiteralPath $RomPath))   { Write-Host "FAIL: missing ROM $RomPath" -ForegroundColor Red; exit 2 }

# Clean slate.
Get-Process -Name "flycast" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

# ---- Window 1: server -------------------------------------------------
$env:MAPLECAST                  = "1"
$env:MAPLECAST_MIRROR_SERVER    = "1"
$env:MAPLECAST_HEADLESS_AUTOLOAD= "1"
# Per-match continuous recording: every match gets its own .mcrec
# under <repo>/recordings/, retention 7 days.
$env:MAPLECAST_RECORD_MATCHES   = "1"
$env:MAPLECAST_RECORDINGS_DIR   = (Join-Path $repoRoot "recordings")
Remove-Item env:MAPLECAST_MIRROR_CLIENT -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_SERVER_HOST   -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_SERVER_PORT   -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_REPLAY_OUT    -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_REPLAY_IN     -ErrorAction SilentlyContinue

$server = Start-Process -FilePath $serverExe -ArgumentList "`"$RomPath`"" -PassThru
Write-Host "[local-play] server PID: $($server.Id)" -ForegroundColor Cyan

# Wait for :7200 to accept connections.
$deadline = (Get-Date).AddSeconds(20)
$ready = $false
while ((Get-Date) -lt $deadline) {
    if ($server.HasExited) {
        Write-Host "FAIL: server exited before :7200 opened." -ForegroundColor Red
        exit 3
    }
    $tcp = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $tcp.BeginConnect("127.0.0.1", 7200, $null, $null)
        if ($iar.AsyncWaitHandle.WaitOne(250)) {
            try { $tcp.EndConnect($iar); $ready = $true; break } catch {}
        }
    } finally { $tcp.Close() }
    Start-Sleep -Milliseconds 250
}
if (-not $ready) {
    Write-Host "FAIL: :7200 never accepted connections." -ForegroundColor Red
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    exit 3
}
Write-Host "[local-play] server accepting on :7200" -ForegroundColor Cyan

# ---- Window 2: client -------------------------------------------------
$env:MAPLECAST_MIRROR_CLIENT= "1"
$env:MAPLECAST_SERVER_HOST  = "127.0.0.1"
$env:MAPLECAST_SERVER_PORT  = "7200"
Remove-Item env:MAPLECAST_MIRROR_SERVER     -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_HEADLESS_AUTOLOAD -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_RECORD_MATCHES    -ErrorAction SilentlyContinue
Remove-Item env:MAPLECAST_RECORDINGS_DIR    -ErrorAction SilentlyContinue

$client = Start-Process -FilePath $clientExe -PassThru
Write-Host "[local-play] client PID: $($client.Id)" -ForegroundColor Cyan
Write-Host "[local-play] press F9 in the game window to record/stop." -ForegroundColor Cyan
Write-Host "[local-play] close the game window to shut everything down." -ForegroundColor Cyan

# Block until the client window closes.
$client.WaitForExit()

Write-Host "[local-play] client exited. stopping server." -ForegroundColor Cyan
Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
Get-Process -Name "flycast" -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host "[local-play] done."
