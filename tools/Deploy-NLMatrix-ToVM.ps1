param(
    [Parameter(Mandatory=$true)][string]$Vmx,
    [string]$GuestRoot = 'C:\NLMatrix',
    [string]$Vmrun = 'C:\Program Files (x86)\VMware\VMware Player\vmrun.exe',
    [Parameter(Mandatory=$true)][string]$GuestUser,
    [Parameter(Mandatory=$true)][string]$GuestPassword
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cred = @('-T','ws','-gu',$GuestUser,'-gp',$GuestPassword)
$productVersion = '0.4.0-alpha'

function GuestHash([string]$path) {
    $guestOut = Join-Path $GuestRoot 'hash-result.txt'
    $ps = Join-Path $env:TEMP ('nlmatrix-hash-' + [guid]::NewGuid().ToString('N') + '.ps1')
    Set-Content -Path $ps -Value "Get-FileHash '$path' -Algorithm SHA256 | Select-Object -ExpandProperty Hash | Set-Content '$guestOut'" -Encoding ASCII
    $guestPs = Join-Path $GuestRoot 'hash-probe.ps1'
    & $Vmrun @cred CopyFileFromHostToGuest $Vmx $ps $guestPs | Out-Null
    & $Vmrun @cred runProgramInGuest $Vmx 'C:\Windows\System32\cmd.exe' "/c powershell -NoProfile -ExecutionPolicy Bypass -File $guestPs" | Out-Null
    $local = Join-Path $env:TEMP ('nlmatrix-hash-' + [guid]::NewGuid().ToString('N') + '.txt')
    & $Vmrun @cred CopyFileFromGuestToHost $Vmx $guestOut $local 2>$null | Out-Null
    $hash = if (Test-Path $local) { (Get-Content $local -Raw).Trim() } else { '' }
    Remove-Item $ps,$local -Force -ErrorAction SilentlyContinue
    return $hash
}
function DeployFile([string]$hostPath, [string]$name) {
    $guestPath = Join-Path $GuestRoot $name
    $hostHash = (Get-FileHash $hostPath -Algorithm SHA256).Hash.ToLowerInvariant()
    & $Vmrun @cred CopyFileFromHostToGuest $Vmx $hostPath $guestPath | Out-Null
    $guestHash = GuestHash $guestPath
    if (-not $guestHash -or $guestHash.ToLowerInvariant() -ne $hostHash) {
        throw "Deployment hash mismatch for $name (host=$hostHash guest=$guestHash)"
    }
    Write-Output ("OK  {0}  {1}" -f $name, $hostHash)
}

& $Vmrun @cred runProgramInGuest $Vmx 'C:\Windows\System32\cmd.exe' '/c taskkill /F /IM NLMatrixUI.exe' 2>$null
$binaries = @(
    'app\build\NLMatrixUI_artefacts\Release\NLMatrixUI.exe',
    'app\build\NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe',
    'app\build\NLMatrixBridge_artefacts\Release\NLMatrixBridge.exe',
    'app\build\NLMatrixProbe_artefacts\Release\NLMatrixProbe.exe',
    'app\build\NLMatrixLive_artefacts\Release\NLMatrixLive.exe',
    'app\build\NLMatrixLoopbackTest.exe'
)
foreach ($relative in $binaries) {
    $hostPath = Join-Path $root $relative
    if (Test-Path $hostPath) { DeployFile $hostPath (Split-Path -Leaf $hostPath) }
}
$driverPackage = Join-Path $root 'driver\package\ComponentizedAudioSample.inf'
if (Test-Path $driverPackage) {
    $driverGuest = Join-Path $GuestRoot 'driver-deploy'
    & $Vmrun @cred runProgramInGuest $Vmx 'C:\Windows\System32\cmd.exe' "/c mkdir $driverGuest" 2>$null
    foreach ($name in @('NLMatrixAudio.sys','ComponentizedAudioSample.inf','sysvad.cat')) {
        $src = Join-Path $root (Join-Path 'driver\package' $name)
        if (Test-Path $src) {
            & $Vmrun @cred CopyFileFromHostToGuest $Vmx $src (Join-Path $driverGuest $name) | Out-Null
            Write-Output ("OK  driver/{0}  {1}" -f $name, (Get-FileHash $src -Algorithm SHA256).Hash.ToLowerInvariant())
        }
    }
    Write-Output "Driver package copied. Install with: pnputil /add-driver $driverGuest\ComponentizedAudioSample.inf /install"
}
Write-Output "Deployment complete for $productVersion."
