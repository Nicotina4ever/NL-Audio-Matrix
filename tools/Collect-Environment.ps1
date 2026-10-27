# NL Audio Matrix - local READ-ONLY inventory, Windows PowerShell 5.1+
# Does not modify devices, registry, drivers, sample rate or Windows defaults.
# It writes a report within the project's reports/ folder; review before sharing.
$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $PSScriptRoot
$ReportDir = Join-Path $Root 'reports'
New-Item -ItemType Directory -Path $ReportDir -Force | Out-Null
$Report = Join-Path $ReportDir 'F0_environment_local.txt'
$Lines = New-Object System.Collections.Generic.List[string]
$Lines.Add('NL Audio Matrix F0 - READ ONLY - ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
$Lines.Add('OS: ' + [Environment]::OSVersion.VersionString)
$Lines.Add('64-bit OS: ' + [Environment]::Is64BitOperatingSystem)
$Lines.Add('PowerShell: ' + $PSVersionTable.PSVersion.ToString())
foreach($name in @('cmake','git','cl')) {
  $cmd = Get-Command $name -ErrorAction SilentlyContinue
  $Lines.Add('Tool '+$name+': '+ $(if($cmd){$cmd.Source}else{'not on PATH'}))
}
$Lines.Add('=== Win32_SoundDevice name (may include unrelated devices) ===')
try { Get-CimInstance Win32_SoundDevice | ForEach-Object { $Lines.Add($_.Name) } } catch { $Lines.Add('query failed: '+$_.Exception.Message) }
$Lines.Add('=== ASIO registry display names, 64/32 bit (read-only) ===')
foreach($key in @('HKLM:\SOFTWARE\ASIO','HKLM:\SOFTWARE\WOW6432Node\ASIO')) {
  $Lines.Add('KEY: '+$key)
  if (Test-Path $key) {
    Get-ChildItem $key -ErrorAction SilentlyContinue | ForEach-Object { $Lines.Add(' - '+$_.PSChildName) }
  } else { $Lines.Add('not found') }
}
$Lines.Add('IMPORTANT: device names alone do not prove ability to share ASIO with REAPER')
$Lines | Set-Content -Path $Report -Encoding UTF8
Write-Host ('Report saved to: '+$Report)
Write-Host 'Review report before sharing it with an AI agent.'
