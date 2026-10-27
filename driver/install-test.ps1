#Requires -RunAsAdministrator
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

Write-Host "Installing test certificate..."
certutil -addstore -f Root $cer | Out-Null
certutil -addstore -f TrustedPublisher $cer | Out-Null

Write-Host "Enabling test signing (reboot required)..."
bcdedit /set testsigning on | Out-Null

Write-Host "Staging and creating the root-enumerated device..."
pnputil /add-driver $inf /install
if (-not (Test-Path $devcon)) { throw "devcon.exe is required in package; place the WDK devcon.exe there" }
& $devcon install $inf "Root\sysvad_ComponentizedAudioSample"

Write-Host "Reboot the VM, then verify with:"
Write-Host "  pnputil /enum-devices /class MEDIA"
Write-Host "  NLMatrixDriverTest.exe"
