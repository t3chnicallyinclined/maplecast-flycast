# scripts/replay-stop.ps1 — bind to a hotkey to stop the active recording.
# Sends record_stop over the headless server's control-WS (loopback :7211).
#
# Bind via AutoHotkey:
#   F10::Run, powershell -NoProfile -ExecutionPolicy Bypass -File "C:\Users\trist\projects\maplecast-flycast\scripts\replay-stop.ps1"

$json = @{
    cmd      = "record_stop"
    reply_id = "rec-stop-$([guid]::NewGuid().ToString('N').Substring(0,8))"
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

    Write-Host "[record-stop] reply: $reply"
} catch {
    Write-Host "[record-stop] FAILED: $_" -ForegroundColor Red
    Write-Host "Is flycast running with the headless server up (control-WS on :7211)?"
}
