# scripts/replay-start.ps1 — bind to a hotkey to start recording an .mcrec
# from your current mid-game position.
#
# Sends record_start over the headless server's control-WS (loopback :7211).
# Output path defaults to ~/Documents/MapleCastReplays/<timestamp>.mcrec —
# pass -Path to override.
#
# Bind via AutoHotkey:
#   F9::Run, powershell -NoProfile -ExecutionPolicy Bypass -File "C:\Users\trist\projects\maplecast-flycast\scripts\replay-start.ps1"
#
# Or via Windows shortcut: target the .ps1, set hotkey in the shortcut props.

param(
    [string]$Path = ""
)

if ([string]::IsNullOrWhiteSpace($Path)) {
    $dir = Join-Path $env:USERPROFILE "Documents\MapleCastReplays"
    if (!(Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $stamp = (Get-Date).ToString("yyyyMMdd-HHmmss")
    $Path = Join-Path $dir "mvc2-$stamp.mcrec"
}

$json = @{
    cmd      = "record_start"
    path     = $Path
    p1_name  = $env:USERNAME
    reply_id = "rec-$([guid]::NewGuid().ToString('N').Substring(0,8))"
} | ConvertTo-Json -Compress

try {
    $ws  = New-Object System.Net.WebSockets.ClientWebSocket
    $cts = New-Object System.Threading.CancellationTokenSource
    $cts.CancelAfter(5000)
    $ws.ConnectAsync([System.Uri]::new("ws://127.0.0.1:7211"), $cts.Token).Wait()

    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $seg   = New-Object System.ArraySegment[byte] -ArgumentList @(,$bytes)
    $ws.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $cts.Token).Wait()

    $buf      = New-Object byte[] 4096
    $recvSeg  = New-Object System.ArraySegment[byte] -ArgumentList @(,$buf)
    $result   = $ws.ReceiveAsync($recvSeg, $cts.Token).Result
    $reply    = [System.Text.Encoding]::UTF8.GetString($buf, 0, $result.Count)
    $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "done", $cts.Token).Wait()

    Write-Host "[record-start] reply: $reply"
    Write-Host "[record-start] saving to: $Path"
} catch {
    Write-Host "[record-start] FAILED: $_" -ForegroundColor Red
    Write-Host "Is flycast running with the headless server up (control-WS on :7211)?"
}
