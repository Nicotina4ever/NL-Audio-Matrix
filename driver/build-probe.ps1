# build-probe.ps1 - build the minimal ABI v3 transport probe (host x64 MSVC).
param(
    [string]$OutDir = '',
    [string]$TestDir = ''
)
$ErrorActionPreference = 'Stop'
if ($TestDir -eq '') {
    $scriptFile = $MyInvocation.MyCommand.Path
    if (-not $scriptFile) { $scriptFile = $MyInvocation.MyCommand.Source }
    $scriptDir = if ($scriptFile) { Split-Path -Parent $scriptFile }
                 elseif ($PSScriptRoot) { $PSScriptRoot }
                 else { Join-Path (Get-Location).Path 'driver' }
    $TestDir = Join-Path $scriptDir 'test'
}
if (-not (Test-Path (Join-Path $TestDir 'nltransport_probe.cpp'))) {
    throw "nltransport_probe.cpp not found in '$TestDir'"
}
$driverDir = Split-Path -Parent $TestDir
if ($OutDir -eq '') { $OutDir = Join-Path $TestDir 'bin' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$vsRoot = 'D:\Program Files\Microsoft Visual Studio\2022\Community'
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    $candidate = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio', 'D:\Program Files\Microsoft Visual Studio' -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
        Where-Object { $_.Name -in @('Community','Professional','Enterprise','BuildTools') } |
        ForEach-Object { Join-Path $_.FullName 'VC\Auxiliary\Build\vcvars64.bat' } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($candidate) { $vcvars = $candidate }
}
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found (VS 2022 C++ toolchain required)" }

$source = Join-Path $TestDir 'nltransport_probe.cpp'
$exe = Join-Path $OutDir 'NLMatrixTransportProbe.exe'
Push-Location $OutDir
try {
    $cmdLine = 'call "' + $vcvars + '" >nul && cl /nologo /EHsc /O2 /W3 /std:c++17 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /I"' + $driverDir + '" "' + $source + '" /Fe:"' + $exe + '" /link /SUBSYSTEM:CONSOLE'
    cmd /c $cmdLine
    if ($LASTEXITCODE -ne 0) { throw "probe build failed ($LASTEXITCODE)" }
}
finally {
    Pop-Location
}
Write-Host "Probe ready: $exe"
Get-Item $exe | Select-Object FullName, Length, LastWriteTime
