#requires -Version 5.1
<#
.SYNOPSIS
    Read-only preflight for installing NL Audio Matrix (app + NL test-signed driver)
    on this computer.

.DESCRIPTION
    Checks the real requirements of the NL kernel driver on the current host and
    prints a verdict with the manual steps that would be needed.  It never installs
    anything, never changes the boot configuration, Secure Boot, HVCI, certificates,
    drivers or Windows audio settings: every check is a read.

    Checks:
      * Windows edition/build/architecture and free space on the install drive
      * administrator token (some checks need it), firmware type and Secure Boot
      * Windows test signing (TESTSIGNING), HVCI/Memory integrity, VBS, blocklist
      * existing NL app/driver state (ARP entry, DriverStore INFs, PnP device,
        the eight NL endpoints, test certificate trust)
      * pending reboot and the NL setup pending-reboot marker
      * verdict: READY / BLOCKED / NEEDS ADMINISTRATOR, with the exact manual steps

    Output is a compact text report plus a JSON file (default
    reports/host-preflight/<timestamp>.json) so a later automated step or the
    owner can read only the summary.

.PARAMETER OutputDir
    Folder for the JSON + text report.  Default: reports/host-preflight/<timestamp>.

.PARAMETER Quiet
    Do not print the text report (the JSON is still written).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\Diagnose-InstallerHost.ps1

.NOTES
    Exit codes: 0 = requirements look satisfied, 12 = a security configuration
    blocks the test-signed driver, 2 = this host cannot install NL at all,
    1 = the check itself failed.  Read-only in every case.
#>
[CmdletBinding()]
param(
    [string]$OutputDir = '',
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$hostName = $env:COMPUTERNAME

function Add-Problem {
    param([System.Collections.ArrayList]$List, [string]$Text)
    [void]$List.Add($Text)
}

# ---------------------------------------------------------------- OS + rights
$os = $null
try { $os = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop } catch { }
$build = 0
$caption = 'unknown'
$version = ''
if ($null -ne $os) {
    $caption = $os.Caption
    $version = $os.Version
    try { $build = [int]($os.BuildNumber) } catch { $build = 0 }
}
$is64 = [Environment]::Is64BitOperatingSystem
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

# ---------------------------------------------------------------- firmware/SB
$firmware = 'unknown'
try {
    $fw = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name 'PEFirmwareType' -ErrorAction Stop
    if ($null -ne $fw.PEFirmwareType) {
        $firmware = if ([int]$fw.PEFirmwareType -eq 2) { 'uefi' } elseif ([int]$fw.PEFirmwareType -eq 1) { 'bios' } else { 'unknown' }
    }
} catch { }
if ($firmware -eq 'unknown') { $firmware = 'not reported' }

$secureBoot = 'unknown'
try {
    $sb = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -ErrorAction Stop
    if ($null -ne $sb.UEFISecureBootEnabled) {
        $secureBoot = if ([int]$sb.UEFISecureBootEnabled -eq 1) { 'enabled' } else { 'disabled' }
    }
} catch { $secureBoot = 'not exposed (legacy BIOS or policy)' }

# --------------------------------------------------------------- TESTSIGNING
$testSigning = 'unknown'
$testSigningNote = ''
try {
    $bcd = & bcdedit.exe /enum '{current}' 2>&1
    if ($LASTEXITCODE -eq 0) {
        $testSigning = 'disabled'
        foreach ($line in ($bcd -split "`r?`n")) {
            if ($line -match '^\s*testsigning\s+(Yes|On|Si)') { $testSigning = 'enabled' }
        }
    } else {
        $testSigningNote = 'bcdedit requires administrator rights'
    }
} catch { $testSigningNote = 'bcdedit unavailable: ' + $_.Exception.Message }

# ------------------------------------------------------------ HVCI/VBS/blocklist
$readDword = {
    param([string]$Path, [string]$Name)
    try {
        $item = Get-ItemProperty -Path $Path -Name $Name -ErrorAction Stop
        return [int]$item.$Name
    } catch { return $null }
}
$hvciValue = & $readDword 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' 'Enabled'
$vbsValue = & $readDword 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard' 'EnableVirtualizationBasedSecurity'
$blocklistValue = & $readDword 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Config' 'VulnerableDriverBlocklistEnable'
$hvci = if ($null -eq $hvciValue) { 'disabled' } elseif ($hvciValue -eq 1) { 'enabled' } else { 'disabled' }
$vbs = if ($null -eq $vbsValue) { 'disabled' } elseif ($vbsValue -eq 1) { 'enabled' } else { 'disabled' }
$blocklist = if ($null -eq $blocklistValue) { 'disabled' } elseif ($blocklistValue -eq 1) { 'enabled' } else { 'disabled' }

# ------------------------------------------------------------------- NL state
$appDir = 'C:\NLMatrix'
$arpVersion = ''
try {
    $arp = Get-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NLMatrix' -ErrorAction SilentlyContinue
    if ($null -ne $arp) { $arpVersion = [string]$arp.DisplayVersion; if ($arp.InstallLocation) { $appDir = [string]$arp.InstallLocation } }
} catch { }

$driverPackages = @()
$windowsInf = Join-Path $env:WINDIR 'INF'
if (Test-Path $windowsInf) {
    foreach ($inf in Get-ChildItem $windowsInf -Filter 'oem*.inf' -ErrorAction SilentlyContinue) {
        try {
            $text = Get-Content $inf.FullName -Raw -ErrorAction Stop
            if ($text -match 'NLMatrix Virtual Audio Device|Root\\NLMatrixVirtualAudio|nlmatrixaudio\.sys') {
                $ver = ''
                foreach ($line in ($text -split "`r?`n")) {
                    if ($line -match '^\s*DriverVer\s*=\s*(.+)$') { $ver = $Matches[1].Trim(); break }
                }
                $driverPackages += [ordered]@{ inf = $inf.Name; driverVer = $ver }
            }
        } catch { }
    }
}

$devicePresent = $false
$deviceInstance = ''
$endpointNames = @()
try {
    $devices = Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction SilentlyContinue |
        Where-Object { $_.DeviceName -like '*NLMatrix*' -or $_.HardwareID -like '*NLMatrixVirtualAudio*' }
    foreach ($device in $devices) {
        $devicePresent = $true
        if (-not $deviceInstance) { $deviceInstance = [string]$device.DeviceID }
    }
} catch { }
try {
    foreach ($endpoint in Get-PnpDevice -Class AudioEndpoint -PresentOnly -ErrorAction SilentlyContinue) {
        if ($endpoint.FriendlyName -match '^NL (Mic|Return) \d') { $endpointNames += $endpoint.FriendlyName }
    }
} catch { }

$certTrusted = $false
foreach ($storeName in @('Root', 'TrustedPublisher')) {
    try {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
        $store.Open('ReadOnly')
        foreach ($certificate in $store.Certificates) {
            if ($certificate.Subject -match 'CN=NLMatrix Test Driver') { $certTrusted = $true }
        }
        $store.Close()
    } catch { }
}

# ----------------------------------------------------------------- disk + pending
$freeGb = $null
try {
    $drive = (Split-Path -Qualifier $appDir)
    $volume = Get-CimInstance -ClassName Win32_LogicalDisk -ErrorAction SilentlyContinue |
        Where-Object { $_.DeviceID -eq $drive } | Select-Object -First 1
    if ($null -ne $volume) { $freeGb = [math]::Round($volume.FreeSpace / 1GB, 1) }
} catch { }

$pendingReboot = $false
try {
    $pfro = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction SilentlyContinue
    if ($null -ne $pfro) { $pendingReboot = $true }
} catch { }
$setupMarker = Join-Path $env:ProgramData 'NL Audio Matrix\pending-reboot.txt'
if (Test-Path $setupMarker) { $pendingReboot = $true }

# -------------------------------------------------------------------- verdict
$problems = New-Object System.Collections.ArrayList
$warnings = New-Object System.Collections.ArrayList
$verdict = 'READY'

if (-not $is64) { Add-Problem $problems 'Windows is not 64-bit: NL Audio Matrix requires x64.'; $verdict = 'IMPOSSIBLE' }
if ($build -gt 0 -and $build -lt 19041) { Add-Problem $problems "Windows build $build is older than 19041 (Windows 10 2004)." ; if ($verdict -eq 'READY') { $verdict = 'IMPOSSIBLE' } }
if ($null -ne $freeGb -and $freeGb -lt 1) { Add-Problem $problems "Only $freeGb GB free on the installation drive." ; if ($verdict -eq 'READY') { $verdict = 'IMPOSSIBLE' } }

if ($hvci -eq 'enabled') {
    Add-Problem $problems 'HVCI (Memory integrity) is enabled and blocks test-signed kernel drivers.'
    $verdict = 'BLOCKED'
}
if ($secureBoot -eq 'enabled' -and $testSigning -ne 'enabled') {
    Add-Problem $problems 'Secure Boot is enabled and test signing is not active.'
    $verdict = 'BLOCKED'
}
if ($testSigning -eq 'disabled') {
    Add-Problem $problems 'Windows test signing is disabled, so the test-signed NL driver cannot load.'
    if ($verdict -ne 'BLOCKED') { $verdict = 'BLOCKED' }
}
if ($testSigning -eq 'unknown') {
    Add-Problem $problems ('The test-signing state could not be read: ' + $testSigningNote + ' - run this check from an elevated PowerShell for the definitive answer.')
    if ($verdict -eq 'READY') { $verdict = 'NEEDS-ADMIN' }
}
if ($testSigning -eq 'enabled' -and $secureBoot -eq 'enabled') {
    [void]$warnings.Add('Secure Boot is on while test signing is active: keep Secure Boot disabled until a production-signed driver exists.')
}
if ($secureBoot -eq 'unknown') {
    [void]$warnings.Add('Secure Boot state is not exposed by this firmware/OS; verify it in the firmware settings.')
}
if ($blocklist -eq 'enabled') {
    [void]$warnings.Add('The Microsoft vulnerable-driver blocklist is enabled; it does not list the NL driver but a future entry could block it.')
}
if (-not $isAdmin) {
    [void]$warnings.Add('This check ran without administrator rights: secure boot/test signing reads may be incomplete.')
}
if ($pendingReboot) {
    [void]$warnings.Add('Windows has a pending reboot; driver installation should wait until it is completed.')
}

# -------------------------------------------------------------------- output
if (-not $OutputDir) {
    $OutputDir = Join-Path $root ('reports\host-preflight\' + (Get-Date -Format 'yyyy-MM-dd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$stamp = Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz'

$report = [ordered]@{
    schema = 1
    kind = 'host-preflight'
    readOnly = $true
    host = $hostName
    utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    localTime = $stamp
    os = [ordered]@{
        caption = $caption
        version = $version
        build = $build
        x64 = $is64
        administrator = $isAdmin
    }
    firmware = [ordered]@{
        type = $firmware
        secureBoot = $secureBoot
        testSigning = $testSigning
        testSigningNote = $testSigningNote
        hvci = $hvci
        vbs = $vbs
        vulnerableDriverBlocklist = $blocklist
    }
    nl = [ordered]@{
        appDir = $appDir
        installedVersion = $arpVersion
        driverPackages = $driverPackages
        devicePresent = $devicePresent
        deviceInstance = $deviceInstance
        endpoints = $endpointNames
        endpointCount = $endpointNames.Count
        certificateTrusted = $certTrusted
    }
    environment = [ordered]@{
        freeGbOnInstallDrive = $freeGb
        pendingReboot = $pendingReboot
    }
    verdict = $verdict
    problems = @($problems)
    warnings = @($warnings)
    manualSteps = @(
        if ($hvci -eq 'enabled') { 'Windows Security > Device security > Core isolation > turn Memory integrity off, then reboot.' }
        if ($secureBoot -eq 'enabled') { 'Disable Secure Boot in the firmware (BIOS/UEFI) settings on this PC; this reduces boot protection until a production-signed driver exists.' }
        if ($testSigning -ne 'enabled') { 'Enable test signing: bcdedit /set testsigning on (administrator), then reboot, or let the NL setup do it with explicit consent.' }
        if ($pendingReboot) { 'Complete the pending Windows reboot before installing the driver.' }
    )
}
$jsonPath = Join-Path $OutputDir 'host-preflight.json'
$report | ConvertTo-Json -Depth 6 | Set-Content -Path $jsonPath -Encoding UTF8

if (-not $Quiet) {
    Write-Host ''
    Write-Host 'NL Audio Matrix host preflight - READ ONLY, nothing was installed or changed' -ForegroundColor Cyan
    Write-Host ('  Host            : ' + $hostName)
    Write-Host ('  Windows         : ' + $caption + ' ' + $version + ' (build ' + $build + ', x64=' + $is64 + ')')
    Write-Host ('  Administrator   : ' + $isAdmin)
    Write-Host ('  Secure Boot     : ' + $secureBoot + '   TESTSIGNING: ' + $testSigning + '   HVCI: ' + $hvci + '   VBS: ' + $vbs)
    Write-Host ('  NL app          : ' + $(if ($arpVersion) { $arpVersion + ' in ' + $appDir } else { 'not installed' }))
    Write-Host ('  NL driver       : ' + $(if ($driverPackages.Count -gt 0) { ($driverPackages | ForEach-Object { $_.inf + ' [' + $_.driverVer + ']' }) -join ', ' } else { 'not staged' }))
    Write-Host ('  NL device       : ' + $(if ($devicePresent) { 'present (' + $deviceInstance + ')' } else { 'absent' }))
    Write-Host ('  NL endpoints    : ' + $endpointNames.Count + ' of 8' + $(if ($endpointNames.Count -gt 0) { ' (' + ($endpointNames -join ', ') + ')' } else { '' }))
    Write-Host ('  Free space      : ' + $(if ($null -ne $freeGb) { "$freeGb GB" } else { 'unknown' }) + '   pending reboot: ' + $pendingReboot)
    Write-Host ''
    Write-Host ('  VERDICT: ' + $verdict) -ForegroundColor $(if ($verdict -eq 'READY') { 'Green' } elseif ($verdict -eq 'IMPOSSIBLE') { 'Red' } else { 'Yellow' })
    foreach ($problem in $problems) { Write-Host ('    - ' + $problem) -ForegroundColor Yellow }
    foreach ($warning in $warnings) { Write-Host ('    * ' + $warning) -ForegroundColor DarkYellow }
    if ($problems.Count -gt 0) {
        Write-Host '  Manual steps (nothing was changed by this check):'
        foreach ($step in $report.manualSteps) { Write-Host ('    > ' + $step) }
    }
    Write-Host ''
    Write-Host ('  Report: ' + $jsonPath)
}

if ($verdict -eq 'IMPOSSIBLE') { exit 2 }
if ($verdict -eq 'BLOCKED') { exit 12 }
if ($verdict -eq 'NEEDS-ADMIN') { exit 3 }
exit 0
