#requires -Version 5.1
<#
.SYNOPSIS
    Quick Connect from the real GUI, user-mode path: the production
    --quickconnecttest action builds the project for an already-signed Windows
    render endpoint (no NL kernel driver), starts the engine and sends the test
    tone through the VB-CABLE; an independent ffmpeg client records the cable
    capture side.

.DESCRIPTION
    Runs NLMatrixUI.exe in the guest with:
        --quickconnecttest --qc-test-tone --qc-device FlexASIO --qc-windows-endpoint
        --qc-render-endpoint "CABLE Input (VB-Audio Virtual Cable)"
        --qc-hold <n> --qc-log <file> --qc-preset-dir <dir>
    then records "CABLE Output" with ffmpeg and checks, on the host:
      * the GUI log shows the Quick Connect ran and the engine started;
      * the Quick Connect preset was written (own directory, no Studio overwrite);
      * the cable really carries the 1000 Hz test tone (level + continuity);
      * the GUI and the engine exit and leave no NLMatrixRouter process.

    Writes a JSON summary. Exit 0 = PASS.

.EXAMPLE
    powershell -File tools\vm\Run-QuickConnectUserModeCheck.ps1
#>
[CmdletBinding()]
param(
    [string]$GuiExe = '',
    [string]$OutputDir = '',
    [string]$RenderEndpoint = 'CABLE Input (VB-Audio Virtual Cable)',
    [string]$PythonExe = 'K:\AI\Agente\runtime\python.exe'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'NLVM.ps1')
$script:Workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $GuiExe) { $GuiExe = Join-Path $script:Workspace 'app\build\NLMatrixUI_artefacts\Release\NLMatrixUI.exe' }
if (-not (Test-Path $GuiExe)) { throw "GUI executable not found: $GuiExe" }
if (-not $OutputDir) { $OutputDir = Join-Path $script:Workspace ('reports\quickconnect_usermode\' + (Get-Date -Format 'yyyy-MM-dd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$analyser = Join-Path $script:Workspace 'tools\analyze_send_path.py'
$logPath = Join-Path $OutputDir 'qc_host.log'
function Log([string]$text) {
    $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $text)
    Write-Host $line
    Add-Content -Path $logPath -Value $line -Encoding UTF8
}
function New-TempFile([string]$name, [string]$content) {
    $path = Join-Path $env:TEMP $name
    Set-Content -Path $path -Value $content -Encoding ASCII
    return $path
}

$guestDir = 'C:\NLMatrixTools'
$guestGui = "$guestDir\NLMatrixUI-qc.exe"
$guestLog = 'C:\Windows\Temp\nl-qc.log'
$guestStdout = 'C:\Windows\Temp\nl-qc-stdout.log'
$guestRunBat = 'C:\Windows\Temp\nl-qc-run.bat'
$guestStartBat = 'C:\Windows\Temp\nl-qc-start.bat'
$guestPresets = 'C:\Windows\Temp\nl-qc-presets'
$guestWav = 'C:\Windows\Temp\nl-qc.wav'
$cableCapture = 'CABLE Output (VB-Audio Virtual Cable)'
$guestFfmpeg = "$guestDir\ffmpeg.exe"

Log 'preparing the guest'
NL-Run ("if not exist $guestDir mkdir $guestDir") | Out-Null
NL-Run ('taskkill /F /T /IM NLMatrixUI-qc.exe 2>nul & taskkill /F /T /IM NLMatrixRouter.exe 2>nul & echo CLEARED') | Out-Null
Start-Sleep -Milliseconds 600
NL-Run ("del /f /q " + $guestGui) | Out-Null
$guiHash = NL-CopyTo $GuiExe $guestGui
Log "GUI copied (sha256 $guiHash)"
# The GUI spawns the engine as "NLMatrixRouter.exe" next to itself; make sure the
# current build is there (the guest may hold an older installed copy).
$routerExe = Join-Path $script:Workspace 'app\build\NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe'
if (-not (Test-Path $routerExe)) { throw "router executable not found: $routerExe" }
NL-Run ('taskkill /F /T /IM NLMatrixRouter.exe 2>nul & del /f /q ' + $guestDir + '\NLMatrixRouter.exe 2>nul & echo GONE') | Out-Null
Start-Sleep -Milliseconds 500
$routerHash = NL-CopyTo $routerExe ($guestDir + '\NLMatrixRouter.exe')
Log "engine copied next to the GUI (sha256 $routerHash)"

$runBat = New-TempFile 'nl-qc-run.bat' (@(
    '@echo off',
    "`"$guestGui`" --quickconnecttest --qc-test-tone --qc-device FlexASIO --qc-windows-endpoint --qc-render-endpoint `"$RenderEndpoint`" --qc-hold 14 --qc-log `"$guestLog`" --qc-preset-dir `"$guestPresets`" > `"$guestStdout`" 2>&1"
) -join "`r`n")
NL-CopyTo $runBat $guestRunBat | Out-Null
$startBat = New-TempFile 'nl-qc-start.bat' (@(
    '@echo off',
    "start `"NLMatrixQC`" /min cmd /c `"$guestRunBat`""
) -join "`r`n")
NL-CopyTo $startBat $guestStartBat | Out-Null
NL-Run ("del /f /q " + $guestLog) | Out-Null
NL-Run ("rmdir /s /q " + $guestPresets) | Out-Null
NL-Run $guestStartBat | Out-Null
Log 'GUI Quick Connect started (user-mode endpoint mode)'
Start-Sleep -Seconds 8

# The engine must be running and the GUI must still hold the session.
$running = NL-Exec 'tasklist /FI "IMAGENAME eq NLMatrixRouter.exe" /NH' -TimeoutSec 60
$engineRunning = ($running -match 'NLMatrixRouter')
Log ('engine process running during the session: ' + $engineRunning)

$recordCommand = '"' + $guestFfmpeg + '" -hide_banner -loglevel error -y -f dshow -i "audio=' + $cableCapture + '"' +
                 ' -t 5 -ar 48000 -ac 2 -c:a pcm_s16le "' + $guestWav + '"'
$rec = NL-Exec $recordCommand -TimeoutSec 180
$recExit = -999
if ($rec -match 'EXITCODE=(-?\d+)') { $recExit = [int]$Matches[1] }
Log ('recorded cable capture, ffmpeg exit=' + $recExit)

# Wait for the GUI to finish its hold window and exit.
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep -Milliseconds 500
    $guiNow = NL-Exec 'tasklist /FI "IMAGENAME eq NLMatrixUI-qc.exe" /NH' -TimeoutSec 60
    if (-not ($guiNow -match 'NLMatrixUI-qc')) { break }
}
NL-Run ('taskkill /F /T /IM NLMatrixUI-qc.exe 2>nul & taskkill /F /T /IM NLMatrixRouter.exe 2>nul & echo DONE') | Out-Null
Start-Sleep -Milliseconds 800
$residual = NL-Exec 'tasklist /FI "IMAGENAME eq NLMatrixRouter.exe" /NH' -TimeoutSec 60
$routerResidual = ($residual -match 'NLMatrixRouter')
Log ('router process after the session: ' + $routerResidual)

$qcLogLocal = Join-Path $OutputDir 'qc-gui.log'
$stdoutLocal = Join-Path $OutputDir 'qc-stdout.log'
$wavLocal = Join-Path $OutputDir 'qc-cable.wav'
$presetsLocal = Join-Path $OutputDir 'qc-presets-list.txt'
try { NL-CopyFrom $guestLog $qcLogLocal | Out-Null } catch { Log ('qc log: ' + $_.Exception.Message) }
try { NL-CopyFrom $guestStdout $stdoutLocal | Out-Null } catch { Log ('qc stdout: ' + $_.Exception.Message) }
try { NL-CopyFrom $guestWav $wavLocal | Out-Null } catch { Log ('qc wav: ' + $_.Exception.Message) }
$presetList = NL-Exec ('dir /b ' + $guestPresets) -TimeoutSec 60
Set-Content -Path $presetsLocal -Value $presetList -Encoding UTF8

$qcText = ''
if (Test-Path $qcLogLocal) { $qcText = Get-Content $qcLogLocal -Raw }
$presetWritten = ($presetList -match '\.json')

$measurements = [ordered]@{}
if (Test-Path $wavLocal) {
    $analysisPath = Join-Path $OutputDir 'qc-analysis.json'
    & $PythonExe $analyser $wavLocal --tone-left 1000 --tone-right 1040 --silence-channel 0 --json $analysisPath | Out-Null
    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    $measurements['cable'] = $analysis.files[0]
}

$checks = New-Object System.Collections.Generic.List[object]
function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    $checks.Add([ordered]@{ name = $name; ok = $ok; detail = $detail })
    Log (('[{0}] {1} - {2}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail))
}

$cable = $measurements['cable']
Add-Check 'Quick Connect ran from the GUI in Windows-endpoint mode' (($qcText -match 'quick connect') -and ($qcText -match 'Windows endpoint|render')) ('log lines=' + (($qcText -split "`r?`n").Count))
Add-Check 'engine started for the session' ($engineRunning -or ($qcText -match 'engine')) ('engineRunning=' + $engineRunning)
Add-Check 'Quick Connect preset written in its own directory' $presetWritten ('files: ' + (($presetList -split "`r?`n") -join ', '))
if ($cable) {
    Add-Check 'cable carries the Quick Connect test tone' (($cable.toneLeftDb -ge -30) -or ($cable.toneRightDb -ge -30)) ('tones=' + $cable.toneLeftDb + '/' + $cable.toneRightDb + ' dBFS')
    Add-Check 'Quick Connect continuity through the cable (120 ms includes the engine pre-roll)' ($cable.longestInteriorSilenceMs -le 120) ('interior silence=' + $cable.longestInteriorSilenceMs + ' ms')
} else { Add-Check 'cable recording measured' $false ('no data (ffmpeg exit ' + $recExit + ')') }
Add-Check 'no engine process left behind' (-not $routerResidual) ('routerResidual=' + $routerResidual)

$failed = @($checks | Where-Object { -not $_.ok }).Count
$summary = [ordered]@{
    tool = 'Run-QuickConnectUserModeCheck'
    utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    gui = [ordered]@{ path = $GuiExe; sha256 = $guiHash }
    mode = [ordered]@{ windowsEndpoint = $RenderEndpoint; testTone = $true; kernelDriver = $false }
    measurements = $measurements
    checks = $checks
    failed = $failed
    verdict = if ($failed -eq 0) { 'PASS' } else { 'FAIL' }
}
$summaryPath = Join-Path $OutputDir 'quickconnect-usermode-summary.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryPath -Encoding UTF8
Log ("summary: $summaryPath")
Log ('verdict: ' + $summary.verdict)
if ($failed -eq 0) { exit 0 }
exit 1






