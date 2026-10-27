$ErrorActionPreference = 'SilentlyContinue'
"=== IDLE CHECK ==="
Get-Date -Format o
Get-Service Audiosrv,AudioEndpointBuilder | ForEach-Object { "service " + $_.Name + "=" + $_.Status }
$procs = Get-Process | Where-Object { $_.ProcessName -match 'reaper|NLMatrix' }
if ($procs) { $procs | ForEach-Object { "ACTIVE " + $_.ProcessName + " pid=" + $_.Id } } else { "IDLE: no reaper/NLMatrix processes" }
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='sysvad_componentizedaudiosample'"
"driverService state=" + $svc.State
$eps = Get-PnpDevice -Class AudioEndpoint | Where-Object { $_.FriendlyName -like '*NLMatrix*' -or $_.FriendlyName -like 'NL Mic *' -or $_.FriendlyName -like 'NL Return *' }
$eps | ForEach-Object { "ENDPOINT " + $_.Status + " | " + $_.FriendlyName }
"endpointCount=" + @($eps).Count
"IDLE_CHECK_END"
