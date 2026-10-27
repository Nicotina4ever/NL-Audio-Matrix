# host-disconnect-test.ps1 - deploy + run the guest recovery test, fetch evidence.
# Host side only; never installs anything on the host PC.
param([string]$Root = 'K:\AI\NL Audio Matrix')
$ErrorActionPreference = 'Stop'
. (Join-Path $Root 'tools\vm\NLVM.ps1')
$router = Join-Path $Root 'vm\baseline\NLMatrixRouter_engine_v2_ACCEPT2.exe'
$nlc = Join-Path $Root 'app\build\NLMatrixDriverAudioTest_artefacts\Release\NLMatrixDriverAudioTest.exe'
$cfg = Join-Path $Root 'vm\engine\disconnect_two_dest.json'
$script = Join-Path $Root 'driver\test\guest-disconnect-test.ps1'
NL-CopyTo $router 'C:\NLMatrix\NLMatrixRouterFinal2.exe' | Out-Null
NL-CopyTo $cfg 'C:\NLMatrix\disconnect.json' | Out-Null
NL-CopyTo $script 'C:\NLMatrix\guest-disconnect-test.ps1' | Out-Null
$out = NL-Exec 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\NLMatrix\guest-disconnect-test.ps1' 'C:\NLMatrix\disconnect_test.out' 240
$out | Set-Content (Join-Path $Root 'reports\engine\DISCONNECT_RECOVERY_TEST.log') -Encoding UTF8
NL-CopyFrom 'C:\NLMatrix\disconnect_router.log' (Join-Path $Root 'reports\engine\disconnect_router.log')
Write-Output $out
