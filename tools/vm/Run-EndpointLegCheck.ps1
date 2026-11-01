#requires -Version 5.1
<#
.SYNOPSIS
    Endpoint leg check: how many independent channels one Windows endpoint really carries.

.DESCRIPTION
    Runs inside the VMware guest and uses only the product binaries:

      1. NLMatrixRouter --endpoint-info --json <file>
         lists every WASAPI render/capture endpoint with the channel count of
         its shared-mode format (the count the engine really opens);

      2. NLMatrixRouter --leg-test "<endpoint>" --channels N --seconds S --json <file>
         writes a distinct sine per channel through that render endpoint and
         captures the same endpoint with the native WASAPI loopback, so a PASS
         means N independent channels arrived end to end through one endpoint
         with the right channel identity.

    Nothing is installed and no driver or Windows setting is touched. The host
    only orchestrates and reads the JSON verdict.

    Exit code: 0 when the leg test passes, 1 when it fails, 2 when the run
    cannot be performed (missing endpoint / copy failure).

.PARAMETER Endpoint
    Render endpoint name (or name fragment). Empty = the endpoint with the
    highest channel count reported by --endpoint-info.

.PARAMETER Channels
    Channels to test. 0 = automatic (min(4, endpoint channels)).

.PARAMETER Seconds
    Measurement window per leg test.

.EXAMPLE
    powershell -File tools\vm\Run-EndpointLegCheck.ps1
    powershell -File tools\vm\Run-EndpointLegCheck.ps1 -Endpoint "CABLE In 16 Ch" -Channels 10
#>
[CmdletBinding()]
param(
    [string]$Endpoint = '',
    [int]$Channels = 0,
    [double]$Seconds = 4,
    [string]$RouterExe = '',
    [string]$OutputDir = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'NLVM.ps1')

$script:Workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $RouterExe) {
    $RouterExe = Join-Path $script:Workspace 'app\build\NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe'
}
if (-not (Test-Path $RouterExe)) { throw "router executable not found: $RouterExe" }
if (-not $OutputDir) {
    $OutputDir = Join-Path $script:Workspace ('reports\endpoint_leg\' + (Get-Date -Format 'yyyy-MM-dd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$guestExe = 'C:\NLMatrixTools\NLMatrixRouter-leg.exe'
$guestEndpoints = 'C:\Windows\Temp\nl-endpoints.json'
$guestLeg = 'C:\Windows\Temp\nl-leg.json'

function Log([string]$text) {
    $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $text)
    Write-Host $line
    Add-Content -Path (Join-Path $OutputDir 'endpoint_leg_host.log') -Value $line -Encoding UTF8
}

Log 'preparing the guest'
NL-Run 'if not exist C:\NLMatrixTools mkdir C:\NLMatrixTools' | Out-Null
NL-Kill 'NLMatrixRouter-leg.exe' | Out-Null
$routerHash = NL-CopyTo $RouterExe $guestExe
Log "router copied (sha256 $routerHash)"

Log 'probing WASAPI endpoints'
$probeOutput = NL-Exec "$guestExe --endpoint-info --json $guestEndpoints" -TimeoutSec 180
Add-Content -Path (Join-Path $OutputDir 'endpoint_info.log') -Value $probeOutput -Encoding UTF8
$localEndpoints = Join-Path $OutputDir 'endpoints.json'
NL-CopyFrom $guestEndpoints $localEndpoints | Out-Null
$caps = Get-Content $localEndpoints -Raw | ConvertFrom-Json
$renderEndpoints = @($caps.endpoints | Where-Object { $_.kind -eq 'render' })
if ($renderEndpoints.Count -eq 0) {
    Log 'FAIL: no WASAPI render endpoint was reported'
    exit 2
}
foreach ($item in ($caps.endpoints | Sort-Object -Property channels -Descending)) {
    Log ("endpoint {0}: {1} ch, {2} Hz ({3})" -f $item.name, $item.channels, $item.sampleRate, $item.kind)
}

if ($Endpoint) {
    $target = $renderEndpoints | Where-Object { $_.name -like "*$Endpoint*" } | Select-Object -First 1
    if (-not $target) { Log "FAIL: no render endpoint matches '$Endpoint'"; exit 2 }
} else {
    $target = $renderEndpoints | Sort-Object -Property channels -Descending | Select-Object -First 1
}

$channels = if ($Channels -gt 0) { $Channels } else { [Math]::Min(4, [int]$target.channels) }
if ($channels -lt 1) { $channels = 1 }
if ($channels -gt [int]$target.channels) {
    Log ("FAIL: asked for {0} channels but '{1}' exposes {2}" -f $channels, $target.name, $target.channels)
    exit 2
}

Log ("leg test: '{0}' with {1} channel(s) for {2}s" -f $target.name, $channels, $Seconds)
$legOutput = NL-Exec ('"' + $guestExe + '" --leg-test "' + $target.name + '" --channels ' + $channels + ' --seconds ' + $Seconds + ' --json ' + $guestLeg) -TimeoutSec 240
Add-Content -Path (Join-Path $OutputDir 'leg_test.log') -Value $legOutput -Encoding UTF8
Write-Host $legOutput

$localLeg = Join-Path $OutputDir 'leg.json'
NL-CopyFrom $guestLeg $localLeg | Out-Null
$leg = Get-Content $localLeg -Raw | ConvertFrom-Json

$summary = [ordered]@{
    date          = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
    routerSha256  = $routerHash
    endpoint      = $leg.endpoint
    nativeChannels = $leg.nativeChannels
    testedChannels = $leg.testedChannels
    sampleRate    = $leg.sampleRate
    verdict       = $leg.verdict
    channels      = $leg.channels
    endpoints     = $caps.endpoints
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -Path (Join-Path $OutputDir 'endpoint_leg-summary.json') -Encoding UTF8

Log ("verdict: {0} - {1} of {2} channel(s) on '{3}'" -f $leg.verdict, $leg.testedChannels, $leg.nativeChannels, $leg.endpoint)
if ($leg.verdict -eq 'PASS') { exit 0 }
exit 1
