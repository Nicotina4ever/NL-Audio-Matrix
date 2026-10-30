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
$notes = Get-Content $NotesFile -Raw

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
if ($null -eq $existing) {
    Step 'creating the pre-release'
    $create = '{"tag_name":' + (ConvertTo-Json $Tag -Compress) + ',' + $payload.TrimStart('{')
    $release = Invoke-RestMethod -Method Post -Uri "$api/releases" -Headers $headers `
        -ContentType 'application/json; charset=utf-8' -Body $create
} else {
    Step ("updating the existing release " + $existing.id)
    $release = Invoke-RestMethod -Method Patch -Uri ("$api/releases/" + $existing.id) -Headers $headers `
        -ContentType 'application/json; charset=utf-8' -Body $payload
}
Step ('release id ' + $release.id)

# --------------------------------------------------------------------- uploads
$files = Get-ChildItem $ReleaseDir -File | Sort-Object Name
$present = @{}
foreach ($asset in $release.assets) { $present[$asset.name] = $asset }
foreach ($file in $files) {
    if ($present.ContainsKey($file.Name)) { Step ('asset present: ' + $file.Name); continue }
    Step ('uploading ' + $file.Name + ' (' + $file.Length + ' bytes)')
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

$assets = @{}
foreach ($asset in $release.assets) { $assets[$asset.name] = $asset }
$tmp = Join-Path $env:TEMP ('nlrelease-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
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
