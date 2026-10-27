# TASKS — NL Audio Matrix v2

**Versione:** 3.0 — scope universale multi-device  
**Data:** 24 ottobre 2026  
**Stato:** P0 completato; P1 pronto all'esecuzione.  
**Legenda stato:** `DONE`, `READY`, `BLOCKED`, `IN PROGRESS`, `TODO`.

---

## 0. Regole

- Ogni task ha un criterio di completamento verificabile (DoD) e file/ambiti previsti.
- P1 non richiede driver; P3+ richiede WDK/VM e non tocca il PC di lavoro.
- Nessun task può scrivere output ASIO senza rotta esplicita e conferma.
- Nessun task modifica i repository di riferimento (`obs-asio`, `JUCE`, `Windows-driver-samples`, `MicyWMD`, `SweetlyAudioDrivers`).
- Aggiornare `STATUS.md` al termine di ogni fase.

---

## 1. Percorso rapido — vertical slice (priorità assoluta)

Questi task anticipano e assorbono P1-01…P1-05; servono a ottenere una Matrix standalone funzionante prima di completare l'architettura.

| ID | Task | Dipendenze | File/ambiti | DoD |
|---|---|---|---|---|
| MVP-01 | Scheletro `app/` CMake + JUCE 9 console, target `NLMatrixProbe` | P0 | `app/CMakeLists.txt`, `app/src/main.cpp` | **DONE**: build x64, `--list` stampa 14 device ASIO |
| MVP-02 | Enumerazione completa ASIO: device, input/output names, count, rate/buffer | MVP-01 | `app/src/main.cpp` | **DONE (base)**: lista device; `--inspect` canali; resta dump JSON |
| MVP-03 | Cattura separata di tutti gli input in WAV + contatori + mappa canale↔buffer | MVP-02 | `app/src/main.cpp` | **DONE su ReaRoute**: 4 WAV, 48k campioni, report mappa; Apollo da verificare |
| MVP-04 | Verifica canale↔segnale con toni/PRN e `channel_map.json` | MVP-03 | `app/src/Analysis.*`, `reports/` | PASS/FAIL per canale; nessuno scambio; Apollo/osservazione reale |
| MVP-05 | `NLMatrixLive`: matrice runtime nel callback ASIO (canali→bus stereo configurabili) | MVP-03 | `app/src/live_main.cpp` | **DONE (CLI)**: ASIO→matrice→WASAPI; gain/L/R; meter |
| MVP-06 | Output WASAPI diagnostico + ring + meter/panic | MVP-05 | `app/src/live_main.cpp` | **DONE (diagnostica)**: tono 440 Hz → CABLE Input → CABLE Output verificato; WAV/peak |
| MVP-07 | Report slice e decisione di consolidamento | MVP-04, MVP-06 | `reports/MVP/REPORT.md`, `STATUS.md` | **IN CORSO**: report aggiornato; resta Apollo reale |
| MVP-08 | SRC/drift control input↔output | MVP-06 | `app/src/live_main.cpp` | ring stabile 8h, rate 44.1/48/96, no overflow/underrun patologici |
| MVP-09 | `NLMatrixBridge`: multi-bridge bidirezionale Windows↔ASIO con canali associati e nomi | MVP-05 | `app/src/bridge_main.cpp` | **DONE (test)**: Windows→ASIO con tono −20 dB; ASIO→Windows; due bridge nello stesso processo |
| MVP-10 | Configurazione bridge persistente + GUI minima | MVP-09 | `app/`, `reports/` | salvataggio/caricamento preset; selezione endpoint e canali; meter |
| MVP-11 | Loopback WASAPI nativo per sorgenti render | MVP-05 | `app/src/wasapi_loopback.*` | **DONE**: CABLE Input → ASIO −20 dB senza CABLE Output |
| MVP-12 | Router N×M con bus, route esplicite e mix-minus | MVP-11 | `app/src/router_main.cpp`, `app/router_test.json` | **DONE (test)**: una sorgente → due ASIO; bus → due ASIO; guadagni indipendenti |
| MVP-13 | Profilo 3 attori e binding endpoint | MVP-12 | `app/profiles/live_doppiaggio_3.json` | **DONE (validate)**: 4 bus, 8 sorgenti, 7 destinazioni, 20 route; runtime richiede endpoint/driver |
| MVP-14 | Endpoint Windows proprietari `NL Mic/Return` | MVP-13 | `driver/`, VM | **DONE (pool)**: driver ABI 2, 4 coppie indipendenti, 8 endpoint Windows, binding GUI/router, test simultanei e isolamento; provisioning dinamico add/remove ancora TODO. |
| MVP-15 | Application Loopback per sorgenti processo | MVP-12 | `app/src/process_loopback.*` | **DONE (test)**: processo `NLMatrixLive.exe` → ASIO −20 dB, senza CABLE Output |
| MVP-16 | Profilo 3 attori con ritorni come processi | MVP-15 | `app/profiles/live_doppiaggio_3_process.json` | **DONE (validate)**: 4 bus, 8 sorgenti, 7 destinazioni, 20 route |
| MVP-17 | GUI NLMatrixUI per profili, source/dest, bus, route, meter, save/load | MVP-12 | `app/src/ui_main.cpp` | **DONE (v2)**: patchbay visuale drag&drop, live apply, tray, palette/icona brand; resta rifinitura meter | 
| MVP-18 | Hot-routing e hot-plug runtime | MVP-17 | `app/src/router_main.cpp`, `app/src/ui_main.cpp` | **DONE (test VM)**: live reload senza restart, process/loopback/render watchdog, chiusura GUI→tray |
| MVP-19 | Installer/disinstaller EXE | MVP-14 | `installer/` | **DONE dev**: setup embedded + uninstaller testati `/silent` in VM; firma distribuzione da fare |
| MVP-20 | Canvas interattivo, cable editing, meter reali, tray | MVP-17 | `app/src/ui_main.cpp`, `app/src/router_main.cpp` | **DONE (test GUI)**: selftest 28 PASS, clicktest mouse reale 6/6, tono reale −9.1 dBFS, bus in/out, live gain/mute, offline nodes, tray |

> La slice usa VB-CABLE solo come banco di prova. La release finale userà il driver `NLMatrixAudio.sys` (P3/P4). VB-Matrix/ReaRoute sono opzionali, non dipendenze; il codice non hardcoda i loro nomi.

---

## 2. P1 — Acquisizione completa e convivenza (backlog completo)

> Nota: il probe MVP attuale è volutamente concentrato in `app/src/main.cpp`. I nomi file dei task P1 indicano la struttura prevista per il refactor (AsioProbe/Capture/Analysis), da fare quando la slice si consolida.

### P1-01 — Scheletro progetto app e build riproducibile
**Stato:** READY  
**Dipendenze:** P0  
**File/ambiti:** `app/CMakeLists.txt`, `app/CMakePresets.json`, `app/external/JUCE` (pin 9.0.4), `app/src/core/`, `app/src/backends/asio/`, `app/src/tools/NLMatrixProbe.cpp`.
**Attività:**
- CMake x64, C++20, `JUCE_ASIO=1`, target `NLMatrixProbe` e `NLMatrixCore`.
- Pin JUCE 9.0.4 (submodule o FetchContent con tag); nessun fork.
- Preset Debug/Release; output in `build/`.
- README build con comandi esatti.
**DoD:** `cmake --build` produce `NLMatrixProbe.exe`; build ripetibile su due run; nessuna installazione di sistema; versioni annotate.

### P1-02 — Verifica toolchain P1
**Stato:** READY  
**Dipendenze:** P0  
**File/ambiti:** `reports/P1/toolchain.md`.
**Attività:** VS 2022 BuildTools, CMake (installare se assente), Windows SDK, git; NON serve WDK in P1.
**DoD:** versioni e path documentati; build P1-01 eseguita.

### P1-03 — Enumerazione completa ASIO/WASAPI
**Stato:** READY  
**Dipendenze:** P1-01  
**File/ambiti:** `app/src/backends/asio/AsioHost.*`, `app/src/core/DeviceDescriptor.h`, `app/src/tools/NLMatrixProbe.cpp`.
**Attività:**
- `scanForDevices`; per device: nome, input/output channel names, count, sample types, rate, buffer, latency.
- Enumerazione WASAPI read-only (input/output/loopback) e altri ASIO.
- Output JSON versionato; distinzione input/output.
**DoD:** `NLMatrixProbe --enumerate --json reports/P1/enumerate.json` completo; conteggi reali (non assunti 10/6); differenze per rate documentate.

### P1-04 — Apertura e acquisizione di tutti gli ingressi
**Stato:** READY  
**Dipendenze:** P1-03  
**File/ambiti:** `app/src/backends/asio/AsioCapture.*`, `app/src/core/RingBuffer.*`, `app/src/tools/NLMatrixProbe.cpp`.
**Attività:**
- Aprire tutti gli input; output mask disabilitata di default; callback planare; buffer preallocati.
- Contatori callback/frame/XRUN; timestamp; nessuna alloc/log nel callback.
- Registrazione WAV separata per canale, nome con indice assoluto e nome driver; metadata JSON.
- Mappatura indice compatto ↔ bit maschera ↔ nome.
**DoD:** `NLMatrixProbe --capture-all --seconds 30 --out reports/P1/wav` produce N WAV (N = canali attivi) con dati; nessuna scrittura output; contatori coerenti.

### P1-05 — Mappa canali e verifica con segnali di test
**Stato:** READY  
**Dipendenze:** P1-04  
**File/ambiti:** `app/src/tools/NLMatrixProbe.cpp` (`--verify-map`), `app/src/core/SignalAnalysis.*`, `reports/P1/channel_map.json`, `reports/P1/REPORT.md`.
**Attività:**
- Generare/definire segnali distinti per sorgente (toni a frequenza diversa o PRN con seed).
- Analisi: presenza, frequenza/PRN correlation, livello, bleed; PASS/FAIL per canale.
- Ripetere a 48 kHz e 44.1 kHz (e 96 se disponibile).
- Compilare `channel_map.json` con indice assoluto/compatto, sorgente attesa/misurata.
**DoD:** ogni canale atteso è identificato nel canale corretto; nessuno scambio; report con misure e file; mappa riproducibile su due run.

### P1-06 — Procedura di test signal e supporto REAPER/Console
**Stato:** READY  
**Dipendenze:** P1-04  
**File/ambiti:** `reports/P1/TEST_SIGNALS.md`, progetto REAPER di test, note UAD Console.
**Attività:** documentare come alimentare Mic/Line, Monitor, Virtual, AUX con segnali separati; tracce REAPER/UAD Console; livelli bassi; nessun dato cliente.
**DoD:** procedura ripetibile; nessuna modifica permanente a REAPER/Console.

### P1-07 — Matrice di convivenza REAPER
**Stato:** READY  
**Dipendenze:** P1-04  
**File/ambiti:** `app/src/tools/NLMatrixProbe.cpp`, `reports/P1/coexistence.md`.
**Attività:**
- REAPER chiuso; REAPER prima; REAPER dopo; REAPER + OBS-ASIO.
- Buffer 128/256/512; rate 44.1/48/96; cambio rate/buffer.
- Misure: apertura, callback, errori, XRUN, stato REAPER, ascolto Console.
**DoD:** esito per combinazione; failure mode documentati; nessuna modifica a REAPER/Console.

### P1-08 — Uscite ASIO: enumerazione e dry-open
**Stato:** READY  
**Dipendenze:** P1-03  
**File/ambiti:** `app/src/tools/NLMatrixProbe.cpp` (`--outputs --dry-open`), `reports/P1/outputs.md`.
**Attività:** enumerare output; aprire maschera output senza scrivere per 10 s con REAPER/Console; osservare; documentare.
**DoD:** elenco output completo; esito dry-open; policy input-only confermata o motivata.

### P1-09 — Inventario altri device
**Stato:** READY  
**Dipendenze:** P1-03  
**File/ambiti:** `reports/P1/devices.md`.
**Attività:** ASIO/WASAPI disponibili, capability, loopback, conflitti noti.
**DoD:** inventario per P5.

### P1-10 — Report e gate G1
**Stato:** TODO (dopo P1-03…P1-09)  
**Dipendenze:** P1-01…P1-09  
**File/ambiti:** `reports/P1/REPORT.md`, `STATUS.md`.
**Attività:** consolidare ambiente, comandi, misure, PASS/FAIL, limiti, prossima azione.
**DoD:** G1 deciso con evidenze; `STATUS.md` aggiornato con prova PC reale sì/no.

### P1-11 — Base ring/queue e test unitari (parallel P2)
**Stato:** READY  
**Dipendenze:** P1-01  
**File/ambiti:** `app/src/core/RingBuffer.*`, `app/tests/ring_tests.cpp`.
**Attività:** SPSC, atomici, wrap, overrun/underrun, capacità vs fill; benchmark.
**DoD:** test verdi; nessuna allocazione nel percorso; usabile da P1-04.

---

## 3. P2 — Motore di routing e mixaggio

| ID | Task | Dipendenze | File/ambiti | DoD |
|---|---|---|---|---|
| P2-01 | GraphScheduler (timer alta risoluzione, MMCSS, late tick, modalità) | P1-11 | `core/GraphScheduler.*` | jitter/late tick misurati; funziona senza device |
| P2-02 | GraphEngine: nodi, archi, bus dinamici multicanale, somma, gain/pan/mute/solo/polarità | P1-11 | `core/GraphEngine.*`, `core/GraphSnapshot.*` | N×M corretto; nessun limite fisso; rampe |
| P2-03 | Snapshot RCU 3 slot | P2-02 | `core/GraphSnapshot.*` | nessun lock audio; cambio senza click |
| P2-04 | AdaptiveResampler + controller fill (capacità vs banda, ±ppm) | P2-01 | `core/AdaptiveResampler.*`, `core/FillController.*` | rate 44.1/48/96; drift; nessun 50% fisso |
| P2-05 | DeviceManager skeleton (Test + ASIO), lifecycle | P1-03 | `core/DeviceManager.*`, `backends/asio/*` | apertura/chiusura fuori audio; stati/errori |
| P2-06 | PresetStore v1 (schema, validazione, backup, migrazione) | P2-02 | `core/PresetStore.*`, `config/` | load/save/round-trip; invalid rifiutato |
| P2-07 | Mix-minus/anti-feedback compiler | P2-02 | `core/GraphCompiler.*` | client non riceve il proprio return; cicli bloccati |
| P2-08 | Test offline: WAV→grafo→WAV, null test, 8h | P2-02…P2-07 | `tests/`, `reports/P2/` | misure; nessun leak |
| P2-09 | Integrazione Apollo (dopo G1) | P1-10, P2-01…P2-08 | `backends/asio/*`, `tools/` | tutti i canali instradabili; output off default |

**Gate G2:** routing completo, null test, 8h, nessun limite fisso, scheduler funzionante senza ASIO.

---

## 4. P3 — Primo endpoint virtuale bidirezionale (VM)

| ID | Task | Dipendenze | File/ambiti | DoD |
|---|---|---|---|---|
| P3-01 | Vendor upstream Windows-driver-samples pinnato; WIL/INX/LICENSE | P0 | `driver/external/`, `driver/` | tree buildabile; licenze |
| P3-02 | Fork SysVAD ridotto 1 capture + 1 render; tabelle/INF | P3-01 | `driver/NLMatrixAudio/**` | build WDK; 1+1 endpoint |
| P3-03 | Control device KMDF + IOCTL + SDDL | P3-02 | `driver/.../transport.*` | IOCTL validati; ACL |
| P3-04 | Trasporto non-paged + MDL mapping + heartbeat | P3-03 | `driver/.../ring.*`, `transport_probe` | PCM in/out; cleanup |
| P3-05 | Hook WriteBytes/ReadBytes/SetState/TimerNotifyRT | P3-04 | `minwavertstream.cpp` | audio corretto; underrun/overrun |
| P3-06 | INF/nomi/formato 48k/2ch/float; provisioning | P3-02 | `driver/Package/**` | endpoint visibili |
| P3-07 | Test VM: install, app terza, engine kill, uninstall, Verifier | P3-05, P3-06 | `reports/P3/` | G3 |

**Gate G3:** endpoint visibile/funzionante su VM; PCM corretto; silenzio su engine kill; uninstall pulito.

---

## 5. P4 — Più endpoint e mix-minus

| ID | Task | Dipendenze | DoD |
|---|---|---|---|
| P4-01 | Estensione a 4+2 e massimo di build; provisioning | P3 | set configurabile; motore dinamico |
| P4-02 | Discovery set e assegnazione bus→endpoint | P3 | nessun hardcode |
| P4-03 | Mix-minus/anti-feedback UI/engine | P2-07, P4-02 | nessun eco; conferme |
| P4-04 | Test 4+2 simultanei, app target | P4-01…P4-03 | segnali indipendenti |
| P4-05 | Provisioning add/remove/restart | P4-01 | profilo preservato |

**Gate G4:** 4+2 simultanei, mix-minus, app target, uninstall.

---

## 6. P5 — Multi-scheda e clock

| ID | Task | Dipendenze | DoD |
|---|---|---|---|
| P5-01 | WasapiBackend nativo (input/output/loopback) | P2-05 | audio corretto; loopback |
| P5-02 | Multi-istanza ASIO, conflitti/stati | P2-05 | device indipendenti o conflict documentato |
| P5-03 | Scheduler multi-device, controller fill, modalità latenza | P2-01…P2-04 | clock indipendenti; drift |
| P5-04 | Hotplug/recovery ASIO/WASAPI/PnP | P5-01…P5-03 | reconnect; profilo |
| P5-05 | Failover GraphScheduler/watchdog | P5-03 | mute/recovery |
| P5-06 | Test multi-device 8h, rate misti, sleep/resume | P5-01…P5-05 | G5 |

**Gate G5:** multi-device stabile, clock indipendenti, hotplug, nessuna regressione REAPER/Console.

---

## 7. P6 — GUI e gestione

| ID | Task | Dipendenze | DoD |
|---|---|---|---|
| P6-01 | Matrice dinamica e patchbay | P4, P5 | routing completo via GUI |
| P6-02 | Pannello dispositivi e profili | P5 | aggiunta/rimozione/stato |
| P6-03 | Client/mix-minus/anti-feedback UI | P4-03 | conferme/blocchi |
| P6-04 | Preset, backup, migrazione, restore | P2-06 | persistenza |
| P6-05 | Diagnostica, panic, tray, accessibilità, localizzazione | P5 | G6 |

**Gate G6:** tutte le funzioni senza file di config; nessuna regressione.

---

## 8. P7 — Affidabilità e distribuzione

| ID | Task | Dipendenze | DoD |
|---|---|---|---|
| P7-01 | Stress 8–24h multi-device | P6 | nessun crash/leak |
| P7-02 | Sleep/resume, crash, disable/enable, uninstall | P6 | recovery/rollback |
| P7-03 | Sicurezza: ACL, fuzz IOCTL, mapping, log privacy | P5, P6 | nessuna vulnerabilità critica |
| P7-04 | Installer, firma/attestazione, upgrade/rollback | P6 | installer firmato su VM pulita |
| P7-05 | Accettazione PC di lavoro | P7-01…P7-04 | prova reale con backup |
| P7-06 | ReaRoute/ASIO virtuale opzionale | P7-05 | opzionale, non bloccante |

**Gate G7:** prodotto distribuibile e accettato.

---

## 9. Stato sintetico

| Fase | Stato | Note |
|---|---|---|
| P0 | DONE | Documenti prodotti; nessun codice |
| MVP | IN PROGRESS | Probe/live/bridge/router/GUI compilati; loopback, bus, mix-minus, Application Loopback, driver 4 coppie in VM, SRC device-rate e hot-route verificati; Apollo e multicanale/VST3 mancanti |
| P1 | READY | Backlog P1-01…P1-11 completo; assorbe MVP-01…05 |
| P2 | TODO | Dipende da MVP/P1; scheduler/grafo completo |
| P3 | TODO | Richiede WDK/VM; può partire in parallelo |
| P4–P7 | TODO | Dipendenze come sopra |

---

## 10. Primi cinque task da eseguire

1. **MVP-14b** — più coppie NL Mic/NL Return, indice endpoint nel trasporto, provisioning e binding GUI/router.
2. **MVP-04** — verifica reale Apollo dei 10 canali con segnali distinti e `channel_map.json`.
3. **MVP-08** — SRC/drift control input↔output e test 44.1/48/96 kHz.
4. **MVP-17 rifinitura** — meter con nomi leggibili, edit diretto route, gestione errori router.
5. **MVP-07** — consolidamento slice e report con Apollo reale.

Poi P1-06/P1-07 (convivenza REAPER/Console), P1-08 (output dry-open), P3 driver.
