$ErrorActionPreference = 'SilentlyContinue'
"=== POST-REBOOT VERIFY (v2 driver untouched, NO install) ==="
Get-Date -Format o
$os = Get-CimInstance Win32_OperatingSystem
"os: " + $os.Caption + " build " + $os.BuildNumber
"lastBootUpTime: " + $os.LastBootUpTime
"uptimeMinutes: " + [math]::Round(((Get-Date) - $os.LastBootUpTime).TotalMinutes, 1)
"=== service (must be current v2 install) ==="
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='sysvad_componentizedaudiosample'"
if ($svc) {
    "state=" + $svc.State + " startMode=" + $svc.StartMode
    "path=" + $svc.PathName
    $img = $svc.PathName
    if ($img) {
        $full = $img
        if ($full -like '\??\*') { $full = $full.Substring(4) }
        $full = $full -replace '^\\SystemRoot\\', 'C:\Windows\'
        if (Test-Path $full) { "loaded image sha256=" + (Get-FileHash $full -Algorithm SHA256).Hash }
    }
} else { "service NOT FOUND" }
"=== audio services ==="
Get-Service Audiosrv,AudioEndpointBuilder | ForEach-Object { $_.Name + "=" + $_.Status }
"=== NLMatrix endpoints (all statuses) ==="
$eps = Get-PnpDevice -Class AudioEndpoint | Where-Object { $_.FriendlyName -like '*NLMatrix*' -or $_.FriendlyName -like 'NL Mic *' -or $_.FriendlyName -like 'NL Return *' }
$eps | ForEach-Object { "ENDPOINT " + $_.Status + " | " + $_.FriendlyName }
"endpointCount=" + @($eps).Count
"micOK=" + @($eps | Where-Object { $_.FriendlyName -like 'NL Mic *' -and $_.Status -eq 'OK' }).Count + " returnNamedOK=" + @($eps | Where-Object { $_.FriendlyName -like 'NL Return *' -and $_.Status -eq 'OK' }).Count
"=== MEDIA node ==="
Get-PnpDevice -Class MEDIA | Where-Object { $_.FriendlyName -like '*NLMatrix*' } | ForEach-Object { $_.Status + " | " + $_.InstanceId + " | " + $_.FriendlyName }
"=== test tools present ==="
Get-Item 'C:\NLMatrix\NLMatrixDriverAudioTest.exe','C:\NLMatrix\NLMatrixDriverTest.exe' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name + " | " + $_.Length }
"=== leftover processes before cleanup ==="
Get-Process | Where-Object { $_.ProcessName -match 'reaper|NLMatrix' } | ForEach-Object { $_.ProcessName + " | pid=" + $_.Id }
foreach ($n in @('reaper','NLMatrixUI','NLMatrixRouter','NLMatrixRouterNew','NLMatrixRouterMetrics','NLMatrixDriverAudioTest','NLMatrixDriverTest','NLMatrixLive','NLMatrixProbe','NLMatrixBridge','NLMatrixLoopbackTest')) {
    $p = Get-Process -Name $n -ErrorAction SilentlyContinue
    if ($p) { $p | Stop-Process -Force; "killed " + $n }
}
Start-Sleep -Milliseconds 500
"=== leftover processes after cleanup ==="
$left = Get-Process | Where-Object { $_.ProcessName -match 'reaper|NLMatrix' }
if ($left) { $left | ForEach-Object { $_.ProcessName + " | pid=" + $_.Id } } else { "none" }
"=== END ==="
