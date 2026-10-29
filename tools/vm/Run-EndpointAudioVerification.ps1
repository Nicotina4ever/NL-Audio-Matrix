#requires -Version 5.1
<#
.SYNOPSIS
    Run-EndpointAudioVerification.ps1 - verify all eight NL endpoints with real audio.

.DESCRIPTION
    Both phases run inside the VMware guest; the host only orchestrates and
    analyzes the WAVs.

    Mic 1..4    - router feeds four distinct tones (1000/1040/1080/1120 Hz) to
                  NL Mic 1..4; ffmpeg (a real Windows client) records each endpoint.
                  PASS = signal present, expected frequency, no dropouts/splices.

    Return 1..4 - four WASAPI playback clients play distinct tones into
                  NL Return 1..4; the router captures each render endpoint with its
                  native WASAPI loopback source and routes Return N -> Mic N; ffmpeg
                  records each Mic.  PASS = the tone played on Return N appears on
                  Mic N with the expected frequency.

    Evidence (WAV + analyzer JSON) stays in reports/endpoint_audio/<date>/ (git-ignored).
#>
[CmdletBinding()]
param(
    [string]$Phase = 'Both',   # Mic | Return | Both
    [string]$OutputDir = '',
    [string]$PythonExe = 'K:\AI\Agente\runtime\python.exe',
    [int]$CaptureSeconds = 10,
    [string]$FfmpegHost = 'C:\Users\Nicola\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0.2-full_build\bin\ffmpeg.exe'
)
$ErrorActionPreference = 'Stop'
$script:Workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $PSScriptRoot 'NLVM.ps1')

$guestQc = 'C:\NLMatrixTools\audio'
$guestFfmpeg = 'C:\NLMatrixTools\ffmpeg.exe'
$guestPlay = 'C:\NLMatrixTools\NLMatrixDriverAudioTest.exe'
$guestRouter = 'C:\NLMatrix\NLMatrixRouter.exe'
$analyzer = Join-Path $script:Workspace 'tools\analyze_continuity.py'

if (-not $OutputDir) { $OutputDir = Join-Path $script:Workspace ('reports\endpoint_audio\' + (Get-Date -Format 'yyyy-MM-dd')) }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$logPath = Join-Path $OutputDir 'endpoint_audio_host.log'
if (Test-Path $logPath) { Remove-Item $logPath -Force }
function Log([string]$m) { $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m); Write-Host $line; Add-Content $logPath $line -Encoding UTF8 }

function New-GuestBat([string]$guestPath, [string[]]$lines) {
    $f = Join-Path $env:TEMP ([guid]::NewGuid().ToString('N') + '.bat')
    $content = '@echo off' + "`r`n" + ($lines -join "`r`n") + "`r`n"
    Set-Content -Path $f -Value $content -Encoding ASCII -NoNewline
    NL-CopyTo $f $guestPath | Out-Null
    Remove-Item $f -Force -ErrorAction SilentlyContinue
}
function GuestSha256([string]$p) {
    $o = NL-Exec "if exist $p certutil -hashfile $p SHA256" -TimeoutSec 120
    $h = ($o -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^[0-9a-fA-F]{64}$' } | Select-Object -First 1)
    if ($h) { return $h.ToLowerInvariant() } else { return '' }
}
function Analyze([string]$wav, [double]$expectedHz, [string]$label) {
    $json = [System.IO.Path]::ChangeExtension($wav, '.json')
    & $PythonExe $analyzer $wav --tone $expectedHz --expect-signal --json $json 2>&1 |
        Set-Content (Join-Path $OutputDir ("analysis_$label.log".Replace(' ', '_'))) -Encoding UTF8
    $code = $LASTEXITCODE
    $a = Get-Content $json -Raw | ConvertFrom-Json
    $ch = $a.channels | Select-Object -First 1
    $freq = $null
    if ($ch.tone) { $freq = [double]$ch.tone.freq_fft_hz }
    $ok = ($code -eq 0) -and ($a.verdict.result -eq 'PASS')
    [pscustomobject]@{
        endpoint = $label; expectedHz = $expectedHz; detectedHz = $freq
        rmsDbfs = $ch.rms_dbfs; gapsMs = $ch.gaps.total_ms; verdict = $a.verdict.result; pass = $ok
    }
}
function Capture([int]$n, [string]$outName, [double]$expectedHz, [string]$label) {
    $cmd = "$guestFfmpeg -hide_banner -f dshow -i audio=`"NL Mic $n (NLMatrix Virtual Audio Device)`" -t $CaptureSeconds -ac 1 -ar 48000 -y $guestQc\$outName.wav > $guestQc\cap_$outName.log 2>&1"
    NL-Exec $cmd -TimeoutSec 120 | Out-Null
    $wav = Join-Path $OutputDir ("$outName.wav")
    NL-CopyFrom "$guestQc\$outName.wav" $wav | Out-Null
    return (Analyze $wav $expectedHz $label)
}

NL-Exec "if not exist $guestQc mkdir $guestQc" | Out-Null
$hostFfmpegHash = (Get-FileHash $FfmpegHost -Algorithm SHA256).Hash.ToLowerInvariant()
if ((GuestSha256 $guestFfmpeg) -ne $hostFfmpegHash) { Log 'copying ffmpeg to guest'; NL-CopyTo $FfmpegHost $guestFfmpeg | Out-Null }
NL-CopyTo (Join-Path $script:Workspace 'app\build\NLMatrixDriverAudioTest_artefacts\Release\NLMatrixDriverAudioTest.exe') $guestPlay | Out-Null
foreach ($c in @('four_test_flex.json', 'return_loopback_test.json')) {
    NL-CopyTo (Join-Path $PSScriptRoot (Join-Path 'configs' $c)) "$guestQc\$c" | Out-Null
}
Log 'tools and configs deployed'

$results = @()
$tones = @{ 1 = 1000; 2 = 1040; 3 = 1080; 4 = 1120 }

if ($Phase -in @('Mic', 'Both')) {
    Log 'phase Mic: router 4 tones -> NL Mic 1..4, ffmpeg captures each endpoint'
    NL-Kill 'NLMatrixRouter.exe'; NL-Kill 'ffmpeg.exe'
    NL-Exec "del /q $guestQc\mic*.wav $guestQc\cap_mic*.log 2>nul" -TimeoutSec 60 | Out-Null
    New-GuestBat "$guestQc\mic_router.bat" @("$guestRouter --config $guestQc\four_test_flex.json --seconds 70 --telemetry $guestQc\mic_meters.jsonl --log $guestQc\mic_router.log > $guestQc\mic_router_out.log 2>&1")
    New-GuestBat "$guestQc\mic_start.bat" @('start "nlrouter" /b C:\NLMatrixTools\audio\mic_router.bat')
    NL-Exec "$guestQc\mic_start.bat" -TimeoutSec 60 | Out-Null
    Start-Sleep -Seconds 4
    foreach ($n in 1..4) {
        $r = Capture $n "mic$n" $tones[$n] "NL Mic $n"
        $results += $r
        Log ("Mic $n : detected=" + $r.detectedHz + " expected=" + $tones[$n] + " rms=" + $r.rmsDbfs + " gaps_ms=" + $r.gapsMs + " -> " + $r.verdict)
    }
    NL-Kill 'NLMatrixRouter.exe'
    try { NL-CopyFrom "$guestQc\mic_router_out.log" (Join-Path $OutputDir 'mic_router_out.log') | Out-Null } catch { }
}

if ($Phase -in @('Return', 'Both')) {
    Log 'phase Return: play tones into NL Return 1..4, loopback -> NL Mic 1..4, ffmpeg captures'
    NL-Kill 'NLMatrixRouter.exe'; NL-Kill 'ffmpeg.exe'; NL-Kill 'NLMatrixDriverAudioTest.exe'
    NL-Exec "del /q $guestQc\ret*.wav $guestQc\cap_ret*.log $guestQc\play*.log 2>nul" -TimeoutSec 60 | Out-Null
    foreach ($n in 1..4) {
        New-GuestBat "$guestQc\play$n.bat" @("$guestPlay --play --tone-hz $($tones[$n]) --output `"NL Return $n`" --seconds 70 > $guestQc\play$n.log 2>&1")
    }
    New-GuestBat "$guestQc\ret_router.bat" @("$guestRouter --config $guestQc\return_loopback_test.json --seconds 66 --telemetry $guestQc\ret_meters.jsonl --log $guestQc\ret_router.log > $guestQc\ret_router_out.log 2>&1")
    New-GuestBat "$guestQc\ret_start.bat" @(
        'start "play1" /b C:\NLMatrixTools\audio\play1.bat',
        'start "play2" /b C:\NLMatrixTools\audio\play2.bat',
        'start "play3" /b C:\NLMatrixTools\audio\play3.bat',
        'start "play4" /b C:\NLMatrixTools\audio\play4.bat',
        'start "nlrouter" /b C:\NLMatrixTools\audio\ret_router.bat')
    NL-Exec "$guestQc\ret_start.bat" -TimeoutSec 60 | Out-Null
    Start-Sleep -Seconds 5
    foreach ($n in 1..4) {
        $r = Capture $n "ret$n" $tones[$n] "NL Return $n"
        $results += $r
        Log ("Return $n : detected=" + $r.detectedHz + " expected=" + $tones[$n] + " rms=" + $r.rmsDbfs + " gaps_ms=" + $r.gapsMs + " -> " + $r.verdict)
    }
    NL-Kill 'NLMatrixRouter.exe'; NL-Kill 'NLMatrixDriverAudioTest.exe'
    foreach ($n in 1..4) { try { NL-CopyFrom "$guestQc\play$n.log" (Join-Path $OutputDir ("play$n.log")) | Out-Null } catch { } }
    try { NL-CopyFrom "$guestQc\ret_router_out.log" (Join-Path $OutputDir 'ret_router_out.log') | Out-Null } catch { }
}

$results | Export-Csv (Join-Path $OutputDir 'endpoint_audio_summary.csv') -NoTypeInformation -Encoding UTF8
$failed = @($results | Where-Object { -not $_.pass })
Log ("endpoints verified: " + $results.Count + ", failed: " + $failed.Count)
if ($failed.Count -gt 0) {
    foreach ($f in $failed) { Log ("FAIL " + $f.endpoint + " expected=" + $f.expectedHz + " detected=" + $f.detectedHz + " rms=" + $f.rmsDbfs + " gaps=" + $f.gapsMs) }
    exit 1
}
Log 'VERDICT: PASS - all verified endpoints carried real audio'
exit 0
