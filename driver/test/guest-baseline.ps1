$ErrorActionPreference='SilentlyContinue'
"=== date ==="
Get-Date -Format o
"=== os ==="
$os = Get-CimInstance Win32_OperatingSystem
$os.Caption + " | build " + $os.BuildNumber + " | " + $os.Version
"=== driver file ==="
Get-Item C:\Windows\System32\drivers\NLMatrixAudio.sys | Format-List Length,LastWriteTime
"=== sys sha256 ==="
(Get-FileHash C:\Windows\System32\drivers\NLMatrixAudio.sys -Algorithm SHA256).Hash
"=== service ==="
sc.exe query sysvad_componentizedaudiosample
"=== testsigning ==="
bcdedit /enum '{current}' | findstr /i "testsigning"
"=== pnputil drivers matching NLMatrix ==="
pnputil /enum-drivers | Select-String -Pattern 'NLMatrix' -Context 1,10
"=== pnputil MEDIA devices ==="
pnputil /enum-devices /class MEDIA
"=== C:\\NLMatrix contents ==="
Get-ChildItem C:\NLMatrix | Sort-Object Name | Format-Table Name,Length,LastWriteTime -AutoSize
"=== NLMatrix processes ==="
Get-Process | Where-Object { $_.ProcessName -match 'NLMatrix|router' } | Format-Table ProcessName,Id -AutoSize
"=== vmrun list ==="
