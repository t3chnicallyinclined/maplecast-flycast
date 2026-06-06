$env:MAPLECAST_STATE_REPLICA = "149.28.44.118:7201"

$flycast = 'C:\Users\trist\projects\maplecast-flycast\build\flycast.exe'
$log     = 'C:\Users\trist\projects\maplecast-flycast\replica-run.log'

Write-Host "[launcher] starting state-replica client (no ROM, booting from server state)..." -ForegroundColor Cyan
& $flycast 2>&1 | Tee-Object -FilePath $log
