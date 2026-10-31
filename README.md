# NL AUDIO MATRIX v2 — matrice audio universale per Windows

**Stato:** 27 ottobre 2026 — engine **FINAL5** congelato e accettato; GUI con
pannello comprimibile, selezione canvas↔pannello, sample rate e buffer size reali.
Windows 11 x64, C++20 + JUCE 9, driver audio virtuale SysVAD (ABI v3).

## Cos'è

Una matrice audio/patchbay/mixer universale: acquisisce **tutti i canali** di più
dispositivi ASIO e WASAPI, li instrada con bus dinamici e multicanale, e li espone
come endpoint Windows virtuali capture/render selezionabili dalle applicazioni.
Include gain/pan/mute/solo/polarità, mix-minus, meter, preset/profili,
hotplug/recovery e diagnostica.

**Apollo/Focusrite sono dispositivi di test**, non il limite del prodotto.
Obiettivo pratico: eliminare OBS e VB-CABLE dalle sessioni remote mantenendo
REAPER e UAD Console sovrani (nessun plugin obbligatorio, nessun routing Console
modificato).

## Stato verificato (27/10/2026)

- **Engine FINAL5**: `vm/baseline/NLMatrixRouter_engine_v5_FINAL.exe`,
  SHA256 `2AE82FEBEF4DF4BB7DE9F7D2C149F2A28202CC7F4C73A7EC2EC58D43EEED0B86`.
  Accettazione su clock accurato: safe x2, balanced, late-consumer =
  engine/writer **PASS** + continuità WAV **PASS** (0 gap, 0 splice reali steady,
  0 drop/underrun steady); multi-ASIO resta limitazione documentata (ReaRoute
  richiede un client). Evidenze: `reports/engine/final_acceptance_v5/` e
  `reports/engine/FINAL_STATUS.md`.
- **Baseline storiche** (non cancellare): FINAL2 `38B4F6C6`, FINAL3 `7BA50557`,
  FINAL4 `AB5E363F`, legacy `E0C4553C` — tutte in `vm/baseline/` con `.sha256`.
- **GUI**: build `DBD69A86…`, selftest **54/54**, layout **8/8** (1366/1600/1920,
  IT/EN × Dark/Light/System), 20 screenshot in
  `reports/MVP/gui_verify_20261027/`. Include buffer modes, sample rate/buffer
  reali con apply graceful-stop, pannello sinistro comprimibile, selezione
  canvas↔pannello, localizzazione IT/EN, temi.
- **Limiti noti**: Focusrite non trasmette nella VM (passthrough USB VMware, non
  prodotto — righe hardware pronte per PC fisico); latenza definitiva da misurare
  su hardware (marker FINAL5 ~43 ms); DPI 125/150 non verificati.

## Struttura del repository

```text
app/                 router (engine v2), GUI JUCE, tool audio, legacy A/B, profili
  src/               router_main.cpp, ui_main.cpp, engine_tuning.h, driver client...
  legacy/            motore pre-refactor congelato per l'A/B (solo confronto)
  build/             build CMake/Ninja (artefatti Release; cache oggetti rigenerabile)
driver/              SysVAD fork NLMatrix: sorgenti kernel, IOCTL ABI v3, test/
tools/               build-app.ps1, analyzer continuità, latency, deploy VM
  vm/                harness di collaudo + README operativo
vm/                  ESCLUSO dal repository: VM, config, profili, evidenze,
                     binari congelati e baseline (NON spostare/cancellare);
                     lo scratch di sessione obsoleto è archiviato in da_eliminare/
reports/             TEST_STATUS.md + evidenze engine/A/B/GUI (log, JSON, WAV)
docs/                ENGINE_V2.md, 08_TOOLCHAIN_BUILD.md, 11_RISKS_LICENSES.md
docs/history/        documenti storici/superati (non canonici)
assets/ config/ installer/   icone, schema preset, sorgenti installer
da_eliminare/        materiale obsoleto archiviato per data (es.
                     2026-10-31-cleanup/), reversibile; revisionare e cancellare
                     solo dopo verifica
```

Dipendenze vendorizzate (non modificarle): `JUCE/`, `Windows-driver-samples/`,
`obs-asio/`, `MicyWMD/`, `SweetlyAudioDrivers/`; cache WDK in
`tools/wdk-packages/` (serve alla build driver). `.gitignore` esclude build,
VM, certificati, cache e `da_eliminare/`.

## Build

Prerequisiti: Visual Studio 2022 Build Tools (C++), CMake, Ninja; WDK NuGet per il
driver (vedi `docs/08_TOOLCHAIN_BUILD.md` e `driver/README.md`).

```powershell
# applicazione (router, UI, recorder/test, legacy A/B)
pwsh tools/build-app.ps1 -Targets NLMatrixRouter,NLMatrixUI,NLMatrixDriverAudioTest

# oppure un target
pwsh tools/build-app.ps1 -Targets NLMatrixRouter
```

Gli eseguibili finiscono in `app/build/<Target>_artefacts/Release/`.
Gli artefatti intermedi restano fuori da Git e sono rigenerabili; il materiale
archiviato per data è in `da_eliminare/` e si può cancellare a mano per liberare spazio.

## Uso rapido

```powershell
$router = 'K:\AI\NL Audio Matrix\app\build\NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe'
$ui     = 'K:\AI\NL Audio Matrix\app\build\NLMatrixUI_artefacts\Release\NLMatrixUI.exe'

& $router --list
& $router --config 'K:\AI\NL Audio Matrix\vm\engine\internal_balanced.json' --seconds 10
& $ui
```

Opzioni router: `--config`, `--seconds`, `--telemetry <file>`, `--log <file>`,
`--validate`, `--list`, `--control <file>` (stop graceful usato dalla GUI),
`--asio-info`, `--resampler-test`, `--matrix-test`.
Profili di esempio in `app/profiles/` e `vm/engine/` (questi ultimi per il collaudo).

## Test e collaudo

```powershell
. tools\vm\NLVM.ps1                       # NL-Exec / NL-CopyTo / NL-CopyFrom / NL-Kill
pwsh tools\vm\Run-FinalAcceptance.ps1 -RouterExe <router.exe> -OutRoot <dir>
pwsh tools\vm\Run-ContinuityTest.ps1 -Name ... -ConfigJson ... -RouterExe ...
pwsh tools\Run-ChannelSplitTest.ps1 -Mode Validate|Deterministic|Hardware
python tools\analyze_continuity.py <wav> --tone 1000
```

Il collaudo usa la VM (esclusa dal repo) e il clock accurato FlexASIO→endpoint
virtuale NLMatrix. Evidenze: `reports/engine/` con split verdict engine/WAV/clock;
stato sintetico: `reports/TEST_STATUS.md`; backlog: `TASKS.md`.

## VM e driver (regole)

- Test driver **solo su VM** con test signing; nessuna installazione sul PC di
  lavoro, nessuna modifica a REAPER/UAD Console, nessuno snapshot VM.
- La VM vive in `vm/` (~32 GB) e non fa parte del repository: non spostarla in
  `da_eliminare/`. Gli script `tools/vm/*.ps1` usano `vm/baseline/` e `vm/engine/`.
- Focusrite in VM: passthrough USB VMware non trasmette (ASIO non apre, WASAPI
  0 frame); non è un difetto dell'engine. Test hardware su PC fisico.

## Licenze

JUCE 9 AGPLv3/commerciale, ASIO SDK, driver SysVAD MS-PL, componenti Microsoft;
nessun codice GPL di OBS-ASIO copiato. Vedi `docs/11_RISKS_LICENSES.md`;
decisione di distribuzione prima della release.

## Documentazione autorevole

- [TASKS.md](TASKS.md) — backlog unico con stati.
- [DEVELOPMENT_STATE.md](DEVELOPMENT_STATE.md) — stato sintetico per riprendere.
- [reports/TEST_STATUS.md](reports/TEST_STATUS.md) — risultati verificati.
- [docs/ENGINE_V2.md](docs/ENGINE_V2.md) — architettura e tuning engine v2.
- [ARCHITECTURE.md](ARCHITECTURE.md) — architettura generale (design).
- [docs/08_TOOLCHAIN_BUILD.md](docs/08_TOOLCHAIN_BUILD.md) — toolchain e build.
- [docs/11_RISKS_LICENSES.md](docs/11_RISKS_LICENSES.md) — rischi e licenze.
- [driver/sysvad-nl/README.md](driver/sysvad-nl/README.md) — driver kernel e test signing.
- [tools/vm/README.md](tools/vm/README.md) — harness VM e comandi di collaudo.
- `docs/history/` — documenti superati (audit, piani, ADR, roadmap, STATUS
  vecchio): consultazione storica, non autorità.