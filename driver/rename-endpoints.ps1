$ErrorActionPreference = 'Stop'
$friendlyKey = '{a45c254e-df1c-4efd-8020-67d146a850e0},14'
$base = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio'
$changed = 0
foreach ($dir in @('Render', 'Capture')) {
    $root = Join-Path $base $dir
    if (-not (Test-Path $root)) { continue }
    Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
        $propsPath = Join-Path $_.PSPath 'Properties'
        if (-not (Test-Path $propsPath)) { return }
        try { $props = Get-ItemProperty $propsPath -ErrorAction Stop } catch { return }
        $desc = ''
        try { $desc = [string](Get-ItemProperty $_.PSPath -ErrorAction Stop).DeviceDesc } catch {}
        $friendly = ''
        try { $friendly = [string]$props.$friendlyKey } catch {}
        if ($desc -notmatch 'NLMatrix Virtual Audio Device') { return }
        if ($friendly -eq 'SinkDescription Sample') {
            Set-ItemProperty -Path $propsPath -Name $friendlyKey -Value 'NL Return 4' -ErrorAction Stop
            $changed++
        }
        elseif ($friendly -eq 'Altoparlanti') {
            Set-ItemProperty -Path $propsPath -Name $friendlyKey -Value 'NL Return 1' -ErrorAction Stop
            $changed++
        }
    }
}
Write-Output "Changed=$changed"
