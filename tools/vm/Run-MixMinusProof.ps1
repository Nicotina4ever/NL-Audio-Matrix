#requires -Version 5.1
<#
.SYNOPSIS
    Run-MixMinusProof.ps1 - deterministic engine-level proof of mix-minus and bus
    routing, from the router's own METERS telemetry (no WAV analysis).

.DESCRIPTION
    Three short headless runs in the guest, all with test sources (no capture
    path involved, so A21/A22 are not touched):

      A  two independent test tones -> NL Mic 2 destination, no mix-minus
      B  same two tones, but the destination mix-minus excludes tone 2
      C  one tone -> internal bus -> NL Mic 2 destination

    The destination level comes from the last METERS line: with one source the
    peak is -12 dBFS, with two summed tones it is clearly higher; mix-minus must
    bring it back to the single-source level, and the bus must route a single
    source at the same level.  This is a synthetic pass/fail criterion, read once
    per run: no log grinding, no repeated attempts.
#>
[CmdletBinding()]
param(
    [string]$OutputDir = '',
    [int]$Seconds = 8,
    [double]$ExpectedSingleDb = -12.0,
    [double]$ToleranceDb = 3.0
)

$ErrorActionPreference = 'Stop'
$script:Workspace = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $PSScriptRoot 'NLVM.ps1')

$buildDir = Join-Path $script:Workspace 'app\build'
$hostRouter = Join-Path $buildDir 'NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe'
$guestRouter = 'C:\NLMatrix\NLMatrixRouter.exe'
$guestDir = 'C:\NLMatrixTools\mixminus'
$script:DestinationId = 'mm-dst'

if (-not $OutputDir) {
    $OutputDir = Join-Path $script:Workspace ('reports\mixminus\' + (Get-Date -Format 'yyyy-MM-dd'))
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$logPath = Join-Path $OutputDir 'mixminus_host.log'
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

function New-TestSource {
    param([string]$Id, [string]$Name, [double]$Frequency)
    return ('{{ "id": "{0}", "name": "{1}", "type": "test", "channels": [0], "enabled": true, "baseFrequency": {2}, "level": 0.25 }}' -f $Id, $Name, $Frequency)
}

function New-ConfigJson {
    param([string]$Name, [string]$Sources, [string]$Destinations, [string]$Buses, [string]$Routes)
    $device = '{ "id": "flex", "backend": "asio", "driver": "FlexASIO", "sampleRate": 48000, "buffer": 256, "clockMaster": true, "rateAuto": true }'
    return ('{{ "name": "{0}", "asio": "FlexASIO", "version": 3, "devices": [ {1} ], "buses": [ {2} ], "sources": [ {3} ], "destinations": [ {4} ], "routes": [ {5} ] }}' -f `
        $Name, $device, $Buses, $Sources, $Destinations, $Routes)
}

Assert-True (Test-Path $hostRouter) "router binary missing: $hostRouter"
$routerHash = (Get-FileHash $hostRouter -Algorithm SHA256).Hash.ToLowerInvariant()
Log "router sha256=$routerHash"

NL-Exec 'taskkill /F /IM NLMatrixRouter.exe 2>nul & echo CLEARED' -TimeoutSec 90 | Out-Null
NL-Exec "if not exist $guestDir mkdir $guestDir" | Out-Null
NL-CopyTo $hostRouter $guestRouter | Out-Null
Log 'router deployed to guest'

function Get-GuestDestinationDb {
    param([string]$runName, [string]$configText)
    $guestTelemetry = "$guestDir\$runName.jsonl"
    $localTelemetry = Join-Path $OutputDir "$runName.jsonl"
    NL-Exec "del /f /q `"$guestTelemetry`" 2>nul & echo CLEARED" -TimeoutSec 60 | Out-Null
    $configLocal = Join-Path $env:TEMP ("mixminus_" + $runName + ".json")
    $configGuest = "$guestDir\$runName.json"
    Set-Content -Path $configLocal -Value $configText -Encoding ASCII
    NL-CopyTo $configLocal $configGuest | Out-Null
    Remove-Item $configLocal -Force
    $out = NL-Exec ("$guestRouter --config $configGuest --seconds $Seconds --telemetry $guestTelemetry > $guestDir\$runName.out.log 2>&1 & echo ROUTER_DONE") -TimeoutSec ($Seconds + 90)
    if ($out -notmatch 'ROUTER_DONE') { throw "router run '$runName' did not finish" }
    NL-CopyFrom $guestTelemetry $localTelemetry | Out-Null
    $lines = @(Get-Content $localTelemetry -ErrorAction Stop | Where-Object { $_ -match '^METERS ' })
    Assert-True ($lines.Count -gt 0) "no METERS telemetry for '$runName'"
    $json = $lines[$lines.Count - 1] -replace '^METERS ', ''
    $last = $json | ConvertFrom-Json
    $destNode = $last.nodes | Where-Object { $_.kind -eq 'destination' -and $_.id -eq $script:DestinationId } | Select-Object -First 1
    Assert-True ($null -ne $destNode) "destination node '$script:DestinationId' missing in telemetry '$runName'"
    $level = [double] $destNode.l
    if ($null -ne $destNode.r) {
        $right = [double] $destNode.r
        if ($right -gt $level) { $level = $right }
    }
    Log ("{0}: destination level {1:N1} dBFS (online={2})" -f $runName, $level, $destNode.online)
    return $level
}

$sourceTwo = (New-TestSource 't1' 'tone 1000' 1000) + ', ' + (New-TestSource 't2' 'tone 1040' 1040)
$sourceOne = New-TestSource 't1' 'tone 1000' 1000
$destinationPlain = '{ "id": "mm-dst", "name": "NL Mic 2", "type": "driver", "driverEndpoint": 1, "channels": [0], "enabled": true }'
$destinationMixMinus = '{ "id": "mm-dst", "name": "NL Mic 2", "type": "driver", "driverEndpoint": 1, "channels": [0], "enabled": true, "mixMinus": [ "t2" ] }'
$routesBoth = '{ "source": "t1", "destination": "mm-dst", "gainDb": 0 }, { "source": "t2", "destination": "mm-dst", "gainDb": 0 }'
$routesBus = '{ "source": "t1", "destination": "bus-1", "gainDb": 0 }, { "source": "bus-src-1", "destination": "mm-dst", "gainDb": 0 }'

$configA = New-ConfigJson 'mixminus A: two tones, no mix-minus' $sourceTwo $destinationPlain '' $routesBoth
$configB = New-ConfigJson 'mixminus B: tone 2 excluded' $sourceTwo $destinationMixMinus '' $routesBoth
$configC = New-ConfigJson 'mixminus C: single tone through bus' ($sourceOne + ', { "id": "bus-src-1", "name": "Bus 1", "type": "bus", "bus": "bus-1", "channels": [0, 1], "enabled": true }') $destinationPlain '{ "id": "bus-1", "name": "Bus 1", "enabled": true, "gainDb": 0, "channels": 2 }' $routesBus

$levelBoth = Get-GuestDestinationDb 'a_both' $configA
$levelMixMinus = Get-GuestDestinationDb 'b_mixminus' $configB
$levelBus = Get-GuestDestinationDb 'c_bus' $configC

$twoTonesLouder = ($levelBoth - $levelMixMinus) -ge 3.0
$mixMinusNearSingle = ([math]::Abs($levelMixMinus - $ExpectedSingleDb)) -le $ToleranceDb
$busNearSingle = ([math]::Abs($levelBus - $ExpectedSingleDb)) -le $ToleranceDb

$summary = [ordered]@{
    suite = 'mixminus'
    routerSha256 = $routerHash
    seconds = $Seconds
    levelsDb = [ordered]@{ bothSources = $levelBoth; mixMinus = $levelMixMinus; bus = $levelBus }
    checks = [ordered]@{
        twoSourcesLouderThanOne = $twoTonesLouder
        mixMinusAttenuates = $twoTonesLouder
        mixMinusSingleSourceLevel = $mixMinusNearSingle
        busSingleSourceLevel = $busNearSingle
    }
    evidenceDir = $OutputDir
}
$allOk = ($summary.checks.Values | Where-Object { -not $_ }).Count -eq 0
$summary['verdict'] = if ($allOk) { 'PASS' } else { 'FAIL' }
$summary | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputDir 'mixminus_summary.json') -Encoding UTF8
Log ("checks: " + (($summary.checks.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '))
Log "evidence: $OutputDir"
if ($allOk) { Log 'VERDICT: PASS - engine mix-minus and bus routing verified from METERS'; exit 0 }
Log 'VERDICT: FAIL'
exit 1




