#requires -Version 5.1
<#
.SYNOPSIS
    Run-QuickConnectGuiRealClient.ps1 - Quick Connect acceptance, driven by the
    real GUI and consumed by a real Windows client.

.DESCRIPTION
    This harness does not start the router by hand: it launches the GUI with the
    production Quick Connect action (--quickconnecttest, the same code path the
    Quick Connect dialog runs), waits for the engine to be Running, and then lets
    the guest's own ffmpeg capture the NL Mic endpoint through DirectShow - the
    same path a meeting/recording application uses.

    It also proves the two Quick Connect invariants that are easy to break:
      * the Quick Connect preset is written in its own directory and the Studio
        profile loaded before it is byte-identical afterwards;
      * the real application exit path stops the engine without leaving a router
        process behind.

    The deterministic 1 kHz test tone source is used on purpose: the point here is
    the GUI -> engine -> endpoint -> client path, not the ASIO input device, which
    the router-level harness (A17) already covers.  A21/A22 are not touched.
#>
[CmdletBinding()]
param(
    [string]$Device = 'FlexASIO',
    [int]$Channel = 1,
    [int]$EndpointIndex = 0,
    [string]$Endpoint = 'NL Mic 1 (NLMatrix Virtual Audio Device)',
    [int]$CaptureSeconds = 12,
    [string]$OutputDir = '',
    [string]$PythonExe = 'K:\AI\Agente\runtime\python.exe',
    [string]$FfmpegHost = 'C:\Users\Nicola\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0.2-full_build\bin\ffmpeg.exe'
)

$ErrorActionPreference = 'Stop'
$script:Workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $PSScriptRoot 'NLVM.ps1')

$guestRoot     = 'C:\NLMatrix'
$guestTools    = 'C:\NLMatrixTools'
$guestQcDir    = 'C:\NLMatrixTools\qc'
$guestTemp     = 'C:\Users\NLMatrix\AppData\Local\Temp'
$guestGui      = 'C:\NLMatrix\NLMatrixUI.exe'
$guestRouter   = 'C:\NLMatrix\NLMatrixRouter.exe'
$guestFfmpeg   = 'C:\NLMatrixTools\ffmpeg.exe'
$guiLogGuest   = "$guestTemp\nlmatrix_qc_gui.log"
$readyGuest    = "$guestTemp\nlmatrix_qc_ready.txt"
$presetGuest   = "$guestTemp\nlmatrix_qc_presets"
$studioGuest   = "$guestQcDir\StudioTake1.json"
$captureGuest  = "$guestQcDir\qc_gui_capture.wav"
$modelGuest    = 'C:\NLMatrixTools\qc\gui_model.txt'
$buildDir      = Join-Path $script:Workspace 'app\build'
$hostUi        = Join-Path $buildDir 'NLMatrixUI_artefacts\Release\NLMatrixUI.exe'
$hostRouter    = Join-Path $buildDir 'NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe'

if (-not $OutputDir) {
    $OutputDir = Join-Path $script:Workspace ('reports\quickconnect\' + (Get-Date -Format 'yyyy-MM-dd') + '-gui')
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$logPath = Join-Path $OutputDir 'quickconnect_gui_host.log'
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

function Get-GuestSha256 {
    param([string]$GuestPath)
    $out = NL-Exec "if exist `"$GuestPath`" certutil -hashfile `"$GuestPath`" SHA256" -TimeoutSec 120
    $hex = ($out -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^[0-9a-fA-F]{64}$' } | Select-Object -First 1)
    if ($hex) { return $hex.ToLowerInvariant() }
    return ''
}

# ------------------------------------------------------------------ prerequisites
Assert-True (Test-Path $hostUi) "GUI binary missing: $hostUi"
Assert-True (Test-Path $hostRouter) "router binary missing: $hostRouter"
$uiHash = (Get-FileHash $hostUi -Algorithm SHA256).Hash.ToLowerInvariant()
$routerHash = (Get-FileHash $hostRouter -Algorithm SHA256).Hash.ToLowerInvariant()
Log "build GUI sha256=$uiHash"
Log "build router sha256=$routerHash"

# Guest ffmpeg (real third-party client), deployed only when the hash differs.
$hostFfmpegHash = (Get-FileHash $FfmpegHost -Algorithm SHA256).Hash.ToLowerInvariant()
$guestFfmpegHash = Get-GuestSha256 $guestFfmpeg
if ($guestFfmpegHash -ne $hostFfmpegHash) {
    Log 'deploying guest ffmpeg'
    NL-Exec "if not exist $guestTools mkdir $guestTools" | Out-Null
    NL-CopyTo $FfmpegHost $guestFfmpeg | Out-Null
    $guestFfmpegHash = Get-GuestSha256 $guestFfmpeg
}
Assert-True ($guestFfmpegHash -eq $hostFfmpegHash) 'guest ffmpeg hash'
Log "guest ffmpeg sha256=$guestFfmpegHash"

NL-Exec "if not exist $guestQcDir mkdir $guestQcDir" | Out-Null
NL-Exec "if not exist $guestRoot mkdir $guestRoot" | Out-Null

# Deploy the build snapshot and reset the test artefacts.
NL-Exec 'taskkill /F /IM NLMatrixUI.exe 2>nul & taskkill /F /IM NLMatrixRouter.exe 2>nul & echo CLEARED' -TimeoutSec 90 | Out-Null
NL-CopyTo $hostUi $guestGui | Out-Null
NL-CopyTo $hostRouter $guestRouter | Out-Null
Assert-True ((Get-GuestSha256 $guestGui) -eq $uiHash) 'guest GUI hash'
Assert-True ((Get-GuestSha256 $guestRouter) -eq $routerHash) 'guest router hash'
NL-Exec ("del /f /q `"$guiLogGuest`" `"$readyGuest`" `"$captureGuest`" 2>nul & rmdir /s /q `"$presetGuest`" 2>nul & echo CLEARED") -TimeoutSec 60 | Out-Null
Log 'guest snapshot deployed'

# A real Studio profile with a fixed identity: it must survive Quick Connect.
$studioProfile = [ordered]@{
    version = 3
    name = 'Studio Take 1'
    asio = $Device
    devices = @()
    buses = @()
    sources = @(
        [ordered]@{ id = 'st-src'; name = 'Studio ASIO in'; type = 'asio-in'; channels = @(0); enabled = $true }
    )
    destinations = @(
        [ordered]@{ id = 'st-dst'; name = 'Studio ASIO out'; type = 'asio-out'; channels = @(0); enabled = $true }
    )
    routes = @(
        [ordered]@{ source = 'st-src'; destination = 'st-dst'; gainDb = 0; enabled = $true }
    )
}
$studioLocal = Join-Path $env:TEMP 'nlmatrix_studio_take1.json'
Set-Content -Path $studioLocal -Value ($studioProfile | ConvertTo-Json -Depth 6) -Encoding UTF8
NL-CopyTo $studioLocal $studioGuest | Out-Null
Remove-Item $studioLocal -Force
$studioHashBefore = Get-GuestSha256 $studioGuest
Assert-True ($studioHashBefore -ne '') 'Studio profile fixture deployed'
Log "Studio profile fixture sha256=$studioHashBefore"

# ------------------------------------------------------- launch the real GUI action
$holdSeconds = $CaptureSeconds + 14
$guiArgs = @(
    '--quickconnecttest',
    '--qc-device', $Device,
    '--qc-channel', "$Channel",
    '--qc-endpoint', "$EndpointIndex",
    '--qc-test-tone',
    '--qc-hold', "$holdSeconds",
    '--qc-log', $guiLogGuest,
    '--qc-studio-profile', $studioGuest,
    '--qc-preset-dir', $presetGuest
)
$cred = NL-VmCred
Log ("launching GUI Quick Connect: channel=$Channel endpoint=NL Mic " + ($EndpointIndex + 1) + " hold=${holdSeconds}s")
& $script:Vmrun @cred runProgramInGuest $script:Vmx -noWait $guestGui @guiArgs 2>&1 | Out-Null
Assert-True ($LASTEXITCODE -eq 0) 'vmrun runProgramInGuest -noWait'

$ready = $false
for ($attempt = 0; $attempt -lt 12 -and -not $ready; $attempt++) {
    Start-Sleep -Seconds 1
    $out = NL-Exec "if exist `"$readyGuest`" (type `"$readyGuest`") else (echo WAITING)" -TimeoutSec 60
    if ($out -match 'READY engine=1') { $ready = $true }
    elseif ($out -match 'READY engine=0') { throw 'GUI Quick Connect reported the engine as not running' }
}
if (-not $ready) {
    $dump = NL-Exec "if exist `"$guiLogGuest`" type `"$guiLogGuest`" else echo NO_QC_LOG" -TimeoutSec 60
    Log ("readiness failed; GUI log:`n" + $dump)
    throw 'GUI Quick Connect did not reach engine Running'
}
Log 'GUI reports engine Running'

# ----------------------------------------------------------------- real client capture
$captureOut = NL-Exec (
    "del /q `"$captureGuest`" 2>nul & $guestFfmpeg -hide_banner -f dshow -i audio=`"$Endpoint`" " +
    "-t $CaptureSeconds -ac 1 -ar 48000 -y `"$captureGuest`" > $guestQcDir\ffmpeg_gui_client.log 2>&1") -TimeoutSec ($CaptureSeconds + 90)
$captureOut | Set-Content (Join-Path $OutputDir 'guest_ffmpeg.log') -Encoding UTF8

# Wait for the GUI to take its own real-exit path.
$exited = $false
for ($attempt = 0; $attempt -lt ($holdSeconds + 30) -and -not $exited; $attempt++) {
    $running = NL-Exec 'tasklist /FO CSV /NH 2>nul | findstr /I "NLMatrixUI.exe"' -TimeoutSec 60
    if (-not ($running -match 'NLMatrixUI\.exe')) { $exited = $true; break }
    Start-Sleep -Seconds 1
}
Log ("GUI process exited=" + $exited)

$wav = Join-Path $OutputDir 'quickconnect_gui_capture.wav'
NL-CopyFrom $captureGuest $wav | Out-Null
$json = Join-Path $OutputDir 'quickconnect_gui_capture.json'
& $PythonExe (Join-Path $script:Workspace 'tools\analyze_continuity.py') $wav --tone 1000 --expect-signal --json $json 2>&1 |
    Tee-Object -FilePath (Join-Path $OutputDir 'analysis.log') | Out-Null
$analysisExit = $LASTEXITCODE
$analysis = Get-Content $json -Raw | ConvertFrom-Json
$ch0 = $analysis.channels | Select-Object -First 1

$guiLog = NL-Exec "if exist `"$guiLogGuest`" type `"$guiLogGuest`" else echo NO_QC_LOG" -TimeoutSec 60
$guiLog | Set-Content (Join-Path $OutputDir 'nlmatrix_qc_gui.log') -Encoding UTF8
$presetLocal = Join-Path $OutputDir 'quick_connect_last.json'
NL-CopyFrom "$presetGuest\quick_connect_last.json" $presetLocal | Out-Null
$preset = Get-Content $presetLocal -Raw | ConvertFrom-Json
$presetAsio = "$($preset.asio)"
$presetChannel = -1
if ($preset.sources -and @($preset.sources).Count -gt 0) {
    $channels = @($preset.sources[0].channels)
    if ($channels.Count -gt 0) { $presetChannel = [int] $channels[0] }
}
$presetEndpoint = -1
if ($preset.destinations -and @($preset.destinations).Count -gt 0) {
    $presetEndpoint = [int] $preset.destinations[0].driverEndpoint
}
$studioHashAfter = Get-GuestSha256 $studioGuest

$guiPass = ([regex]::Matches($guiLog, '(?m)^PASS ')).Count
$guiFail = ([regex]::Matches($guiLog, '(?m)^FAIL ')).Count
$checks = [ordered]@{
    engineRunningFromGui = ($guiLog -match 'PASS engine running from the GUI Quick Connect action')
    projectModel         = ($guiLog -match 'PASS quick connect project model')
    presetWritten        = ($guiLog -match 'PASS quick connect preset written')
    presetCarriesChoice  = ($guiLog -match 'PASS quick connect preset carries the selected device and channel') `
                           -and ($presetAsio -eq $Device) `
                           -and ($presetChannel -eq ($Channel - 1)) `
                           -and ($presetEndpoint -eq $EndpointIndex)
    presetIsolated       = ($guiLog -match 'PASS quick connect preset isolated from Studio profiles')
    studioUntouched      = ($guiLog -match 'PASS studio profile untouched') -and ($studioHashBefore -eq $studioHashAfter)
    engineReleasedOnExit = ($guiLog -match 'PASS engine released on real application exit')
    guiFinished          = ($guiLog -match 'QC DONE')
    noGuiFailures        = ($guiFail -eq 0)
}
$guiModelLine = (($guiLog -split "`r?`n") | Where-Object { $_ -match '^MODEL ' } | Select-Object -First 1)

$summary = [ordered]@{
    suite             = 'guiquickconnect'
    gui               = 'NLMatrixUI.exe --quickconnecttest (production Quick Connect action)'
    guiSha256         = $uiHash
    routerSha256      = $routerHash
    device            = $Device
    channel           = $Channel
    endpoint          = "NL Mic " + ($EndpointIndex + 1)
    client            = 'ffmpeg 9.0.2 (DirectShow capture of the Windows endpoint)'
    clientSha256      = $guestFfmpegHash
    captureSeconds    = $CaptureSeconds
    guiChecks         = $checks
    guiPassCount      = $guiPass
    guiFailCount      = $guiFail
    guiModel          = $guiModelLine
    presetAsio        = $presetAsio
    presetChannel0    = $presetChannel
    presetEndpoint0   = $presetEndpoint
    studioProfileHash = $studioHashBefore
    studioProfileSame = ($studioHashBefore -eq $studioHashAfter)
    durationSec       = $analysis.audio.duration_s
    rmsDbfs           = $ch0.rms_dbfs
    peakDbfs          = $ch0.peak_dbfs
    tone              = $ch0.tone
    gaps              = $ch0.gaps
    audioVerdict      = $analysis.verdict.result
    reasons           = $analysis.verdict.reasons
    evidenceDir       = $OutputDir
}

$allChecks = ($checks.Values | Where-Object { -not $_ }).Count -eq 0
$pass = ($analysisExit -eq 0) -and ($analysis.verdict.result -eq 'PASS') -and $allChecks
$summary['verdict'] = if ($pass) { 'PASS' } else { 'FAIL' }
$summary | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutputDir 'quickconnect_gui_summary.json') -Encoding UTF8

Log ("GUI checks: pass=$guiPass fail=$guiFail " + (($checks.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '))
Log ("audio verdict={0} rms={1} dBFS tone=[{2}] gaps=[{3}]" -f $analysis.verdict.result, $ch0.rms_dbfs,
     ($ch0.tone | ConvertTo-Json -Compress -Depth 3), ($ch0.gaps | ConvertTo-Json -Compress -Depth 3))
Log "evidence: $OutputDir"
if ($pass) { Log 'VERDICT: PASS - the GUI Quick Connect action fed NL Mic 1 and a real client captured it' }
else { Log 'VERDICT: FAIL' }
if ($pass) { exit 0 } else { exit 1 }

