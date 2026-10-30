#requires -Version 5.1
<#
.SYNOPSIS
    Run-TestSuite.ps1 - lightweight local test coordinator for NL Audio Matrix.

.DESCRIPTION
    Launches one of the existing VM harnesses in an independent process (or via
    the Windows Task Scheduler), against a build that is hash-pinned for the
    whole run, and writes a compact JSON summary on disk.  The model only reads
    summary.json at a later synchronisation point: no polling, no LLM calls,
    no repeated automatic retries of the same failure.

    Concurrency: one run at a time (global mutex + lock file).  Before an audio
    suite starts, a busy guest (router, GUI, ffmpeg) aborts the run as BUSY so
    the user's own listening session is never disturbed.

.PARAMETER Suite
    smoke          build identity + guest endpoints/driver (read-only, seconds)
    guiselftest    deploy snapshot GUI+router, run --selftest, parse SUMMARY
    quickconnect   tools/vm/Run-QuickConnectRealClient.ps1
    endpoints      tools/vm/Run-EndpointAudioVerification.ps1 -Phase Both
    matrix         tools/vm/Run-EngineMatrix.ps1 -Profile engine2

.PARAMETER OutRoot
    Root for run directories (default reports/automation).

.PARAMETER BuildDir
    Build directory (default app/build).

.PARAMETER TimeoutMinutes
    Whole-run deadline.  Steps that exceed it are killed with their tree.

.PARAMETER Wait
    Block until the run finishes (interactive use).  Default: spawn detached
    and return immediately with the run id and the summary path.

.PARAMETER KeepRuns
    Number of run directories to keep (default 5).

.EXAMPLE
    .\tools\Run-TestSuite.ps1 -Suite smoke
    .\tools\Run-TestSuite.ps1 -Suite guiselftest -Wait
#>
[CmdletBinding()]
param(
    [ValidateSet('smoke','guiselftest','guilifecycle','guistudio','guiquickconnect','quickconnect','endpoints','endpoints8','matrix')]
    [string]$Suite = 'smoke',
    [string]$OutRoot = '',
    [string]$BuildDir = '',
    [int]$TimeoutMinutes = 40,
    [int]$KeepRuns = 5,
    [switch]$Wait,
    [switch]$Force,
    [string]$ScheduleAt = '',
    [switch]$UnregisterTask,
    # internal
    [switch]$Unattended,
    [string]$RunId = ''
)

$ErrorActionPreference = 'Stop'
$script:Workspace = Split-Path -Parent $PSScriptRoot
$script:VmHelpers = Join-Path $PSScriptRoot 'vm\NLVM.ps1'
$script:StepLogs = @()
$script:Anomalies = @()

if (-not $OutRoot)   { $OutRoot = Join-Path $script:Workspace 'reports\automation' }
if (-not $BuildDir)  { $BuildDir = Join-Path $script:Workspace 'app\build' }
$OutRoot = [System.IO.Path]::GetFullPath($OutRoot)
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)

function Write-Line([string]$m) { Write-Host $m }

function Get-BuildManifest([string]$dir) {
    $paths = [ordered]@{
        'NLMatrixUI.exe'      = Join-Path $dir 'NLMatrixUI_artefacts\Release\NLMatrixUI.exe'
        'NLMatrixRouter.exe'  = Join-Path $dir 'NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe'
        'NLMatrixAudio.sys'   = Join-Path $script:Workspace 'driver\package\NLMatrixAudio.sys'
        'NLMatrixAudio.inf'   = Join-Path $script:Workspace 'driver\package\ComponentizedAudioSample.inf'
        'NLMatrixRouter'      = Join-Path $dir 'NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe'
    }
    $hashes = [ordered]@{}
    foreach ($key in $paths.Keys) {
        $p = $paths[$key]
        if (Test-Path $p) {
            $hashes[$key] = (Get-FileHash $p -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $ui = $paths['NLMatrixUI.exe']
    $router = $paths['NLMatrixRouter.exe']
    if (-not (Test-Path $ui))     { throw "GUI binary missing: $ui" }
    if (-not (Test-Path $router)) { throw "Router binary missing: $router" }
    $commit = (& git -C $script:Workspace rev-parse --short HEAD 2>$null)
    $dirty = @(& git -C $script:Workspace status --porcelain 2>$null | Where-Object { $_ -notmatch 'Avvio Servizio USB' }).Count -gt 0
    $id = (($hashes['NLMatrixUI.exe'].Substring(0,8)) + '-' + ($hashes['NLMatrixRouter.exe'].Substring(0,8)) + '-' + $commit).ToUpperInvariant()
    return [pscustomobject]@{ id = $id; commit = "$commit"; dirty = $dirty; hashes = $hashes }
}

function Assert-BuildUnchanged($manifest, $stepName) {
    $now = Get-BuildManifest $BuildDir
    foreach ($key in $manifest.hashes.Keys) {
        if ($now.hashes[$key] -ne $manifest.hashes[$key]) {
            throw "build changed during step '$stepName' ($key); refusing to trust the result"
        }
    }
}

function Get-SuiteSteps([string]$suite) {
    switch ($suite) {
        'smoke' {
            return @(
                @{ name = 'guest endpoints'; kind = 'guest-probe' },
                @{ name = 'guest driver info'; kind = 'guest-driver' }
            )
        }
        'guiselftest' {
            return @(@{ name = 'GUI selftest'; kind = 'gui-selftest' })
        }
        'guilifecycle' {
            return @(@{ name = 'GUI lifecycle + tray'; kind = 'gui-lifecycle' })
        }
        'guistudio' {
            return @(@{ name = 'Studio surface'; kind = 'gui-studiotest' })
        }
        'quickconnect' {
            return @(@{ name = 'Quick Connect real client'; kind = 'script';
                       script = 'tools\vm\Run-QuickConnectRealClient.ps1'; args = @() })
        }
        'guiquickconnect' {
            # GUI-driven Quick Connect: the production GUI action starts the
            # engine, a real Windows client captures NL Mic 1, and the Studio
            # profile / preset invariants are checked in the same run.
            return @(@{ name = 'Quick Connect from GUI + real client'; kind = 'script';
                       script = 'tools\vm\Run-QuickConnectGuiRealClient.ps1';
                       # Channel 3 (not the default channel 1) proves the GUI
                       # selection reaches the preset and the engine project.
                       args = @('-CaptureSeconds','12','-Channel','3') })
        }
        'endpoints8' {
            # Enumeration only: all 8 NLMatrix endpoint identities must be
            # present.  Audio verification of every endpoint stays the separate
            # 'endpoints' suite (A22 makes it fail for now): the two criteria are
            # deliberately not merged.
            return @(@{ name = 'All 8 endpoints enumerated'; kind = 'guest-endpoints8' })
        }
        'endpoints' {
            return @(@{ name = 'Endpoint audio verification'; kind = 'script';
                       script = 'tools\vm\Run-EndpointAudioVerification.ps1'; args = @('-Phase','Both') })
        }
        'matrix' {
            return @(@{ name = 'Engine matrix (engine2)'; kind = 'script';
                       script = 'tools\vm\Run-EngineMatrix.ps1'; args = @('-Profile','engine2') })
        }
    }
}

function Invoke-GuestProbe {
    . $script:VmHelpers
    # DirectShow enumeration only: lists the installed endpoint names without
    # opening or playing anything.
    return NL-Exec 'C:\NLMatrixTools\ffmpeg.exe -hide_banner -list_devices true -f dshow -i dummy' -TimeoutSec 120
}

function Invoke-GuestDriverInfo {
    . $script:VmHelpers
    $text = NL-Exec 'C:\NLMatrixTools\NLMatrixDriverTest.exe' -TimeoutSec 90
    return $text
}

function Invoke-GuestEndpoints8 {
    . $script:VmHelpers
    # PnP endpoint identities (names as Windows shows them to users).  This is a
    # read-only enumeration: it proves provisioning, not audio transport, and is
    # the criterion distinct from the 'smoke' capture-only check.
    return NL-Exec 'powershell -NoProfile -Command "(Get-PnpDevice -Class AudioEndpoint -PresentOnly).FriendlyName"' -TimeoutSec 120
}

function Run-ChildScript([string]$scriptRel, [string[]]$scriptArgs, [int]$timeoutSec, [string]$logPath) {
    $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path $ps)) { $ps = 'powershell.exe' }
    $argString = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $script:Workspace $scriptRel) + '"'
    foreach ($a in $scriptArgs) { $argString += ' "' + $a + '"' }
    $p = Start-Process -FilePath $ps -ArgumentList $argString -RedirectStandardOutput $logPath `
                       -RedirectStandardError ($logPath + '.err') -NoNewWindow -PassThru
    # Start-Process -PassThru only reports ExitCode when the process handle has
    # been cached before waiting: otherwise ExitCode is $null and a passing child
    # script would be scored as a failure.
    $null = $p.Handle
    if (-not $p.WaitForExit($timeoutSec * 1000)) {
        try { & taskkill /PID $p.Id /T /F 2>$null | Out-Null } catch { }
        throw "TIMEOUT after ${timeoutSec}s"
    }
    $p.Refresh()
    if ($null -eq $p.ExitCode) { throw 'child exit code unavailable' }
    return [int] $p.ExitCode
}

function New-RunDirectory([string]$suite) {
    if (-not (Test-Path $OutRoot)) { New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $dir = Join-Path $OutRoot ("{0}-{1}" -f $stamp, $suite)
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'logs') | Out-Null
    return $dir
}

function Set-LastSummary([string]$path) {
    Copy-Item $path (Join-Path $OutRoot 'latest-summary.json') -Force
}

function Get-ActiveRun() {
    $lock = Join-Path $env:TEMP 'nlmatrix-test-run.json'
    if (-not (Test-Path $lock)) { return $null }
    try { return Get-Content $lock -Raw | ConvertFrom-Json } catch { return $null }
}

function Test-AudioSessionBusy() {
    . $script:VmHelpers
    $out = NL-Exec 'tasklist /FO CSV /NH 2>nul | findstr /I "NLMatrixRouter.exe NLMatrixUI.exe NLMatrixDriverAudioTest.exe ffmpeg.exe"' -TimeoutSec 60
    if ($out -match 'NLMatrixRouter\.exe|NLMatrixUI\.exe') { return 'guest has a running NL Audio Matrix session' }
    if ($out -match 'NLMatrixDriverAudioTest\.exe') { return 'guest has a playback test running' }
    return $null
}

function Deploy-SnapshotToGuest() {
    . $script:VmHelpers
    NL-Exec ('taskkill /F /IM NLMatrixUI.exe 2>nul & taskkill /F /IM NLMatrixRouter.exe 2>nul & echo CLEARED') -TimeoutSec 90 | Out-Null
    foreach ($pair in @(
        @((Join-Path $BuildDir 'NLMatrixUI_artefacts\Release\NLMatrixUI.exe'), 'C:\NLMatrix\NLMatrixUI.exe'),
        @((Join-Path $BuildDir 'NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe'), 'C:\NLMatrix\NLMatrixRouter.exe'))) {
        NL-CopyTo $pair[0] $pair[1] | Out-Null
    }
}

function Invoke-GuiLifecycle([string]$runDir, [int]$timeoutSec) {
    . $script:VmHelpers
    Deploy-SnapshotToGuest
    $log = Join-Path $runDir 'logs\gui-lifecycle.log'
    $guestLog = 'C:\Users\NLMatrix\AppData\Local\Temp\nlmatrix_lifecycle.log'
    NL-Exec ('del /f /q "' + $guestLog + '" 2>nul & echo CLEARED') -TimeoutSec 60 | Out-Null
    $text = NL-Exec 'C:\NLMatrix\NLMatrixUI.exe --lifecycletest' -TimeoutSec $timeoutSec
    $report = ''
    try { $report = NL-Exec ('type "' + $guestLog + '"') -TimeoutSec 60 } catch { }
    Set-Content -Path $log -Value ($text + "`n" + $report) -Encoding UTF8
    $pass = ([regex]::Matches($report, '(?m)^PASS ')).Count
    $fail = ([regex]::Matches($report, '(?m)^FAIL ')).Count
    $residual = NL-Exec 'tasklist /FO CSV /NH 2>nul | findstr /I "NLMatrixUI.exe NLMatrixRouter.exe"' -TimeoutSec 60
    $clean = -not ($residual -match 'NLMatrixUI\.exe|NLMatrixRouter\.exe')
    $metrics = [ordered]@{ pass = $pass; fail = $fail; resourcesReleased = $clean }
    $status = if ($fail -eq 0 -and $pass -ge 4 -and $clean) { 'PASS' } else { 'FAIL' }
    if (-not $report) { $status = 'FAIL' }
    return [pscustomobject]@{ status = $status; metrics = $metrics }
}

function Invoke-GuiSelftest([string]$runDir, [int]$timeoutSec) {
    . $script:VmHelpers
    $log = Join-Path $runDir 'logs\gui-selftest.log'
    $guestTemp = 'C:\Users\NLMatrix\AppData\Local\Temp\nlmatrix_selftest.log'
    Deploy-SnapshotToGuest
    $text = NL-Exec 'C:\NLMatrix\NLMatrixUI.exe --selftest' -TimeoutSec $timeoutSec
    Set-Content -Path $log -Value $text -Encoding UTF8
    $guestLog = 'C:\NLMatrixTools\gui_selftest_result.log'
    $selftest = ''
    try { $selftest = NL-Exec ('type "' + $guestTemp + '"') -TimeoutSec 60 } catch { }
    if (-not $selftest) { $selftest = $text }
    Add-Content -Path $log -Value $selftest -Encoding UTF8
    $m = [regex]::Match($selftest, 'SUMMARY\s+(\d+)/(\d+)\s+checks passed')
    if (-not $m.Success) {
        $m2 = [regex]::Match($text, 'SUMMARY\s+(\d+)/(\d+)\s+checks passed')
        $m = $m2
    }
    if (-not $m.Success) { throw 'selftest SUMMARY not found' }
    $passed = [int]$m.Groups[1].Value; $total = [int]$m.Groups[2].Value
    # The tray test appends its own PASS/FAIL lines after the SUMMARY block:
    # count them too, otherwise a tray regression would be hidden.
    $extraFail = ([regex]::Matches(($selftest + "`n" + $text), '(?m)^FAIL ')).Count
    $status = if ($passed -eq $total -and $extraFail -eq 0) { 'PASS' } else { 'FAIL' }
    return [pscustomobject]@{ passed = $passed; total = $total; failed = $extraFail; status = $status }
}

function Invoke-GuiStudioTest([string]$runDir, [int]$timeoutSec) {
    . $script:VmHelpers
    $log = Join-Path $runDir 'logs\gui-studio.log'
    Deploy-SnapshotToGuest
    $guestLog = 'C:\Users\NLMatrix\AppData\Local\Temp\nlmatrix_studiotest.log'
    NL-Exec ('del /f /q "' + $guestLog + '" 2>nul & echo CLEARED') -TimeoutSec 60 | Out-Null
    $text = NL-Exec 'C:\NLMatrix\NLMatrixUI.exe --studiotest' -TimeoutSec $timeoutSec
    $report = ''
    try { $report = NL-Exec ('type "' + $guestLog + '"') -TimeoutSec 60 } catch { }
    Set-Content -Path $log -Value ($text + "`n" + $report) -Encoding UTF8
    $pass = ([regex]::Matches($report, '(?m)^PASS ')).Count
    $fail = ([regex]::Matches($report, '(?m)^FAIL ')).Count
    $complete = $report -match 'SUMMARY studio complete'
    $metrics = [ordered]@{ pass = $pass; fail = $fail; studioComplete = $complete }
    $status = if ($fail -eq 0 -and $pass -ge 4 -and $complete) { 'PASS' } else { 'FAIL' }
    return [pscustomobject]@{ status = $status; metrics = $metrics }
}

function Invoke-Run([string]$suite, [string]$runId, [string]$runDir, $manifest, [int]$timeoutMinutes) {
    $deadline = (Get-Date).AddMinutes($timeoutMinutes)
    $steps = @()
    foreach ($def in (Get-SuiteSteps $suite)) {
        $remaining = [Math]::Max(30, [int]($deadline - (Get-Date)).TotalSeconds)
        $logPath = Join-Path $runDir ('logs\' + ($def.name -replace '[^A-Za-z0-9]+','-') + '.log')
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $status = 'PASS'; $metrics = [ordered]@{}; $detail = ''
        try {
            switch ($def.kind) {
                'script' {
                    $code = Run-ChildScript $def.script $def.args $remaining $logPath
                    if ($code -ne 0) { $status = 'FAIL'; $detail = "exit $code" }
                }
                'guest-probe' {
                    $out = Invoke-GuestProbe
                    Set-Content -Path $logPath -Value $out -Encoding UTF8
                    # DirectShow lists capture endpoints only; the render side
                    # (NL Return 1..4) is covered by the driver-info step.
                    $mics = ([regex]::Matches($out, 'NL Mic \d')).Count
                    $metrics['captureMics'] = $mics
                    $metrics['namesOk'] = (($out -match 'NL Mic 1') -and ($out -match 'NL Mic 4'))
                    if ($mics -ne 4 -or -not $metrics['namesOk'])
                    { $status = 'FAIL'; $detail = 'NL Mic endpoint names missing' }
                }
                'guest-driver' {
                    $out = Invoke-GuestDriverInfo
                    Set-Content -Path $logPath -Value $out -Encoding UTF8
                    $m = [regex]::Match($out, 'endpoints=(\d+) filters=(\d+)/(\d+)')
                    if ($m.Success) {
                        $metrics['endpoints'] = [int]$m.Groups[1].Value
                        $metrics['filters'] = "$($m.Groups[2].Value)/$($m.Groups[3].Value)"
                    } else { $status = 'FAIL'; $detail = 'driver info not readable' }
                }
                'guest-endpoints8' {
                    $out = Invoke-GuestEndpoints8
                    Set-Content -Path $logPath -Value $out -Encoding UTF8
                    $mics = 0; $returns = 0
                    for ($i = 1; $i -le 4; $i++) {
                        if ($out -match ("(?m)^NL Mic $i \(NLMatrix")) { $mics++ }
                        if ($out -match ("(?m)^NL Return $i \(NLMatrix")) { $returns++ }
                    }
                    $metrics['mics'] = $mics
                    $metrics['returns'] = $returns
                    $metrics['namedEndpoints'] = $mics + $returns
                    if ($mics -ne 4 -or $returns -ne 4) {
                        $status = 'FAIL'
                        $detail = "expected 8 NLMatrix endpoints, found mics=$mics returns=$returns"
                    }
                }
                'gui-selftest' {
                    $r = Invoke-GuiSelftest $runDir ([Math]::Min($remaining, 420))
                    $metrics['checksPassed'] = $r.passed
                    $metrics['checksTotal'] = $r.total
                    $metrics['trayFailures'] = $r.failed
                    if ($r.status -ne 'PASS') { $status = 'FAIL' }
                }
                'gui-lifecycle' {
                    $r = Invoke-GuiLifecycle $runDir ([Math]::Min($remaining, 420))
                    foreach ($k in $r.metrics.Keys) { $metrics[$k] = $r.metrics[$k] }
                    if ($r.status -ne 'PASS') { $status = 'FAIL' }
                }
                'gui-studiotest' {
                    $r = Invoke-GuiStudioTest $runDir ([Math]::Min($remaining, 420))
                    foreach ($k in $r.metrics.Keys) { $metrics[$k] = $r.metrics[$k] }
                    if ($r.status -ne 'PASS') { $status = 'FAIL' }
                }
            }
            Assert-BuildUnchanged $manifest $def.name
        } catch {
            $status = if ($_.Exception.Message -match 'TIMEOUT') { 'TIMEOUT' } else { 'ERROR' }
            $detail = $_.Exception.Message
            if (-not (Test-Path $logPath)) { Set-Content -Path $logPath -Value $detail -Encoding UTF8 }
        }
        $sw.Stop()
        if ($status -eq 'PASS' -and (Test-Path $logPath)) {
            # Keep detailed logs only when they are useful: passing steps keep a tail.
            $lines = Get-Content $logPath -ErrorAction SilentlyContinue
            if ($lines -and $lines.Count -gt 120) {
                Set-Content -Path $logPath -Value ($lines | Select-Object -Last 120) -Encoding UTF8
            }
        }
        $steps += [pscustomobject]@{
            name = $def.name; status = $status; durationSec = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
            metrics = $metrics; detail = $detail; log = (('logs\' + ($def.name -replace '[^A-Za-z0-9]+','-') + '.log'))
        }
        if ($status -ne 'PASS') { break }
    }
    $overall = if ($steps.Count -gt 0 -and ($steps | Where-Object { $_.status -ne 'PASS' })) { 'FAIL' } else { 'PASS' }
    return [pscustomobject]@{ status = $overall; steps = $steps }
}

function Remove-OldRuns([int]$keep) {
    $dirs = Get-ChildItem $OutRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d{8}-\d{6}-' } | Sort-Object LastWriteTime -Descending
    $removed = 0
    foreach ($d in ($dirs | Select-Object -Skip $keep)) {
        Remove-Item $d.FullName -Recurse -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path $d.FullName)) { $removed++ }
    }
    return $removed
}

# ---------------------------------------------------------------- unattended run
if ($Unattended) {
    $mutex = New-Object System.Threading.Mutex($false, 'Global\NLMatrixAudioTest')
    if (-not $mutex.WaitOne(0)) { Write-Host 'BUSY another NL Audio Matrix test run is active'; exit 3 }
    try {
        if (-not $RunId) { $RunId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Suite }
        $runDir = Join-Path $OutRoot $RunId
        New-Item -ItemType Directory -Force -Path (Join-Path $runDir 'logs') | Out-Null
        $lock = Join-Path $env:TEMP 'nlmatrix-test-run.json'
        Set-Content -Path $lock -Value (@{ runId = $RunId; suite = $Suite; pid = $PID; startedAt = (Get-Date).ToString('o') } | ConvertTo-Json) -Encoding UTF8
        $started = Get-Date
        $manifest = Get-BuildManifest $BuildDir
        $summaryPath = Join-Path $runDir 'summary.json'
        $result = 'ERROR'; $steps = @(); $summaryText = ''
        try {
            if (-not $Force -and $Suite -in @('quickconnect','guiquickconnect','endpoints','matrix')) {
                $busy = Test-AudioSessionBusy
                if ($busy) { throw "BUSY: $busy" }
            }
            $r = Invoke-Run $Suite $RunId $runDir $manifest $TimeoutMinutes
            $result = $r.status; $steps = $r.steps
            $summaryText = (($steps | ForEach-Object { "$($_.name): $($_.status)" }) -join '; ')
        } catch {
            $msg = $_.Exception.Message
            if ($msg -match '^BUSY') { $result = 'BUSY' } else { $result = 'ERROR' }
            $summaryText = $msg
            $script:Anomalies += $msg
        } finally {
            $finished = Get-Date
            $script:FinalCode = switch ($result) { 'PASS' { 0 } 'FAIL' { 1 } 'BUSY' { 3 } 'TIMEOUT' { 4 } default { 2 } }
            $summary = [ordered]@{
                runId = $RunId; suite = $Suite; status = $result
                exitCode = $script:FinalCode
                build = [ordered]@{ id = $manifest.id; commit = $manifest.commit; dirty = $manifest.dirty; hashes = $manifest.hashes }
                startedAt = $started.ToString('o'); finishedAt = $finished.ToString('o')
                durationSec = [Math]::Round(($finished - $started).TotalSeconds, 1)
                summary = $summaryText
                steps = $steps
                anomalies = $script:Anomalies
                logsDir = (Join-Path $runDir 'logs')
                cleanup = [ordered]@{ removedRuns = (Remove-OldRuns $KeepRuns) }
            }
            $summary | ConvertTo-Json -Depth 6 | Set-Content -Path $summaryPath -Encoding UTF8
            Set-LastSummary $summaryPath
            Remove-Item $lock -Force -ErrorAction SilentlyContinue
            Write-Line ("RUN {0} {1} -> {2}" -f $RunId, $Suite, $result)
            Write-Line ("SUMMARY {0}" -f $summaryPath)
        }
        exit $script:FinalCode
    } finally {
        $mutex.ReleaseMutex(); $mutex.Dispose()
    }
}

# ---------------------------------------------------------------- launcher mode
if (-not (Test-Path $OutRoot)) { New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null }
$active = Get-ActiveRun
if ($active -and $active.runId) {
    $proc = Get-Process -Id ([int]$active.pid) -ErrorAction SilentlyContinue
    if ($proc) {
        Write-Line ("BUSY run {0} ({1}) is still active" -f $active.runId, $active.suite)
        exit 3
    }
}
$runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Suite
$ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if (-not (Test-Path $ps)) { $ps = 'powershell.exe' }

if ($UnregisterTask) {
    Unregister-ScheduledTask -TaskName ("NLMatrixTest-" + $Suite) -Confirm:$false -ErrorAction SilentlyContinue
    Write-Line ("TASK removed: NLMatrixTest-{0}" -f $Suite)
    exit 0
}
if ($ScheduleAt) {
    # One-shot Task Scheduler entry: no periodic timer, no LLM wait.  The task
    # runs the same detached launcher and writes the same summary.json.
    $when = [datetime]::Parse($ScheduleAt)
    $taskArgs = ('-NoProfile -ExecutionPolicy Bypass -File "{0}" -Suite {1} -OutRoot "{2}" -BuildDir "{3}" ' +
                 '-TimeoutMinutes {4} -KeepRuns {5} -Unattended -RunId scheduled-{1}-{6}') -f `
                 $PSCommandPath, $Suite, $OutRoot, $BuildDir, $TimeoutMinutes, $KeepRuns, $when.ToString('HHmmss')
    $action = New-ScheduledTaskAction -Execute $ps -Argument $taskArgs
    $trigger = New-ScheduledTaskTrigger -Once -At $when
    Register-ScheduledTask -TaskName ("NLMatrixTest-" + $Suite) -Action $action -Trigger $trigger -Force | Out-Null
    Write-Line ("TASK NLMatrixTest-{0} at {1}" -f $Suite, $when.ToString('s'))
    exit 0
}
$childArgs = ('-NoProfile -ExecutionPolicy Bypass -File "{0}" -Suite {1} -OutRoot "{2}" -BuildDir "{3}" ' +
              '-TimeoutMinutes {4} -KeepRuns {5} -Unattended -RunId {6}') -f `
              $PSCommandPath, $Suite, $OutRoot, $BuildDir, $TimeoutMinutes, $KeepRuns, $runId
if ($Force) { $childArgs += ' -Force' }
$child = Start-Process -FilePath $ps -ArgumentList $childArgs -WindowStyle Hidden -PassThru
Write-Line ("RUN {0} started (pid {1})" -f $runId, $child.Id)
Write-Line ("SUMMARY {0}" -f (Join-Path $OutRoot ("{0}\summary.json" -f $runId)))
Write-Line ("LATEST  {0}" -f (Join-Path $OutRoot 'latest-summary.json'))
if ($Wait) { $child.WaitForExit(); exit $child.ExitCode }
exit 0
