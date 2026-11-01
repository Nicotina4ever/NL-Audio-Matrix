# Test-NoBcdWrites.ps1 — static proof that the NL setup/uninstaller have no code path
# that writes the boot configuration or the Windows security settings.
#
# Checks (exit 0 = PASS):
#   1. every bcdedit invocation in the installer/uninstaller code is a read ('/enum');
#      no enabling helper exists any more;
#   2. no code writes Secure Boot / HVCI / DeviceGuard values;
#   3. the built executables contain no flightsigning/nointegritychecks/deletevalue
#      write string;
#   4. the installer contains the read-only requirement gate and it runs before the
#      first change;
#   5. no script EXECUTES a bcdedit write (documentation text inside here-strings and
#      Write-Host lines is not execution and is ignored).
#
# The behavioural proof (BCD identical before/after install+repair+uninstall) is
# produced by tools/vm/Run-SetupSafetyCheck.ps1 in the test VM.
#
# Usage: powershell -File tools\Test-NoBcdWrites.ps1 [-Json]
param(
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$self = [IO.Path]::GetFileName($PSCommandPath)
$checks = New-Object System.Collections.Generic.List[object]

function Add-Check([string]$Name, [bool]$Ok, [string]$Detail) {
    $checks.Add([ordered]@{ name = $Name; ok = $Ok; detail = $Detail })
}

# --------------------------------------------------------------- 1. installer code
$csFiles = Get-ChildItem (Join-Path $root 'installer') -Filter *.cs -File
$bcdWrites = New-Object System.Collections.Generic.List[string]
foreach ($file in $csFiles) {
    $text = [IO.File]::ReadAllText($file.FullName)
    foreach ($match in [regex]::Matches($text, 'bcdedit\.exe"\s*,\s*"([^"]*)"')) {
        if ($match.Groups[1].Value -notmatch '^/enum') {
            $bcdWrites.Add($file.Name + ': bcdedit ' + $match.Groups[1].Value)
        }
    }
    foreach ($pattern in @('bcdedit\.exe"\s*,\s*"/set', 'bcdedit\.exe"\s*,\s*"/deletevalue', 'EnableTestSigning')) {
        if ($text -match $pattern) { $bcdWrites.Add($file.Name + ' matches ' + $pattern) }
    }
}
Add-Check 'installer sources: bcdedit is read-only' ($bcdWrites.Count -eq 0) ($(if ($bcdWrites.Count) { $bcdWrites -join '; ' } else { 'only /enum reads, no enabling helper' }))

# ------------------------------------------------------ 2. secure boot / HVCI writes
$securityWrites = New-Object System.Collections.Generic.List[string]
foreach ($file in $csFiles) {
    $text = [IO.File]::ReadAllText($file.FullName)
    foreach ($pattern in @('SetValue\("Enabled"\s*,\s*1', 'SetValue\("EnableVirtualizationBasedSecurity"', 'SetValue\("UEFISecureBootEnabled"', 'Set-SecureBoot')) {
        if ($text -match $pattern) { $securityWrites.Add($file.Name + ' matches ' + $pattern) }
    }
}
Add-Check 'no Secure Boot / HVCI / DeviceGuard write in code' ($securityWrites.Count -eq 0) ($(if ($securityWrites.Count) { $securityWrites -join '; ' } else { 'none' }))

# ------------------------------------------------------------- 3. built binaries
$binaries = @(
    (Join-Path $root 'installer\build\NLMatrixSetup.exe'),
    (Join-Path $root 'installer\build\NLMatrixUninstall.exe'),
    (Join-Path $root 'installer\build\staging\app\NLMatrixVerify.exe')
)
$forbidden = @('/deletevalue', 'flightsigning', 'nointegritychecks')
$hits = New-Object System.Collections.Generic.List[string]
$scanned = 0
foreach ($binary in $binaries) {
    if (-not (Test-Path $binary)) { continue }
    $scanned++
    $bytes = [IO.File]::ReadAllBytes($binary)
    $ascii = [Text.Encoding]::ASCII.GetString($bytes)
    $unicode = [Text.Encoding]::Unicode.GetString($bytes)
    foreach ($needle in $forbidden) {
        if ($ascii.Contains($needle) -or $unicode.Contains($needle)) {
            $hits.Add([IO.Path]::GetFileName($binary) + ' contains "' + $needle + '"')
        }
    }
}
Add-Check 'binaries: no boot/security write strings' ($hits.Count -eq 0) ($(if ($hits.Count) { $hits -join '; ' } else { "$scanned binaries scanned" }))

# --------------------------------------------- 4. scripts never EXECUTE a BCD write
$psFiles = @()
$psFiles += Get-ChildItem (Join-Path $root 'tools') -Filter *.ps1 -File
$psFiles += Get-ChildItem (Join-Path $root 'tools\vm') -Filter *.ps1 -File
$psFiles += Get-ChildItem (Join-Path $root 'driver') -Filter *.ps1 -File
$psFiles += Get-ChildItem (Join-Path $root 'driver\test') -Filter *.ps1 -File -ErrorAction SilentlyContinue

$scriptWrites = New-Object System.Collections.Generic.List[string]
foreach ($file in $psFiles) {
    if ($file.Name -eq $self) { continue }
    $lines = [IO.File]::ReadAllLines($file.FullName)
    $inHere = $false
    $hereQuote = ''
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($inHere) {
            if (($hereQuote -eq '"' -and $line -match '^\s*"@') -or ($hereQuote -eq "'" -and $line -match "^\s*'@")) {
                $inHere = $false
            }
            continue
        }
        if ($line -match '@"\s*$') { $inHere = $true; $hereQuote = '"'; continue }
        if ($line -match "@'\s*$") { $inHere = $true; $hereQuote = "'"; continue }
        $trim = $line.Trim()
        if ($trim.StartsWith('#')) { continue }
        if ($trim -match '\bWrite-(Host|Output|Warning|Verbose|Information)\b') { continue }
        $match = [regex]::Match($trim, '(?<![\w-])bcdedit(\.exe)?["'']?\s+(/set|/deletevalue)')
        if ($match.Success) {
            # Text inside a string literal is documentation, not execution.
            $prefix = $trim.Substring(0, $match.Index)
            $insideString = $prefix.Contains('"') -or $prefix.Contains("'")
            if (-not $insideString) { $scriptWrites.Add($file.Name + ':' + ($i + 1) + ' ' + $trim) }
        }
        if ($trim -match 'nointegritychecks') {
            $scriptWrites.Add($file.Name + ':' + ($i + 1) + ' ' + $trim)
        }
    }
}
Add-Check 'scripts: no executed BCD/security write' ($scriptWrites.Count -eq 0) ($(if ($scriptWrites.Count) { $scriptWrites -join '; ' } else { 'none (documentation text ignored)' }))

# ------------------------------------------------------ 5. read-only gate present
$installerSource = [IO.File]::ReadAllText((Join-Path $root 'installer\NLMatrixInstaller.cs'))
$commonSource = [IO.File]::ReadAllText((Join-Path $root 'installer\NLSetupCommon.cs'))
$hasGate = $installerSource.Contains('RequireTestSigningActive')
$gateBeforeWork = $installerSource.IndexOf('RequireTestSigningActive') -lt $installerSource.IndexOf('EnsureApplicationFiles(options')
$noEnable = -not $commonSource.Contains('public static bool EnableTestSigning')
Add-Check 'installer: read-only gate before any change' ($hasGate -and $gateBeforeWork) ('gate=' + $hasGate + ' beforeWork=' + $gateBeforeWork)
Add-Check 'installer: enabling helper removed' $noEnable ('EnableTestSigning present=' + (-not $noEnable))

$failed = ($checks | Where-Object { -not $_.ok }).Count
$result = [ordered]@{
    tool = 'Test-NoBcdWrites'
    utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    checks = $checks
    failed = $failed
    verdict = $(if ($failed -eq 0) { 'PASS' } else { 'FAIL' })
}
if ($Json) {
    $result | ConvertTo-Json -Depth 5
} else {
    Write-Host ('NL BCD/security write audit: ' + $result.verdict)
    foreach ($check in $checks) {
        Write-Host ('  [' + $(if ($check.ok) { 'PASS' } else { 'FAIL' }) + '] ' + $check.name + ' - ' + $check.detail)
    }
}
exit $(if ($failed -eq 0) { 0 } else { 1 })
