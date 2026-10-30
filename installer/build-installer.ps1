param(
    [string]$Configuration = "Release",
    [switch]$Dev
)

# Builds the NL Audio Matrix installer/uninstaller pair without producing a
# release. The staging layout mirrors tools/build-release.ps1 so the same
# payload verification (SHA256SUMS.txt) works in both builds.
#
# Output (installer/build):
#   staging/payload/app        NLMatrixUI.exe, NLMatrixRouter.exe, NLMatrixUninstall.exe
#   staging/payload/driver     NLMatrixAudio.sys, INF, CAT, test CER, devcon.exe
#   staging/payload/licenses   third-party licences
#   staging/payload/VERSION.txt, BUILD_INFO.json, SHA256SUMS.txt
#   NLMatrixSetup[-dev].exe    the setup executable with payload.zip embedded
#   NLMatrixUninstall.exe      standalone uninstaller (also present in the payload)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $PSScriptRoot "build"
$staging = Join-Path $out "staging"
$app = Join-Path $staging "app"
$driver = Join-Path $staging "driver"
$licenses = Join-Path $staging "licenses"
$payload = Join-Path $staging "payload"
$csc = "D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe"
if (-not (Test-Path $csc)) { throw "csc.exe not found: $csc" }

$ui = Join-Path $root "app\build\NLMatrixUI_artefacts\$Configuration\NLMatrixUI.exe"
$router = Join-Path $root "app\build\NLMatrixRouter_artefacts\$Configuration\NLMatrixRouter.exe"
$package = Join-Path $root "driver\package"
if (-not (Test-Path $ui)) { throw "Build NLMatrixUI first: $ui" }
if (-not (Test-Path $router)) { throw "Build NLMatrixRouter first: $router" }
if (-not (Test-Path $package)) { throw "Driver package not found: $package" }

Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $app, $driver, $licenses | Out-Null

Copy-Item $ui (Join-Path $app "NLMatrixUI.exe") -Force
Copy-Item $router (Join-Path $app "NLMatrixRouter.exe") -Force
foreach ($name in @("NLMatrixAudio.sys", "ComponentizedAudioSample.inf", "sysvad.cat", "NLMatrixTest.cer", "devcon.exe")) {
    Copy-Item (Join-Path $package $name) (Join-Path $driver $name) -Force
}
Copy-Item (Join-Path $PSScriptRoot "licenses\*") $licenses -Force

# Build identity: prefer the router's own --buildinfo, fall back gracefully so a
# local setup build never fails only because git or the router is unavailable.
$buildInfo = ""
try {
    $buildInfo = (& $router --buildinfo 2>&1 | Select-Object -First 1)
    if ($null -ne $buildInfo) { $buildInfo = $buildInfo.Trim() }
} catch { $buildInfo = "" }
$parts = @($buildInfo -split '\s+')
$version = if ($parts.Count -gt 0 -and $parts[0]) { $parts[0] } else { "0.0.0-dev" }
$buildId = if ($parts.Count -gt 1 -and $parts[1]) { $parts[1] } else { Get-Date -Format "yyyyMMdd-HHmm" }
$commit = "unknown"
try {
    $gitHead = (git -C $root rev-parse HEAD 2>$null | Select-Object -First 1)
    if ($gitHead) { $commit = $gitHead.Trim() }
} catch { $commit = "unknown" }
$shortCommit = if ($commit -ne "unknown" -and $commit.Length -ge 7) { $commit.Substring(0, 7) } else { "unknown" }
$builtUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")

$info = [ordered]@{
    product      = "NL Audio Matrix"
    version      = $version
    buildId      = $buildId
    commit       = $commit
    shortCommit  = $shortCommit
    configuration = $Configuration
    builtUtc     = $builtUtc
    routerSha256 = (Get-FileHash $router -Algorithm SHA256).Hash.ToLowerInvariant()
    uiSha256     = (Get-FileHash $ui -Algorithm SHA256).Hash.ToLowerInvariant()
    driverSha256 = (Get-FileHash (Join-Path $driver "NLMatrixAudio.sys") -Algorithm SHA256).Hash.ToLowerInvariant()
}
$info | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $staging "BUILD_INFO.json") -Encoding UTF8
$versionText = @(
    "version=$version",
    "buildId=$buildId",
    "commit=$commit",
    "shortCommit=$shortCommit",
    "builtUtc=$builtUtc"
) -join [Environment]::NewLine
Set-Content (Join-Path $staging "VERSION.txt") -Value ($versionText + [Environment]::NewLine) -Encoding ASCII

$manifest = Join-Path $PSScriptRoot $(if ($Dev) { "dev.manifest" } else { "installer.manifest" })
$icon = Join-Path $root "assets\nlmatrix_icon.ico"
$commonSource = Join-Path $PSScriptRoot "NLSetupCommon.cs"
$uninstallerSource = Join-Path $PSScriptRoot "NLMatrixUninstaller.cs"
$installerSource = Join-Path $PSScriptRoot "NLMatrixInstaller.cs"
$diagnosticsSource = Join-Path $PSScriptRoot "NLSetupDiagnostics.cs"
$verifySource = Join-Path $PSScriptRoot "NLMatrixVerify.cs"
$uiSource = Join-Path $PSScriptRoot "NLSetupUI.cs"
$verifyManifest = Join-Path $PSScriptRoot "verify.manifest"
$refs = @(
    "/r:System.dll", "/r:System.Core.dll", "/r:System.Windows.Forms.dll",
    "/r:System.Drawing.dll", "/r:System.Security.dll",
    "/r:System.IO.Compression.dll", "/r:System.IO.Compression.FileSystem.dll"
)

# Uninstaller first: it is part of the application payload and must therefore be
# covered by SHA256SUMS.txt.
$uninstallerExe = Join-Path $app "NLMatrixUninstall.exe"
$uninstallerArgs = @(
    "/nologo", "/target:winexe", "/platform:x64", "/optimize+",
    ("/out:" + $uninstallerExe),
    ("/win32manifest:" + $manifest),
    ("/win32icon:" + $icon),
    $commonSource,
    $uninstallerSource
) + $refs
& $csc $uninstallerArgs
if ($LASTEXITCODE -ne 0) { throw "uninstaller build failed (csc exit $LASTEXITCODE)" }

# Read-only verifier installed with the application: RunOnce uses it after a
# reboot, so it must NOT require administrator (asInvoker manifest).
$verifyExe = Join-Path $app "NLMatrixVerify.exe"
$verifyArgs = @(
    "/nologo", "/target:winexe", "/platform:x64", "/optimize+",
    ("/out:" + $verifyExe),
    ("/win32manifest:" + $verifyManifest),
    ("/win32icon:" + $icon),
    $commonSource,
    $diagnosticsSource,
    $verifySource
) + $refs
& $csc $verifyArgs
if ($LASTEXITCODE -ne 0) { throw "verifier build failed (csc exit $LASTEXITCODE)" }

function Write-HashList([string]$baseDir, [string]$outFile) {
    $lines = New-Object System.Collections.Generic.List[string]
    Get-ChildItem $baseDir -Recurse -File | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($baseDir.Length).TrimStart('\\')
        $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $lines.Add($hash + '  ' + $rel)
    }
    Set-Content -Path $outFile -Value ($lines -join [Environment]::NewLine) -Encoding ASCII
}
Write-HashList $staging (Join-Path $staging "SHA256SUMS.txt")

# Installer payload: app + driver + licenses + metadata.
New-Item -ItemType Directory -Force -Path $payload | Out-Null
Copy-Item $app (Join-Path $payload "app") -Recurse -Force
Copy-Item $driver (Join-Path $payload "driver") -Recurse -Force
Copy-Item $licenses (Join-Path $payload "licenses") -Recurse -Force
foreach ($name in @("BUILD_INFO.json", "VERSION.txt", "SHA256SUMS.txt")) {
    Copy-Item (Join-Path $staging $name) (Join-Path $payload $name) -Force
}

# Never ship signing material: the package may carry the public test
# certificate (.cer) only, never a private key or a signing container.
$forbidden = Get-ChildItem $payload -Recurse -File | Where-Object {
    $_.Extension -in @('.pfx', '.pvk', '.snk', '.key')
}
if ($forbidden.Count -gt 0) {
    throw ("signing material must not be in the payload: " + (($forbidden | ForEach-Object { $_.Name }) -join ', '))
}
$payloadFiles = Get-ChildItem $payload -Recurse -File
Write-Host ("Payload files: " + $payloadFiles.Count + " (no private key material)")

$payloadZip = Join-Path $staging "payload.zip"
Compress-Archive -Path $payload -DestinationPath $payloadZip -CompressionLevel Optimal
Copy-Item $payloadZip (Join-Path $out "payload.zip") -Force

$installerExe = Join-Path $out $(if ($Dev) { "NLMatrixSetup-dev.exe" } else { "NLMatrixSetup.exe" })
$installerArgs = @(
    "/nologo", "/target:winexe", "/platform:x64", "/optimize+",
    "/main:Installer",
    ("/out:" + $installerExe),
    ("/resource:" + $payloadZip + ",payload.zip"),
    ("/win32manifest:" + $manifest),
    ("/win32icon:" + $icon),
    $commonSource,
    $diagnosticsSource,
    $uiSource,
    $verifySource,
    $installerSource
) + $refs
& $csc $installerArgs
if ($LASTEXITCODE -ne 0) { throw "installer build failed (csc exit $LASTEXITCODE)" }

Write-Host ""
Write-Host ("Payload : " + $payloadZip + "  " + (Get-FileHash $payloadZip -Algorithm SHA256).Hash.ToLowerInvariant())
Write-Host ("Setup   : " + $installerExe + "  " + (Get-FileHash $installerExe -Algorithm SHA256).Hash.ToLowerInvariant() + "  " + (Get-Item $installerExe).Length + " bytes")
$builtUninstaller = Join-Path $app "NLMatrixUninstall.exe"
Write-Host ("Uninst  : " + $builtUninstaller + "  " + (Get-FileHash $builtUninstaller -Algorithm SHA256).Hash.ToLowerInvariant() + "  " + (Get-Item $builtUninstaller).Length + " bytes")
Get-ChildItem $out -File | Select-Object Name, Length | Format-Table -AutoSize
