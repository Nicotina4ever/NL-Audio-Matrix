# ProbeEndpoints.ps1 - dump NLMatrix audio endpoint state (MMDevices, KS, PnP).
# Read-only. Used to compare what the driver registers with what Windows exposes.
$ErrorActionPreference = 'SilentlyContinue'
$out = @()
function Line([string]$s) { $script:out += $s }
function Prop($key, $name) {
    try {
        $v = (Get-ItemProperty -Path $key -Name $name -ErrorAction Stop).$name
        if ($v -is [byte[]]) { return ([System.Text.Encoding]::Unicode.GetString($v) -replace "`0", '') }
        return $v
    } catch { return $null }
}

# PKEY_Device_FriendlyName  {a45c254e-df1c-4efd-8020-67d146a850e0},14
# PKEY_Device_DeviceDesc    {a45c254e-df1c-4efd-8020-67d146a850e0},2
# PKEY_AudioEndpoint_FormFactor / PKEY_AudioEndpoint_Association
$fnName = '{a45c254e-df1c-4efd-8020-67d146a850e0},14'
$descName = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'
$assoc = '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},2'

foreach ($flow in @('Render', 'Capture')) {
    Line "### MMDevices $flow"
    $base = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\$flow"
    Get-ChildItem $base | ForEach-Object {
        $guid = $_.PSChildName
        $props = Join-Path $_.PSPath 'Properties'
        $name = Prop $props $fnName
        $desc = Prop $props $descName
        $state = (Get-ItemProperty -Path $_.PSPath -Name DeviceState).DeviceState
        $assocV = Prop $props $assoc
        if ("$name" -match 'NL |NLMatrix|SinkDescription|Altoparlanti|Microfono' -or "$desc" -match 'NLMatrix') {
            Line ("{0} state={1} name='{2}' desc='{3}' assoc={4}" -f $guid, $state, $name, $desc, $assocV)
            # enumerate subkeys that carry the KS filter instance
            $childKeys = Get-ChildItem $_.PSPath | ForEach-Object { $_.PSChildName }
            Line ("   children: " + ($childKeys -join ','))
        }
    }
}

Line "### PnP audio endpoints with NLMatrix"
Get-PnpDevice -Class AudioEndpoint | Where-Object { $_.FriendlyName -match 'NL|NLMatrix|SinkDescription|Altoparlanti|Microfono' } |
    ForEach-Object {
        $inst = $_.InstanceId
        $st = $_.Status
        $fn = $_.FriendlyName
        Line ("{0} status={1} name='{2}'" -f $inst, $st, $fn)
    }

Line "### KS device interfaces (audio/render/capture/topology)"
foreach ($cat in @('{6994AD04-93EF-11D0-A3CC-00A0C9223196}', '{65E8773E-8F56-11D0-A3B9-00A0C9223196}', '{65E8773D-8F56-11D0-A3B9-00A0C9223196}', '{DDA54A40-1E4C-11D1-A050-405705C10000}')) {
    $k = "HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses\$cat"
    $n = 0
    if (Test-Path $k) {
        $nodes = Get-ChildItem $k
        $n = @($nodes).Count
        Line ("$cat count=$n")
        foreach ($node in $nodes) {
            $dev = $node.PSChildName
            if ($dev -match 'NLMatrix|ROOT#MEDIA') {
                $ref = Prop $node.PSPath '#1'
                Line ("   $dev ref=" + ($ref -join ''))
                Get-ChildItem $node.PSPath | ForEach-Object {
                    $ch = $_.PSChildName
                    $ref2 = Prop $_.PSPath '#1'
                    Line ("     child $ch ref=" + ($ref2 -join ''))
                }
            }
        }
    } else { Line "$cat MISSING" }
}

$out | Set-Content -Path 'C:\NLMatrixTools\endpoint_probe.txt' -Encoding UTF8
$out | ForEach-Object { $_ }
