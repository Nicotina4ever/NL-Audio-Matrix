#requires -Version 5.1
<#
.SYNOPSIS
    Full VB-CABLE send path: NL Matrix -> WASAPI render CABLE Input -> VB-CABLE
    driver -> WASAPI capture CABLE Output -> recording by an independent client.

.DESCRIPTION
    The earlier leg test proves the render endpoint plus the loopback of the SAME
    endpoint; this harness proves the real cable. The engine renders into the
    signed VB-CABLE render endpoint, Windows routes the audio through the driver
    and an independent client (ffmpeg, a plain WASAPI capture client) records the
    cable's capture endpoint.

    Measured on the recording itself (host-side analysis, the guest only records):
      * PCM present, left 1000 Hz / right 1040 Hz distinct tones;
      * gain: a config hot-reload to -12 dB must drop the level by 12 dB;
      * mute: a config hot-reload mute must reach the floor;
      * unmute: the tone must come back at the original level;
      * continuity: no silence gaps or dropouts while unmuted;
      * resource release: after the engine stops, a capture client still opens the
        cable and records silence; a second engine start sends the tone again.

    Writes a JSON summary in the evidence directory. Exit 0 = PASS.

.PARAMETER RouterExe
    NLMatrixRouter.exe to test (default: the current Release build).

.PARAMETER OutputDir
    Evidence directory (default reports\send_cable\<timestamp>).

.EXAMPLE
    powershell -File tools\vm\Run-SendCableCheck.ps1
#>
[CmdletBinding()]
param(
    [string]$RouterExe = '',
    [string]$OutputDir = '',
    [int]$PhaseSeconds = 4,
    [string]$PythonExe = 'K:\AI\Agente\runtime\python.exe'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'NLVM.ps1')

$script:Workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $RouterExe) {
    $RouterExe = Join-Path $script:Workspace 'app\build\NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe'
}
if (-not (Test-Path $RouterExe)) { throw "router executable not found: $RouterExe" }
if (-not $OutputDir) {
    $OutputDir = Join-Path $script:Workspace ('reports\send_cable\' + (Get-Date -Format 'yyyy-MM-dd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$analyser = Join-Path $script:Workspace 'tools\analyze_send_path.py'
if (-not (Test-Path $analyser)) { throw "analyser not found: $analyser" }
$logPath = Join-Path $OutputDir 'send_cable_host.log'
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
function CommandExit([string]$output) {
    if ($output -match 'EXITCODE=(-?\d+)') { return [int]$Matches[1] }
    return -999
}

$guestDir = 'C:\NLMatrixTools'
$guestExe = "$guestDir\NLMatrixRouter-send.exe"
$guestConfig = 'C:\Windows\Temp\nl-send-config.json'
$guestControl = 'C:\Windows\Temp\nl-send-control.txt'
$guestEngineLog = 'C:\Windows\Temp\nl-send-engine.log'
$guestRunBat = 'C:\Windows\Temp\nl-send-run.bat'
$guestStartBat = 'C:\Windows\Temp\nl-send-start.bat'
$guestFfmpeg = "$guestDir\ffmpeg.exe"
$cableRender = 'CABLE Input (VB-Audio Virtual Cable)'
$cableCapture = 'CABLE Output (VB-Audio Virtual Cable)'
$engineImage = 'NLMatrixRouter-send.exe'

function SendConfig([int]$gainDb, [bool]$muted) {
    $mutedText = if ($muted) { 'true' } else { 'false' }
    return @"
{
  "name": "Send cable check",
  "asio": "FlexASIO",
  "devices": [
    { "id": "flex", "backend": "asio", "driver": "FlexASIO", "sampleRate": 48000, "buffer": 256, "clockMaster": true, "rateAuto": true }
  ],
  "buses": [],
  "sources": [
    { "id": "tone", "name": "local tone", "type": "test", "channels": [0, 1], "level": 0.25, "baseFrequency": 1000, "enabled": true }
  ],
  "destinations": [
    { "id": "cableIn", "name": "CABLE Input", "type": "render", "endpoint": "$cableRender", "channels": [0, 1], "gainDb": $gainDb, "muted": $mutedText, "enabled": true }
  ],
  "routes": [
    { "source": "tone", "destination": "cableIn", "gainDb": 0 }
  ]
}
"@
}

function PushConfig([int]$gainDb, [bool]$muted, [string]$tag) {
    $local = New-TempFile ("nl-send-" + $tag + ".json") (SendConfig $gainDb $muted)
    NL-Run ("del /f /q " + $guestConfig) | Out-Null
    NL-CopyTo $local $guestConfig | Out-Null
    Log ("config pushed: gainDb=$gainDb muted=$muted")
}

function EngineRunning {
    $output = NL-Exec ('tasklist /FI "IMAGENAME eq ' + $engineImage + '" /NH') -TimeoutSec 60
    return ($output -match [regex]::Escape($engineImage))
}

function Start-Engine {
    $runBat = New-TempFile 'nl-send-run.bat' (@(
        '@echo off',
        "`"$guestExe`" --config `"$guestConfig`" --control `"$guestControl`" --seconds 120 > `"$guestEngineLog`" 2>&1"
    ) -join "`r`n")
    NL-CopyTo $runBat $guestRunBat | Out-Null
    $startBat = New-TempFile 'nl-send-start.bat' (@(
        '@echo off',
        "start `"NLMatrixSend`" /min cmd /c `"$guestRunBat`""
    ) -join "`r`n")
    NL-CopyTo $startBat $guestStartBat | Out-Null
    # Stop file must not exist, otherwise the engine exits at once.
    NL-Run ("del /f /q " + $guestControl) | Out-Null
    NL-Run ("del /f /q " + $guestEngineLog) | Out-Null
    NL-Run $guestStartBat | Out-Null
    Start-Sleep -Seconds 5
    if (-not (EngineRunning)) {
        $logText = NL-Exec ('type "' + $guestEngineLog + '"') -TimeoutSec 60
        throw ("engine did not start. Guest log: `r`n" + $logText)
    }
    Log 'engine started and running'
}

function Stop-Engine {
    $stopFile = New-TempFile 'nl-send-stop.txt' 'stop'
    NL-Run ("del /f /q " + $guestControl) | Out-Null
    NL-CopyTo $stopFile $guestControl | Out-Null
    Log 'stop requested'
    for ($i = 0; $i -lt 12; $i++) {
        Start-Sleep -Milliseconds 500
        if (-not (EngineRunning)) { Log 'engine exited'; return $true }
    }
    NL-Kill $engineImage | Out-Null
    Log 'engine did not exit on the control file; killed'
    return $false
}

function Record-Wav([string]$guestWav, [int]$seconds, [string]$tag) {
    $command = '"' + $guestFfmpeg + '" -hide_banner -loglevel error -y -f dshow -i "audio=' + $cableCapture + '"' +
               ' -t ' + $seconds + ' -ar 48000 -ac 2 -c:a pcm_s16le "' + $guestWav + '"'
    $out = NL-Exec $command -TimeoutSec 180
    $code = CommandExit $out
    Log ("recorded $tag (" + $seconds + " s) ffmpeg exit=" + $code)
    return $code
}

# ------------------------------------------------------------------ run
Log 'preparing the guest'
NL-Run ("if not exist $guestDir mkdir $guestDir") | Out-Null
$ffmpegProbe = NL-Exec 'if exist C:\NLMatrixTools\ffmpeg.exe (echo FFMPEG_YES) else (echo FFMPEG_NO)' -TimeoutSec 60
if ($ffmpegProbe -notmatch 'FFMPEG_YES') { throw 'ffmpeg is missing in the guest (C:\NLMatrixTools\ffmpeg.exe)' }
NL-Kill $engineImage | Out-Null
NL-Kill 'ffmpeg.exe' | Out-Null
# FlexASIO needs a valid device target: the guest TOML must not point at the
# (now absent) NL driver endpoints.
$flexToml = Join-Path $script:Workspace 'tools\vm\FlexASIO-exclusive-48000.toml'
if (Test-Path $flexToml) { NL-CopyTo $flexToml 'C:\Users\NLMatrix\FlexASIO.toml' | Out-Null; Log 'FlexASIO.toml deployed (speakers, exclusive)' }
$routerHash = NL-CopyTo $RouterExe $guestExe
Log "router copied (sha256 $routerHash)"

PushConfig 0 $false 'phase1'
Start-Engine

$records = [ordered]@{}
$records['p1'] = Record-Wav 'C:\Windows\Temp\nl-send-p1.wav' $PhaseSeconds 'p1-nominal'

PushConfig -12 $false 'phase2'
Start-Sleep -Milliseconds 1500
$records['p2'] = Record-Wav 'C:\Windows\Temp\nl-send-p2.wav' $PhaseSeconds 'p2-gain-minus12'

PushConfig -12 $true 'phase3'
Start-Sleep -Milliseconds 1500
$records['p3'] = Record-Wav 'C:\Windows\Temp\nl-send-p3.wav' 2 'p3-muted'

PushConfig 0 $false 'phase4'
Start-Sleep -Milliseconds 1500
$records['p4'] = Record-Wav 'C:\Windows\Temp\nl-send-p4.wav' $PhaseSeconds 'p4-unmuted'

$cleanStop = Stop-Engine
$engineLogLocal = Join-Path $OutputDir 'engine.log'
try { NL-CopyFrom $guestEngineLog $engineLogLocal | Out-Null } catch { Log ("engine log not available: " + $_.Exception.Message) }
$engineLogText = ''
if (Test-Path $engineLogLocal) { $engineLogText = Get-Content $engineLogLocal -Raw }
$engineRouted = ($engineLogText -match '"engineHealthy": true') -and ($engineLogText -match 'destinations: 1')

# The cable must be free again after the engine released it.
$records['p5'] = Record-Wav 'C:\Windows\Temp\nl-send-p5.wav' 2 'p5-after-stop'

# A second start must send audio through the cable again.
PushConfig 0 $false 'phase6'
Start-Engine
$records['p6'] = Record-Wav 'C:\Windows\Temp\nl-send-p6.wav' 3 'p6-restarted'
$secondStop = Stop-Engine

# ------------------------------------------------------------------ fetch + analyze on the host
$wavLocals = @{}
foreach ($name in @('p1', 'p2', 'p3', 'p4', 'p5', 'p6')) {
    $local = Join-Path $OutputDir ("nl-send-" + $name + ".wav")
    try {
        NL-CopyFrom ("C:\Windows\Temp\nl-send-" + $name + ".wav") $local | Out-Null
        $wavLocals[$name] = $local
    } catch {
        Log ("wav not available: " + $name + " (" + $_.Exception.Message + ")")
    }
}

$analysisPath = Join-Path $OutputDir 'analysis.json'
$analysis = $null
if ($wavLocals.Count -gt 0) {
    $pyArgs = @($analyser) + @($wavLocals.Values) + @('--json', $analysisPath)
    & $PythonExe @pyArgs | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'host analysis failed' }
    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
}
$measured = @{}
if ($analysis) {
    foreach ($entry in $analysis.files) {
        $key = [IO.Path]::GetFileNameWithoutExtension($entry.file) -replace '^nl-send-', ''
        $measured[$key] = $entry
    }
}

# ------------------------------------------------------------------ verdict
$checks = New-Object System.Collections.Generic.List[object]
function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    $checks.Add([ordered]@{ name = $name; ok = $ok; detail = $detail })
    Log (('[{0}] {1} - {2}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail))
}

Add-Check 'engine started, ran and routed to CABLE Input' ($engineRouted -and $cleanStop) ('log=' + $engineRouted + ' cleanStop=' + $cleanStop)
Add-Check 'independent client recorded PCM through the cable' (($records['p1'] -eq 0) -and $measured.ContainsKey('p1')) ('ffmpeg exit=' + $records['p1'])
$p1 = $measured['p1']; $p2 = $measured['p2']; $p3 = $measured['p3']; $p4 = $measured['p4']; $p5 = $measured['p5']; $p6 = $measured['p6']

if ($p1) {
    Add-Check 'phase1 level around -12 dBFS peak' (($p1.meanDb -ge -18) -and ($p1.meanDb -le -12) -and ($p1.peakDb -ge -14) -and ($p1.peakDb -le -8)) ('mean=' + $p1.meanDb + ' dBFS peak=' + $p1.peakDb)
    Add-Check 'phase1 left channel carries 1000 Hz' (($p1.toneLeftDb -ge -30) -and ($p1.toneRatioLeft -ge 10)) ('tone=' + $p1.toneLeftDb + ' dBFS ratio=' + $p1.toneRatioLeft)
    Add-Check 'phase1 right channel carries 1040 Hz' (($p1.toneRightDb -ge -30) -and ($p1.toneRatioRight -ge 10)) ('tone=' + $p1.toneRightDb + ' dBFS ratio=' + $p1.toneRatioRight)
    Add-Check 'phase1 continuity (no silence runs)' ($p1.longestSilenceAfterWarmupMs -le 30) ('longest silence after warmup=' + $p1.longestSilenceAfterWarmupMs + ' ms visible=' + $p1.longestSilenceMs + ' ms')
} else { Add-Check 'phase1 measured' $false 'no data' }

if ($p1 -and $p2) {
    $delta = [math]::Round($p1.meanDb - $p2.meanDb, 2)
    Add-Check 'gain -12 dB applied through the cable' (($delta -ge 10) -and ($delta -le 14)) ('delta=' + $delta + ' dB')
} else { Add-Check 'gain step measured' $false 'no data' }

if ($p3) {
    Add-Check 'mute reaches the floor' ($p3.meanDb -le -70) ('mean=' + $p3.meanDb + ' dBFS')
    Add-Check 'mute removes both tones' (($p3.toneLeftDb -le -60) -and ($p3.toneRightDb -le -60)) ('tones=' + $p3.toneLeftDb + '/' + $p3.toneRightDb + ' dBFS')
} else { Add-Check 'mute measured' $false 'no data' }

if ($p1 -and $p4) {
    Add-Check 'unmute restores the level' ([math]::Abs($p4.meanDb - $p1.meanDb) -le 2.0) ('mean=' + $p4.meanDb + ' vs ' + $p1.meanDb)
    Add-Check 'unmute restores continuity' ($p4.longestSilenceAfterWarmupMs -le 30) ('longest silence after warmup=' + $p4.longestSilenceAfterWarmupMs + ' ms')
} else { Add-Check 'unmute measured' $false 'no data' }

if ($p5) {
    Add-Check 'cable free and recording after engine stop' ($records['p5'] -eq 0) ('ffmpeg exit=' + $records['p5'])
    Add-Check 'no leftover tone after engine stop' ($p5.meanDb -le -70) ('mean=' + $p5.meanDb + ' dBFS')
} else { Add-Check 'post-stop capture measured' $false 'no data' }

if ($p6) {
    Add-Check 'second engine start sends the tone again' (($p6.meanDb -ge -16) -and ($p6.meanDb -le -8) -and ($records['p6'] -eq 0)) ('mean=' + $p6.meanDb + ' dBFS')
} else { Add-Check 'restart measured' $false 'no data' }
Add-Check 'second engine exits cleanly' $secondStop ('cleanStop=' + $secondStop)

$failed = @($checks | Where-Object { -not $_.ok }).Count
$summary = [ordered]@{
    tool = 'Run-SendCableCheck'
    utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    router = [ordered]@{ path = $RouterExe; sha256 = $routerHash }
    cable = [ordered]@{ render = $cableRender; capture = $cableCapture; client = 'ffmpeg dshow WASAPI capture'; analyser = 'tools/analyze_send_path.py' }
    recordings = $records
    measurements = $measured
    checks = $checks
    failed = $failed
    verdict = if ($failed -eq 0) { 'PASS' } else { 'FAIL' }
}
$summaryPath = Join-Path $OutputDir 'send-cable-summary.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryPath -Encoding UTF8
Log ("summary: $summaryPath")
Log ("verdict: " + $summary.verdict)
if ($failed -eq 0) { exit 0 }
exit 1



