param(
    [string]$Configuration = "Release",
    [switch]$Dev
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $PSScriptRoot "build"
$payload = Join-Path $out "payload"
$app = Join-Path $payload "app"
$drv = Join-Path $payload "drv"
$csc = "D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe"
if (-not (Test-Path $csc)) { throw "csc.exe not found: $csc" }

Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $app, $drv | Out-Null

$ui = Join-Path $root "app\build\NLMatrixUI_artefacts\$Configuration\NLMatrixUI.exe"
$router = Join-Path $root "app\build\NLMatrixRouter_artefacts\$Configuration\NLMatrixRouter.exe"
$package = Join-Path $root "driver\package"
if (-not (Test-Path $ui)) { throw "Build NLMatrixUI first: $ui" }
if (-not (Test-Path $router)) { throw "Build NLMatrixRouter first: $router" }

Copy-Item $ui $app -Force
Copy-Item $router $app -Force
Copy-Item (Join-Path $package "NLMatrixAudio.sys") $drv -Force
Copy-Item (Join-Path $package "ComponentizedAudioSample.inf") $drv -Force
Copy-Item (Join-Path $package "sysvad.cat") $drv -Force
Copy-Item (Join-Path $package "NLMatrixTest.cer") $drv -Force
Copy-Item (Join-Path $package "devcon.exe") $drv -Force
Copy-Item (Join-Path $package "devcon.exe") $app -Force

$refs = @(
    "/r:System.dll", "/r:System.Core.dll", "/r:System.Windows.Forms.dll",
    "/r:System.IO.Compression.dll", "/r:System.IO.Compression.FileSystem.dll"
)

$uninstallerExe = Join-Path $app "NLMatrixUninstall.exe"
$manifest = Join-Path $PSScriptRoot $(if ($Dev) { "dev.manifest" } else { "installer.manifest" })
$icon = Join-Path $root "assets\nlmatrix_icon.ico"
$uninstallerSource = Join-Path $PSScriptRoot "NLMatrixUninstaller.cs"
$uninstallerArgs = @('/nologo', '/target:winexe', '/platform:x64', '/optimize+',
    ('/out:"' + $uninstallerExe + '"'),
    ('/win32manifest:"' + $manifest + '"'),
    ('/win32icon:"' + $icon + '"'),
    $uninstallerSource) + $refs
& $csc $uninstallerArgs
if ($LASTEXITCODE -ne 0) { throw "uninstaller build failed" }

$zip = Join-Path $out "payload.zip"
Compress-Archive -Path $payload -DestinationPath $zip -CompressionLevel Optimal

$installerExe = Join-Path $out $(if ($Dev) { "NLMatrixSetup-dev.exe" } else { "NLMatrixSetup.exe" })
$installerSource = Join-Path $PSScriptRoot "NLMatrixInstaller.cs"
$installerArgs = @('/nologo', '/target:winexe', '/platform:x64', '/optimize+',
    ('/out:"' + $installerExe + '"'),
    ('/resource:"' + $zip + '",payload.zip'),
    ('/win32manifest:"' + $manifest + '"'),
    ('/win32icon:"' + $icon + '"'),
    $installerSource) + $refs
& $csc $installerArgs
if ($LASTEXITCODE -ne 0) { throw "installer build failed" }

Write-Host "Installer: $installerExe"
Get-Item $installerExe | Select-Object FullName, Length
