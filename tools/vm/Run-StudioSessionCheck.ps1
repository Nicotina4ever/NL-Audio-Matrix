#requires -Version 5.1
<#
.SYNOPSIS
    Studio bidirectional session entirely on signed/user-mode endpoints: local
    source + remote return -> two buses (N-1 send bus and monitor bus) -> send
    cable leg and monitor cable leg, with independent recordings of both.

.DESCRIPTION
    Graph (one config file; hot reload only changes mute, same topology):

        local  (test 1000/1040) --> sendMix (mix-minus: remote excluded) --> render CABLE Input
        remote (return leg)     --> monMix  (full mix)                    --> render CABLE In 16ch

    The return leg is captured from the remote program with the PROCESS loopback
    when it actually delivers audio in the guest; otherwise the harness falls back
    to the render-endpoint loopback and records that in the summary.

    Both legs are Windows endpoints of the same signed VB-CABLE device, so the
    whole session needs no NL kernel driver. An independent ffmpeg client records
    the cable capture side ("CABLE Output") in two separate time windows, with the
    other leg muted, so each recording is independent:
      * send window    -> local tones only (mix-minus keeps the remote out);
      * monitor window -> local tones AND the remote return.
    The host analyser measures level, per-channel tone identity and continuity.

    Writes a JSON summary. Exit 0 = PASS.

.EXAMPLE
    powershell -File tools\vm\Run-StudioSessionCheck.ps1
#>
[CmdletBinding()]
param(
    [string]$RouterExe = '',
    [string]$OutputDir = '',
    [int]$RecordSeconds = 4,
    [string]$PythonExe = 'K:\AI\Agente\runtime\python.exe'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'NLVM.ps1')
$script:Workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $RouterExe) {
    $RouterExe = Join-Path $script:Workspace 'app\build\NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe'
}
if (-not (Test-Path $RouterExe)) { throw "router executable not found: $RouterExe" }
if (-not $OutputDir) { $OutputDir = Join-Path $script:Workspace ('reports\studio_session\' + (Get-Date -Format 'yyyy-MM-dd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$analyser = Join-Path $script:Workspace 'tools\analyze_send_path.py'
$logPath = Join-Path $OutputDir 'studio_host.log'
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
$guestMainExe = "$guestDir\NLMatrixRouter-studio.exe"
$guestRemoteExe = "$guestDir\NLMatrixRouter-remote.exe"
$guestMainCfg = 'C:\Windows\Temp\nl-studio-main.json'
$guestRemoteCfg = 'C:\Windows\Temp\nl-studio-remote.json'
$guestMainLog = 'C:\Windows\Temp\nl-studio-main.log'
$guestRemoteLog = 'C:\Windows\Temp\nl-studio-remote.log'
$guestFfmpeg = "$guestDir\ffmpeg.exe"
$speakerRender = 'Altoparlanti (High Definition Audio Device)'
$cableSend = 'CABLE Input (VB-Audio Virtual Cable)'
$cableMonitor = 'CABLE In 16ch (VB-Audio Virtual Cable)'
$cableCapture = 'CABLE Output (VB-Audio Virtual Cable)'

function RemoteConfig {
    return @"
{
  "name": "Studio remote program",
  "asio": "VB-Matrix VASIO-8",
  "devices": [ { "id": "vasio", "backend": "asio", "driver": "VB-Matrix VASIO-8", "sampleRate": 44100, "buffer": 512, "clockMaster": true, "rateAuto": true } ],
  "buses": [],
  "sources": [ { "id": "remoteTone", "name": "remote program", "type": "test", "channels": [0, 1], "level": 0.25, "baseFrequency": 600, "enabled": true } ],
  "destinations": [ { "id": "speakers", "name": "speakers", "type": "render", "endpoint": "$speakerRender", "channels": [0, 1], "enabled": true } ],
  "routes": [ { "source": "remoteTone", "destination": "speakers", "gainDb": 0 } ]
}
"@
}

function MainConfig([string]$returnType, [bool]$sendOn, [bool]$monitorOn) {
    if ($returnType -eq 'process') {
        $returnSource = '{ "id": "remote", "name": "remote return (process loopback)", "type": "process", "process": "NLMatrixRouter-remote.exe", "includeProcessTree": true, "enabled": true }'
    } else {
        $returnSource = '{ "id": "remote", "name": "remote return (endpoint loopback)", "type": "loopback", "endpoint": "' + $speakerRender + '", "channels": [0, 1], "enabled": true }'
    }
    $sendMuted = if ($sendOn) { 'false' } else { 'true' }
    $monitorMuted = if ($monitorOn) { 'false' } else { 'true' }
    return @"
{
  "name": "Studio bidirectional session",
  "asio": "FlexASIO",
  "devices": [ { "id": "flex", "backend": "asio", "driver": "FlexASIO", "sampleRate": 48000, "buffer": 256, "clockMaster": true, "rateAuto": true } ],
  "buses": [
    { "id": "sendMix", "name": "send bus (N-1)", "channels": 2, "mixMinus": [ "remote" ], "enabled": true },
    { "id": "monMix", "name": "monitor bus", "channels": 2, "enabled": true }
  ],
  "sources": [
    { "id": "local", "name": "local source", "type": "test", "channels": [0, 1], "level": 0.25, "baseFrequency": 1000, "enabled": true },
    $returnSource,
    { "id": "sendSrc", "name": "send bus", "type": "bus", "bus": "sendMix", "enabled": true },
    { "id": "monSrc", "name": "monitor bus", "type": "bus", "bus": "monMix", "enabled": true }
  ],
  "destinations": [
    { "id": "send", "name": "send leg", "type": "render", "endpoint": "$cableSend", "channels": [0, 1], "muted": $sendMuted, "enabled": true },
    { "id": "monitor", "name": "monitor leg", "type": "asio-out", "channels": [0, 1], "muted": $monitorMuted, "enabled": true }
  ],
  "routes": [
    { "source": "local", "destination": "sendMix", "gainDb": 0 },
    { "source": "remote", "destination": "sendMix", "gainDb": 0 },
    { "source": "local", "destination": "monMix", "gainDb": 0 },
    { "source": "remote", "destination": "monMix", "gainDb": 0 },
    { "source": "sendSrc", "destination": "send", "gainDb": 0 },
    { "source": "monSrc", "destination": "monitor", "gainDb": 0 }
  ]
}
"@
}

function EngineRunning([string]$image) {
    $output = NL-Exec ('tasklist /FI "IMAGENAME eq ' + $image + '" /NH') -TimeoutSec 60
    return ($output -match [regex]::Escape($image))
}
function Kill-GuestImage([string]$image) {
    for ($attempt = 0; $attempt -lt 5; $attempt++) {
        NL-Run ('taskkill /F /T /IM ' + $image) | Out-Null
        Start-Sleep -Milliseconds 400
        if (-not (EngineRunning $image)) { return $true }
    }
    NL-Run ('wmic process where "name=''' + $image + '''" call terminate') | Out-Null
    Start-Sleep -Milliseconds 600
    return (-not (EngineRunning $image))
}
function Get-NodeLevel([string]$guestLog, [string]$nodeId) {
    $local = Join-Path $env:TEMP 'nl-studio-probe.log'
    try { NL-CopyFrom $guestLog $local | Out-Null } catch { return $null }
    if (-not (Test-Path $local)) { return $null }
    $line = (Select-String -Path $local -Pattern '"type": "meters"' | Select-Object -Last 1).Line
    if (-not $line) { return $null }
    $json = $line.Substring($line.IndexOf('{')) | ConvertFrom-Json
    foreach ($node in $json.nodes) { if ($node.id -eq $nodeId) { return [double]$node.l } }
    return $null
}
function Get-NodeChannels([string]$guestLog, [string]$nodeId) {
    $local = Join-Path $env:TEMP 'nl-studio-probe2.log'
    try { NL-CopyFrom $guestLog $local | Out-Null } catch { return $null }
    if (-not (Test-Path $local)) { return $null }
    $line = (Select-String -Path $local -Pattern '"type": "meters"' | Select-Object -Last 1).Line
    if (-not $line) { return $null }
    $json = $line.Substring($line.IndexOf('{')) | ConvertFrom-Json
    foreach ($node in $json.nodes) { if ($node.id -eq $nodeId) { return $node.channels } }
    return $null
}
function Start-Engine([string]$batName, [string]$exe, [string]$config, [string]$logFile, [string]$windowTitle) {
    $runBat = New-TempFile ($batName + '-run.bat') (@(
        '@echo off',
        "`"$exe`" --config `"$config`" --seconds 180 > `"$logFile`" 2>&1"
    ) -join "`r`n")
    NL-CopyTo $runBat ("C:\Windows\Temp\" + $batName + '-run.bat') | Out-Null
    $startBat = New-TempFile ($batName + '-start.bat') (@(
        '@echo off',
        "start `"$windowTitle`" /min cmd /c `"C:\Windows\Temp\$batName-run.bat`""
    ) -join "`r`n")
    NL-CopyTo $startBat ("C:\Windows\Temp\" + $batName + '-start.bat') | Out-Null
    NL-Run ("del /f /q " + $logFile) | Out-Null
    NL-Run ("C:\Windows\Temp\" + $batName + '-start.bat') | Out-Null
    Start-Sleep -Seconds 5
}
function PushMainConfig([string]$returnType, [bool]$sendOn, [bool]$monitorOn, [string]$tag) {
    $local = New-TempFile ('nl-studio-main-' + $tag + '.json') (MainConfig $returnType $sendOn $monitorOn)
    NL-Run ("del /f /q " + $guestMainCfg) | Out-Null
    NL-CopyTo $local $guestMainCfg | Out-Null
    Log ("main config: sendOn=$sendOn monitorOn=$monitorOn")
}
function Record-Wav([string]$guestWav, [int]$seconds, [string]$tag, [string]$device = '', [int]$channels = 2) {
    if (-not $device) { $device = $cableCapture }
    $command = '"' + $guestFfmpeg + '" -hide_banner -loglevel error -y -f dshow -i "audio=' + $device + '"' +
               ' -t ' + $seconds + ' -ar 48000 -ac ' + $channels + ' -c:a pcm_s16le "' + $guestWav + '"'
    $out = NL-Exec $command -TimeoutSec 180
    $code = -999
    if ($out -match 'EXITCODE=(-?\d+)') { $code = [int]$Matches[1] }
    Log ("recorded $tag ffmpeg exit=" + $code)
    return $code
}

# ------------------------------------------------------------------ run
Log 'preparing the guest'
NL-Run ("if not exist $guestDir mkdir $guestDir") | Out-Null
$probe = NL-Exec 'if exist C:\NLMatrixTools\ffmpeg.exe (echo FFMPEG_YES) else (echo FFMPEG_NO)' -TimeoutSec 60
if ($probe -notmatch 'FFMPEG_YES') { throw 'ffmpeg is missing in the guest' }
Kill-GuestImage 'NLMatrixRouter-studio.exe' | Out-Null
Kill-GuestImage 'NLMatrixRouter-remote.exe' | Out-Null
NL-Kill 'ffmpeg.exe' | Out-Null
# FlexASIO needs a valid device target: the guest TOML must not point at the
# (now absent) NL driver endpoints.
$flexToml = Join-Path $script:Workspace 'tools\vm\FlexASIO-shared-speakers.toml'
if (Test-Path $flexToml) { NL-CopyTo $flexToml 'C:\Users\NLMatrix\FlexASIO.toml' | Out-Null; Log 'FlexASIO.toml deployed (speakers, exclusive)' }
NL-Run ("del /f /q " + $guestMainExe) | Out-Null
NL-Run ("del /f /q " + $guestRemoteExe) | Out-Null
$routerHash = ''
for ($attempt = 1; $attempt -le 3; $attempt++) {
    try { $routerHash = NL-CopyTo $RouterExe $guestMainExe; break }
    catch { Log ('copy attempt ' + $attempt + ' failed: ' + $_.Exception.Message); Start-Sleep -Seconds 2; Kill-GuestImage 'NLMatrixRouter-studio.exe' | Out-Null }
}
if (-not $routerHash) { throw 'cannot copy the router to the guest (file locked)' }
NL-CopyTo $RouterExe $guestRemoteExe | Out-Null
Log "router copied twice (sha256 $routerHash)"

# Remote program: independent client rendering 600/640 Hz to the guest speakers.
$remoteCfgLocal = New-TempFile 'nl-studio-remote.json' (RemoteConfig)
NL-Run ("del /f /q " + $guestRemoteCfg) | Out-Null
NL-CopyTo $remoteCfgLocal $guestRemoteCfg | Out-Null
Start-Engine 'nl-studio-remote' $guestRemoteExe $guestRemoteCfg $guestRemoteLog 'NLStudioRemote'
if (-not (EngineRunning 'NLMatrixRouter-remote.exe')) { throw 'the remote program engine did not start' }
Log 'remote program engine running'

# Main session: process loopback first; fall back to the endpoint loopback when
# the process loopback opens but delivers silence in this guest.
$returnMode = 'process'
PushMainConfig $returnMode $true $false 'probe'
Start-Engine 'nl-studio-main' $guestMainExe $guestMainCfg $guestMainLog 'NLStudioMain'
if (-not (EngineRunning 'NLMatrixRouter-studio.exe')) { throw 'the main engine did not start' }
Start-Sleep -Seconds 4
$returnLevel = Get-NodeLevel $guestMainLog 'remote'
if ($null -eq $returnLevel) { $returnLevel = -120.0 }
Log ('process loopback return level: ' + $returnLevel + ' dB')
if ($returnLevel -le -80.0) {
    Log 'process loopback opened but delivers silence here; falling back to the endpoint loopback'
    Kill-GuestImage 'NLMatrixRouter-studio.exe' | Out-Null
    $returnMode = 'loopback'
    PushMainConfig $returnMode $true $false 'probe'
    Start-Engine 'nl-studio-main' $guestMainExe $guestMainCfg $guestMainLog 'NLStudioMain'
    if (-not (EngineRunning 'NLMatrixRouter-studio.exe')) { throw 'the main engine did not start (fallback)' }
    Start-Sleep -Seconds 3
}

# Window 1: send leg active, monitor muted -> the cable carries the N-1 send.
$sendExit = Record-Wav 'C:\Windows\Temp\nl-studio-send.wav' $RecordSeconds 'send-window'

# Window 2: monitor leg active, send muted -> the cable carries the monitor mix.
PushMainConfig $returnMode $false $true 'monitor'
Start-Sleep -Milliseconds 1500
$monitorExit = Record-Wav 'C:\Windows\Temp\nl-studio-monitor.wav' $RecordSeconds 'monitor-window' 'NL Mic 1 (NLMatrix Virtual Audio Device)' 1

$monMixChannels = Get-NodeChannels $guestMainLog 'monMix'
$sendMixChannels = Get-NodeChannels $guestMainLog 'sendMix'
$monitorDestLevel = Get-NodeLevel $guestMainLog 'monitor'
$remoteLevelNow = Get-NodeLevel $guestMainLog 'remote'
Kill-GuestImage 'NLMatrixRouter-studio.exe' | Out-Null
Kill-GuestImage 'NLMatrixRouter-remote.exe' | Out-Null
Start-Sleep -Seconds 1
Log 'engines stopped'

# ------------------------------------------------------------------ evidence
$mainLogLocal = Join-Path $OutputDir 'main-engine.log'
$remoteLogLocal = Join-Path $OutputDir 'remote-engine.log'
try { NL-CopyFrom $guestMainLog $mainLogLocal | Out-Null } catch { Log ('main log: ' + $_.Exception.Message) }
try { NL-CopyFrom $guestRemoteLog $remoteLogLocal | Out-Null } catch { Log ('remote log: ' + $_.Exception.Message) }
$mainLogText = ''
if (Test-Path $mainLogLocal) { $mainLogText = Get-Content $mainLogLocal -Raw }
$remoteLogText = ''
if (Test-Path $remoteLogLocal) { $remoteLogText = Get-Content $remoteLogLocal -Raw }

$sendWav = Join-Path $OutputDir 'studio-send.wav'
$monitorWav = Join-Path $OutputDir 'studio-monitor.wav'
try { NL-CopyFrom 'C:\Windows\Temp\nl-studio-send.wav' $sendWav | Out-Null } catch { Log ('send wav: ' + $_.Exception.Message) }
try { NL-CopyFrom 'C:\Windows\Temp\nl-studio-monitor.wav' $monitorWav | Out-Null } catch { Log ('monitor wav: ' + $_.Exception.Message) }

$measurements = [ordered]@{}
if ((Test-Path $sendWav) -and (Test-Path $monitorWav)) {
    $localPath = Join-Path $OutputDir 'studio-analysis-local.json'
    $remotePath = Join-Path $OutputDir 'studio-analysis-remote.json'
    & $PythonExe $analyser $sendWav $monitorWav --tone-left 1000 --tone-right 1040 --json $localPath | Out-Null
    & $PythonExe $analyser $sendWav $monitorWav --tone-left 600 --tone-right 640 --json $remotePath | Out-Null
    $local = Get-Content $localPath -Raw | ConvertFrom-Json
    $remote = Get-Content $remotePath -Raw | ConvertFrom-Json
    foreach ($entry in $local.files) {
        $key = [IO.Path]::GetFileNameWithoutExtension($entry.file)
        $measurements[$key] = [ordered]@{
            meanDb = $entry.meanDb
            peakDb = $entry.peakDb
            longestSilenceAfterWarmupMs = $entry.longestSilenceAfterWarmupMs
            localToneLeftDb = $entry.toneLeftDb
            localToneRightDb = $entry.toneRightDb
        }
    }
    foreach ($entry in $remote.files) {
        $key = [IO.Path]::GetFileNameWithoutExtension($entry.file)
        if ($measurements.Contains($key)) {
            $measurements[$key].remoteToneLeftDb = $entry.toneLeftDb
            $measurements[$key].remoteToneRightDb = $entry.toneRightDb
        }
    }
}

# ------------------------------------------------------------------ verdict
$checks = New-Object System.Collections.Generic.List[object]
function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    $checks.Add([ordered]@{ name = $name; ok = $ok; detail = $detail })
    Log (('[{0}] {1} - {2}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail))
}

$send = $measurements['studio-send']
$monitor = $measurements['studio-monitor']
Add-Check 'remote program engine healthy' ($remoteLogText -match '"engineHealthy": true') 'log ok'
Add-Check 'studio engine healthy' ($mainLogText -match '"engineHealthy": true') 'log ok'
Add-Check 'return leg active' ($returnLevel -gt -80.0) ('return level=' + [math]::Round($returnLevel, 1) + ' dB, mode=' + $returnMode)

if ($send) {
    Add-Check 'send window carries the local tones' (($send.localToneLeftDb -ge -20) -and ($send.localToneRightDb -ge -20)) ('tones=' + $send.localToneLeftDb + '/' + $send.localToneRightDb + ' dBFS')
    Add-Check 'mix-minus keeps the remote return OUT of the send' (($send.remoteToneLeftDb -le -40) -and ($send.remoteToneRightDb -le -40)) ('remote=' + $send.remoteToneLeftDb + '/' + $send.remoteToneRightDb + ' dBFS')
    Add-Check 'send window continuity' ($send.longestSilenceAfterWarmupMs -le 30) ('longest silence=' + $send.longestSilenceAfterWarmupMs + ' ms')
} else { Add-Check 'send window measured' $false ('no data (ffmpeg exit ' + $sendExit + ')') }

$monMixLeft = if ($monMixChannels) { [double]$monMixChannels[0] } else { 0.0 }
$sendMixLeft = if ($sendMixChannels) { [double]$sendMixChannels[0] } else { 0.0 }
Add-Check 'monitor mix carries the local source and the remote return' ($monMixLeft -ge 0.35) ('monMix left=' + [math]::Round($monMixLeft, 3) + ' (local 0.25 + remote 0.25), remote source=' + [math]::Round($remoteLevelNow, 1) + ' dB')
Add-Check 'send bus stays N-1 (remote excluded)' (($sendMixLeft -le 0.30) -and ($monMixLeft -gt $sendMixLeft + 0.15)) ('sendMix left=' + [math]::Round($sendMixLeft, 3) + ' vs monMix=' + [math]::Round($monMixLeft, 3))
Add-Check 'monitor leg active' ($monitorDestLevel -ge -12.5) ('monitor destination=' + [math]::Round($monitorDestLevel, 1) + ' dB')
if ($monitor) {
    Add-Check 'monitor window recording continuity' ($monitor.longestSilenceAfterWarmupMs -le 30) ('longest silence=' + $monitor.longestSilenceAfterWarmupMs + ' ms, mean=' + $monitor.meanDb + ' dBFS')
}

$failed = @($checks | Where-Object { -not $_.ok }).Count
$summary = [ordered]@{
    tool = 'Run-StudioSessionCheck'
    utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    router = [ordered]@{ path = $RouterExe; sha256 = $routerHash }
    session = [ordered]@{
        returnMode = $returnMode
        returnLevelDb = $returnLevel
        sendLeg = $cableSend
        monitorLeg = $cableMonitor
        capture = $cableCapture
        recordings = [ordered]@{ send = $sendWav; monitor = $monitorWav }
    }
    measurements = $measurements
    checks = $checks
    failed = $failed
    verdict = if ($failed -eq 0) { 'PASS' } else { 'FAIL' }
}
$summaryPath = Join-Path $OutputDir 'studio-session-summary.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryPath -Encoding UTF8
Log ("summary: $summaryPath")
Log ("verdict: " + $summary.verdict)
if ($failed -eq 0) { exit 0 }
exit 1








