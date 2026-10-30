#requires -Version 5.1
<#
.SYNOPSIS
    Run-SetupLifecycleCheck.ps1 - verifies the complete NL setup package in the
    VMware test VM: setup GUI, diagnose/verify reports, install, repair, upgrade,
    reinstall, uninstall, reboot handling, post-reboot verification and the eight
    NL endpoints.

.DESCRIPTION
    The coordinator reads only the JSON verdict written to -OutputDir.

    What is proven, in order:
      1. host-side release hygiene: no private key material, no credentials in
         the release text assets;
      2. --mode diagnose: read-only report with driver/PnP identity, eight
         endpoints, Secure Boot/TESTSIGNING/HVCI and the setup decision;
      3. the wizard starts and shows its window (screenshot evidence);
      4. --mode verify /app-selftest: app starts, shows its window and exits
         normally (--exit-on-close), driver/device/endpoints healthy;
      5. install / repair (a deleted file must come back) / upgrade / repeated
         install: exactly one device instance and one NL driver package, profiles
         preserved;
      6. uninstall: app, ARP, Start Menu, certificate, package, device and
         endpoints removed; reboot; verify --expect-absent is clean;
      7. clean reinstall with /register-post-reboot-verify: RunOnce entry written,
         reboot, the asInvoker verifier runs at logon, writes a healthy report and
         clears the pending marker; eight endpoints return;
      8. final verify with the app start/stop self-test.

    Nothing here installs a driver on the host: every driver operation happens in
    the VM.  A21/A22 remain open audio defects; this suite does not re-run audio
    transport proofs.
#>
[CmdletBinding()]
param(
    [string]$ReleaseDir = '',
    [string]$OutputDir = '',
    [string]$SetupExe = ''
)

$ErrorActionPreference = 'Stop'
$script:Workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $PSScriptRoot 'NLVM.ps1')

if (-not $ReleaseDir) { $ReleaseDir = Join-Path $script:Workspace 'dist\0.5.0-alpha' }
if (-not $OutputDir) { $OutputDir = Join-Path $script:Workspace ('reports\setupcheck\' + (Get-Date -Format 'yyyy-MM-dd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$logPath = Join-Path $OutputDir 'setupcheck_host.log'
if (Test-Path $logPath) { Remove-Item $logPath -Force }

$checks = New-Object System.Collections.Generic.List[object]
$script:Failed = 0

function Log {
    param([string]$Message)
    $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message)
    Write-Host $line
    Add-Content -Path $logPath -Value $line -Encoding UTF8
}

function Check {
    param([string]$Name, [bool]$Ok, [string]$Detail = '')
    $status = if ($Ok) { 'PASS' } else { 'FAIL' }
    Log ("$status $Name" + $(if ($Detail) { " - $Detail" } else { '' }))
    $checks.Add([ordered]@{ name = $Name; ok = $Ok; detail = $Detail })
    if (-not $Ok) { $script:Failed++ }
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "ASSERT FAILED: $Message" }
}

# ----------------------------------------------------------------- release input
if (-not $SetupExe) {
    $candidate = Get-ChildItem $ReleaseDir -Filter '*-Setup-test-signed.exe' | Select-Object -First 1
    if ($null -eq $candidate) { throw "setup executable not found in $ReleaseDir" }
    $SetupExe = $candidate.FullName
}
Assert-True (Test-Path $SetupExe) "setup executable missing: $SetupExe"
$setupHash = (Get-FileHash $SetupExe -Algorithm SHA256).Hash.ToLowerInvariant()
Log ("setup: $SetupExe")
Log ("sha256: $setupHash")

$guestRoot = 'C:\NLMatrixDist'
$guestInstaller = "$guestRoot\NLMatrixSetup.exe"
$guestTemp = 'C:\Users\NLMatrix\AppData\Local\Temp'
$guestProgramData = 'C:\ProgramData\NL Audio Matrix'
$guestReports = "$guestProgramData\reports"

# ------------------------------------------------------- 1. release hygiene (host)
$releaseFiles = Get-ChildItem $ReleaseDir -Recurse -File
$signingMaterial = $releaseFiles | Where-Object { $_.Extension -in @('.pfx', '.pvk', '.snk', '.key') }
Check 'release contains no signing private key' ($signingMaterial.Count -eq 0) ("files=" + $releaseFiles.Count)

$tokenPattern = 'ghp_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|gho_[A-Za-z0-9]{20,}|ghs_[A-Za-z0-9]{20,}'
$credentialHits = @()
foreach ($file in $releaseFiles) {
    if ($file.Length -gt 4MB) { continue }
    if ($file.Extension -notin @('.txt', '.md', '.json', '.yml', '.yaml', '.ps1', '.cs', '.log', '.exe')) { continue }
    try {
        $text = Get-Content $file.FullName -Raw -ErrorAction Stop
        if ($text -match $tokenPattern) { $credentialHits += $file.Name }
    } catch { }
}
Check 'release assets contain no GitHub token pattern' ($credentialHits.Count -eq 0) ("hits=" + $credentialHits.Count)

# ----------------------------------------------------------- guest setup + helper
NL-Exec "if exist $guestRoot rmdir /s /q $guestRoot" | Out-Null
NL-Exec "mkdir $guestRoot" | Out-Null
$guestHash = NL-CopyTo $SetupExe $guestInstaller
Check 'setup copied to the VM with matching SHA256' ($guestHash -eq $setupHash) $guestHash

function Invoke-GuestPowerShell {
    param([string]$ScriptText, [int]$TimeoutSec = 180)
    $name = 'setupcheck_' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $local = Join-Path $env:TEMP ($name + '.ps1')
    Set-Content -Path $local -Value $ScriptText -Encoding ASCII
    $guest = "C:\NLMatrixTools\$name.ps1"
    NL-CopyTo $local $guest | Out-Null
    $out = NL-Exec "powershell -NoProfile -ExecutionPolicy Bypass -File $guest" -TimeoutSec $TimeoutSec
    Remove-Item $local -Force -ErrorAction SilentlyContinue
    return $out
}

$stateScript = @'
$packages = @(Get-ChildItem "$env:WINDIR\INF" -Filter 'oem*.inf' -ErrorAction SilentlyContinue | Where-Object {
    try { (Get-Content $_.FullName -Raw -ErrorAction Stop) -match 'Root\\NLMatrixVirtualAudio|nlmatrixaudio\.sys' } catch { $false }
})
$device = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -like 'NLMatrix Virtual Audio Device*' })
$endpoints = @(Get-PnpDevice -Class AudioEndpoint -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -match '^NL (Mic|Return) \d' })
$certRoot = @(Get-ChildItem Cert:\LocalMachine\Root -ErrorAction SilentlyContinue | Where-Object { $_.Subject -match 'CN=NLMatrix Test Driver' })
$certPublisher = @(Get-ChildItem Cert:\LocalMachine\TrustedPublisher -ErrorAction SilentlyContinue | Where-Object { $_.Subject -match 'CN=NLMatrix Test Driver' })
$arp = $null
try { $arp = [string](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NLMatrix' -ErrorAction Stop).DisplayVersion } catch { }
$startMenu = Test-Path "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\NL Audio Matrix"
$app = Test-Path 'C:\NLMatrix\NLMatrixUI.exe'
$verify = Test-Path 'C:\NLMatrix\NLMatrixVerify.exe'
$profileMarker = Test-Path "$env:APPDATA\NL Audio Matrix\setupcheck-marker.txt"
$runOnce = $null
try { $runOnce = [string](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce' -Name NLMatrixVerifyAfterReboot -ErrorAction Stop).NLMatrixVerifyAfterReboot } catch { }
$pendingMarker = Test-Path 'C:\ProgramData\NL Audio Matrix\pending-reboot.txt'
$reports = 'C:\ProgramData\NL Audio Matrix\reports'
$lastVerify = Test-Path (Join-Path $reports 'last-verify.json')
$lastDiagnose = Test-Path (Join-Path $reports 'last-diagnose.json')
$state = [ordered]@{
    packages = $packages.Count
    devices = $device.Count
    deviceName = if ($device.Count -gt 0) { $device[0].FriendlyName } else { '' }
    endpoints = $endpoints.Count
    certRoot = $certRoot.Count
    certPublisher = $certPublisher.Count
    arp = $arp
    startMenu = $startMenu
    app = $app
    verifyExe = $verify
    profileMarker = $profileMarker
    runOnce = $runOnce
    pendingMarker = $pendingMarker
    lastVerify = $lastVerify
    lastDiagnose = $lastDiagnose
}
'SETUPCHECK_STATE=' + ($state | ConvertTo-Json -Compress)
'@

function Get-GuestState {
    $text = Invoke-GuestPowerShell $stateScript
    $match = [regex]::Match($text, 'SETUPCHECK_STATE=(\{.*\})')
    if (-not $match.Success) { throw "guest state could not be read: $text" }
    return ($match.Groups[1].Value | ConvertFrom-Json)
}

function RunSetupMode {
    param([string]$Mode, [string]$Extra = '', [int]$TimeoutSec = 900)
    $command = "$guestInstaller --mode $Mode /silent /accept-testsigning $Extra"
    $result = NL-Exec $command -TimeoutSec $TimeoutSec
    $codes = [regex]::Matches($result, 'EXITCODE=(\d+)')
    $code = if ($codes.Count -gt 0) { [int] $codes[$codes.Count - 1].Groups[1].Value } else { -999 }
    try { NL-CopyFrom "$guestTemp\NLMatrixSetup-$Mode.log" (Join-Path $OutputDir "setup-$Mode.log") | Out-Null } catch { }
    Log ("--mode $Mode -> $code")
    return [int]$code
}

function Restart-Guest {
    $vmrun = 'C:\Program Files (x86)\VMware\VMware Player\vmrun.exe'
    $vmx = 'K:\AI\NL Audio Matrix\vm\Windows 10 and later x64.vmx'
    & $vmrun -T ws reset $vmx 2>&1 | Out-Null
    Start-Sleep -Seconds 35
    for ($attempt = 0; $attempt -lt 15; $attempt++) {
        $ready = NL-Exec 'echo READY' -TimeoutSec 60
        if ($ready -match 'READY') { Log 'guest rebooted'; return }
        Start-Sleep -Seconds 10
    }
    throw 'guest did not come back after reboot'
}

function Copy-GuestReport {
    param([string]$Name, [string]$Local)
    try { NL-CopyFrom "$guestReports\$Name" $Local | Out-Null; return $true } catch { return $false }
}

# ------------------------------------------------------ initial state snapshot
$state = Get-GuestState
Log ('initial state: ' + ($state | ConvertTo-Json -Compress))
$initiallyInstalled = ($state.app -eq $true)

# ------------------------------------------------- 2. read-only diagnose report
$diagnoseCode = RunSetupMode 'diagnose'
Check 'diagnose exits 0' ($diagnoseCode -eq 0) ("exit=" + $diagnoseCode)
$diagnoseCopied = Copy-GuestReport 'last-diagnose.json' (Join-Path $OutputDir 'last-diagnose.json')
Check 'diagnose writes a JSON report' $diagnoseCopied
if ($diagnoseCopied) {
    $diag = Get-Content (Join-Path $OutputDir 'last-diagnose.json') -Raw | ConvertFrom-Json
    Check 'diagnose reports 8 active endpoints' ($diag.endpoints.active -eq 8 -and $diag.endpoints.activeCapture -eq 4 -and $diag.endpoints.activeRender -eq 4) ($diag.endpoints.active)
    Check 'diagnose identifies the loadable driver (bound version + PnP instance)' ([bool]($diag.driver.boundVersion) -and [bool]($diag.device.instanceId)) ($diag.device.instanceId)
    Check 'diagnose finds no Code Integrity rejection' ($diag.codeIntegrity.driverEvents -eq 0) ("events=" + $diag.codeIntegrity.driverEvents)
    Check 'diagnose security verdict is ok (test signing / Secure Boot / HVCI)' ($diag.security.verdict -eq 'ok') ($diag.security.verdict)
    Check 'diagnose shows the loaded driver file version' ([bool]($diag.driver.loadedFileVersion)) ($diag.driver.loadedFileVersion)
    Check 'diagnose computes an install decision' ([bool]($diag.decision)) ($diag.decision)
    $diagnoseText = Get-Content (Join-Path $OutputDir 'setup-diagnose.log') -Raw
    Check 'diagnose text includes security and PnP evidence' ($diagnoseText -match 'Secure Boot' -and $diagnoseText -match 'HVCI' -and $diagnoseText -match 'PnP instance')
}

# --------------------------------------------------------- 3. wizard GUI smoke
$vmrun = 'C:\Program Files (x86)\VMware\VMware Player\vmrun.exe'
$vmx = 'K:\AI\NL Audio Matrix\vm\Windows 10 and later x64.vmx'
NL-Exec 'taskkill /F /IM NLMatrixSetup.exe' -TimeoutSec 60 | Out-Null
Start-Sleep -Seconds 2
# A requireAdministrator GUI started from vmrun lives on session 0 (invisible) and
# its window cannot be inspected cross-session.  Both the wizard and a small probe
# run as interactive tasks (/it), so the probe can read the real window title and
# the screenshot captures the actual desktop.
$probeScript = @'
$out = 'C:\NLMatrixTools\gui_title.txt'
Remove-Item $out -ErrorAction SilentlyContinue
$deadline = (Get-Date).AddSeconds(45)
while ((Get-Date) -lt $deadline) {
    $p = Get-Process NLMatrixSetup -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $p) {
        $p.Refresh()
        if ($p.MainWindowHandle -ne 0 -and $p.MainWindowTitle) {
            Set-Content -Path $out -Value ('TITLE=' + $p.MainWindowTitle) -Encoding ASCII
            exit 0
        }
    }
    Start-Sleep -Milliseconds 500
}
Set-Content -Path $out -Value 'TITLE=none' -Encoding ASCII
'@
$probeLocal = Join-Path $env:TEMP 'nl_gui_probe.ps1'
Set-Content -Path $probeLocal -Value $probeScript -Encoding ASCII
NL-CopyTo $probeLocal 'C:\NLMatrixTools\gui_probe_win.ps1' | Out-Null
Remove-Item $probeLocal -Force -ErrorAction SilentlyContinue

$guiScript = @'
schtasks /delete /f /tn NLSetupGuiProbe 2>$null | Out-Null
schtasks /delete /f /tn NLSetupGuiTest 2>$null | Out-Null
schtasks /create /f /tn NLSetupGuiProbe /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\NLMatrixTools\gui_probe_win.ps1" /sc once /st 23:59 /rl highest /it | Out-Null
schtasks /run /tn NLSetupGuiProbe | Out-Null
Start-Sleep -Seconds 2
schtasks /create /f /tn NLSetupGuiTest /tr "C:\NLMatrixDist\NLMatrixSetup.exe" /sc once /st 23:59 /rl highest /it | Out-Null
schtasks /run /tn NLSetupGuiTest | Out-Null
'@
$guiLocal = Join-Path $env:TEMP 'nl_gui_start.ps1'
Set-Content -Path $guiLocal -Value $guiScript -Encoding ASCII
NL-CopyTo $guiLocal 'C:\NLMatrixTools\gui_start.ps1' | Out-Null
Remove-Item $guiLocal -Force -ErrorAction SilentlyContinue
NL-Exec 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\NLMatrixTools\gui_start.ps1' -TimeoutSec 120 | Out-Null

$windowTitle = ''
for ($attempt = 0; $attempt -lt 20; $attempt++) {
    Start-Sleep -Seconds 2
    $titleText = NL-Exec 'if exist C:\NLMatrixTools\gui_title.txt type C:\NLMatrixTools\gui_title.txt' -TimeoutSec 60
    $titleMatch = [regex]::Match($titleText, 'TITLE=(.*)')
    if ($titleMatch.Success) { $windowTitle = $titleMatch.Groups[1].Value.Trim() }
    if ($windowTitle -and $windowTitle -ne 'none') { break }
}
Check 'setup wizard starts and shows its window' ($windowTitle -like 'NL Audio Matrix*') $windowTitle
$screenshot = Join-Path $OutputDir 'setup-wizard.png'
try { $cred = NL-VmCred; & $vmrun @cred captureScreen $vmx $screenshot 2>&1 | Out-Null } catch { }
Check 'wizard screenshot captured' (Test-Path $screenshot) $screenshot
if (-not ($windowTitle -like 'NL Audio Matrix*')) {
    # Keep the reason: a .NET crash or a missing payload leaves an Event Log entry.
    $crashScript = @'
try {
    Get-WinEvent -LogName Application -MaxEvents 40 -ErrorAction Stop |
        Where-Object { $_.ProviderName -match 'Application Error|\.NET Runtime' -and $_.Message -match 'NLMatrixSetup' } |
        Select-Object -First 3 |
        ForEach-Object { 'CRASH=' + ($_.Message -replace "`r?`n", ' ') }
} catch { 'CRASH=none' }
'@
    $crashText = Invoke-GuestPowerShell $crashScript -TimeoutSec 120
    Add-Content -Path $logPath -Value $crashText -Encoding UTF8
}
NL-Exec 'taskkill /F /IM NLMatrixSetup.exe 2>nul & schtasks /delete /f /tn NLSetupGuiTest 2>nul & schtasks /delete /f /tn NLSetupGuiProbe 2>nul & echo CLEANED' | Out-Null
Start-Sleep -Seconds 2

# ------------------------------------------------- 4. verify with app selftest
# The app start/stop self-test needs the current app build (installed by this
# package); on a machine that still has an older build run verify without it and
# prove start/stop after the reinstall below.
$initialState = Get-GuestState
$appSelfTestAvailable = ($initialState.verifyExe -eq $true)
$verifyExtra = if ($appSelfTestAvailable) { '/app-selftest' } else { '' }
$verifyCode = RunSetupMode 'verify' $verifyExtra
Check 'verify exits 0' ($verifyCode -eq 0) ("exit=" + $verifyCode)
$verifyCopied = Copy-GuestReport 'last-verify.json' (Join-Path $OutputDir 'last-verify.json')
Check 'verify writes a JSON report' $verifyCopied
if ($verifyCopied) {
    $verify = Get-Content (Join-Path $OutputDir 'last-verify.json') -Raw | ConvertFrom-Json
    Check 'verify: driver running, device present and 8 endpoints active' `
        ($verify.driver.serviceRunning -eq $true -and $verify.device.present -eq $true -and $verify.endpoints.active -eq 8) `
        ("endpoints=" + $verify.endpoints.active)
    if ($appSelfTestAvailable) {
        Check 'verify: application started and stopped normally' ($verify.appSelfTest.ran -eq $true -and $verify.appSelfTest.ok -eq $true) ($verify.appSelfTest.summary)
    } else {
        Log 'app start/stop self-test deferred: the installed build predates NLMatrixVerify.exe'
    }
}

# --------------------------------------------------------- profiles + marker
$profileScript = @'
New-Item -ItemType Directory -Force "$env:APPDATA\NL Audio Matrix\quickconnect" | Out-Null
Set-Content -Path "$env:APPDATA\NL Audio Matrix\quickconnect\setupcheck-preset.json" -Value '{"name":"setupcheck"}' -Encoding ASCII
Set-Content -Path "$env:APPDATA\NL Audio Matrix\setupcheck-marker.txt" -Value 'keep-me' -Encoding ASCII
'PROFILES_READY'
'@
$profileText = Invoke-GuestPowerShell $profileScript
Assert-True ($profileText -match 'PROFILES_READY') 'could not create the profile marker files'

# --------------------------------------------------------- 5. install/repair/upgrade
# Always run install once: it brings an existing installation to this package
# (deploying the new verifier) and is idempotent when nothing is missing.
$initialState = Get-GuestState
$installCode = RunSetupMode 'install'
if ($installCode -eq 20) { Restart-Guest }
Check 'install/update to this package succeeds' ($installCode -eq 0 -or $installCode -eq 20) ("exit=" + $installCode)
$state = Get-GuestState
Check 'install: one NLMatrix device instance' ($state.devices -eq 1) ("devices=" + $state.devices)
Check 'install: one NL driver package in the DriverStore' ($state.packages -eq 1) ("packages=" + $state.packages)
Check 'install: 4 NL Mic + 4 NL Return endpoints present' ($state.endpoints -eq 8) ("endpoints=" + $state.endpoints)
Check 'install: verification helper installed in the app folder' ($state.verifyExe -eq $true)

# Repair: remove a file that repair must restore.
NL-Exec 'del /f /q C:\NLMatrix\NLMatrixUI.exe 2>nul & echo CLEARED' | Out-Null
$repairCode = RunSetupMode 'repair'
if ($repairCode -eq 20) { Restart-Guest }
Check 'repair exits 0/20' ($repairCode -eq 0 -or $repairCode -eq 20) ("exit=" + $repairCode)
$state = Get-GuestState
Check 'repair restored NLMatrixUI.exe' ($state.app -eq $true)
Check 'repair kept the user profiles' ($state.profileMarker -eq $true)
Check 'repair kept one device instance and one driver package' ($state.devices -eq 1 -and $state.packages -eq 1) ("devices=" + $state.devices + " packages=" + $state.packages)

$upgradeCode = RunSetupMode 'upgrade'
if ($upgradeCode -eq 20) { Restart-Guest }
Check 'upgrade exits 0/20' ($upgradeCode -eq 0 -or $upgradeCode -eq 20) ("exit=" + $upgradeCode)
$state = Get-GuestState
Check 'upgrade kept profiles, one device and one package' ($state.profileMarker -eq $true -and $state.devices -eq 1 -and $state.packages -eq 1) ("devices=" + $state.devices + " packages=" + $state.packages)
Check 'upgrade kept the eight endpoints' ($state.endpoints -eq 8) ("endpoints=" + $state.endpoints)

$againCode = RunSetupMode 'install'
if ($againCode -eq 20) { Restart-Guest }
Check 'repeated install is idempotent' ($againCode -eq 0 -or $againCode -eq 20) ("exit=" + $againCode)
$state = Get-GuestState
Check 'repeated install created no duplicate device or package' ($state.devices -eq 1 -and $state.packages -eq 1) ("devices=" + $state.devices + " packages=" + $state.packages)

# ------------------------------------------------------------- 6. uninstall + reboot
$uninstallCode = RunSetupMode 'uninstall'
Check 'uninstall exits 0/20' ($uninstallCode -eq 0 -or $uninstallCode -eq 20) ("exit=" + $uninstallCode)
$state = Get-GuestState
Check 'uninstall removed the application files' ($state.app -eq $false)
Check 'uninstall removed the Add/Remove entry' ([string]::IsNullOrEmpty($state.arp))
Check 'uninstall removed the Start Menu folder' ($state.startMenu -eq $false)
Check 'uninstall removed the test certificate trust' ($state.certRoot -eq 0 -and $state.certPublisher -eq 0) ("root=" + $state.certRoot + " publisher=" + $state.certPublisher)
Check 'uninstall removed the driver package' ($state.packages -eq 0) ("packages=" + $state.packages)
Check 'uninstall removed the NL device' ($state.devices -eq 0) ("devices=" + $state.devices)
Check 'uninstall removed the eight endpoints' ($state.endpoints -eq 0) ("endpoints=" + $state.endpoints)
Check 'uninstall kept the user profiles' ($state.profileMarker -eq $true)
if ($uninstallCode -eq 20) {
    Check 'reboot-required uninstall wrote the pending-reboot marker' ($state.pendingMarker -eq $true)
    Check 'reboot-required uninstall registered the post-reboot verifier' (-not [string]::IsNullOrEmpty($state.runOnce) -and $state.runOnce -match 'NLMatrixVerify\.exe') $state.runOnce
}

Restart-Guest
# The RunOnce verifier must have run at logon and reported a clean removal.
$uninstallReportOk = $false
if ($uninstallCode -eq 20) {
    for ($attempt = 0; $attempt -lt 18; $attempt++) {
        if (Copy-GuestReport 'last-verify.json' (Join-Path $OutputDir 'last-verify-uninstall.json')) {
            $removal = Get-Content (Join-Path $OutputDir 'last-verify-uninstall.json') -Raw | ConvertFrom-Json
            if ($removal.kind -eq 'verify-uninstall' -and $removal.healthy -eq $true) { $uninstallReportOk = $true; break }
        }
        Start-Sleep -Seconds 10
    }
    Check 'post-reboot verifier confirmed the clean removal' $uninstallReportOk
    $state = Get-GuestState
    Check 'post-reboot verifier cleared the marker and RunOnce entry' ($state.pendingMarker -eq $false -and [string]::IsNullOrEmpty($state.runOnce)) ("marker=" + $state.pendingMarker)
    NL-Exec 'taskkill /F /IM NLMatrixVerify.exe' -TimeoutSec 60 | Out-Null
}
$absentCode = RunSetupMode 'verify' '/expect-absent'
Check 'post-reboot verify reports a clean removal' ($absentCode -eq 0) ("exit=" + $absentCode)
$absentCopied = Copy-GuestReport 'last-verify.json' (Join-Path $OutputDir 'last-verify-absent.json')
if ($absentCopied) {
    $absent = Get-Content (Join-Path $OutputDir 'last-verify-absent.json') -Raw | ConvertFrom-Json
    Check 'post-reboot absence report is healthy' ($absent.healthy -eq $true) (($absent.problems) -join '; ')
}

# ------------------------------------------- 7. clean reinstall + RunOnce verification
$reinstallCode = RunSetupMode 'install' '/register-post-reboot-verify'
Check 'clean reinstall exits 0/20' ($reinstallCode -eq 0 -or $reinstallCode -eq 20) ("exit=" + $reinstallCode)
$state = Get-GuestState
if ($reinstallCode -eq 20) {
    Check 'reboot-required install registered the post-reboot verifier' (-not [string]::IsNullOrEmpty($state.runOnce) -and $state.runOnce -match 'NLMatrixVerify\.exe') $state.runOnce
    Check 'reboot-required install wrote the pending-reboot marker' ($state.pendingMarker -eq $true)
    Restart-Guest
    $postRebootOk = $false
    $reports = $null
    for ($attempt = 0; $attempt -lt 18; $attempt++) {
        try {
            if (Copy-GuestReport 'last-verify.json' (Join-Path $OutputDir 'last-verify-post-reboot.json')) {
                $reports = Get-Content (Join-Path $OutputDir 'last-verify-post-reboot.json') -Raw | ConvertFrom-Json
                if ($reports.kind -eq 'verify-post-reboot' -and $reports.healthy -eq $true) { $postRebootOk = $true; break }
            }
        } catch { }
        Start-Sleep -Seconds 10
    }
    Check 'RunOnce verifier ran at logon and reported healthy' $postRebootOk
    $state = Get-GuestState
    Check 'post-reboot verifier cleared the pending marker and RunOnce entry' ($state.pendingMarker -eq $false -and [string]::IsNullOrEmpty($state.runOnce)) ("marker=" + $state.pendingMarker)
    Check 'post-reboot state has 8 endpoints and one device' ($state.endpoints -eq 8 -and $state.devices -eq 1) ("endpoints=" + $state.endpoints + " devices=" + $state.devices)
    NL-Exec 'taskkill /F /IM NLMatrixVerify.exe' -TimeoutSec 60 | Out-Null
} else {
    Log 'reinstall needed no reboot: RunOnce path not exercised in this run'
    Check 'reinstall without reboot leaves no pending marker' ($state.pendingMarker -eq $false)
}
Check 'reinstall restored the eight endpoints' ((Get-GuestState).endpoints -eq 8) ((Get-GuestState).endpoints)
Check 'reinstall kept the user profiles' ((Get-GuestState).profileMarker -eq $true)

# ------------------------------------------------------------- 8. final verification
$finalCode = RunSetupMode 'verify' '/app-selftest'
Check 'final verify with app start/stop exits 0' ($finalCode -eq 0) ("exit=" + $finalCode)
$finalCopied = Copy-GuestReport 'last-verify.json' (Join-Path $OutputDir 'last-verify-final.json')
if ($finalCopied) {
    $final = Get-Content (Join-Path $OutputDir 'last-verify-final.json') -Raw | ConvertFrom-Json
    Check 'final state: driver, device, 8 endpoints and app start/stop healthy' ($final.healthy -eq $true -and $final.endpoints.active -eq 8 -and $final.appSelfTest.ok -eq $true) ($final.appSelfTest.summary)
}

# -------------------------------------------------------------------- verdict
$ok = ($script:Failed -eq 0)
$summary = [ordered]@{
    suite = 'setupcheck'
    setupExe = $SetupExe
    setupSha256 = $setupHash
    checks = $checks
    failed = $script:Failed
    verdict = if ($ok) { 'PASS' } else { 'FAIL' }
    evidenceDir = $OutputDir
}
$summary | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputDir 'setupcheck_summary.json') -Encoding UTF8
Log ('checks: ' + $checks.Count + ' failed: ' + $script:Failed)
Log "evidence: $OutputDir"
if ($ok) { Log 'VERDICT: PASS - complete installer verified in the VM'; exit 0 }
Log 'VERDICT: FAIL'
exit 1
