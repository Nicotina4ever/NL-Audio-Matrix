# guest-disconnect-test.ps1 - recovery test for driver endpoint loss while the router runs.
# Runs in the throw-away Windows test VM only. Disables the NLMatrix root media
# device for ~6 s, re-enables it, then inspects the router log for offline and
# reconnection messages. No product code depends on this script.
$ErrorActionPreference = 'SilentlyContinue'
$root = 'C:\NLMatrix'
$log = "$root\disconnect_router.log"
"=== DISCONNECT RECOVERY TEST ==="
Get-Date -Format o
Get-Process NLMatrixRouterFinal2, NLMatrixRouter, NLMatrixDriverAudioTest -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
Remove-Item $log -Force -ErrorAction SilentlyContinue
$router = Start-Process -FilePath "$root\NLMatrixRouterFinal2.exe" -ArgumentList '--config', "$root\disconnect.json", '--seconds', '30' -RedirectStandardOutput $log -NoNewWindow -PassThru
"router pid=" + $router.Id
Start-Sleep -Seconds 8
$nl = Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -like '*NLMatrix*' -and $_.Status -eq 'OK' }
"nl devices before disable: " + @($nl).Count
foreach ($d in $nl) { "disabling " + $d.InstanceId; Disable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 6
"re-enabling"
foreach ($d in $nl) { Enable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false -ErrorAction SilentlyContinue }
pnputil /scan-devices | Out-Null
Start-Sleep -Seconds 12
$router.WaitForExit(20000) | Out-Null
"router exited: " + $router.HasExited + " code=" + $router.ExitCode
"=== LOG HIGHLIGHTS ==="
$lines = Get-Content $log -ErrorAction SilentlyContinue
$lines | Where-Object { $_ -match 'offline|reconnected|connected|error|failed|Engine|Stopped' } | Select-Object -First 40 | ForEach-Object { $_ }
$meters = @($lines | Where-Object { $_ -like 'METERS*' })
"meterLines=" + $meters.Count
if ($meters.Count -gt 0) { "first=" + $meters[0].Substring(0, [Math]::Min(300, $meters[0].Length)); "last=" + $meters[-1].Substring(0, [Math]::Min(300, $meters[-1].Length)) }
"=== TEST END ==="
