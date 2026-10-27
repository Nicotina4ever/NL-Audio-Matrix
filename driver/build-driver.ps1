param(
    [string]$Configuration = "Release",
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sysvad = Join-Path $PSScriptRoot "sysvad-nl"
$package = Join-Path $PSScriptRoot "package"
$msbuild = "D:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
$makecat = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\makecat.exe"
$signtool = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\signtool.exe"
$cert = Join-Path $PSScriptRoot "test-certs\NLMatrixTest.pfx"

if (-not (Test-Path $msbuild)) { throw "MSBuild not found: $msbuild" }
New-Item -ItemType Directory -Force -Path $package | Out-Null

$common = @("/m", "/p:Configuration=$Configuration", "/p:Platform=x64",
            "/p:EnableTestSign=false", "/p:EnableInf2cat=false",
            "/p:ApiValidator_Enable=false", "/v:minimal", "/nologo")

& $msbuild (Join-Path $sysvad "EndpointsCommon\EndpointsCommon.vcxproj") @common
if ($LASTEXITCODE -ne 0) { throw "EndpointsCommon build failed" }

& $msbuild (Join-Path $sysvad "TabletAudioSample\TabletAudioSample.vcxproj") @common
if ($LASTEXITCODE -ne 0) { throw "driver build failed" }

$out = Join-Path $sysvad "TabletAudioSample\x64\$Configuration"
Copy-Item (Join-Path $out "NLMatrixAudio.sys") $package -Force
Copy-Item (Join-Path $out "ComponentizedAudioSample.inf") $package -Force
$devcon = "K:\AI\NL Audio Matrix\tools\wdk-packages\microsoft.windows.wdk.x64\10.0.26100.6584\c\tools\10.0.26100.0\x64\devcon.exe"
if (Test-Path $devcon) { Copy-Item $devcon $package -Force }
$cer = Join-Path $PSScriptRoot "test-certs\NLMatrixTest.cer"
if (Test-Path $cer) { Copy-Item $cer $package -Force }

$cdf = Join-Path $package "NLMatrixAudio.cdf"
@"
[CatalogHeader]
Name=sysvad.cat
ResultDir=$package
PublicVersion=0x00000001
EncodingType=0x00010000

[CatalogFiles]
<HASH>NLMatrixAudio.sys=$package\NLMatrixAudio.sys
<HASH>ComponentizedAudioSample.inf=$package\ComponentizedAudioSample.inf
"@ | Set-Content -LiteralPath $cdf -Encoding ASCII
Remove-Item (Join-Path $package "sysvad.cat") -Force -ErrorAction SilentlyContinue
& $makecat -v $cdf | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makecat failed" }

if (Test-Path $cert) {
    & $signtool sign /f $cert /p "nlmatrix-dev" /fd SHA256 /a (Join-Path $package "sysvad.cat") | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "catalog signing failed" }
}

Write-Host "Package ready: $package"
Get-ChildItem $package | Select-Object Name, Length
