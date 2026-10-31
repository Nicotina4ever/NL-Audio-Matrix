#requires -Version 5.1
<#
.SYNOPSIS
    Publish (or refresh) the NL Audio Matrix GitHub pre-release from a local assets
    folder, using the credential that git already has.

.DESCRIPTION
    No token is typed, printed, written to a file or embedded in a remote URL: the
    script asks git for the github.com credential through the configured helper,
    keeps it in memory and never echoes it.  Typical use:

        powershell -File tools\Publish-GitHubRelease.ps1 -Tag v0.5.0-alpha.3

    It verifies that the tag exists locally and on the remote (same commit), creates
    the pre-release or updates its title/notes, uploads the assets that are missing,
    then downloads every asset again and compares its SHA256 with the local file.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$ReleaseDir = '',
    [string]$NotesFile = '',
    [string]$Repository = 'Nicotina4ever/NL-Audio-Matrix'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = $Tag.TrimStart('v')

if (-not $ReleaseDir) {
    # Tag vX.Y.Z-alpha.N ships the assets of the app version X.Y.Z-alpha.
    $candidate = Join-Path $root ('dist\' + $version)
    if (-not (Test-Path $candidate)) { $candidate = Join-Path $root ('dist\' + ($version -replace '\.\d+$', '')) }
    $ReleaseDir = $candidate
}
if (-not $NotesFile) { $NotesFile = Join-Path $ReleaseDir 'RELEASE_NOTES.md' }
if (-not (Test-Path $ReleaseDir)) { throw "release folder not found: $ReleaseDir" }
if (-not (Test-Path $NotesFile)) { throw "release notes not found: $NotesFile" }

function Step([string]$Text) { Write-Host ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Text) }

# ------------------------------------------------------------------- notes text
# The release body must be the Markdown file as a plain UTF-8 string. Never pass
# the result of Get-Item/Get-Content -Raw through ConvertTo-Json: a FileInfo or
# PSObject serializes PSPath, PSDrive, PSProvider and other local metadata into
# the public body. File.ReadAllText returns a [string] and is the only reader used.
$BodyMarkers = @('PSPath', 'PSDrive', 'PSProvider', 'PSParentPath', 'PSChildName',
                 'PSIsContainer', 'ReadCount', '=>', 'ghp_', 'github_pat_',
                 'Authorization:', 'password=')

function ConvertFrom-Mojibake([string]$Text) {
    # UTF-8 bytes decoded as Windows-1252 produce sequences such as a-euro-curlyquote.
    if (-not ($Text.Contains([string][char]0x00E2) -or $Text.Contains([string][char]0x00C3))) { return $Text }
    try { $cp = [System.Text.Encoding]::GetEncoding(1252) }
    catch { throw 'release notes look like mojibake and code page 1252 is unavailable for repair' }
    $repaired = [System.Text.Encoding]::UTF8.GetString($cp.GetBytes($Text))
    if ($repaired.Contains([string][char]0xFFFD)) { throw 'release notes contain undecodable characters' }
    if ($repaired.Contains([string][char]0x00E2) -or $repaired.Contains([string][char]0x00C3)) {
        throw 'release notes still look like mojibake after UTF-8/Windows-1252 repair'
    }
    Step 'repaired legacy mojibake in the release notes'
    return $repaired
}

function Read-NotesText([string]$Path) {
    $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    $text = $text.TrimStart([char[]]@([char]0xFEFF))
    return ConvertFrom-Mojibake $text
}

function Assert-NotesText($Notes) {
    if ($null -eq $Notes -or $Notes -isnot [string]) { throw 'release notes must be a plain string' }
    if ($Notes.Trim().Length -eq 0) { throw 'release notes are empty' }
    if (-not $Notes.StartsWith('#')) { throw 'release notes do not start with a Markdown heading' }
    if ($Notes.Contains([string][char]0xFFFD)) { throw 'release notes contain undecodable characters' }
    foreach ($marker in $BodyMarkers) {
        if ($Notes.Contains($marker)) {
            throw ('release notes contain ' + $marker + '; refusing to publish object metadata or a credential')
        }
    }
}

# ------------------------------------------------------------------ tag identity
Step "checking tag $Tag locally and on the remote"
& git -C $root rev-parse --verify ("refs/tags/" + $Tag) 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { throw "local tag $Tag does not exist" }
$localCommit = (& git -C $root rev-parse ($Tag + '^{commit}')).Trim()
$remoteLine = (& git -C $root ls-remote --tags origin ("refs/tags/" + $Tag + "^{}") 2>$null)
if (-not $remoteLine) { $remoteLine = (& git -C $root ls-remote --tags origin ("refs/tags/" + $Tag) 2>$null) }
if (-not $remoteLine) { throw "tag $Tag is not on the remote; push it first" }
$remoteCommit = (($remoteLine -split "`t")[0]).Trim()
if ($remoteCommit -ne $localCommit) { throw "tag mismatch: local $localCommit, remote $remoteCommit" }
Step "tag $Tag -> commit $localCommit"

# --------------------------------------------------------- credential (in memory)
$cred = "protocol=https`nhost=github.com`n`n" | & git credential fill 2>$null
if ($LASTEXITCODE -ne 0 -or -not $cred) { throw 'git has no github.com credential; push once so git stores it' }
$credText = ($cred -join "`n")
$user = [regex]::Match($credText, '(?m)^username=(.*)$').Groups[1].Value.Trim()
$secret = [regex]::Match($credText, '(?m)^password=(.*)$').Groups[1].Value.Trim()
if (-not $secret) { throw 'git credential did not return a password' }
$basic = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($user + ':' + $secret))
$headers = @{ Authorization = 'Basic ' + $basic; 'User-Agent' = 'NL-Audio-Matrix-release'; Accept = 'application/vnd.github+json' }
$downloadHeaders = @{ Authorization = 'Basic ' + $basic; 'User-Agent' = 'NL-Audio-Matrix-release'; Accept = 'application/octet-stream' }
$cred = $null; $credText = $null; $secret = $null

$api = "https://api.github.com/repos/$Repository"
$uploadApi = "https://uploads.github.com/repos/$Repository"
$title = 'NL Audio Matrix ' + $version + ' - complete installer (test-signed driver)'
$notes = Read-NotesText $NotesFile
Assert-NotesText $notes
Step ('release notes: ' + $notes.Length + ' chars, UTF-8 plain text')
$buildInfoPath = Join-Path $ReleaseDir 'BUILD_INFO.json'
if (Test-Path $buildInfoPath) {
    $buildInfo = [System.IO.File]::ReadAllText($buildInfoPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($buildInfo.buildId -and -not $notes.Contains([string]$buildInfo.buildId)) {
        throw ('release notes do not mention the Build ID ' + $buildInfo.buildId)
    }
    if ($buildInfo.commit -and -not $notes.Contains([string]$buildInfo.commit)) {
        throw ('release notes do not mention the source commit ' + $buildInfo.commit)
    }
    Step ('notes match BUILD_INFO: ' + $buildInfo.buildId + ' / ' + $buildInfo.shortCommit)
}

# ----------------------------------------------------------------- release object
$existing = $null
try { $existing = Invoke-RestMethod -Method Get -Uri "$api/releases/tags/$Tag" -Headers $headers }
catch {
    $status = $null
    try { $status = [int]$_.Exception.Response.StatusCode } catch { }
    if ($status -ne 404) { throw }
}
$payload = '{"name":' + (ConvertTo-Json $title -Compress) + ',"body":' + (ConvertTo-Json $notes -Compress) +
           ',"prerelease":true,"draft":false}'
$utf8 = [System.Text.Encoding]::UTF8
if ($null -eq $existing) {
    Step 'creating the pre-release'
    $create = '{"tag_name":' + (ConvertTo-Json $Tag -Compress) + ',' + $payload.TrimStart('{')
    $release = Invoke-RestMethod -Method Post -Uri "$api/releases" -Headers $headers `
        -ContentType 'application/json; charset=utf-8' -Body $utf8.GetBytes($create)
} else {
    Step ("updating the existing release " + $existing.id)
    $release = Invoke-RestMethod -Method Patch -Uri ("$api/releases/" + $existing.id) -Headers $headers `
        -ContentType 'application/json; charset=utf-8' -Body $utf8.GetBytes($payload)
}
Step ('release id ' + $release.id)

# GitHub must return exactly the Markdown that was sent, not a serialized object.
$stored = Invoke-RestMethod -Method Get -Uri ("$api/releases/" + $release.id) -Headers $headers
if ([string]$stored.body -cne $notes) {
    throw ('GitHub stored a different release body: sent ' + $notes.Length + ' chars, got ' + ([string]$stored.body).Length + ' chars')
}
foreach ($marker in $BodyMarkers) {
    if (([string]$stored.body).Contains($marker)) { throw ('release body contains ' + $marker) }
}
Step ('body verified on GitHub: ' + $notes.Length + ' chars, plain Markdown')

# --------------------------------------------------------------------- uploads
$files = Get-ChildItem $ReleaseDir -File | Sort-Object Name
$tmp = Join-Path $env:TEMP ('nlrelease-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$present = @{}
foreach ($asset in $release.assets) { $present[$asset.name] = $asset }
foreach ($file in $files) {
    $want = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $asset = if ($present.ContainsKey($file.Name)) { $present[$file.Name] } else { $null }
    if ($null -ne $asset) {
        $same = $false
        if ([int64]$asset.size -eq $file.Length) {
            $probe = Join-Path $tmp ('probe-' + $file.Name)
            Invoke-WebRequest -Method Get -Uri $asset.url -Headers $downloadHeaders -OutFile $probe | Out-Null
            $same = ((Get-FileHash $probe -Algorithm SHA256).Hash.ToLowerInvariant() -eq $want)
            Remove-Item $probe -Force -ErrorAction SilentlyContinue
        }
        if ($same) { Step ('asset unchanged: ' + $file.Name); continue }
        Step ('replacing changed asset ' + $file.Name + ' (old id ' + $asset.id + ')')
        Invoke-RestMethod -Method Delete -Uri ($api + '/releases/assets/' + $asset.id) -Headers $headers | Out-Null
    } else {
        Step ('uploading ' + $file.Name + ' (' + $file.Length + ' bytes)')
    }
    $uri = $uploadApi + '/releases/' + $release.id + '/assets?name=' + [uri]::EscapeDataString($file.Name)
    Invoke-RestMethod -Method Post -Uri $uri -Headers $headers -InFile $file.FullName `
        -ContentType 'application/octet-stream' | Out-Null
}
$release = Invoke-RestMethod -Method Get -Uri "$api/releases/tags/$Tag" -Headers $headers

# ------------------------------------------------------------------ verification
$problems = New-Object System.Collections.Generic.List[string]
if ($release.tag_name -ne $Tag) { $problems.Add('tag_name mismatch: ' + $release.tag_name) }
if (-not $release.prerelease) { $problems.Add('release is not a pre-release') }
if ($release.draft) { $problems.Add('release is a draft') }
if ([string]$release.body -cne $notes) { $problems.Add('release body differs from RELEASE_NOTES.md') }
foreach ($marker in $BodyMarkers) {
    if (([string]$release.body).Contains($marker)) { $problems.Add('release body contains ' + $marker) }
}

$assets = @{}
foreach ($asset in $release.assets) { $assets[$asset.name] = $asset }
foreach ($file in $files) {
    if (-not $assets.ContainsKey($file.Name)) { $problems.Add('missing asset: ' + $file.Name); continue }
    $asset = $assets[$file.Name]
    if ([int64]$asset.size -ne $file.Length) { $problems.Add('size mismatch: ' + $file.Name) }
    $download = Join-Path $tmp $file.Name
    Invoke-WebRequest -Method Get -Uri $asset.url -Headers $downloadHeaders -OutFile $download | Out-Null
    $want = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $got = (Get-FileHash $download -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($want -ne $got) { $problems.Add('checksum mismatch: ' + $file.Name) }
    else { Step ('verified ' + $file.Name + ' ' + $got) }
}
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue

Step ('tag commit ' + $localCommit)
Step ('url ' + $release.html_url)
if ($problems.Count -gt 0) {
    foreach ($problem in $problems) { Step ('FAIL ' + $problem) }
    throw ('release verification failed: ' + $problems.Count + ' problem(s)')
}
Step ('VERDICT: PASS - ' + $release.assets.Count + ' assets, pre-release, tag ' + $Tag + ' verified')
