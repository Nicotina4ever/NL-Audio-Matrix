#Requires -RunAsAdministrator
# Developer helper: installs the NL test driver in an isolated test VM.
# It never changes the boot configuration: test signing must already be active.
param(
    [switch]$ConfirmInstall
)

$ErrorActionPreference = "Stop"
$package = Join-Path $PSScriptRoot "package"
$cer = Join-Path $PSScriptRoot "test-certs\NLMatrixTest.cer"
$devcon = Join-Path $package "devcon.exe"
$inf = Join-Path $package "ComponentizedAudioSample.inf"

if (-not $ConfirmInstall) {
    Write-Host "Refusing to install without -ConfirmInstall (kernel driver test on an isolated VM only)."
    exit 1
}
if (-not (Test-Path $inf)) { throw "Driver package missing; run build-driver.ps1" }
if (-not (Test-Path $cer)) { throw "Test certificate missing" }

# Read-only requirement check: this script never enables test signing itself.
$bcd = & bcdedit.exe /enum '{current}' 2>&1 | Out-String
if ($bcd -notmatch '(?im)^\s*testsigning\s+(Yes|On)\b') {
    Write-Host "Test signing is not configured in the boot entry."
    Write-Host "Enable it yourself and restart the VM first:  bcdedit /set testsigning on"
    Write-Host "This script does not change the boot configuration."
    exit 2
}
Write-Host "Test signing is configured. The running VM must already show the 'Test Mode' watermark;"
Write-Host "if it does not, restart the VM before continuing (the driver cannot load otherwise)."

Write-Host "Installing test certificate..."
certutil -addstore -f Root $cer | Out-Null
certutil -addstore -f TrustedPublisher $cer | Out-Null

Write-Host "Staging and creating the root-enumerated device..."
pnputil /add-driver $inf /install
if (-not (Test-Path $devcon)) { throw "devcon.exe is required in package; place the WDK devcon.exe there" }
& $devcon install $inf "Root\sysvad_ComponentizedAudioSample"

Write-Host "Reboot the VM, then verify with:"
Write-Host "  pnputil /enum-devices /class MEDIA"
Write-Host "  NLMatrixDriverTest.exe"
