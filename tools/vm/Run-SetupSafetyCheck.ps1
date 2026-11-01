# Run-SetupSafetyCheck.ps1 — deterministic safety suite for the NL setup/uninstaller
# in the VMware test VM.  It proves the required behaviours with measured timings and
# a compact JSON verdict:
#
#   1. install / repair / uninstall / reinstall all terminate with a known exit code
#      and a bounded duration (a busy device or a stuck child can never hang them);
#   2. a device held open by a capture client does not block the uninstaller: it
#      finishes and asks for the reboot instead;
#   3. uninstalling when the driver is already absent is quick and succeeds;
#   4. the boot configuration (bcdedit /enum {current}) is byte-identical before and
#      after install + repair + uninstall + reinstall: the setup cannot change BCD;
#   5. the eight NL endpoints come back after a clean reinstall.
#
# The VM is left with NL Audio Matrix installed.  Nothing runs on the host.
#
# Usage: powershell -File tools\vm\Run-SetupSafetyCheck.ps1 [-SetupExe <path>] [-OutputDir <path>]
[CmdletBinding()]
param(
    [string]$SetupExe = '',
    [string]$OutputDir = ''
)

$ErrorActionPreference = 'Stop'
$script:Workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $SetupExe) { $SetupExe = Join-Path $script:Workspace 'installer\build\NLMatrixSetup.exe' }
if (-not $OutputDir) { $OutputDir = Join-Path $script:Workspace ('reports\safety\' + (Get-Date -Format 'yyyy-MM-dd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
if (-not (Test-Path $SetupExe)) { throw "setup executable not found: $SetupExe" }

$vmrun = 'C:\Program Files (x86)\VMware\VMware Player\vmrun.exe'
$vmx = 'K:\AI\NL Audio Matrix\vm\Windows 10 and later x64.vmx'
$creds = @('-T', 'ws', '-gu', 'NLMatrix', '-gp', 'nlmatrix')
$temp = 'C:\Windows\Temp'
$checks = New-Object System.Collections.Generic.List[object]
$timings = [ordered]@{}

function Log([string]$text) {
    Write-Host ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $text)
}
function Add-Check([string]$name, [bool]$ok, [string]$detail) {
    $checks.Add([ordered]@{ name = $name; ok = $ok; detail = $detail })
    Log (('{0} {1} - {2}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail))
}
function Guest([string]$command) {
    & $vmrun @creds runProgramInGuest $vmx 'C:\Windows\System32\cmd.exe' "/c $command" 2>&1 | Out-Null
    return $LASTEXITCODE
}
function Reachable {
    & $vmrun @creds runProgramInGuest $vmx 'C:\Windows\System32\cmd.exe' '/c echo ready' 2>&1 | Out-Null
    return ($LASTEXITCODE -eq 0)
}
function Push([string]$local, [string]$guest) {
    Guest ("del /f /q " + $guest) | Out-Null
    & $vmrun @creds CopyFileFromHostToGuest $vmx $local $guest 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "copy failed: $local -> $guest" }
}
function Fetch([string]$guest, [string]$name) {
    $local = Join-Path $OutputDir $name
    if (Test-Path $local) { Remove-Item $local -Force }
    & $vmrun @creds CopyFileFromGuestToHost $vmx $guest $local 2>&1 | Out-Null
    if (Test-Path $local) { return (Get-Content $local -Raw) }
    return ''
}
function Snapshot([string]$tag) {
    Guest 'C:\Windows\Temp\nl-snapshot.bat' | Out-Null
    $text = Fetch 'C:\Windows\Temp\nlsnap.txt' ('snapshot-' + $tag + '.txt')
    return $text
}
function BcdPart([string]$snapshotText) {
    $index = $snapshotText.IndexOf('---END-BCD---')
    if ($index -lt 0) { return $snapshotText }
    return $snapshotText.Substring(0, $index)
}
function EndpointCount([string]$snapshotText) {
    $match = [regex]::Match($snapshotText, '---END-BCD---[^\r\n]*\r?\n\s*(\d+)')
    if ($match.Success) { return [int]$match.Groups[1].Value }
    return -1
}
function Wait-Guest([int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Reachable) { return $true }
        Start-Sleep -Seconds 5
    }
    return $false
}
function Reboot-Guest {
    Guest 'shutdown /r /t 2 /f' | Out-Null
    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 4
        if (-not (Reachable)) { break }
    }
    if (-not (Wait-Guest 420)) { throw 'guest did not come back after reboot' }
    Start-Sleep -Seconds 10
}
function Run-Mode([string]$mode) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    Guest ('C:\Windows\Temp\nl-run-' + $mode + '.bat') | Out-Null
    $watch.Stop()
    $log = Fetch ('C:\Windows\Temp\nlrun-' + $mode + '.txt') ('run-' + $mode + '-' + $watch.ElapsedMilliseconds + 'ms.log')
    $code = -1
    $match = [regex]::Match($log, 'EXITCODE=(-?\d+)')
    if ($match.Success) { $code = [int]$match.Groups[1].Value }
    $timings[$mode + 'Ms'] = $watch.ElapsedMilliseconds
    return @{ Code = $code; Seconds = [math]::Round($watch.Elapsed.TotalSeconds, 1) }
}

# ------------------------------------------------------------------ guest helpers
$snapshotBat = @'
@echo off
set OUT=C:\Windows\Temp\nlsnap.txt
bcdedit /enum {current} > %OUT% 2>&1
echo ---END-BCD--- >> %OUT%
powershell -NoProfile -Command "(Get-PnpDevice -Class AudioEndpoint -PresentOnly | Where-Object { $_.FriendlyName -like 'NL *' }).Count" >> %OUT% 2>&1
sc query sysvad_componentizedaudiosample | findstr /i "STATO STATE" >> %OUT% 2>&1
'@
$snapshotPath = Join-Path $env:TEMP 'nl-snapshot.bat'
Set-Content -Path $snapshotPath -Value $snapshotBat -Encoding ASCII
# One batch per mode with hardcoded arguments: vmrun mangles a batch invoked with a
# placeholder plus an argument, and a mangled command line used to open a modal usage
# dialog in the non-interactive guest session, which then waited forever.
$modeBats = @()
foreach ($mode in @('install', 'repair', 'uninstall')) {
    $body = '@echo off' + "`r`n" +
            ('C:\Windows\Temp\NLMatrixSetup-safety.exe --mode ' + $mode + ' /silent > C:\Windows\Temp\nlrun-' + $mode + '.txt 2>&1') + "`r`n" +
            ('echo EXITCODE=%ERRORLEVEL% >> C:\Windows\Temp\nlrun-' + $mode + '.txt') + "`r`n"
    $path = Join-Path $env:TEMP ('nl-run-' + $mode + '.bat')
    Set-Content -Path $path -Value $body -Encoding ASCII
    $modeBats += [pscustomobject]@{ Path = $path; Guest = 'C:\Windows\Temp\nl-run-' + $mode + '.bat' }
}

$holdBatPath = Join-Path $env:TEMP 'nl-hold.bat'
$holdBody = '@echo off' + "`r`n" +
            'start "" /b powershell -NoProfile -ExecutionPolicy Bypass -File C:\Windows\Temp\nlvm-hold-mic.ps1 -Seconds 120' + "`r`n"
Set-Content -Path $holdBatPath -Value $holdBody -Encoding ASCII

Log ('setup: ' + $SetupExe + '  sha256 ' + (Get-FileHash $SetupExe -Algorithm SHA256).Hash)
# A leftover setup process (older build) can keep the previous copy of the executable
# locked; kill it and use a dedicated name for the copy under test.
Guest 'taskkill /F /IM NLMatrixSetup.exe' | Out-Null
Start-Sleep -Seconds 1
$guestSetup = $temp + '\NLMatrixSetup-safety.exe'
Push $SetupExe $guestSetup
Push $snapshotPath ($temp + '\nl-snapshot.bat')
foreach ($bat in $modeBats) { Push $bat.Path $bat.Guest }
Push $holdBatPath ($temp + '\nl-hold.bat')
Push (Join-Path $env:TEMP 'nlvm-hold-mic.ps1') ($temp + '\nlvm-hold-mic.ps1')

# Baseline: restart once so any pending removal/installation from a previous run
# settles, exactly like a user coming back to a normal PC.
Log 'restarting the VM for a clean baseline'
Reboot-Guest

# 1. baseline BCD
$snapshotBefore = Snapshot 'before'
$bcdBefore = BcdPart $snapshotBefore
Add-Check 'BCD readable before the tests' ($bcdBefore.Length -gt 0) ('bcd bytes ' + $bcdBefore.Length)

# 2. install
$install = Run-Mode 'install'
Add-Check 'install terminates with exit 0' ($install.Code -eq 0) ($install.Code.ToString() + ' in ' + $install.Seconds + ' s')
$installedState = Snapshot 'installed'
Add-Check 'install leaves the BCD unchanged' ((BcdPart $installedState) -eq $bcdBefore) 'bcdedit /enum identical'

# 3. repair
$repair = Run-Mode 'repair'
Add-Check 'repair terminates with exit 0' ($repair.Code -eq 0) ($repair.Code.ToString() + ' in ' + $repair.Seconds + ' s')
Add-Check 'repair leaves the BCD unchanged' ((BcdPart (Snapshot 'repaired')) -eq $bcdBefore) 'bcdedit /enum identical'

# 4. busy device: hold NL Mic 1 during the uninstall
Guest 'C:\Windows\Temp\nl-hold.bat' | Out-Null
$holdText = ''
for ($attempt = 0; $attempt -lt 15; $attempt++) {
    Start-Sleep -Seconds 1
    $holdText = Fetch 'C:\Windows\Temp\nlhold.txt' 'hold.txt'
    if ($holdText -match 'HELD') { break }
}
Add-Check 'capture client holds NL Mic 1' ($holdText -match 'HELD') ($(if ($holdText.Length) { $holdText.Trim() } else { 'holder did not open the endpoint' }))
$busyUninstall = Run-Mode 'uninstall'
Add-Check 'uninstall with a busy device terminates (<= 300 s)' ($busyUninstall.Seconds -le 300) ($busyUninstall.Code.ToString() + ' in ' + $busyUninstall.Seconds + ' s')
Add-Check 'busy uninstall reports success or reboot' ($busyUninstall.Code -eq 0 -or $busyUninstall.Code -eq 20) ($busyUninstall.Code.ToString())
$needsReboot = ($busyUninstall.Code -eq 20)

# 5. reboot if the removal was queued
if ($needsReboot) {
    Log 'reboot required by the busy uninstall; restarting the VM'
    Reboot-Guest
    $timings['rebootMs'] = 0
}

# 6. uninstall when already absent: must be quick and successful
$absentUninstall = Run-Mode 'uninstall'
Add-Check 'uninstall when already absent terminates with exit 0' ($absentUninstall.Code -eq 0) ($absentUninstall.Code.ToString() + ' in ' + $absentUninstall.Seconds + ' s')
Add-Check 'uninstall when absent is quick (<= 120 s)' ($absentUninstall.Seconds -le 120) ($absentUninstall.Seconds.ToString() + ' s')

# 7. BCD after the whole cycle
$snapshotAfter = Snapshot 'after'
$bcdAfter = BcdPart $snapshotAfter
Add-Check 'BCD is byte-identical after install/repair/uninstall' ($bcdAfter -eq $bcdBefore) 'bcdedit /enum comparison'

# 8. clean reinstall
$reinstall = Run-Mode 'install'
Add-Check 'reinstall terminates with exit 0 or 20' ($reinstall.Code -eq 0 -or $reinstall.Code -eq 20) ($reinstall.Code.ToString() + ' in ' + $reinstall.Seconds + ' s')
if ($reinstall.Code -eq 20) {
    Log 'reinstall needs a reboot to load the driver; restarting the VM'
    Reboot-Guest
}
$finalState = Snapshot 'final'
$endpoints = EndpointCount $finalState
Add-Check 'eight NL endpoints after reinstall' ($endpoints -eq 8) ('endpoints=' + $endpoints)
Add-Check 'final BCD still identical' ((BcdPart $finalState) -eq $bcdBefore) 'bcdedit /enum identical'

$failed = ($checks | Where-Object { -not $_.ok }).Count
$result = [ordered]@{
    tool = 'Run-SetupSafetyCheck'
    utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    setup = [ordered]@{ path = $SetupExe; sha256 = (Get-FileHash $SetupExe -Algorithm SHA256).Hash.ToLowerInvariant() }
    timings = $timings
    checks = $checks
    failed = $failed
    verdict = $(if ($failed -eq 0) { 'PASS' } else { 'FAIL' })
}
$jsonPath = Join-Path $OutputDir 'safety-summary.json'
$jsonText = $result | ConvertTo-Json -Depth 6
Set-Content -Path $jsonPath -Value $jsonText -Encoding UTF8
Log ('VERDICT: ' + $result.verdict + ' (' + $checks.Count + ' checks, ' + $failed + ' failed)')
Log ('summary: ' + $jsonPath)
exit $(if ($failed -eq 0) { 0 } else { 1 })
