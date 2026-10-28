# NLMatrix deployment: install the current release package into the VM,
# verify SHA256 and build identity, check the NL endpoints, then launch the GUI.
# The installer owns the C:\NLMatrix cleanup: obsolete test binaries, logs, WAVs
# and old FINAL copies are removed; %APPDATA%\NL Audio Matrix user profiles and
# the installed Windows audio devices/DriverStore are untouched.
param(
    [string]$Version = '',
    [string]$InstallerPath = '',
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'vm\NLVM.ps1')

if (-not $Version) {
    $latest = Get-ChildItem (Join-Path $root 'dist') -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'BUILD_INFO.json') } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $latest) { throw 'No release found under dist/; run tools/build-release.ps1 first.' }
    $Version = $latest.Name
}
$releaseDir = Join-Path $root (Join-Path 'dist' $Version)
$infoFile = Join-Path $releaseDir 'BUILD_INFO.json'
if (-not (Test-Path $infoFile)) { throw "Missing release metadata: $infoFile" }
$info = Get-Content $infoFile -Raw | ConvertFrom-Json
if (-not $InstallerPath) {
    $InstallerPath = Join-Path $releaseDir ("NL-Audio-Matrix-$Version-VM-Setup.exe")
}
if (-not (Test-Path $InstallerPath)) { throw "Installer not found: $InstallerPath" }

function GuestHash([string]$guestPath) {
    $safe = $guestPath.Replace("'", "''")
    $ps = 'powershell -NoProfile -Command "try { (Get-FileHash ''' + $safe + ''' -Algorithm SHA256).Hash } catch { ''MISSING'' }"'
    $out = NL-Exec $ps -TimeoutSec 120
    if ($out -match '([0-9A-Fa-f]{64})') { return $matches[1].ToUpperInvariant() }
    return ''
}

Write-Output "=== NL Audio Matrix deployment $Version ==="
Write-Output ("Build ID : " + $info.buildId)
Write-Output ("Commit   : " + $info.commit)
Write-Output ("Installer: " + $InstallerPath)

$hostHash = (Get-FileHash $InstallerPath -Algorithm SHA256).Hash.ToUpperInvariant()
$guestInstaller = 'C:\Windows\Temp\NLMatrixSetup.exe'
NL-Exec ('taskkill /F /IM NLMatrixUI.exe 2>nul & taskkill /F /IM NLMatrixRouter.exe 2>nul & del /f /q "' + $guestInstaller + '" 2>nul & echo CLEARED') -TimeoutSec 120 | Out-Null
NL-CopyTo $InstallerPath $guestInstaller | Out-Null
$guestHash = GuestHash $guestInstaller
if ($guestHash -ne $hostHash) { throw "Installer SHA256 mismatch: host=$hostHash guest=$guestHash" }
Write-Output ("OK  installer SHA256 " + $hostHash)

Write-Output '=== INSTALL (silent, clean install folder) ==='
$installOutput = NL-Exec ('"' + $guestInstaller + '" /silent /dir=C:\NLMatrix') -TimeoutSec 600
$exitCode = -1
if ($installOutput -match 'EXITCODE=(\d+)') { $exitCode = [int]$matches[1] }
Write-Output ($installOutput | Select-Object -Last 8)
if ($exitCode -ne 0 -and $exitCode -ne 20) { throw "Installer exit code $exitCode" }
if ($exitCode -eq 20) { Write-Warning 'Installer reports reboot required (test-signing change). Endpoints may not be live until reboot.' }

Write-Output '=== SHA256 / IDENTITY VERIFICATION ==='
$checks = @(
    @{ Path = 'C:\NLMatrix\NLMatrixUI.exe'; Expected = $info.uiSha256; Name = 'NLMatrixUI.exe' },
    @{ Path = 'C:\NLMatrix\NLMatrixRouter.exe'; Expected = $info.routerSha256; Name = 'NLMatrixRouter.exe' },
    @{ Path = 'C:\NLMatrix\driver\NLMatrixAudio.sys'; Expected = $info.driverSha256; Name = 'NLMatrixAudio.sys' }
)
foreach ($c in $checks) {
    $actual = GuestHash $c.Path
    if ($actual -ne $c.Expected.ToUpperInvariant()) { throw ("SHA256 mismatch " + $c.Name + ": expected " + $c.Expected + " actual " + $actual) }
    Write-Output ("OK  " + $c.Name + "  " + $actual)
}
$guestVersion = NL-Exec 'type C:\NLMatrix\VERSION.txt' -TimeoutSec 60
Write-Output $guestVersion
$buildLine = NL-Exec 'C:\NLMatrix\NLMatrixRouter.exe --buildinfo' -TimeoutSec 120
Write-Output $buildLine
if ($buildLine -notmatch [regex]::Escape($info.version) -or $buildLine -notmatch [regex]::Escape($info.buildId)) {
    throw "Router build identity mismatch (expected $($info.version) / $($info.buildId))"
}

Write-Output '=== GUI SELFTEST (build identity + installed endpoints) ==='
$selftest = NL-Exec 'C:\NLMatrix\NLMatrixUI.exe --selftest' -TimeoutSec 420
Write-Output ($selftest | Select-Object -Last 4)
try { NL-CopyFrom 'C:\Users\NLMatrix\AppData\Local\Temp\nlmatrix_selftest.log' (Join-Path $root 'vm\deployed_gui_selftest.log') | Out-Null } catch { Write-Warning 'Could not fetch GUI selftest log' }
$selftestLog = Get-Content (Join-Path $root 'vm\deployed_gui_selftest.log') -Raw -ErrorAction SilentlyContinue
if ($selftestLog -notmatch ('id=' + [regex]::Escape($info.buildId))) { throw 'GUI build ID differs from router/package BUILD_INFO.json' }
if ($selftestLog -notmatch 'SUMMARY 56/56 checks passed') { throw 'GUI selftest did not pass 56/56' }
Write-Output 'OK  GUI/router same Build ID; GUI selftest 56/56'

Write-Output '=== NL ENDPOINTS ==='
$endpoints = NL-Exec 'powershell -NoProfile -Command "Get-PnpDevice -Class MEDIA | Where-Object { $_.FriendlyName -like ''*NLMatrix*'' } | Select-Object Status,Present,FriendlyName,InstanceId | Format-Table -AutoSize | Out-String; Get-PnpDevice -Class AudioEndpoint | Where-Object { $_.FriendlyName -like ''NL Mic*'' -or $_.FriendlyName -like ''NL Return*'' } | Select-Object Status,Present,FriendlyName | Format-Table -AutoSize | Out-String"' -TimeoutSec 120
Write-Output $endpoints
$endpointLines = @(($endpoints -split '\r?\n') | Where-Object { $_ -match 'NL (Mic|Return)' })
if ($endpointLines.Count -lt 6) { throw 'Expected NL Mic/Return endpoints were not all present' }
Write-Output ('OK  NL endpoints present: ' + $endpointLines.Count)

Write-Output '=== CLEAN GUEST TEMP INSTALLER ==='
NL-Exec ('del /f /q "' + $guestInstaller + '" 2>nul & echo REMOVED') -TimeoutSec 60 | Out-Null

if (-not $NoLaunch) {
    Write-Output '=== LAUNCH INSTALLED GUI ==='
    NL-Exec 'powershell -NoProfile -Command "Start-Process -FilePath ''C:\NLMatrix\NLMatrixUI.exe'' -WorkingDirectory ''C:\NLMatrix''"' -TimeoutSec 120 | Out-Null
    Start-Sleep -Seconds 3
    $procs = NL-Exec 'tasklist /FI "IMAGENAME eq NLMatrixUI.exe" /FO CSV' -TimeoutSec 60
    Write-Output $procs
    if ($procs -notmatch 'NLMatrixUI.exe') { throw 'Installed GUI did not start' }
    Write-Output 'OK  installed GUI is running'
}
Write-Output '=== DEPLOYMENT COMPLETE ==='
