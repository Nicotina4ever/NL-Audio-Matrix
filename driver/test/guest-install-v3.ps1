$ErrorActionPreference = 'SilentlyContinue'
$pkg = 'C:\NLMatrix\abi-v3'
$expectedSysHash = '__EXPECTED_SYS_SHA256__'
"=== INSTALL START ==="
Get-Date -Format o
"hostname: " + $env:COMPUTERNAME

"=== BEFORE: service ==="
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='sysvad_componentizedaudiosample'"
if ($svc) {
    "service state=" + $svc.State + " startMode=" + $svc.StartMode + " path=" + $svc.PathName
    $img = $svc.PathName
    if ($img) {
        $full = $img
        if ($full -like '\??\*') { $full = $full.Substring(4) }
        $full = $full -replace '^\\SystemRoot\\', 'C:\Windows\'
        if (Test-Path $full) { "loaded image sha256=" + (Get-FileHash $full -Algorithm SHA256).Hash }
    }
} else { "service NOT FOUND" }
"=== BEFORE: NLMatrix MEDIA node ==="
$nlDev = Get-PnpDevice -Class MEDIA | Where-Object { $_.FriendlyName -like '*NLMatrix*' }
$nlDev | ForEach-Object { $_.Status + " | " + $_.InstanceId + " | " + $_.FriendlyName }
"=== BEFORE: NLMatrix endpoints ==="
$eps0 = Get-PnpDevice -Class AudioEndpoint | Where-Object { $_.FriendlyName -like '*NLMatrix*' -or $_.FriendlyName -like 'NL Mic *' -or $_.FriendlyName -like 'NL Return *' }
$eps0 | ForEach-Object { "ENDPOINT " + $_.Status + " | " + $_.FriendlyName }
"endpointCount=" + @($eps0).Count

"=== stopping user-mode consumers ==="
$names = 'reaper','NLMatrixUI','NLMatrixRouter','NLMatrixRouterNew','NLMatrixRouterMetrics','NLMatrixRouterOld','NLMatrixDriverAudioTest','NLMatrixDriverTest','NLMatrixDriverTestMetrics','NLMatrixLive','NLMatrixProbe','NLMatrixBridge','NLMatrixLoopbackTest','NLMatrixEndpointVolume'
foreach ($n in $names) {
    $procs = Get-Process -Name $n -ErrorAction SilentlyContinue
    if ($procs) { $procs | Stop-Process -Force; "stopped " + $n }
}
Start-Sleep -Milliseconds 500

"=== cert trust (idempotent) ==="
certutil -addstore -f Root "$pkg\NLMatrixTest.cer" 2>&1 | Out-Null
certutil -addstore -f TrustedPublisher "$pkg\NLMatrixTest.cer" 2>&1 | Out-Null
"cert stores updated"

"=== pnputil /add-driver /install ==="
$out = pnputil /add-driver "$pkg\ComponentizedAudioSample.inf" /install 2>&1
$out | ForEach-Object { $_ }
"pnputil exit=$LASTEXITCODE"

"=== NLMatrix root device hardware ids ==="
$rootDev = Get-PnpDevice -Class MEDIA | Where-Object { $_.FriendlyName -like '*NLMatrix*' -and $_.InstanceId -like 'ROOT\*' }
$hwids = @()
foreach ($d in $rootDev) {
    $ids = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue).Data
    if ($ids) {
        "device " + $d.InstanceId + " status=" + $d.Status + " hwids=" + ($ids -join ' ; ')
        $hwids += $ids
    }
}
$rootHwid = $hwids | Where-Object { $_ -like 'Root\*' -or $_ -like 'ROOT\*' } | Select-Object -First 1
if (-not $rootHwid) { $rootHwid = 'Root\sysvad_ComponentizedAudioSample' }
"=== devcon update " + $rootHwid + " ==="
& "$pkg\devcon.exe" update "$pkg\ComponentizedAudioSample.inf" $rootHwid 2>&1 | ForEach-Object { $_ }
"devcon exit=$LASTEXITCODE"

"=== AFTER (pre-reboot): published packages matching componentizedaudiosample ==="
pnputil /enum-drivers | Select-String -Pattern 'componentizedaudiosample' -Context 0,9 | ForEach-Object { $_.ToString() }

"=== AFTER: service / image / probe ==="
$svc2 = Get-CimInstance Win32_SystemDriver -Filter "Name='sysvad_componentizedaudiosample'"
$fileHashOk = $false
if ($svc2) {
    "service state=" + $svc2.State + " path=" + $svc2.PathName
    $img2 = $svc2.PathName
    if ($img2) {
        $full2 = $img2
        if ($full2 -like '\??\*') { $full2 = $full2.Substring(4) }
        $full2 = $full2 -replace '^\\SystemRoot\\', 'C:\Windows\'
        if (Test-Path $full2) {
            $h2 = (Get-FileHash $full2 -Algorithm SHA256).Hash
            "service image sha256=" + $h2
            if ($h2 -eq $expectedSysHash) { $fileHashOk = $true }
        }
    }
}
$newStore = Get-ChildItem 'C:\Windows\System32\DriverStore\FileRepository\componentizedaudiosample.inf_*' -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { $f = Join-Path $_.FullName 'nlmatrixaudio.sys'; if (Test-Path $f) { [pscustomobject]@{ Dir = $_.FullName; Hash = (Get-FileHash $f -Algorithm SHA256).Hash } } } |
    Where-Object { $_.Hash -eq $expectedSysHash } | Select-Object -First 1
if ($newStore) { "new package staged at: " + $newStore.Dir + " (hash matches expected)" } else { "WARNING: expected sys hash not found in DriverStore yet" }

# The decisive liveness check: the running kernel image must answer ABI v3.
$probeOut = & "$pkg\NLMatrixTransportProbe.exe" --version 2>&1
"probe pre-reboot output: " + (($probeOut | Select-Object -First 3) -join ' / ')
$abiLive = (($probeOut -join ' ') -match 'ABIV3_OK')

"=== AFTER: NLMatrix devices pre-reboot ==="
Get-PnpDevice -Class MEDIA | Where-Object { $_.FriendlyName -like '*NLMatrix*' } | ForEach-Object { $_.Status + " | " + $_.InstanceId + " | " + $_.FriendlyName }

if ($abiLive) {
    "REBOOT_NOT_REQUIRED: running driver already answers ABI v3"
} else {
    "REBOOT_REQUIRED: running driver is not ABI v3 yet (file match=" + $fileHashOk + ")"
}
"=== INSTALL END ==="
