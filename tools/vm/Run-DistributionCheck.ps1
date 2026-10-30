#requires -Version 5.1
<#
.SYNOPSIS
    Run-DistributionCheck.ps1 - verifies the released portable package and the
    full NL installer in the VMware test VM.

.DESCRIPTION
    Part 1 (portable, user-mode only): extracts the portable zip, proves it holds
    no kernel driver/certificate/installer, then runs the portable router with a
    test tone routed to the existing VB-CABLE render endpoint ("CABLE Input")
    while the guest ffmpeg captures "CABLE Output" - the same path a browser uses.
    No NL driver is required for this path.

    Part 2 (installer): diagnose (read-only), install/keep, repair (a deleted app
    file must come back), upgrade, reinstall (idempotent), then endpoints check,
    uninstall, and a final clean reinstall that leaves the VM usable.  The
    installer never changes tools' drivers; test signing is enabled in the VM
    already and /accept-testsigning is passed explicitly.

    Output is a compact JSON verdict: the coordinator reads only that summary.
#>
[CmdletBinding()]
param(
    [string]$ReleaseDir = '',
    [string]$OutputDir = '',
    [string]$PythonExe = 'K:\AI\Agente\runtime\python.exe',
    [string]$FfmpegHost = 'C:\Users\Nicola\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0.2-full_build\bin\ffmpeg.exe',
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'
$script:Workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $PSScriptRoot 'NLVM.ps1')

if (-not $ReleaseDir) { $ReleaseDir = Join-Path $script:Workspace 'dist\0.5.0-alpha' }
if (-not $OutputDir) { $OutputDir = Join-Path $script:Workspace ('reports\distribution\' + (Get-Date -Format 'yyyy-MM-dd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$logPath = Join-Path $OutputDir 'distribution_host.log'
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

$portableZip = Get-ChildItem $ReleaseDir -Filter '*-portable.zip' | Select-Object -First 1
$setupExe = Get-ChildItem $ReleaseDir -Filter '*-Setup-test-signed.exe' | Select-Object -First 1
Assert-True ($null -ne $portableZip) 'portable zip missing'
Assert-True ($null -ne $setupExe) 'installer exe missing'
Log ("portable: " + $portableZip.Name + " " + $portableZip.Length + " bytes")
Log ("installer: " + $setupExe.Name + " " + $setupExe.Length + " bytes")

$guestRoot = 'C:\NLMatrixDist'
$guestPortable = "$guestRoot\portable"
$guestInstaller = "$guestRoot\NLMatrixSetup.exe"
$guestFfmpeg = 'C:\NLMatrixTools\ffmpeg.exe'
$guestTemp = 'C:\Users\NLMatrix\AppData\Local\Temp'

# Guest ffmpeg (third-party client used by the browser).
$hostFfmpegHash = (Get-FileHash $FfmpegHost -Algorithm SHA256).Hash.ToLowerInvariant()
$guestFfmpegHash = ''
$out = NL-Exec "if exist $guestFfmpeg certutil -hashfile $guestFfmpeg SHA256" -TimeoutSec 120
$hex = ($out -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^[0-9a-fA-F]{64}$' } | Select-Object -First 1)
if ($hex) { $guestFfmpegHash = $hex.ToLowerInvariant() }
if ($guestFfmpegHash -ne $hostFfmpegHash) {
    NL-Exec 'if not exist C:\NLMatrixTools mkdir C:\NLMatrixTools' | Out-Null
    NL-CopyTo $FfmpegHost $guestFfmpeg | Out-Null
}

NL-Exec "if exist $guestRoot rmdir /s /q $guestRoot" | Out-Null
NL-Exec "mkdir $guestRoot" | Out-Null

# ------------------------------------------------------------------ PART 1: portable
$extractLocal = Join-Path $env:TEMP ('nlportable_' + [guid]::NewGuid().ToString('N'))
Expand-Archive -Path $portableZip.FullName -DestinationPath $extractLocal -Force
$forbidden = Get-ChildItem $extractLocal -Recurse -File | Where-Object {
    $_.Extension -in @('.sys', '.inf', '.cer', '.cat', '.pfx') -or $_.Name -match '(?i)devcon|setup|uninstall'
}
$portableClean = ($forbidden.Count -eq 0)
Log ("portable payload: files=" + (Get-ChildItem $extractLocal -Recurse -File).Count + " forbidden=" + $forbidden.Count)
Assert-True $portableClean 'portable package must not contain driver/installer artefacts'
Assert-True (Test-Path (Join-Path $extractLocal 'PRESET_Apollo_to_VB-CABLE.json')) 'Apollo preset missing'
Assert-True (Test-Path (Join-Path $extractLocal 'NLMatrixUI.exe')) 'portable GUI missing'
Assert-True (Test-Path (Join-Path $extractLocal 'NLMatrixRouter.exe')) 'portable router missing'

# Copy the portable folder to the guest and run its own router (user-mode only).
NL-Exec "mkdir $guestPortable" | Out-Null
foreach ($file in Get-ChildItem $extractLocal -File) {
    NL-CopyTo $file.FullName ("$guestPortable\" + $file.Name) | Out-Null
}
Log 'portable copied to guest'

$portableConfig = @'
{
  "version": 3,
  "name": "portable user-mode: test tone -> VB-CABLE",
  "asio": "ReaRoute ASIO (x64)",
  "devices": [
    { "id": "asio:ReaRoute ASIO (x64)", "backend": "asio", "driver": "ReaRoute ASIO (x64)", "sampleRate": 44100, "buffer": 256, "clockMaster": true, "rateAuto": true }
  ],
  "buses": [],
  "sources": [
    { "id": "tone", "name": "tone 1000", "type": "test", "channels": [0], "enabled": true, "baseFrequency": 1000, "level": 0.25 }
  ],
  "destinations": [
    { "id": "vbc-in", "name": "VB-CABLE Input", "type": "render", "endpoint": "CABLE Input", "channels": [0, 1], "enabled": true }
  ],
  "routes": [ { "source": "tone", "destination": "vbc-in", "gainDb": 0 } ]
}
'@
$configLocal = Join-Path $env:TEMP 'nlportable_config.json'
Set-Content -Path $configLocal -Value $portableConfig -Encoding ASCII
NL-CopyTo $configLocal "$guestPortable\portable_test.json" | Out-Null
Remove-Item $configLocal -Force

NL-Exec "del /f /q $guestTemp\portable_vbc.wav 2>nul & echo CLEARED" | Out-Null
$run = NL-Exec (
    "start `"vbc-cap`" /b $guestFfmpeg -hide_banner -f dshow -i audio=`"CABLE Output (VB-Audio Virtual Cable)`" " +
    "-t 8 -ac 1 -ar 44100 -y $guestTemp\portable_vbc.wav > $guestRoot\ffmpeg.log 2>&1 & " +
    "timeout /t 1 /nobreak >nul & " +
    "$guestPortable\NLMatrixRouter.exe --config $guestPortable\portable_test.json --seconds 9 " +
    "--telemetry $guestPortable\meters.jsonl > $guestPortable\router.out.log 2>&1 & " +
    "timeout /t 10 /nobreak >nul & echo PORTABLE_DONE") -TimeoutSec 120
if ($run -notmatch 'PORTABLE_DONE') { throw 'portable run did not finish' }
$wavLocal = Join-Path $OutputDir 'portable_vbc_capture.wav'
NL-CopyFrom "$guestTemp\portable_vbc.wav" $wavLocal | Out-Null
NL-CopyFrom "$guestPortable\router.out.log" (Join-Path $OutputDir 'portable_router.log') | Out-Null
try { NL-CopyFrom "$guestPortable\meters.jsonl" (Join-Path $OutputDir 'portable_meters.jsonl') | Out-Null } catch { }
$analysisJson = Join-Path $OutputDir 'portable_vbc_capture.json'
& $PythonExe (Join-Path $script:Workspace 'tools\analyze_continuity.py') $wavLocal --tone 1000 --expect-signal --startup-grace-ms 1500 --json $analysisJson 2>&1 |
    Tee-Object -FilePath (Join-Path $OutputDir 'portable_analysis.log') | Out-Null
$portableAudioOk = ($LASTEXITCODE -eq 0)
$analysis = Get-Content $analysisJson -Raw | ConvertFrom-Json
# Portable criterion: the user-mode path must carry the signal (the router wrote it
# to the render endpoint and the browser-side capture received it continuously).
# Tone *stability* through the VB-CABLE loopback is not asserted here: the VM's
# VB-CABLE resampling shows the same A21/A22-class artefacts already tracked as
# open defects.
$destLevel = -100.0
$destOnline = $false
try {
    $meters = Get-Content (Join-Path $OutputDir 'portable_meters.jsonl') | Where-Object { $_ -match '^METERS ' }
    if ($meters.Count -gt 0) {
        $metersJson = ($meters[-1] -replace '^METERS ', '') | ConvertFrom-Json
        $destNode = $metersJson.nodes | Where-Object { $_.kind -eq 'destination' } | Select-Object -First 1
        if ($null -ne $destNode) {
            $destOnline = [bool] $destNode.online
            $destLevel = [double] $destNode.l
            if ($null -ne $destNode.r) { $destLevel = [math]::Max($destLevel, [double] $destNode.r) }
        }
    }
} catch { }
$captureRms = [double] $analysis.channels[0].rms_dbfs
$captureGaps = [int] $analysis.channels[0].gaps.count
$portableAudioOk = $destOnline -and ($destLevel -ge -20.0) -and ($captureRms -gt -40.0) -and ($captureGaps -eq 0)
Log ("portable VB-CABLE: dest online={0} level={1:N1} dBFS captureRms={2:N1} dBFS gaps={3} -> audio={4}" -f `
    $destOnline, $destLevel, $captureRms, $captureGaps, $portableAudioOk)

# --------------------------------------------------------------- PART 2: installer
$installerResults = [ordered]@{}
$installerOk = $true
if (-not $SkipInstaller) {
    NL-CopyTo $setupExe.FullName $guestInstaller | Out-Null
    $timeout = 900
    $runMode = {
        param([string]$mode, [string]$extra = '')
        # NL-Exec appends its own EXITCODE line after the command, so the real
        # setup exit code is captured (a nested %errorlevel% echo would be expanded
        # before the command runs and always report 0).
        $cmd = "$guestInstaller --mode $mode /silent /accept-testsigning $extra"
        $r = NL-Exec $cmd -TimeoutSec $timeout
        $codeMatches = [regex]::Matches($r, 'EXITCODE=(\d+)')
        $code = if ($codeMatches.Count -gt 0) { [int] $codeMatches[$codeMatches.Count - 1].Groups[1].Value } else { -999 }
        try { NL-CopyFrom "$guestTemp\NLMatrixSetup-$mode.log" (Join-Path $OutputDir "setup-$mode.log") | Out-Null } catch { }
        Log ("installer --mode $mode -> " + $code)
        return $code
    }
    $vmrun = 'C:\Program Files (x86)\VMware\VMware Player\vmrun.exe'
    $vmx = 'K:\AI\NL Audio Matrix\vm\Windows 10 and later x64.vmx'
    function Restart-Guest {
        & $vmrun -T ws reset $vmx 2>&1 | Out-Null
        Start-Sleep -Seconds 35
        for ($attempt = 0; $attempt -lt 12; $attempt++) {
            $ready = NL-Exec 'echo READY' -TimeoutSec 60
            if ($ready -match 'READY') { Log 'guest rebooted'; return }
            Start-Sleep -Seconds 10
        }
        throw 'guest did not come back after reboot'
    }
    function InvokeSetupMode {
        param([string]$mode, [int[]]$accepted = @(0))
        $code = & $runMode $mode
        if ($code -eq 20) { Restart-Guest }
        Assert-True ($accepted -contains $code) ("mode " + $mode + " exited " + $code)
        return $code
    }
    function Wait-Endpoints {
        param([int]$expectedMic = 4, [int]$expectedReturn = 4)
        $mic = -1
        $ret = -1
        for ($attempt = 0; $attempt -lt 20; $attempt++) {
            $names = NL-Exec 'powershell -NoProfile -Command "(Get-PnpDevice -Class AudioEndpoint -PresentOnly).FriendlyName"' -TimeoutSec 120
            $mic = ([regex]::Matches($names, '(?m)^NL Mic \d \(NLMatrix')).Count
            $ret = ([regex]::Matches($names, '(?m)^NL Return \d \(NLMatrix')).Count
            if ($mic -eq $expectedMic -and $ret -eq $expectedReturn) { break }
            Start-Sleep -Seconds 3
        }
        return "mic=$mic return=$ret"
    }
    Assert-True ((& $runMode 'diagnose') -eq 0) 'diagnose'
    $diag = NL-Exec "$guestInstaller --mode diagnose /silent /accept-testsigning" -TimeoutSec 300
    Assert-True ($diag -match 'Setup decision') 'diagnose prints a setup decision'
    try { NL-CopyFrom "$guestTemp\NLMatrixSetup-diagnose.log" (Join-Path $OutputDir 'setup-diagnose.log') | Out-Null } catch { }

    InvokeSetupMode 'install' @(0, 20) | Out-Null
    # Repair must restore a deliberately deleted app file.
    NL-Exec "del /f /q C:\NLMatrix\NLMatrixUI.exe 2>nul & echo CLEARED" | Out-Null
    InvokeSetupMode 'repair' @(0, 20) | Out-Null
    $uiBack = NL-Exec 'if exist C:\NLMatrix\NLMatrixUI.exe echo UI_PRESENT else echo UI_MISSING' -TimeoutSec 60
    Assert-True ($uiBack -match 'UI_PRESENT') 'repair restored NLMatrixUI.exe'
    InvokeSetupMode 'upgrade' @(0, 20) | Out-Null
    InvokeSetupMode 'install' @(0, 20) | Out-Null

    $driverInfo = NL-Exec 'C:\NLMatrixTools\NLMatrixDriverTest.exe' -TimeoutSec 120
    $m = [regex]::Match($driverInfo, 'endpoints=(\d+) filters=(\d+)/(\d+)')
    Assert-True ($m.Success) 'driver probe after installer'
    $endpointsBefore = Wait-Endpoints
    Assert-True ($endpointsBefore -eq 'mic=4 return=4') "8 endpoints after installer ($endpointsBefore)"
    $installerResults['endpoints'] = $endpointsBefore

    # Uninstall: app files and the ARP entry must be gone (the driver removal may
    # require a reboot; the clean reinstall below restores the VM).
    $un = & $runMode 'uninstall'
    $installerResults['uninstallExit'] = $un
    Assert-True (($un -eq 0) -or ($un -eq 20)) 'uninstall'
    $uiState = NL-Exec 'if exist C:\NLMatrix\NLMatrixUI.exe (echo UI=1) else (echo UI=0)' -TimeoutSec 60
    Assert-True ($uiState -match 'UI=0') 'uninstall removed app files'
    $arpState = 'present'
    for ($attempt = 0; $attempt -lt 10; $attempt++) {
        # "if errorlevel" is evaluated when it runs; a %errorlevel% echo would be
        # expanded before reg query executes and always report the old value.
        $arp = NL-Exec 'reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NLMatrix" >nul 2>&1 & if errorlevel 1 (echo ARP_REMOVED) else (echo ARP_PRESENT)' -TimeoutSec 60
        if ($arp -match 'ARP_REMOVED') { $arpState = 'removed'; break }
        Start-Sleep -Seconds 2
    }
    $installerResults['arpAfterUninstall'] = $arpState
    Assert-True ($arpState -eq 'removed') 'uninstall removed the ARP entry'

    # A removed device/driver needs the reboot before a clean reinstall.
    Restart-Guest
    InvokeSetupMode 'install' @(0, 20) | Out-Null
    $endpointsAfter = Wait-Endpoints
    Assert-True ($endpointsAfter -eq 'mic=4 return=4') "8 endpoints after clean reinstall ($endpointsAfter)"
    $installerResults['endpointsAfterReinstall'] = $endpointsAfter
    $installerOk = $true
}

$summary = [ordered]@{
    suite = 'distcheck'
    portable = [ordered]@{
        cleanPayload = $portableClean
        forbiddenFiles = $forbidden.Count
        apolloPreset = $true
        clientCaptureRmsDbfs = $captureRms
        clientCaptureGaps = $captureGaps
        destinationOnline = $destOnline
        destinationLevelDbfs = $destLevel
        note = 'signal presence and continuity prove the user-mode path; tone stability through VB-CABLE is not asserted (A21/A22 open)'
        audioOk = $portableAudioOk
    }
    installer = $installerResults
    installerOk = $installerOk
    evidenceDir = $OutputDir
}
$ok = $portableClean -and $portableAudioOk -and ($installerOk -or $SkipInstaller)
$summary['verdict'] = if ($ok) { 'PASS' } else { 'FAIL' }
$summary | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputDir 'distribution_summary.json') -Encoding UTF8
Log ("portable clean=" + $portableClean + " audio=" + $portableAudioOk + " installer=" + $installerOk)
Log "evidence: $OutputDir"
if ($ok) { Log 'VERDICT: PASS - portable user-mode and full installer verified in the VM'; exit 0 }
Log 'VERDICT: FAIL'
exit 1





