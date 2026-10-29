#requires -Version 5.1
<#
.SYNOPSIS
    Run-QuickConnectRealClient.ps1 - Quick Connect end-to-end proof with a real
    third-party Windows client capturing the NL virtual microphone.

.DESCRIPTION
    The Quick Connect acceptance criterion is NOT "the router stays alive for N
    seconds".  It is: a real Windows application selects the NL Mic endpoint,
    records audio, and the recording contains the routed signal.

    This harness uses the guest's own ffmpeg (DirectShow capture of the Windows
    capture endpoint - the same path any meeting/recording app uses) as the
    consumer, and the canonical continuity analyzer (tools/analyze_continuity.py)
    to decide PASS/FAIL.  A negative control (capture with no router running)
    proves the client really reads the endpoint and that the signal is not
    ambient/system noise.

    Sequence in the guest:
      1. optional control capture, no router           -> must be silence
      2. start ffmpeg capture of the NL Mic endpoint
      3. start NLMatrixRouter with the Quick Connect config (single ASIO channel
         source -> NL Mic 1) for a fixed number of seconds
      4. collect WAV + router log + METERS, analyze, verdict

.PARAMETER Config
    Config file in tools/vm/configs (default quick_connect_nlmic1.json).

.PARAMETER Endpoint
    DirectShow capture device name as seen by ffmpeg in the guest.

.PARAMETER CaptureSeconds
    Client capture window (default 14). The router runs CaptureSeconds + 4 s.

.PARAMETER SkipControl
    Skip the no-router negative control (the control should normally run).

.EXAMPLE
    .\Run-QuickConnectRealClient.ps1
#>
[CmdletBinding()]
param(
    [string]$Config = 'quick_connect_nlmic1.json',
    [string]$Endpoint = 'NL Mic 1 (NLMatrix Virtual Audio Device)',
    [int]$CaptureSeconds = 14,
    [string]$OutputDir = '',
    [string]$PythonExe = 'K:\AI\Agente\runtime\python.exe',
    [string]$FfmpegHost = 'C:\Users\Nicola\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0.2-full_build\bin\ffmpeg.exe',
    [switch]$SkipControl
)

$ErrorActionPreference = 'Stop'
$script:Workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $PSScriptRoot 'NLVM.ps1')

$guestRoot   = 'C:\NLMatrix'
$guestQc     = 'C:\NLMatrix\qc'
$guestFfmpeg = 'C:\NLMatrixTools\ffmpeg.exe'
$guestRouter = 'C:\NLMatrix\NLMatrixRouter.exe'

if (-not $OutputDir) {
    $OutputDir = Join-Path $script:Workspace ('reports\quickconnect\' + (Get-Date -Format 'yyyy-MM-dd'))
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$logPath = Join-Path $OutputDir 'quickconnect_host.log'
if (Test-Path $logPath) { Remove-Item $logPath -Force }

function Log {
    param([string]$msg)
    $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $msg)
    Write-Host $line
    Add-Content -Path $logPath -Value $line -Encoding UTF8
}

function Assert-True {
    param([bool]$cond, [string]$msg)
    if (-not $cond) { throw "ASSERT FAILED: $msg" }
}

# ---------------------------------------------------------------- prerequisites
function Get-GuestSha256 {
    param([string]$GuestPath)
    $out = NL-Exec "if exist $GuestPath certutil -hashfile $GuestPath SHA256" -TimeoutSec 120
    $hex = ($out -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^[0-9a-fA-F]{64}$' } | Select-Object -First 1)
    if ($hex) { return $hex.ToLowerInvariant() }
    return ''
}

$hostFfmpegHash = (Get-FileHash $FfmpegHost -Algorithm SHA256).Hash.ToLowerInvariant()
$guestHash = Get-GuestSha256 $guestFfmpeg
if (-not $guestHash -or $guestHash.ToLowerInvariant() -ne $hostFfmpegHash) {
    Log "guest ffmpeg missing/mismatched; copying $(Split-Path $FfmpegHost -Leaf) ($([math]::Round((Get-Item $FfmpegHost).Length / 1MB)) MiB)"
    $ffmpegDir = Split-Path $guestFfmpeg -Parent
    NL-Exec "if not exist $ffmpegDir mkdir $ffmpegDir" | Out-Null
    NL-CopyTo $FfmpegHost $guestFfmpeg | Out-Null
    $guestHash = Get-GuestSha256 $guestFfmpeg
}
Assert-True ($guestHash -and $guestHash -eq $hostFfmpegHash) 'guest ffmpeg hash'
Log "guest ffmpeg sha256=$guestHash (real third-party Windows client)"

$routerHash = Get-GuestSha256 $guestRouter
Assert-True ($routerHash -ne '') 'guest router present'
Log "guest router sha256=$routerHash"
Assert-True (Test-Path (Join-Path $PSScriptRoot (Join-Path 'configs' $Config))) "config not found: $Config"

NL-Exec "if not exist $guestQc mkdir $guestQc" | Out-Null
NL-CopyTo (Join-Path $PSScriptRoot (Join-Path 'configs' $Config)) "$guestQc\quick_connect.json" | Out-Null
Log "config deployed: $Config"

NL-Kill 'NLMatrixRouter.exe'
Start-Sleep -Milliseconds 500

# ------------------------------------------------------------------- control run
if (-not $SkipControl) {
    Log 'control: capture NL Mic with no router (expect silence)'
    NL-Exec "del /q $guestQc\control.wav 2>nul & $guestFfmpeg -hide_banner -f dshow -i audio=`"$Endpoint`" -t 5 -ac 1 -ar 48000 -y $guestQc\control.wav > $guestQc\ffmpeg_control.log 2>&1" -TimeoutSec 120 | Out-Null
    $controlWav = Join-Path $OutputDir 'control_no_router.wav'
    NL-CopyFrom "$guestQc\control.wav" $controlWav | Out-Null
    $controlJson = Join-Path $OutputDir 'control_no_router.json'
    & $PythonExe (Join-Path $script:Workspace 'tools\analyze_continuity.py') $controlWav --expect-silence --json $controlJson --quiet | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'negative control must be silent (client reads endpoint, no routed signal)'
    Log 'control PASS: endpoint silent without router'
}

# ------------------------------------------------------------------ positive run
$routerSeconds = $CaptureSeconds + 4
$bat = @"
@echo off
set QC=$guestQc
del /q %QC%\capture.wav %QC%\result.txt 2>nul
start "nlm-qc-client" /b $guestFfmpeg -hide_banner -f dshow -i audio="$Endpoint" -t $CaptureSeconds -ac 1 -ar 48000 -y %QC%\capture.wav > %QC%\ffmpeg_client.log 2>&1
timeout /t 2 /nobreak >nul
$guestRouter --config %QC%\quick_connect.json --seconds $routerSeconds --telemetry %QC%\meters.jsonl --log %QC%\router.log > %QC%\router_out.log 2>&1
echo ROUTER_EXIT=%ERRORLEVEL% > %QC%\result.txt
timeout /t 10 /nobreak >nul
echo CLIENT_DONE >> %QC%\result.txt
"@
$batLocal = Join-Path $env:TEMP ('nl_qc_' + [guid]::NewGuid().ToString('N') + '.bat')
Set-Content -Path $batLocal -Value ($bat -replace "`n", "`r`n") -Encoding ASCII
NL-CopyTo $batLocal "$guestQc\run_qc.bat" | Out-Null
Remove-Item $batLocal -Force -ErrorAction SilentlyContinue

Log "positive: real client captures $Endpoint for ${CaptureSeconds}s while router runs ${routerSeconds}s"
$runOut = NL-Exec "$guestQc\run_qc.bat & type $guestQc\result.txt" -TimeoutSec ($routerSeconds + 90)
$runOut | Set-Content (Join-Path $OutputDir 'guest_run.log') -Encoding UTF8
if ($runOut -match 'ROUTER_EXIT=(\d+)') { $routerExit = [int]$Matches[1] } else { $routerExit = -1 }
Log "router exit code: $routerExit"

$wav = Join-Path $OutputDir 'quickconnect_capture.wav'
NL-CopyFrom "$guestQc\capture.wav" $wav | Out-Null
foreach ($f in @('router.log', 'router_out.log', 'meters.jsonl', 'ffmpeg_client.log')) {
    try { NL-CopyFrom "$guestQc\$f" (Join-Path $OutputDir $f) | Out-Null } catch { Log "warn: cannot fetch $f" }
}

$json = Join-Path $OutputDir 'quickconnect_capture.json'
& $PythonExe (Join-Path $script:Workspace 'tools\analyze_continuity.py') $wav --tone 1000 --expect-signal --json $json 2>&1 |
    Tee-Object -FilePath (Join-Path $OutputDir 'analysis.log')
$analysisExit = $LASTEXITCODE
$analysis = Get-Content $json -Raw | ConvertFrom-Json
$ch0 = $analysis.channels | Select-Object -First 1

$summary = [ordered]@{
    endpoint         = $Endpoint
    client           = 'ffmpeg ' + (Split-Path $guestFfmpeg -Leaf) + ' (DirectShow capture of the Windows endpoint)'
    clientSha256     = $guestHash
    routerSha256     = $routerHash
    config           = $Config
    captureSeconds   = $CaptureSeconds
    routerExitCode   = $routerExit
    analyzerVersion  = $analysis.analyzer.version
    wavSha256        = $analysis.sha256
    durationSec      = $analysis.audio.duration_s
    sampleRate       = $analysis.audio.sample_rate
    rmsDbfs          = $ch0.rms_dbfs
    peakDbfs         = $ch0.peak_dbfs
    tone             = $ch0.tone
    gaps             = $ch0.gaps
    verdict          = $analysis.verdict.result
    reasons          = $analysis.verdict.reasons
    warnings         = $analysis.verdict.warnings
    evidenceDir      = $OutputDir
}
$summary | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputDir 'quickconnect_summary.json') -Encoding UTF8

Log ("analysis verdict={0} reasons=[{1}] rms={2} dBFS tone=[{3}] gaps=[{4}]" -f `
        $analysis.verdict.result, ($analysis.verdict.reasons -join ','), $ch0.rms_dbfs, `
        ($ch0.tone | ConvertTo-Json -Compress -Depth 3), ($ch0.gaps | ConvertTo-Json -Compress -Depth 3))
Log "evidence: $OutputDir"

$pass = ($analysisExit -eq 0) -and ($analysis.verdict.result -eq 'PASS') -and ($routerExit -eq 0)
if ($pass) { Log 'VERDICT: PASS - real Windows client captured the routed Quick Connect signal' }
else { Log 'VERDICT: FAIL' }
if ($pass) { exit 0 } else { exit 1 }
