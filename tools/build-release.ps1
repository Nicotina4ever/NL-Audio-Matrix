
param(
    [string]$Configuration = 'Release',
    [string]$OutputRoot = '',
    [switch]$SkipPortable
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# Windows PowerShell 5.1 reads a BOM-less script as ANSI, which turns the UTF-8
# characters of the generated texts into mojibake. Repair that known UTF-8 ->
# Windows-1252 double encoding before writing the files (no-op when parsed cleanly).
function Repair-Text([string]$Text) {
    if (-not ($Text.Contains([string][char]0x00E2) -or $Text.Contains([string][char]0x00C3))) { return $Text }
    try { $cp = [Text.Encoding]::GetEncoding(1252) } catch { return $Text }
    $fixed = [Text.Encoding]::UTF8.GetString($cp.GetBytes($Text))
    if ($fixed.Contains([string][char]0xFFFD)) { return $Text }
    return $fixed
}
if (-not $OutputRoot) { $OutputRoot = Join-Path $root 'dist' }
$csc = 'D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe'
if (-not (Test-Path $csc)) { throw "csc.exe not found: $csc" }
$router = Join-Path $root "app\build\NLMatrixRouter_artefacts\$Configuration\NLMatrixRouter.exe"
$ui = Join-Path $root "app\build\NLMatrixUI_artefacts\$Configuration\NLMatrixUI.exe"
if (-not (Test-Path $router)) { throw "Build NLMatrixRouter first: $router" }
if (-not (Test-Path $ui)) { throw "Build NLMatrixUI first: $ui" }

$buildInfo = (& $router --buildinfo 2>&1 | Select-Object -First 1).Trim()
$parts = $buildInfo -split '\s+'
$version = $parts[0]
$buildId = if ($parts.Count -gt 1) { $parts[1] } else { 'unknown' }
if (-not $version) { throw 'Cannot read version from router --buildinfo' }
$commit = (git -C $root rev-parse HEAD).Trim()
$shortCommit = (git -C $root rev-parse --short HEAD).Trim()
$dirty = ((git -C $root status --porcelain) -join [Environment]::NewLine).Trim()
if ($dirty) { Write-Warning 'Working tree is dirty; release build is not reproducible from a clean commit.' }
$builtUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')

$stagingRoot = Join-Path $root 'installer\build\staging'
Remove-Item $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
$app = Join-Path $stagingRoot 'app'
$driver = Join-Path $stagingRoot 'driver'
$licenses = Join-Path $stagingRoot 'licenses'
$portable = Join-Path $stagingRoot 'portable'
New-Item -ItemType Directory -Force -Path $app, $driver, $licenses, $portable | Out-Null

Copy-Item $ui (Join-Path $app 'NLMatrixUI.exe') -Force
Copy-Item $router (Join-Path $app 'NLMatrixRouter.exe') -Force
Copy-Item (Join-Path $root 'installer\licenses\*') $licenses -Force
foreach ($name in @('NLMatrixAudio.sys','ComponentizedAudioSample.inf','sysvad.cat','NLMatrixTest.cer','devcon.exe')) {
    Copy-Item (Join-Path $root "driver\package\$name") (Join-Path $driver $name) -Force
}

$info = [ordered]@{
    product = 'NL Audio Matrix'
    version = $version
    buildId = $buildId
    commit = $commit
    shortCommit = $shortCommit
    configuration = $Configuration
    builtUtc = $builtUtc
    routerSha256 = (Get-FileHash $router -Algorithm SHA256).Hash.ToLowerInvariant()
    uiSha256 = (Get-FileHash $ui -Algorithm SHA256).Hash.ToLowerInvariant()
    driverSha256 = (Get-FileHash (Join-Path $driver 'NLMatrixAudio.sys') -Algorithm SHA256).Hash.ToLowerInvariant()
}
$info | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $stagingRoot 'BUILD_INFO.json') -Encoding UTF8
$versionText = @(
    "version=$version",
    "buildId=$buildId",
    "commit=$commit",
    "shortCommit=$shortCommit",
    "builtUtc=$builtUtc"
) -join [Environment]::NewLine
Set-Content (Join-Path $stagingRoot 'VERSION.txt') -Value ($versionText + [Environment]::NewLine) -Encoding ASCII

# Build the uninstaller and the read-only verifier into the installer app payload.
# The setup helpers are shared with the installer (NLSetupCommon.cs).
$uninstallerExe = Join-Path $app 'NLMatrixUninstall.exe'
$manifest = Join-Path $root 'installer\installer.manifest'
$verifyManifest = Join-Path $root 'installer\verify.manifest'
$icon = Join-Path $root 'assets\nlmatrix_icon.ico'
$setupCommon = Join-Path $root 'installer\NLSetupCommon.cs'
$diagnosticsSource = Join-Path $root 'installer\NLSetupDiagnostics.cs'
$verifySource = Join-Path $root 'installer\NLMatrixVerify.cs'
$uiSource = Join-Path $root 'installer\NLSetupUI.cs'
$refs = @(
    '/r:System.dll', '/r:System.Core.dll', '/r:System.Windows.Forms.dll',
    '/r:System.Drawing.dll', '/r:System.Security.dll',
    '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll'
)
& $csc @('/nologo','/target:winexe','/platform:x64','/optimize+',
    ('/out:' + $uninstallerExe),
    ('/win32manifest:' + $manifest),
    ('/win32icon:' + $icon),
    $setupCommon,
    $diagnosticsSource,
    (Join-Path $root 'installer\NLMatrixUninstaller.cs')) @refs
if ($LASTEXITCODE -ne 0) { throw 'Uninstaller build failed' }

# The verifier must run without a UAC prompt at the next logon (asInvoker).
$verifyExe = Join-Path $app 'NLMatrixVerify.exe'
& $csc @('/nologo','/target:winexe','/platform:x64','/optimize+',
    ('/out:' + $verifyExe),
    ('/win32manifest:' + $verifyManifest),
    ('/win32icon:' + $icon),
    $setupCommon,
    $diagnosticsSource,
    $verifySource) @refs
if ($LASTEXITCODE -ne 0) { throw 'Verifier build failed' }

function Write-HashList([string]$baseDir, [string]$outFile) {
    $lines = New-Object System.Collections.Generic.List[string]
    Get-ChildItem $baseDir -Recurse -File | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($baseDir.Length).TrimStart('\\')
        $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $lines.Add($hash + '  ' + $rel)
    }
    Set-Content -Path $outFile -Value ($lines -join [Environment]::NewLine) -Encoding ASCII
}
Write-HashList $stagingRoot (Join-Path $stagingRoot 'SHA256SUMS.txt')

# Installer payload: app + driver + licenses + metadata.
$payload = Join-Path $stagingRoot 'payload'
New-Item -ItemType Directory -Force -Path $payload | Out-Null
Copy-Item $app (Join-Path $payload 'app') -Recurse -Force
Copy-Item $driver (Join-Path $payload 'driver') -Recurse -Force
Copy-Item $licenses (Join-Path $payload 'licenses') -Recurse -Force
foreach ($name in @('BUILD_INFO.json','VERSION.txt','SHA256SUMS.txt')) {
    Copy-Item (Join-Path $stagingRoot $name) (Join-Path $payload $name) -Force
}
# Never ship signing material: the public test certificate only, no private key.
$forbidden = Get-ChildItem $payload -Recurse -File | Where-Object {
    $_.Extension -in @('.pfx','.pvk','.snk','.key')
}
if ($forbidden.Count -gt 0) {
    throw ('signing material must not be in the payload: ' + (($forbidden | ForEach-Object { $_.Name }) -join ', '))
}
$payloadZip = Join-Path $stagingRoot 'payload.zip'
Compress-Archive -Path $payload -DestinationPath $payloadZip -CompressionLevel Optimal

$releaseDir = Join-Path $OutputRoot $version
Remove-Item $releaseDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
$installerExe = Join-Path $releaseDir ("NL-Audio-Matrix-$version-Setup-test-signed.exe")
$installerArgs = @('/nologo','/target:winexe','/platform:x64','/optimize+',
    '/main:Installer',
    ('/out:' + $installerExe),
    ('/resource:' + $payloadZip + ',payload.zip'),
    ('/win32manifest:' + $manifest),
    ('/win32icon:' + $icon),
    $setupCommon,
    $diagnosticsSource,
    $uiSource,
    $verifySource,
    (Join-Path $root 'installer\NLMatrixInstaller.cs')) + $refs
& $csc $installerArgs
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed' }

# Portable user-mode package: no driver, certificate, devcon, installer or test tools.
Copy-Item $ui (Join-Path $portable 'NLMatrixUI.exe') -Force
Copy-Item $router (Join-Path $portable 'NLMatrixRouter.exe') -Force
Copy-Item $licenses (Join-Path $portable 'LICENSES') -Recurse -Force
Copy-Item (Join-Path $stagingRoot 'BUILD_INFO.json') (Join-Path $portable 'BUILD_INFO.json') -Force
Copy-Item (Join-Path $stagingRoot 'VERSION.txt') (Join-Path $portable 'VERSION.txt') -Force
$readme = @"
NL Audio Matrix $version — portable user-mode package
====================================================
Build ID: $buildId
Commit:   $commit

No kernel driver, test certificate, devcon or installer is included.
Run NLMatrixUI.exe; the GUI starts the router executable from the same folder.
The package is for Windows 10/11 x64 and does not install or modify drivers.

QUICK CONNECT — APOLLO MIC 1 -> VB-CABLE (no NL driver needed)
---------------------------------------------------------------
1. Install VB-CABLE if it is not already on the PC (third-party, not included).
2. Open NLMatrixUI.exe, choose "Universal Audio USB ASIO" as the ASIO device,
   then File > Open profile and load PRESET_Apollo_to_VB-CABLE.json; press
   Apply/Start. The preset routes ASIO MIC 1 to the WASAPI render endpoint
   "CABLE Input".
3. In the browser/meeting app select the microphone "CABLE Output".
4. In Diagnostics check requested/actual sample rate and buffer, and that the
   meters move while speaking. Stop the engine when finished.

Using the NL Mic / NL Return endpoints requires the full installer package
(application + engine + NL virtual driver) on a Windows build where the
test-signed driver can load. See RELEASE_NOTES.md.
"@
$readme = Repair-Text $readme
Set-Content -Path (Join-Path $portable 'README.txt') -Value $readme -Encoding UTF8

# Ready-made user-mode project for the manual Apollo test (no NL driver needed).
$apolloPreset = @"
{
  "version": 3,
  "name": "Apollo MIC 1 -> VB-CABLE",
  "asio": "Universal Audio USB ASIO",
  "devices": [
    { "id": "asio:Universal Audio USB ASIO", "backend": "asio", "driver": "Universal Audio USB ASIO",
      "sampleRate": 48000, "buffer": 256, "clockMaster": true, "rateAuto": true }
  ],
  "buses": [],
  "sources": [
    { "id": "apollo-mic1", "name": "Apollo MIC 1", "type": "asio-in",
      "deviceId": "asio:Universal Audio USB ASIO", "channels": [0], "enabled": true }
  ],
  "destinations": [
    { "id": "vbcable-in", "name": "VB-CABLE Input", "type": "render", "endpoint": "CABLE Input",
      "channels": [0, 1], "enabled": true }
  ],
  "routes": [ { "source": "apollo-mic1", "destination": "vbcable-in", "gainDb": 0 } ]
}
"@
Set-Content -Path (Join-Path $portable 'PRESET_Apollo_to_VB-CABLE.json') -Value $apolloPreset -Encoding ASCII
Write-HashList $portable (Join-Path $portable 'SHA256SUMS.txt')
$portableZip = Join-Path $releaseDir ("NL-Audio-Matrix-$version-portable.zip")
if (Test-Path $portableZip) { Remove-Item $portableZip -Force }
Compress-Archive -Path (Join-Path $portable '*') -DestinationPath $portableZip -CompressionLevel Optimal

Copy-Item (Join-Path $stagingRoot 'BUILD_INFO.json') (Join-Path $releaseDir 'BUILD_INFO.json') -Force

$notes = @"
# NL Audio Matrix $version — pre-release (test build)

Build ID: $buildId
Commit:   $commit
Built UTC: $builtUtc

## Packages

1. NL-Audio-Matrix-$version-portable.zip — user-mode only, standard Windows.
   No kernel driver, certificate, devcon or installer inside. Runs on Windows
   10/11 x64 with the devices already present (an ASIO driver, VB-CABLE, or the
   NL Mic/Return endpoints if the full package is installed). Includes
   PRESET_Apollo_to_VB-CABLE.json and README.txt with the steps.

2. NL-Audio-Matrix-$version-Setup-test-signed.exe — the complete NL installer:
   application + engine + NL virtual driver (NL Mic 1-4 / NL Return 1-4) with a
   Windows setup wizard for install, upgrade, repair and uninstall.
   The NL kernel driver is **test-signed**: it loads only while Windows test
   signing is on and Secure Boot/HVCI do not block it. There is no
   production-signed variant in this release.
   - One file: it installs the application, the driver, the test certificate and
     the eight endpoints; no manual SYS/INF/certificate installation is needed.
   - The wizard is available in Italian and English (language selector in the
     footer) and opens at the Windows UI language. The compressed payload (app,
     engine, driver, certificate) is embedded in the same executable and is
     extracted to %TEMP% only while Setup runs.
   - Read-only diagnostics: driver absent/present/outdated/current/damaged,
     INF DriverVer, loaded .sys version, PnP instance and problem code,
     DriverStore packages, the eight endpoints, Secure Boot, TESTSIGNING, HVCI,
     Code Integrity events, and the action setup would choose.
   - Upgrade keeps presets/config and removes only obsolete NL driver packages;
     repair restores missing files/driver/service/endpoints; uninstall removes
     app, driver, certificate and entries while keeping user profiles.
   - Setup never enables test signing unless you tick the explicit option (or
     pass /accept-testsigning), and never changes Secure Boot or HVCI. On
     OEM/self-built PCs with Secure Boot on, the wizard explains the manual
     firmware step and its consequences instead of doing it.
   - After a required reboot, an asInvoker helper verifies driver, device, the
     eight endpoints and application start/stop at the next logon.

## Security configuration (read this before installing the driver)

The driver is test-signed, so the target PC must allow it:
  * Windows test signing on (bcdedit /set testsigning on) — the wizard can do it
    only with your explicit consent;
  * Secure Boot off on systems where it is enabled (manual firmware change);
  * Memory integrity (HVCI) off if it is enabled.
These reduce boot/kernel protection on that PC. Without them use the portable
package only. The installer never changes those settings on its own.

## Diagnostics from the command line

  NL-Audio-Matrix-$version-Setup-test-signed.exe --mode diagnose
  NL-Audio-Matrix-$version-Setup-test-signed.exe --mode verify /app-selftest

Both are read-only for the system; verify writes a JSON report under
%ProgramData%\NL Audio Matrix\reports (or %LOCALAPPDATA% when not writable).

## Status

Pre-release. A21 (return-path loopback frequency accuracy) and A22 (capture path
stability with concurrent clients) are open defects, so this build is not
Stable and must not be promoted to Stable until they are closed.

## Apollo manual test (authorized by the owner, not automated)

Portable: install/keep VB-CABLE, run NLMatrixUI.exe, load
PRESET_Apollo_to_VB-CABLE.json, press Apply/Start, select "CABLE Output" as the
microphone in the browser, check Diagnostics meters/rate/buffer, then stop.
UAD Console and Windows defaults are not modified by NL Audio Matrix.

## Integrity

SHA256SUMS.txt lists every asset in this folder; BUILD_INFO.json carries the
Build ID, commit and component hashes. No signing private key is included.
"@
$notes = Repair-Text $notes
Set-Content -Path (Join-Path $releaseDir 'RELEASE_NOTES.md') -Value $notes -Encoding UTF8

$guide = @"
NL AUDIO MATRIX $version — GUIDA ALL'INSTALLAZIONE
==================================================

COSA INSTALLA IL SETUP COMPLETO
  • Applicazione NLMatrixUI.exe, motore NLMatrixRouter.exe
  • Driver virtuali NL Mic 1-4 e NL Return 1-4 (otto endpoint)
  • Certificato pubblico di test e servizio driver
  Un solo file: non serve installare a mano SYS, INF o certificati.
  Nessuna chiave privata di firma è inclusa nel pacchetto.

IL DRIVER È FIRMATO PER TEST
  Windows carica il driver NL solo con la firma di test attiva (TESTSIGNING).
  Non esiste in questa release una variante con firma di produzione.

REQUISITI DI WINDOWS
  • Windows 10/11 x64 (build 19041 o successiva)
  • Firma di test attiva: il setup può attivarla solo con il tuo consenso esplicito
  • Secure Boot disattivato se presente (modifica manuale nel firmware)
  • Integrità memoria (HVCI) disattivata se attiva
  Queste modifiche riducono le protezioni di avvio di quel PC: usale solo su un
  PC di test o accettandone le conseguenze. Su un Windows standard senza modalità
  test usa la portable.

PC OEM/PREASSEMBLATI O ASSEMBLATI CON SECURE BOOT ATTIVO
  1. Riavvia nel firmware (BIOS/UEFI) e disattiva Secure Boot. È una modifica
     manuale: il setup non la esegue e non la propone automaticamente.
  2. Avvia Setup: nella pagina Opzioni vedrai lo stato reale; se serve abilita
     la firma di test con l'opzione esplicita e riavvia.
  3. Se «Integrità memoria» è attiva: Sicurezza di Windows → Sicurezza del
     dispositivo → Isolamento core → disattivala, poi riavvia.
  4. Riavvia Setup: la diagnostica deve risultare favorevole e l'installazione
     può procedere.

PROCEDURA CONSIGLIATA
  1. Avvia NL-Audio-Matrix-$version-Setup-test-signed.exe.
  2. Scegli la lingua in basso (Italiano / English): il wizard è bilingue.
  3. Premi «Diagnostica (sola lettura)» e controlla driver, endpoint e sicurezza.
  4. Scegli Installa / Aggiorna / Ripara / Disinstalla e premi Avanti.
  5. Al termine, se richiesto, riavvia: al successivo accesso la verifica
     automatica controlla driver, otto endpoint e avvio/arresto dell'app.

COSA VIENE CONSERVATO
  • Profili, preset e preferenze in %APPDATA%\NL Audio Matrix (anche dopo
    aggiornamento, riparazione e disinstallazione).
  • Driver di terzi (UAD, Focusrite, VB-CABLE) e impostazioni audio di Windows.
  • Gli aggiornamenti rimuovono solo i pacchetti driver NL obsoleti.

RIMOZIONE
  Impostazioni → App → NL Audio Matrix → Disinstalla, oppure Setup → Disinstalla.
  Rimuove app, driver, certificato ed endpoint; conserva i profili utente.

LOG E REPORT
  %TEMP%\NLMatrixSetup-<modalità>.log
  %ProgramData%\NL Audio Matrix\reports\last-diagnose.txt / last-verify.json
"@
$guide = Repair-Text $guide
Set-Content -Path (Join-Path $releaseDir 'GUIDA-INSTALLAZIONE.txt') -Value $guide -Encoding UTF8

# Asset-level manifest for the GitHub pre-release: written last so it covers
# every asset of this folder (its own file is excluded by definition).
$assetLines = New-Object System.Collections.Generic.List[string]
Get-ChildItem $releaseDir -File | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object Name | ForEach-Object {
        $assetLines.Add((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $_.Name)
    }
Set-Content -Path (Join-Path $releaseDir 'SHA256SUMS.txt') -Value ($assetLines -join [Environment]::NewLine) -Encoding ASCII

Write-Host ("Release directory: " + $releaseDir)
Get-ChildItem $releaseDir -File | Select-Object Name,Length | Format-Table -AutoSize
Get-Content (Join-Path $releaseDir 'SHA256SUMS.txt')