param(
    [string]$OutputDirectory = (Join-Path ([Environment]::GetFolderPath("MyDocuments")) "NL Audio Matrix Diagnostics"),
    [switch]$InspectAsio,
    [string]$AsioDevice = ""
)

# NL Audio Matrix - non-invasive Windows audio / UAD-ASIO diagnostic.
# Read-only: does not install drivers, change Windows audio, UAD Console,
# REAPER or any routing. ASIO inspection is opt-in because opening an ASIO
# driver can initialize it; use -InspectAsio only with REAPER closed.

$ErrorActionPreference = "Continue"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$log = Join-Path $OutputDirectory ("NL-Audio-diagnostic-" + $stamp + ".log")
$json = Join-Path $OutputDirectory ("NL-Audio-diagnostic-" + $stamp + ".json")
$transcript = Start-Transcript -Path $log -Force

function Section([string]$title) {
    Write-Host ""
    Write-Host ("=== " + $title + " ===")
}

$result = [ordered]@{
    tool = "NL Audio Matrix diagnostic"
    version = "0.4.0-alpha"
    generated = (Get-Date).ToString("o")
    computer = $env:COMPUTERNAME
    user = $env:USERNAME
    inspect_asio = [bool]$InspectAsio
    asio = $null
    audio_devices = @()
    audio_endpoints = @()
    uad = [ordered]@{}
    reaper = [ordered]@{}
    obs_asio_reference = [ordered]@{}
    notes = @(
        "Read-only diagnostic. No driver install, no UAD Console change, no routing change.",
        "ASIO inspection is opt-in and may initialize the driver; close REAPER first."
    )
}

try {
    Section "System"
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
    $cs = Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue
    $cpu = Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1
    [pscustomobject]@{
        OS = $os.Caption
        Version = $os.Version
        Build = $os.BuildNumber
        Architecture = $os.OSArchitecture
        Computer = $cs.Name
        Manufacturer = $cs.Manufacturer
        Model = $cs.Model
        CPU = $cpu.Name
        LogicalProcessors = $cs.NumberOfLogicalProcessors
        RAM_GB = [math]::Round($cs.TotalPhysicalMemory / 1GB, 1)
    } | Format-List
    $result.system = [ordered]@{
        os = $os.Caption; version = $os.Version; build = $os.BuildNumber
        architecture = $os.OSArchitecture; model = $cs.Model
        manufacturer = $cs.Manufacturer; cpu = $cpu.Name
        logical_processors = $cs.NumberOfLogicalProcessors
        ram_gb = [math]::Round($cs.TotalPhysicalMemory / 1GB, 1)
    }

    Section "Audio devices (PnP)"
    $devices = Get-PnpDevice -Class Media, AudioEndpoint -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName } |
        Sort-Object Class, FriendlyName
    $devices | Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize
    $result.audio_devices = @($devices | ForEach-Object {
        [ordered]@{ status = "$($_.Status)"; class = "$($_.Class)"; name = "$($_.FriendlyName)"; instance = "$($_.InstanceId)" }
    })

    Section "Universal Audio / UAD"
    $uadDevices = $devices | Where-Object { $_.FriendlyName -match "Universal Audio|Apollo|UAD" }
    $uadProcesses = Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -match "UAD|Universal|Console" } |
        Select-Object ProcessName, Id, Path
    $uadPaths = @(
        "C:\Program Files\Universal Audio",
        "C:\Program Files (x86)\Universal Audio",
        "$env:ProgramFiles\Universal Audio"
    ) | Where-Object { Test-Path $_ }
    $uadDevices | Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize
    $uadProcesses | Format-Table -AutoSize
    Write-Host ("UAD install paths: " + ($(if ($uadPaths) { $uadPaths -join "; " } else { "not found" })))
    $result.uad = [ordered]@{
        devices = @($uadDevices | ForEach-Object { [ordered]@{ name = "$($_.FriendlyName)"; status = "$($_.Status)"; instance = "$($_.InstanceId)" } })
        processes = @($uadProcesses | ForEach-Object { [ordered]@{ name = $_.ProcessName; id = $_.Id; path = "$($_.Path)" } })
        install_paths = @($uadPaths)
        console_modified = $false
    }

    Section "REAPER"
    $reaper = Get-Process reaper -ErrorAction SilentlyContinue | Select-Object -First 1
    $reaperPath = @(
        "$env:ProgramFiles\REAPER (x64)\reaper.exe",
        "C:\Program Files\REAPER (x64)\reaper.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($reaper) {
        Write-Host ("REAPER running: PID " + $reaper.Id)
    } else {
        Write-Host "REAPER not running."
    }
    Write-Host ("REAPER exe: " + $(if ($reaperPath) { $reaperPath } else { "not found" }))
    $result.reaper = [ordered]@{
        running = [bool]$reaper
        pid = $(if ($reaper) { $reaper.Id } else { $null })
        executable = $(if ($reaperPath) { $reaperPath } else { $null })
        note = "REAPER is not modified by this diagnostic."
    }

    Section "OBS-ASIO reference (optional)"
    $obsExe = @(
        "$env:ProgramFiles\obs-studio\bin\64bit\obs64.exe",
        "C:\Program Files\obs-studio\bin\64bit\obs64.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    Write-Host ("OBS detected: " + $(if ($obsExe) { $obsExe } else { "not installed" }))
    Write-Host "OBS-ASIO is a behavioral reference only; no code or dependency is included."
    $result.obs_asio_reference = [ordered]@{
        obs_installed = [bool]$obsExe
        obs_executable = $(if ($obsExe) { $obsExe } else { $null })
        dependency = $false
    }

    Section "NLMatrixProbe"
    $probe = Join-Path $PSScriptRoot "NLMatrixProbe.exe"
    if (-not (Test-Path $probe)) {
        $probe = Join-Path (Split-Path -Parent $PSScriptRoot) "NLMatrixProbe.exe"
    }
    if (-not (Test-Path $probe)) {
        $probe = Join-Path (Split-Path -Parent $PSScriptRoot) "portable\NLMatrixProbe.exe"
    }
    if (Test-Path $probe) {
        Write-Host ("Probe: " + $probe)
        Write-Host "Running read-only ASIO enumeration..."
        $list = & $probe --list 2>&1 | Out-String
        Write-Host $list
        $result.asio = [ordered]@{
            probe = $probe
            list_output = $list.Trim()
            inspect_requested = [bool]$InspectAsio
            inspect_output = $null
        }
        if ($InspectAsio) {
            Write-Host "ASIO inspection is opt-in; it may initialize the driver."
            $args = @("--inspect")
            if ($AsioDevice) { $args += @("--device", $AsioDevice) }
            $inspect = & $probe @args 2>&1 | Out-String
            Write-Host $inspect
            $result.asio.inspect_output = $inspect.Trim()
        }
    } else {
        Write-Host "NLMatrixProbe.exe not found next to this script."
        $result.asio = [ordered]@{ probe = $null; error = "NLMatrixProbe.exe not found" }
    }

    Section "Summary"
    Write-Host "No changes were made to UAD Console, Windows audio, REAPER or routing."
    Write-Host ("Log: " + $log)
    $result | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 $json
    Write-Host ("JSON: " + $json)
}
catch {
    Write-Host ("Diagnostic error: " + $_.Exception.Message)
    $result.error = $_.Exception.Message
    $result | ConvertTo-Json -Depth 12 | Set-Content -Encoding UTF8 $json
}
finally {
    Stop-Transcript | Out-Null
}
