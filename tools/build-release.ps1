
param(
    [string]$Configuration = 'Release',
    [string]$OutputRoot = '',
    [switch]$SkipPortable
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $OutputRoot) { $OutputRoot = Join-Path $root 'dist' }
$csc = 'D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe'
if (-not (Test-Path $csc)) { throw "csc.exe not found: $csc" }
$router = Join-Path $root "app\build\NLMatrixRouter_artefacts\$Configuration\NLMatrixRouter.exe"
$ui = Join-Path $root "app\build\NLMatrixUI_artefacts\$Configuration\NLMatrixUI.exe"
if (-not (Test-Path $router)) { throw "Build NLMatrixRouter first: $router" }
if (-not (Test-Path $ui)) { throw "Build NLMatrixUI first: $ui" }

$buildInfo = (& $router --buildinfo 2>&1 | Select-Object -First 1).Trim()
$parts = $buildInfo -split '\s+'
$version = $parts[0]
$buildId = if ($parts.Count -gt 1) { $parts[1] } else { 'unknown' }
if (-not $version) { throw 'Cannot read version from router --buildinfo' }
$commit = (git -C $root rev-parse HEAD).Trim()
$shortCommit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = ((git -C $root status --porcelain) -join [Environment]::NewLine).Trim()
if ($dirty) { Write-Warning 'Working tree is dirty; release build is not reproducible from a clean commit.' }
$builtUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')

$stagingRoot = Join-Path $root 'installer\build\staging'
Remove-Item $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
$app = Join-Path $stagingRoot 'app'
$driver = Join-Path $stagingRoot 'driver'
$licenses = Join-Path $stagingRoot 'licenses'
$portable = Join-Path $stagingRoot 'portable'
New-Item -ItemType Directory -Force -Path $app, $driver, $licenses, $portable | Out-Null

Copy-Item $ui (Join-Path $app 'NLMatrixUI.exe') -Force
Copy-Item $router (Join-Path $app 'NLMatrixRouter.exe') -Force
Copy-Item (Join-Path $root 'installer\licenses\*') $licenses -Force
foreach ($name in @('NLMatrixAudio.sys','ComponentizedAudioSample.inf','sysvad.cat','NLMatrixTest.cer','devcon.exe')) {
    Copy-Item (Join-Path $root "driver\package\$name") (Join-Path $driver $name) -Force
}

$info = [ordered]@{
    product = 'NL Audio Matrix'
    version = $version
    buildId = $buildId
    commit = $commit
    shortCommit = $shortCommit
    configuration = $Configuration
    builtUtc = $builtUtc
    routerSha256 = (Get-FileHash $router -Algorithm SHA256).Hash.ToLowerInvariant()
    uiSha256 = (Get-FileHash $ui -Algorithm SHA256).Hash.ToLowerInvariant()
    driverSha256 = (Get-FileHash (Join-Path $driver 'NLMatrixAudio.sys') -Algorithm SHA256).Hash.ToLowerInvariant()
}
$info | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $stagingRoot 'BUILD_INFO.json') -Encoding UTF8
$versionText = @(
    "version=$version",
    "buildId=$buildId",
    "commit=$commit",
    "shortCommit=$shortCommit",
    "builtUtc=$builtUtc"
) -join [Environment]::NewLine
Set-Content (Join-Path $stagingRoot 'VERSION.txt') -Value ($versionText + [Environment]::NewLine) -Encoding ASCII

# Build the uninstaller into the installer app payload. The setup helpers are
# shared with the installer (NLSetupCommon.cs) and must be compiled into both.
$uninstallerExe = Join-Path $app 'NLMatrixUninstall.exe'
$manifest = Join-Path $root 'installer\installer.manifest'
$icon = Join-Path $root 'assets\nlmatrix_icon.ico'
$setupCommon = Join-Path $root 'installer\NLSetupCommon.cs'
$refs = @(
    '/r:System.dll', '/r:System.Core.dll', '/r:System.Windows.Forms.dll',
    '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll'
)
& $csc @('/nologo','/target:winexe','/platform:x64','/optimize+',
    ('/out:' + $uninstallerExe),
    ('/win32manifest:' + $manifest),
    ('/win32icon:' + $icon),
    $setupCommon,
    (Join-Path $root 'installer\NLMatrixUninstaller.cs')) @refs
if ($LASTEXITCODE -ne 0) { throw 'Uninstaller build failed' }

function Write-HashList([string]$baseDir, [string]$outFile) {
    $lines = New-Object System.Collections.Generic.List[string]
    Get-ChildItem $baseDir -Recurse -File | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($baseDir.Length).TrimStart('\\')
        $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $lines.Add($hash + '  ' + $rel)
    }
    Set-Content -Path $outFile -Value ($lines -join [Environment]::NewLine) -Encoding ASCII
}
Write-HashList $stagingRoot (Join-Path $stagingRoot 'SHA256SUMS.txt')

# Installer payload: app + driver + licenses + metadata.
$payload = Join-Path $stagingRoot 'payload'
New-Item -ItemType Directory -Force -Path $payload | Out-Null
Copy-Item $app (Join-Path $payload 'app') -Recurse -Force
Copy-Item $driver (Join-Path $payload 'driver') -Recurse -Force
Copy-Item $licenses (Join-Path $payload 'licenses') -Recurse -Force
foreach ($name in @('BUILD_INFO.json','VERSION.txt','SHA256SUMS.txt')) {
    Copy-Item (Join-Path $stagingRoot $name) (Join-Path $payload $name) -Force
}
$payloadZip = Join-Path $stagingRoot 'payload.zip'
Compress-Archive -Path $payload -DestinationPath $payloadZip -CompressionLevel Optimal

$releaseDir = Join-Path $OutputRoot $version
Remove-Item $releaseDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
$installerExe = Join-Path $releaseDir ("NL-Audio-Matrix-$version-VM-Setup.exe")
$installerArgs = @('/nologo','/target:winexe','/platform:x64','/optimize+',
    ('/out:' + $installerExe),
    ('/resource:' + $payloadZip + ',payload.zip'),
    ('/win32manifest:' + $manifest),
    ('/win32icon:' + $icon),
    $setupCommon,
    (Join-Path $root 'installer\NLMatrixInstaller.cs')) + $refs
& $csc $installerArgs
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed' }

# Portable user-mode package: no driver, certificate, devcon, installer or test tools.
Copy-Item $ui (Join-Path $portable 'NLMatrixUI.exe') -Force
Copy-Item $router (Join-Path $portable 'NLMatrixRouter.exe') -Force
Copy-Item $licenses (Join-Path $portable 'LICENSES') -Recurse -Force
Copy-Item (Join-Path $stagingRoot 'BUILD_INFO.json') (Join-Path $portable 'BUILD_INFO.json') -Force
Copy-Item (Join-Path $stagingRoot 'VERSION.txt') (Join-Path $portable 'VERSION.txt') -Force
$readme = @"
NL Audio Matrix $version — portable user-mode package
====================================================
Build ID: $buildId
Commit:   $commit

No kernel driver, test certificate, devcon or installer is included.
Run NLMatrixUI.exe; the GUI starts the router executable from the same folder.
The package is for Windows 10/11 x64 and does not install or modify drivers.
"@
Set-Content -Path (Join-Path $portable 'README.txt') -Value $readme -Encoding UTF8
Write-HashList $portable (Join-Path $portable 'SHA256SUMS.txt')
$portableZip = Join-Path $releaseDir ("NL-Audio-Matrix-$version-portable.zip")
if (Test-Path $portableZip) { Remove-Item $portableZip -Force }
Compress-Archive -Path (Join-Path $portable '*') -DestinationPath $portableZip -CompressionLevel Optimal

# Asset-level manifest for the GitHub pre-release.
$assetLines = New-Object System.Collections.Generic.List[string]
Get-ChildItem $releaseDir -File | Sort-Object Name | ForEach-Object {
    $assetLines.Add((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $_.Name)
}
Set-Content -Path (Join-Path $releaseDir 'SHA256SUMS.txt') -Value ($assetLines -join [Environment]::NewLine) -Encoding ASCII
Copy-Item (Join-Path $stagingRoot 'BUILD_INFO.json') (Join-Path $releaseDir 'BUILD_INFO.json') -Force

Write-Host ("Release directory: " + $releaseDir)
Get-ChildItem $releaseDir -File | Select-Object Name,Length | Format-Table -AutoSize
Get-Content (Join-Path $releaseDir 'SHA256SUMS.txt')