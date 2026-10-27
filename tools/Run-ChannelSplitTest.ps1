
#requires -Version 5.1
<#
Run-ChannelSplitTest.ps1 - Scarlett Solo independent output-channel test.

Modes:
  Validate       Parse every channel-split config with the frozen FINAL5 router.
  Deterministic  Run the virtual-endpoint matrix in the VM (no Focusrite needed):
                 two tones -> independent NL Mic destinations, mute/gain/swap
                 and bus duplication, then analyse the captures.
  Hardware       Validate the Focusrite configs and print the physical-PC
                 procedure (analog loopback of Output 1 and Output 2).
                 Hardware mode is NOT executed on the VM: the VM USB passthrough
                 is known broken and must not be retried indefinitely.

The deterministic captures are a software observation of the router outputs.
They are not an analog loopback and are not proof of the Apollo driver path.
#>
[CmdletBinding()]
param(
    [ValidateSet('Validate','Deterministic','Hardware')]
    [string]$Mode = 'Validate',
    [string]$RouterExe = '',
    [string]$RecorderExe = '',
    [string]$ConfigDir = '',
    [string]$OutRoot = '',
    [string]$PythonExe = 'C:\Users\Nicola\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
)

$ErrorActionPreference = 'Stop'
$script:Workspace = Split-Path $PSScriptRoot -Parent
if (-not $RouterExe) { $RouterExe = Join-Path $script:Workspace 'app\build\NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe' }
if (-not $RecorderExe) { $RecorderExe = Join-Path $script:Workspace 'app\build\NLMatrixDriverAudioTest_artefacts\Release\NLMatrixDriverAudioTest.exe' }
if (-not $ConfigDir) { $ConfigDir = Join-Path $script:Workspace 'app\profiles\tests\channel_split' }
if (-not $OutRoot) { $OutRoot = Join-Path $script:Workspace 'vm\channel_split_evidence' }
$analyzer = Join-Path $script:Workspace 'tools\analyze_channel_split.py'

$focusConfigs = @(
    'focus_direct.json',
    'focus_direct_mute_out1.json',
    'focus_direct_gain_out2.json',
    'focus_direct_swap.json',
    'focus_bus_split_monitor.json'
)
$virtualConfigs = @(
    'virtual_direct.json',
    'virtual_direct_mute.json',
    'virtual_direct_gain.json',
    'virtual_direct_swap.json',
    'virtual_bus_split_monitor.json'
)

function Invoke-ValidateConfigs {
    $all = @($focusConfigs + $virtualConfigs)
    $failed = 0
    foreach ($name in $all) {
        $path = Join-Path $ConfigDir $name
        if (-not (Test-Path -LiteralPath $path)) {
            Write-Output ('FAIL missing config: ' + $name)
            $failed++
            continue
        }
        $out = & $RouterExe --config $path --validate 2>&1
        $code = $LASTEXITCODE
        $tail = (($out | Select-Object -Last 3) -join ' | ')
        if ($code -ne 0) {
            Write-Output ('FAIL ' + $name + ' exit=' + $code + ' :: ' + $tail)
            $failed++
        } else {
            Write-Output ('PASS ' + $name + ' :: ' + $tail)
        }
    }
    if ($failed -gt 0) { throw ('config validation failed for ' + $failed + ' file(s)') }
}

function Get-RecordingInput([int]$mic) {
    return ('NL Mic ' + $mic + ' (NLMatrix Virtual Audio Device)')
}

function New-GuestScenarioScript {
    param([string]$Scenario, [object[]]$Recordings, [string]$GuestDir, [string]$GuestRouter, [string]$GuestRecorder, [string]$GuestConfig, [int]$Seconds = 8)
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('$ErrorActionPreference = "Stop"')
    $lines.Add("New-Item -ItemType Directory -Force -Path '" + $GuestDir + "' | Out-Null")
    foreach ($rec in $Recordings) {
        $wav = Join-Path $GuestDir ('mic' + $rec.mic + '.wav')
        $log = Join-Path $GuestDir ('mic' + $rec.mic + '_rec.log')
        $input = Get-RecordingInput $rec.mic
        $quotedInput = '"' + $input + '"'
        $lines.Add("Start-Process -FilePath '" + $GuestRecorder + "' -ArgumentList '--record','--input','" + $quotedInput + "','--seconds','" + ($Seconds + 2) + "','--out','" + $wav + "' -PassThru -WindowStyle Hidden -RedirectStandardOutput '" + $log + "' | Out-Null")
    }
    $lines.Add('Start-Sleep -Seconds 1')
    $tel = Join-Path $GuestDir 'router_telemetry.txt'
    $rlog = Join-Path $GuestDir 'router.log'
    $rerr = Join-Path $GuestDir 'router.err.log'
    $lines.Add("Start-Process -FilePath '" + $GuestRouter + "' -ArgumentList '--config','" + $GuestConfig + "','--seconds','" + $Seconds + "','--telemetry','" + $tel + "' -PassThru -WindowStyle Hidden -RedirectStandardOutput '" + $rlog + "' -RedirectStandardError '" + $rerr + "' | Out-Null")
    $lines.Add('Start-Sleep -Seconds ' + ($Seconds + 5))
    return ($lines -join [Environment]::NewLine)
}

function Invoke-Deterministic {
    . (Join-Path $PSScriptRoot 'vm\NLVM.ps1')
    $guestRoot = 'C:\NLMatrix\channel_split'
    $guestRouter = Join-Path $guestRoot 'NLMatrixRouter.exe'
    $guestRecorder = Join-Path $guestRoot 'NLMatrixDriverAudioTest.exe'
    NL-Exec ('if not exist "' + $guestRoot + '" mkdir "' + $guestRoot + '"') -TimeoutSec 60 | Out-Null
    NL-CopyTo $RouterExe $guestRouter | Out-Null
    NL-CopyTo $RecorderExe $guestRecorder | Out-Null
    foreach ($name in ($focusConfigs + $virtualConfigs)) {
        NL-CopyTo (Join-Path $ConfigDir $name) (Join-Path $guestRoot $name) | Out-Null
    }
    New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null

    $scenarios = @(
        @{ name = 'virtual_direct'; config = 'virtual_direct.json'; recs = @(@{ mic = 1; expect = '600' }, @{ mic = 2; expect = '900' }) },
        @{ name = 'virtual_direct_mute'; config = 'virtual_direct_mute.json'; recs = @(@{ mic = 1; expect = '600'; expectSilent = $true }, @{ mic = 2; expect = '900' }) },
        @{ name = 'virtual_direct_gain'; config = 'virtual_direct_gain.json'; recs = @(@{ mic = 1; expect = '600' }, @{ mic = 2; expect = '900'; expectGainDb = -12 }) },
        @{ name = 'virtual_direct_swap'; config = 'virtual_direct_swap.json'; recs = @(@{ mic = 1; expect = '900' }, @{ mic = 2; expect = '600' }) },
        @{ name = 'virtual_bus'; config = 'virtual_bus_split_monitor.json'; recs = @(@{ mic = 1; expect = '600' }, @{ mic = 2; expect = '900' }, @{ mic = 3; expect = '900' }) }
    )

    $baseRms = @{}
    $failed = 0
    foreach ($s in $scenarios) {
        $outDir = Join-Path $OutRoot $s.name
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
        $guestDir = Join-Path $guestRoot $s.name
        $guestConfig = Join-Path $guestRoot $s.config
        $guestScript = New-GuestScenarioScript -Scenario $s.name -Recordings $s.recs -GuestDir $guestDir -GuestRouter $guestRouter -GuestRecorder $guestRecorder -GuestConfig $guestConfig
        $hostScript = Join-Path $env:TEMP ('nlmatrix-channel-split-' + $s.name + '.ps1')
        Set-Content -LiteralPath $hostScript -Value $guestScript -Encoding ASCII
        NL-CopyTo $hostScript (Join-Path $guestRoot ($s.name + '.ps1')) | Out-Null
        Write-Output ('--- ' + $s.name + ' ---')
        $out = NL-Exec ('powershell -NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $guestRoot ($s.name + '.ps1')) + '"') -TimeoutSec 120
        Write-Output ($out | Select-Object -Last 6)
        Start-Sleep -Seconds 2
        foreach ($rec in $s.recs) {
            $gWav = Join-Path $guestDir ('mic' + $rec.mic + '.wav')
            $hWav = Join-Path $outDir ('mic' + $rec.mic + '.wav')
            try { NL-CopyFrom $gWav $hWav | Out-Null } catch { Write-Output ('WARN no WAV ' + $gWav) }
            $gLog = Join-Path $guestDir ('mic' + $rec.mic + '_rec.log')
            try { NL-CopyFrom $gLog (Join-Path $outDir ('mic' + $rec.mic + '_rec.log')) | Out-Null } catch { }
        }
        foreach ($f in @('router.log','router.err.log','router_telemetry.txt')) {
            try { NL-CopyFrom (Join-Path $guestDir $f) (Join-Path $outDir $f) | Out-Null } catch { }
        }
        foreach ($rec in $s.recs) {
            $wav = Join-Path $outDir ('mic' + $rec.mic + '.wav')
            if (-not (Test-Path -LiteralPath $wav)) { Write-Output ('FAIL missing capture ' + $wav); $failed++; continue }
            $jsonPath = Join-Path $outDir ('mic' + $rec.mic + '.json')
            $null = & $PythonExe $analyzer --wav $wav --expect $rec.expect --json $jsonPath 2>&1
            $data = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
            $rms = [double]$data.rms_dbfs
            if ($rec.ContainsKey('expectGainDb')) {
                $key = [string]$rec.expect
                if ($baseRms.ContainsKey($key)) {
                    $ref = [double]$baseRms[$key]
                    Write-Output ('  mic' + $rec.mic + ' rms=' + $rms + ' dBFS (expect ' + $rec.expect + ' Hz, gain ' + $rec.expectGainDb + ' dB, reference ' + $ref + ')')
                    if ($rms -gt ($ref + [double]$rec.expectGainDb + 6.0)) { Write-Output '  FAIL gain not applied'; $failed++ }
                }
            } else {
                Write-Output ('  mic' + $rec.mic + ' rms=' + $rms + ' dBFS dominant=' + $data.dominant_hz + ' Hz expect=' + $rec.expect + ' Hz silent=' + $data.silent)
                if ($rec.ContainsKey('expectSilent') -and $rec.expectSilent) {
                    if (-not [bool]$data.silent -and $rms -gt -60.0) { Write-Output '  FAIL expected silence'; $failed++ }
                } else {
                    $dom = [double]$data.dominant_hz
                    if ($data.silent -or [math]::Abs($dom - [double]$rec.expect) -gt 8.0) { Write-Output '  FAIL tone/mapping'; $failed++ }
                    $key = [string]$rec.expect
                    if (-not $baseRms.ContainsKey($key)) { $baseRms[$key] = $rms }
                }
            }
        }
    }
    if ($failed -gt 0) { throw ('deterministic channel-split test failed: ' + $failed + ' check(s)') }
    Write-Output ('Deterministic channel-split matrix PASS. Evidence: ' + $OutRoot)
}

function Invoke-HardwareInfo {
    Write-Output ''
    Write-Output 'Physical-PC procedure (requires a working Focusrite USB ASIO path):'
    Write-Output '1. Start FINAL5:'
    Write-Output ('   ' + $RouterExe + ' --config ' + (Join-Path $ConfigDir 'focus_direct.json') + ' --seconds 20')
    Write-Output '2. Connect Scarlett Output 1 (L) to the Scarlett input (analog loopback, attenuate if needed).'
    Write-Output '3. Record 10 s from Analogue 1 + 2 (Focusrite USB Audio) and verify the 600 Hz tone.'
    Write-Output '4. Move the loopback to Output 2 (R); verify 900 Hz and absence of 600 Hz cross-talk.'
    Write-Output '5. Repeat with focus_direct_mute_out1.json (Output 1 silent), focus_direct_gain_out2.json (Output 2 -12 dB), focus_direct_swap.json (tones swapped).'
    Write-Output '6. Repeat the bus routing with focus_bus_split_monitor.json: bus A -> Output 1, bus B -> NL Mic 1 and Output 2, monitor bus -> independent HD-Audio device.'
    Write-Output 'Caveat: a software tap / METERS is not an analog loopback, and Focusrite is not proof of the Apollo driver path. Physical evidence must be captured before calling this PASS.'
}

Invoke-ValidateConfigs
switch ($Mode) {
    'Validate' { Write-Output 'Validation only: all channel-split configs are well-formed.' }
    'Deterministic' { Invoke-Deterministic }
    'Hardware' { Invoke-HardwareInfo }
}