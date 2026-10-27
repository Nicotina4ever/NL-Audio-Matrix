$ErrorActionPreference = 'SilentlyContinue'
"CHECK-START"
Get-Date -Format o
$os = Get-CimInstance Win32_OperatingSystem
"lastBootUpTime=" + $os.LastBootUpTime
"uptimeMin=" + [math]::Round(((Get-Date) - $os.LastBootUpTime).TotalMinutes, 1)
"hostName=" + $env:COMPUTERNAME
"engineRunning=" + (Get-Service Audiosrv).Status
$procs = Get-Process | Where-Object { $_.ProcessName -match 'reaper|NLMatrix' }
if ($procs) { $procs | ForEach-Object { "proc " + $_.ProcessName + " pid=" + $_.Id } } else { "no reaper/NLMatrix processes" }
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='sysvad_componentizedaudiosample'"
"svc state=" + $svc.State + " path=" + $svc.PathName
"driverTestExe=" + (Test-Path 'C:\NLMatrix\NLMatrixDriverAudioTest.exe')
"postrebootScript=" + (Test-Path 'C:\NLMatrix\guest-postreboot-v2.ps1')
"CHECK-END"
