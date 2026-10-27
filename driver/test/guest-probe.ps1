$ErrorActionPreference='SilentlyContinue'
"=== service config ==="
sc.exe qc sysvad_componentizedaudiosample
"=== driver files ==="
Get-ChildItem C:\Windows\System32\drivers\NL*.sys -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName + " | " + $_.Length + " | " + $_.LastWriteTime + " | " + (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
Get-ChildItem 'C:\Windows\System32\DriverStore\FileRepository\*\NLMatrixAudio.sys' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName + " | " + $_.Length + " | " + $_.LastWriteTime + " | " + (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
"=== test tools ==="
Get-ChildItem C:\NLMatrix\NLMatrix*.exe -ErrorAction SilentlyContinue | ForEach-Object { $_.Name + " | " + $_.Length + " | " + $_.LastWriteTime }
"=== NL endpoints (OK) ==="
Get-PnpDevice -Class AudioEndpoint -Status OK -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -like '*NL *' } | ForEach-Object { $_.FriendlyName }
"=== all NL endpoint instances ==="
Get-PnpDevice -Class AudioEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -like '*NL *' } | ForEach-Object { $_.Status + " | " + $_.FriendlyName }
