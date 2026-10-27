# host-vm-abi3.ps1 - deploy ABI v3 driver package + probe to the test VM,
# install it, reboot if needed, verify, run the capture smoke test, leave VM idle.
# Host side only; never installs anything on the host PC.
param(
    [string]$Root = 'K:\AI\NL Audio Matrix'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $Root 'tools\vm\NLVM.ps1')
if (-not $Vmrun) { $Vmrun = 'C:\Program Files (x86)\VMware\VMware Player\vmrun.exe' }
if (-not $Vmx) { $Vmx = Join-Path $Root 'vm\Windows 10 and later x64.vmx' }
$evidence = Join-Path $Root 'reports\engine\evidence'
$smokeDir = Join-Path $evidence 'smoke'
New-Item -ItemType Directory -Force -Path $evidence, $smokeDir | Out-Null
$sysHash = (Get-FileHash (Join-Path $Root 'driver\package\NLMatrixAudio.sys') -Algorithm SHA256).Hash
$pkgGuest = 'C:\NLMatrix\abi-v3'

function Get-GuestText([string]$name, [string]$command, [int]$timeoutSec) {
    $out = NL-Exec $command ("C:\NLMatrix\" + $name + ".log") $timeoutSec
    $out | Set-Content -Path (Join-Path $evidence $name) -Encoding UTF8
    return $out
}

function Get-GuestBootLine() {
    $out = NL-Exec 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\NLMatrix\guest-check-state.ps1' 'C:\NLMatrix\boot.log' 120
    return (($out -split [Environment]::NewLine) | Where-Object { $_ -match 'lastBootUpTime=' } | Select-Object -First 1)
}

function Wait-GuestAlive([int]$minutes) {
    $deadline = (Get-Date).AddMinutes($minutes)
    while ((Get-Date) -lt $deadline) {
        if ((NL-Run 'cmd /c echo alive') -eq 0) { return $true }
        Start-Sleep -Seconds 6
    }
    return $false
}

"=== [1/10] VM aliveness check ==="
$alive = NL-Exec 'echo ALIVE' 'C:\NLMatrix\alive.log' 60
if ($alive -notmatch 'ALIVE') { throw "VM not responsive: $alive" }
"VM responsive"

"=== [2/10] guest package directory ==="
NL-Run 'cmd /c if not exist C:\NLMatrix\abi-v3 mkdir C:\NLMatrix\abi-v3' | Out-Null

"=== [3/10] copy package + scripts + probe ==="
$files = @(
    @{ Host = (Join-Path $Root 'driver\package\ComponentizedAudioSample.inf'); Guest = "$pkgGuest\ComponentizedAudioSample.inf" },
    @{ Host = (Join-Path $Root 'driver\package\NLMatrixAudio.sys');            Guest = "$pkgGuest\NLMatrixAudio.sys" },
    @{ Host = (Join-Path $Root 'driver\package\sysvad.cat');                   Guest = "$pkgGuest\sysvad.cat" },
    @{ Host = (Join-Path $Root 'driver\package\NLMatrixTest.cer');             Guest = "$pkgGuest\NLMatrixTest.cer" },
    @{ Host = (Join-Path $Root 'driver\package\devcon.exe');                   Guest = "$pkgGuest\devcon.exe" },
    @{ Host = (Join-Path $Root 'driver\test\bin\NLMatrixTransportProbe.exe');  Guest = "$pkgGuest\NLMatrixTransportProbe.exe" },
    @{ Host = (Join-Path $Root 'driver\test\guest-check-state.ps1');           Guest = 'C:\NLMatrix\guest-check-state.ps1' }
)
foreach ($f in $files) {
    $h = NL-CopyTo $f.Host $f.Guest
    "copied " + (Split-Path -Leaf $f.Host) + " sha256=" + $h
}
foreach ($name in @('guest-install-v3.ps1','guest-verify-v3.ps1','guest-smoke-v3.ps1','guest-idle-check.ps1')) {
    $text = Get-Content (Join-Path $Root ('driver\test\' + $name)) -Raw
    $text = $text.Replace('__EXPECTED_SYS_SHA256__', $sysHash)
    $tmp = Join-Path $env:TEMP $name
    Set-Content -Path $tmp -Value $text -Encoding UTF8
    NL-CopyTo $tmp ("$pkgGuest\" + $name) | Out-Null
    Remove-Item $tmp -Force
}
"scripts copied (expected sys hash = $sysHash)"

"=== [4/10] install (pnputil add-driver /install + devcon update) ==="
$installOut = Get-GuestText 'install-v3.txt' 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\NLMatrix\abi-v3\guest-install-v3.ps1' 300
if ([string]::IsNullOrWhiteSpace($installOut)) { throw "install script produced no output" }
Write-Host $installOut

if ($installOut -match 'REBOOT_REQUIRED') {
    "=== [5/10] reboot to activate the new driver ==="
    $preBoot = Get-GuestBootLine
    "pre-reboot $preBoot"
    $rc = NL-Run 'cmd /c shutdown /r /t 10 /f'
    "guest shutdown rc=$rc; waiting for the guest to restart"
    Start-Sleep -Seconds 30
    $came = Wait-GuestAlive 5
    if (-not $came) {
        "guest did not return from guest shutdown; falling back to vmrun reset"
        & $Vmrun -T ws reset $Vmx | Out-Null
        Start-Sleep -Seconds 30
        $came = Wait-GuestAlive 5
        if (-not $came) { throw "VM did not come back after vmrun reset" }
    }
    Start-Sleep -Seconds 10
    $postBoot = Get-GuestBootLine
    "post-reboot $postBoot"
    if ((-not $postBoot) -or ($postBoot -eq $preBoot)) {
        "guest boot time did NOT change; forcing vmrun reset"
        & $Vmrun -T ws reset $Vmx | Out-Null
        Start-Sleep -Seconds 35
        $came = Wait-GuestAlive 5
        if (-not $came) { throw "VM did not come back after forced reset" }
        Start-Sleep -Seconds 15
        $postBoot = Get-GuestBootLine
        "post-forced-reset $postBoot"
    }
    "guest responsive after reboot"
    for ($i = 0; $i -lt 15; $i++) {
        $aud = NL-Exec 'powershell -NoProfile -Command "(Get-Service Audiosrv).Status"' 'C:\NLMatrix\audiosrv.log' 60
        if ($aud -match 'Running') { "Audiosrv=Running"; break }
        Start-Sleep -Seconds 5
    }
    Start-Sleep -Seconds 10
} else {
    "=== [5/10] no reboot required ==="
}

"=== [6/10] verify (loaded hash, endpoints, ABI v3, Focusrite) ==="
$verifyOut = Get-GuestText 'verify-v3.txt' 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\NLMatrix\abi-v3\guest-verify-v3.ps1' 240
if ([string]::IsNullOrWhiteSpace($verifyOut)) { throw "verify script produced no output" }
Write-Host $verifyOut

"=== [7/10] capture smoke test (target readback + writer -> NL Mic 1 record) ==="
$smokeOut = Get-GuestText 'smoke-v3.txt' 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\NLMatrix\abi-v3\guest-smoke-v3.ps1' 300
if ([string]::IsNullOrWhiteSpace($smokeOut)) { throw "smoke script produced no output" }
Write-Host $smokeOut

"=== [8/10] idle check (VM must be left idle for vm-harness) ==="
$idleOut = Get-GuestText 'idle-v3.txt' 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\NLMatrix\abi-v3\guest-idle-check.ps1' 120
Write-Host $idleOut

"=== [9/10] fetch smoke artifacts ==="
foreach ($extra in @('capture_smoke.wav','rec_smoke.log','rec_smoke_err.log','writer_ep0.log','writer_ep1.log','monitor_ep0.log')) {
    try {
        $local = Join-Path $smokeDir $extra
        $h = NL-CopyFrom ("C:\NLMatrix\abi-v3\" + $extra) $local
        "fetched $extra sha256=$h size=" + (Get-Item $local).Length
    } catch { "WARNING: could not fetch $extra : $_" }
}

"=== [10/10] summary ==="
"driver sys sha256: $sysHash"
if ($verifyOut -match 'DRIVER_HASH_MATCH=YES') { "DRIVER_HASH_MATCH=YES" } else { "DRIVER_HASH_MATCH=NO" }
if ($verifyOut -match 'ABIV3_OK') { "ABIV3_OK" } else { "ABIV3_FAIL" }
($verifyOut -split [Environment]::NewLine) | Where-Object { $_ -match '^(micEndpointsOK|RESULT|ENDPOINT|ABIV3|DRIVER_HASH_MATCH|FOCUSRITE)' } | ForEach-Object { $_ }
"=== DONE ==="
