# TOOLCHAIN & BUILD — NL Audio Matrix v2

**Aggiornato:** 27 ottobre 2026. Questo documento sostituisce il piano toolchain
originale (storico in `docs/history/legacy-docs/08_TOOLCHAIN_BUILD.md`).

## Requisiti

- Windows 10/11 x64.
- Visual Studio 2022 Build Tools con workload C++ x64 (MSVC v143) e Windows SDK.
- CMake ≥ 3.22 e Ninja (inclusi nei Build Tools; il percorso usato dagli script
  è `D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\...`).
- JUCE 9 già vendorizzato in `JUCE/` (non riscaricare/modificare).
- Per il driver: WDK NuGet in `tools/wdk-packages/` (cache locale, non
  ricreare copie) e test signing solo su VM.
- Python (runtime DSH) per analyzer/harness: `C:\Users\Nicola\.dsh\...\python.exe`.

## Build applicazione

Lo script prepara l'ambiente VC++ e invoca CMake/Ninja:

```powershell
pwsh tools/build-app.ps1 -Targets NLMatrixRouter,NLMatrixUI,NLMatrixDriverAudioTest
pwsh tools/build-app.ps1 -Targets NLMatrixRouterLegacy   # A/B legacy
```

Target disponibili: `NLMatrixRouter` (engine v2), `NLMatrixUI` (GUI),
`NLMatrixDriverAudioTest` (recorder/player/latency), `NLMatrixRouterLegacy`
(motore pre-refactor congelato per confronto), più i tool diagnostici
(`NLMatrixLive`, `NLMatrixBridge`, `NLMatrixProbe`, `NLMatrixLoopbackTest`).

Artefatti: `app/build/<Target>_artefacts/Release/<Target>.exe`.
La directory `app/build` contiene CMakeCache/build.ninja e gli artefatti; la
cache oggetti è stata archiviata in `da_eliminare/…/build-cache-app-obj` durante
il cleanup del 2026-10-27: la prima build ricompila da zero (nessun problema).

Build manuale equivalente:

```powershell
cmd /c "call ""D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"" && cmake --build app\build --config Release --target NLMatrixRouter"
```

## Build driver (solo VM)

```powershell
pwsh driver/build-driver.ps1        # usa WDK locale in tools/wdk-packages
pwsh driver/install-test.ps1 -ConfirmInstall   # solo VM con test signing
```

Firma test e installazione restano confinate alla VM; nessun driver sul PC di
lavoro. Dettagli: `driver/sysvad-nl/README.md` e `driver/test/*`.

## Verifica

- La build produce gli eseguibili e li lascia in `app/build/.../Release`.
- Hash/verifica deployment: `tools/Deploy-NLMatrix-ToVM.ps1` e `vm/baseline/*.sha256`.
- Collaudo: `tools/vm/README.md` (Run-ContinuityTest, Run-FinalAcceptance,
  Run-ClockIsolation) + `tools/analyze_continuity.py` 1.2.0.
- Evidenze e stato: `reports/TEST_STATUS.md`, `reports/engine/FINAL_STATUS.md`.
