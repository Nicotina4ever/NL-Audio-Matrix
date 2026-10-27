$ErrorActionPreference = 'SilentlyContinue'
$pkg = 'C:\NLMatrix\abi-v3'
$probe = "$pkg\NLMatrixTransportProbe.exe"
$rec = 'C:\NLMatrix\NLMatrixDriverAudioTest.exe'
"=== SMOKE START ==="
Get-Date -Format o
foreach ($n in @('reaper','NLMatrixUI','NLMatrixRouter','NLMatrixRouterNew','NLMatrixRouterMetrics','NLMatrixDriverAudioTest','NLMatrixLive','NLMatrixProbe','NLMatrixLoopbackTest')) {
    $procs = Get-Process -Name $n -ErrorAction SilentlyContinue
    if ($procs) { $procs | Stop-Process -Force; "stopped " + $n }
}
Start-Sleep -Milliseconds 300

"=== capture target readback / clamp (ABI v3 SET_CAPTURE_TARGET, default format) ==="
& $probe --target 0 30 2>&1 | Where-Object { $_ -match 'SET_CAPTURE_TARGET|RESULT target' } | ForEach-Object { $_ }
& $probe --target 0 50 2>&1 | Where-Object { $_ -match 'SET_CAPTURE_TARGET|RESULT target' } | ForEach-Object { $_ }
& $probe --target 0 5  2>&1 | Where-Object { $_ -match 'SET_CAPTURE_TARGET|RESULT target' } | ForEach-Object { $_ }
& $probe --target 0 900 2>&1 | Where-Object { $_ -match 'SET_CAPTURE_TARGET|RESULT target' } | ForEach-Object { $_ }
& $probe --target 0 30 2>&1 | Where-Object { $_ -match 'SET_CAPTURE_TARGET|RESULT target' } | ForEach-Object { $_ }

"=== reset rings ==="
& $probe --reset 2>&1 | ForEach-Object { $_ }
"=== counters before ==="
& $probe --info 2>&1 | ForEach-Object { $_ }

"=== starting recorder: NL Mic 1, 8 s ==="
$recArgs = @('--record','--input','NL Mic 1','--out',"$pkg\capture_smoke.wav",'--seconds','8')
$recProc = Start-Process -FilePath $rec -ArgumentList $recArgs -RedirectStandardOutput "$pkg\rec_smoke.log" -RedirectStandardError "$pkg\rec_smoke_err.log" -PassThru -NoNewWindow
"recorder pid=" + $recProc.Id
Start-Sleep -Milliseconds 1500

"=== starting writers (ep0 stall 200 ms at mid-run; ep1..3 clean), 6 s ==="
$writers = @()
for ($ep = 0; $ep -le 3; $ep++) {
    $wargs = @('--write', [string]$ep, '6')
    if ($ep -eq 0) { $wargs += @('--stall','200') }
    $wlog = $pkg + '\writer_ep' + $ep + '.log'
    $werr = $pkg + '\writer_ep' + $ep + '_err.log'
    $writers += Start-Process -FilePath $probe -ArgumentList $wargs -RedirectStandardOutput $wlog -RedirectStandardError $werr -PassThru -NoNewWindow
}
"=== starting ep0 monitor, 8 s ==="
$mon = Start-Process -FilePath $probe -ArgumentList @('--monitor','0','8') -RedirectStandardOutput "$pkg\monitor_ep0.log" -RedirectStandardError "$pkg\monitor_ep0_err.log" -PassThru -NoNewWindow

$writers | Wait-Process -Timeout 40
$recProc | Wait-Process -Timeout 40
$mon | Wait-Process -Timeout 40
Start-Sleep -Milliseconds 300

"=== counters after ==="
& $probe --info 2>&1 | ForEach-Object { $_ }
"=== monitor ep0 (underrun growth incl. 200 ms stall at ~t=3s) ==="
Get-Content "$pkg\monitor_ep0.log" -ErrorAction SilentlyContinue | ForEach-Object { $_ }
"=== recorder log tail ==="
Get-Content "$pkg\rec_smoke.log" -ErrorAction SilentlyContinue | Select-Object -Last 6 | ForEach-Object { $_ }
Get-Content "$pkg\rec_smoke_err.log" -ErrorAction SilentlyContinue | Select-Object -Last 4 | ForEach-Object { $_ }
"=== captured wav ==="
Get-Item "$pkg\capture_smoke.wav" -ErrorAction SilentlyContinue | ForEach-Object { $_.Name + " | " + $_.Length + " | " + $_.LastWriteTime }
"=== writer ep0 log tail ==="
Get-Content "$pkg\writer_ep0.log" -ErrorAction SilentlyContinue | Select-Object -Last 8 | ForEach-Object { $_ }
"=== SMOKE END ==="
