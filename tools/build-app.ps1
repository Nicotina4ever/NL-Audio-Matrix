param(
    [string[]]$Targets = @('NLMatrixRouter'),
    [string]$Configuration = 'Release',
    [switch]$Reconfigure
)
$ErrorActionPreference = 'Stop'
$root = 'K:\AI\NL Audio Matrix'
$cmake = 'D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$vcvars = 'D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $cmake)) { throw "cmake not found: $cmake" }
if (-not (Test-Path $vcvars)) { throw "vcvars64 not found: $vcvars" }
if ($Reconfigure) {
    & $cmake -S (Join-Path $root 'app') -B (Join-Path $root 'app\build') -G Ninja
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
}
$targetArgs = ($Targets | ForEach-Object { "--target $_" }) -join ' '
$cmd = '"' + $vcvars + '" >nul && sm && "' + $cmake + '" --build "' + (Join-Path $root 'app\build') + '" --config ' + $Configuration + ' ' + $targetArgs
# 'sm' is not a command; use plain && chaining
$cmd = '"' + $vcvars + '" >nul && "' + $cmake + '" --build "' + (Join-Path $root 'app\build') + '" --config ' + $Configuration + ' ' + $targetArgs
cmd /c $cmd 2>&1 | ForEach-Object { $_ }
exit $LASTEXITCODE
