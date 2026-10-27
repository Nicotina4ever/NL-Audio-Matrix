$ErrorActionPreference = 'SilentlyContinue'
$pkg = 'C:\NLMatrix\abi-v3'
$expectedSysHash = '__EXPECTED_SYS_SHA256__'
$full = ''
"=== VERIFY START ==="
Get-Date -Format o
$os = Get-CimInstance Win32_OperatingSystem
"os: " + $os.Caption + " build " + $os.BuildNumber
"=== testsigning ==="
bcdedit /enum '{current}' | findstr /i "testsigning"
"=== service ==="
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='sysvad_componentizedaudiosample'"
$hashOk = $false
if ($svc) {
    "service state=" + $svc.State + " startMode=" + $svc.StartMode
    "service path=" + $svc.PathName
    $img = $svc.PathName
    if ($img) {
        $full = $img
        if ($full -like '\??\*') { $full = $full.Substring(4) }
        $full = $full -replace '^\\SystemRoot\\', 'C:\Windows\'
        if (Test-Path $full) {
            $h = (Get-FileHash $full -Algorithm SHA256).Hash
            "loaded image sha256=" + $h
            "expected  image sha256=" + $expectedSysHash
            if ($h -eq $expectedSysHash) { $hashOk = $true; "DRIVER_HASH_MATCH=YES" } else { "DRIVER_HASH_MATCH=NO" }
        } else { "loaded image file not found: " + $full }
    }
} else { "service NOT FOUND" }
"=== signature of loaded image ==="
if ($full -and (Test-Path $full)) { (Get-AuthenticodeSignature $full).Status.ToString() }
"=== pnputil packages matching componentizedaudiosample ==="
pnputil /enum-drivers | Select-String -Pattern 'componentizedaudiosample' -Context 0,9 | ForEach-Object { $_.ToString() }
"=== pnputil MEDIA devices (NLMatrix excerpt) ==="
$devout = pnputil /enum-devices /class MEDIA 2>&1
$keep = @()
foreach ($line in $devout) {
    if ($line -match 'NLMatrix') { $keep += $line }
}
$keep | ForEach-Object { $_ }
"=== NLMatrix MEDIA node ==="
Get-PnpDevice -Class MEDIA | Where-Object { $_.FriendlyName -like '*NLMatrix*' } | ForEach-Object { $_.Status + " | " + $_.InstanceId + " | " + $_.FriendlyName }
"=== NLMatrix audio endpoints (all statuses) ==="
$eps = Get-PnpDevice -Class AudioEndpoint | Where-Object { $_.FriendlyName -like '*NLMatrix*' -or $_.FriendlyName -like 'NL Mic *' -or $_.FriendlyName -like 'NL Return *' }
$eps | ForEach-Object { "ENDPOINT " + $_.Status + " | " + $_.FriendlyName }
"endpointCount=" + @($eps).Count
$mic = @($eps | Where-Object { $_.FriendlyName -like 'NL Mic *' -and $_.Status -eq 'OK' }).Count
$ret = @($eps | Where-Object { $_.FriendlyName -like 'NL Return *' -and $_.Status -eq 'OK' }).Count
$nlm = @($eps | Where-Object { $_.FriendlyName -like '*NLMatrix Virtual Audio Device*' -and $_.Status -eq 'OK' }).Count
"micEndpointsOK=" + $mic + " returnEndpointsOK=" + $ret + " otherNLMatrixEndpointsOK=" + $nlm
"=== Focusrite / USB audio enumeration (physical passthrough check) ==="
Get-PnpDevice -Class MEDIA | ForEach-Object { "MEDIA " + $_.Status + " | " + $_.InstanceId + " | " + $_.FriendlyName } | Where-Object { $_ -match 'Focusrite|Scarlett|USB' }
$focus = Get-PnpDevice -Class MEDIA | Where-Object { $_.FriendlyName -like '*Scarlett*' -or $_.FriendlyName -like '*Focusrite*' }
if ($focus) { $focus | ForEach-Object { "FOCUSRITE " + $_.Status + " | " + $_.InstanceId + " | " + $_.FriendlyName } } else { "FOCUSRITE NOT ENUMERATED" }
"=== WASAPI device list (NLMatrixDriverAudioTest --list) ==="
$listOut = & 'C:NLMatrixNLMatrixDriverAudioTest.exe' --list 2>&1
$listOut | ForEach-Object { $_ }
"=== ABI v3 transport probe ==="
$probe = "$pkg\NLMatrixTransportProbe.exe"
if (Test-Path $probe) {
    & $probe --version 2>&1 | ForEach-Object { $_ }
    "probe version exit=$LASTEXITCODE"
} else { "probe MISSING at " + $probe }
"RESULT driverHashMatch=" + $hashOk + " micEndpoints=" + $mic + " returnEndpoints=" + $ret
"=== VERIFY END ==="
