# MVP REPORT — NLMatrix vertical slice (Nicola Lanci)

**Data:** 24 ottobre 2026  
**Fase:** vertical slice / P1 (parziale)  
**Stato:** build e cattura verificati su driver ASIO virtuale; verifica Apollo ancora da fare (hardware non attivo in questa sessione).

## 1. Ambiente

- Windows 11 Enterprise 26300.
- Visual Studio 2022 BuildTools (`D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`).
- CMake 3.31.6-msvc6 (incluso in VS BuildTools), Ninja.
- JUCE 9.0.4 locale (`K:\AI\NL Audio Matrix\JUCE`), usato senza modifiche.
- Nessun WDK; nessun driver installato.

## 2. Build

```powershell
$vcvars='D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$cmake='D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
cmd /c "call `"$vcvars`" >nul && `"$cmake`" -S `"K:\AI\NL Audio Matrix\app`" -B `"K:\AI\NL Audio Matrix\app\build`" -G Ninja -DCMAKE_BUILD_TYPE=Release && `"$cmake`" --build `"K:\AI\NL Audio Matrix\app\build`" --parallel 8"
```

**Esito:** build riuscita. Eseguibile:
`K:\AI\NL Audio Matrix\app\build\NLMatrixProbe_artefacts\Release\NLMatrixProbe.exe`

## 3. Enumerazione ASIO (`--list`)

14 driver ASIO rilevati, tra cui:

- `Universal Audio USB` (Apollo; driver presente)
- `Universal Audio Thunderbolt`
- `ReaRoute ASIO (x64)`
- `VB-Matrix VASIO-128/64A/64B/8`
- `ASIO4ALL v2`, `FlexASIO`, `Realtek ASIO`, `Source Elements Audio Driver`, Focusrite, ecc.

Nessun device è stato aperto da `--list`: sola lettura del registro ASIO.

## 4. Ispezione canali

### Universal Audio USB
`--inspect --device "Universal Audio USB"`:
- 0 canali input, 0 output, rate 0, buffer 0.

**Interpretazione:** il driver è registrato ma l'Apollo non risulta connesso/attivo in questa sessione. Non è una conclusione sul driver UAD: la verifica P1 richiede Apollo collegato e REAPER chiuso o coordinato.

### ReaRoute ASIO (x64)
`--inspect --device "ReaRoute ASIO (x64)"`:
- 4 input: `ReaRoute REAPER=>CLIENT 1..4`
- 4 output: `ReaRoute Client=>REAPER 1..4`
- rate corrente 48 kHz.

## 5. Cattura (`--capture`)

Test su ReaRoute, 1 secondo, input-only:

- 4 canali attivi; maschera input `1111`.
- Output ASIO **non aperti** (`outputsOpened: no`).
- 4 WAV separati, 48.000 campioni ciascuno:
  `reports/MVP/capture_rearoute4/ch00_*.wav` … `ch03_*.wav`.
- Report con mappa compatta→assoluta, nomi, peak/RMS:
  `reports/MVP/capture_rearoute4/capture_report.txt`.
- Nessun overflow dopo l'aggiunta del margine di sicurezza; nessun errore driver.

**Cosa dimostra:** il percorso JUCE ASIO → callback planare → buffer preallocato → WAV per canale → report funziona, inclusa la mappa `indice compatto → canale assoluto`. È la base tecnica di P1, ma **non sostituisce** la verifica Apollo con segnali distinti.

## 6. Cosa non è stato fatto

- Nessuna apertura/cattura su Apollo (hardware non attivo).
- Nessuna scrittura su output ASIO.
- Nessun test con REAPER aperto.
- Nessun driver installato.
- Nessuna GUI, nessun WASAPI output, nessun multi-device.

## 7. Prossimi passi

1. Collegare/aprire Apollo e rieseguire `--inspect` e `--capture` (REAPER chiuso).
2. Verificare i 10 canali con segnali distinti e compilare `channel_map.json` (P1-T03).
3. Aprire REAPER e ripetere la matrice di convivenza (P1-T04).
4. Implementare la GUI minima e la persistenza delle configurazioni bridge.
5. SRC/drift control e test multi-rate.
6. Solo dopo: driver SysVAD su VM.

## 8. Live matrix (NLMatrixLive)

Aggiunto `app/src/live_main.cpp`, target `NLMatrixLive`:

- `--wasapi-list`: 11 output e 10 input WASAPI; `CABLE Input` e `CABLE Output` presenti.
- `--live --input "<ASIO>" --output "<WASAPI>" --send <ch>:<gainDb>:<L|R|B>`: matrice stereo nel callback ASIO, ring SPSC, uscita WASAPI shared.
- `--live --testtone --output "CABLE Input"`: tono 440 Hz a −20 dBFS per verificare il percorso di uscita.
- `--wasapi-capture --capture-input "CABLE Output" --out file.wav`: cattura WASAPI con WAV e stima di frequenza.

**Test end-to-end eseguito senza Apollo:** tono 440 Hz → `CABLE Input` → `CABLE Output` → cattura.
- peak −20.0 dB, RMS −23.1 dB, circa 427 Hz stimati per zero-crossing (tono 440 Hz; stima grezza, non FFT).
- WAV: `reports/MVP/cable_loopback.wav`.

**ASIO + WASAPI simultanei:** `ReaRoute ASIO (x64)` input + `CABLE Input` output, 2 s, input reale silenzioso (ReaRoute senza REAPER): nessun crash, callback e meter attivi; il ring si riempie perché in questa slice non c'è SRC/drift control (osservazione utile per P5).

**Limiti attuali:** nessun SRC (input/output devono avere lo stesso rate), niente GUI, niente persistenza preset, niente multi-bus/multi-device; il driver virtuale non è ancora stato sviluppato.

## 9. Bridging bidirezionale multi-endpoint (NLMatrixBridge)

Aggiunto `app/src/bridge_main.cpp`, target `NLMatrixBridge`:

- Un solo device ASIO aperto full-duplex; più bridge indipendenti nello stesso processo.
- `win-to-asio`: endpoint WASAPI capture → canali ASIO output selezionati.
- `asio-to-win`: canali ASIO input selezionati → endpoint WASAPI render.
- Nome, endpoint e canali per bridge; ring indipendenti; nessuna somma implicita.
- Canali 0-based; `--inspect-asio` stampa i nomi reali.
- `--list` elenca device ASIO e endpoint WASAPI.

**Test eseguiti senza Apollo:**
- Windows→ASIO: tono 440 Hz in `CABLE Output` → canali ASIO ReaRoute 0/1; meter `asioOut −20 dB`, nessun crash.
- ASIO→Windows: ReaRoute input 2/3 → `CABLE Input`; percorso attivo, input silenzioso atteso.
- Due bridge contemporanei (una direzione per ciascuno) nello stesso processo: entrambi attivi.
- `VBMatrix In/Out` testato come coppia virtuale: **non** ha mostrato passthrough senza l'app VB-Matrix, quindi non è usato e non è una dipendenza. Il codice resta generico: qualsiasi endpoint WASAPI disponibile può essere associato.
- CABLE è usato solo come banco di prova; il prodotto finale userà il driver proprietario.

**Limiti:** nessun SRC; un ring stereo per bridge; nessuna GUI; nessun endpoint Windows creato dal programma (serve il driver).

## 10. Router N×M, loopback nativo e bus

Aggiunti:
- `app/src/wasapi_loopback.{h,cpp}` — loopback WASAPI nativo, senza `AUDCLNT_STREAMFLAGS_LOOPBACK` di JUCE e senza coppia capture di un cavo virtuale.
- `app/src/router_main.cpp` — router JSON con:
  - sorgenti `asio-in`, `loopback`, `capture`, `bus`;
  - destinazioni `asio-out`, `render`;
  - bus interni topologicamente ordinati e riutilizzabili come sorgenti;
  - route esplicite con guadagno; mix-minus ottenuto omettendo la route verso il proprio ritorno.
- `app/router_test.json` — una sorgente loopback verso due destinazioni ASIO.
- `app/router_bus_test.json` — sorgente → bus → due destinazioni ASIO.
- `app/profiles/live_doppiaggio_3.json` — profilo 3 attori: 4 bus, 8 sorgenti, 7 destinazioni, 20 route, mix-minus per partecipante, tre coppie ASIO distinte per REAPER.

**Test eseguiti:**
- Router: `test-src` da `CABLE Input` via loopback → ASIO 0/1 e 2/3; meter `-20.0` e `-26.0 dB` (guadagni 0 e −6 dB), `callbacks` attivi.
- Bus: sorgente → bus → ASIO 0/1 e 2/3; meter bus `-20 dB`, destinazioni `-20`/`-26 dB`; nessuna ri-cattura dell'endpoint.
- Mix-minus esplicito: `app/router_mixminus_test.json`; la sorgente alimenta il bus "to-others" ma non il proprio ritorno. Misure: `asio-others -20 dB`, `asio-own -100 dB`, bus `to-others -20 dB`, `own-return -100 dB`.
- Profilo 3 attori: `--validate` OK; il runtime richiede gli endpoint `NL Mic/Return` del driver (o binding temporanei) e Apollo.

**Nota di isolamento:** se una piattaforma restituisce già più persone premiscelate in un unico stream, il routing non può separarle. L'isolamento multitraccia richiede un'uscita distinta per interlocutore, come nel profilo.

## 11. Microsoft Application Loopback

Aggiunto `app/src/process_loopback.{h,cpp}`:

- attivazione con `ActivateAudioInterfaceAsync` + `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`;
- `AUDIOCLIENT_ACTIVATION_PARAMS` con PID target e `INCLUDE_TARGET_PROCESS_TREE`;
- completion handler agile (WRL `FtmBase`) e attivazione su thread MTA, necessari per evitare `E_ILLEGAL_METHOD_CALL`;
- formato richiesto 48 kHz/2ch/16-bit PCM con `AUTOCONVERTPCM`, conversione a float;
- polling `IAudioCaptureClient` in un thread dedicato, come per il loopback endpoint.

**Test:** processo `NLMatrixLive.exe` (tono 440 Hz) → ASIO ReaRoute, meter `-20 dB`. Funziona senza usare `CABLE Output` e senza catturare l'intero endpoint.

**Vantaggi concreti:**
- Source-Connect, SessionLinkPRO e Nexus possono condividere lo stesso dispositivo di uscita e restare sorgenti separate.
- I **NL Return** possono essere process loopback; i **NL Mic** virtuali restano necessari per il percorso bidirezionale verso le applicazioni.
- Nessuna dipendenza da VB-CABLE/VB-Matrix.

**Limiti:** richiede Windows 10 build 20348+; il processo deve essere in esecuzione; il nome può essere ambiguo (usare PID); cattura solo audio WASAPI renderizzato dal processo, non flussi ASIO esclusivi; se l'app mixa già più partecipanti in un unico stream, non è separabile.

Profilo alternativo validato: `app/profiles/live_doppiaggio_3_process.json` (4 bus, 8 sorgenti, 7 destinazioni, 20 route); usa process loopback per i tre ritorni e mantiene le destinazioni `NL Mic` per i futuri endpoint proprietari.

## 12. GUI NLMatrixUI

Aggiunto `app/src/ui_main.cpp`, target `NLMatrixUI`:

- profilo con nome e device ASIO;
- liste sorgenti ASIO, loopback, capture, processo e bus;
- destinazioni ASIO-out e render;
- bus nominabili;
- route con gain, enable/disable, update/remove;
- meter dal processo `NLMatrixRouter`;
- Start/Stop/Apply;
- Save/Load profilo JSON;
- Refresh dispositivi.

La GUI lancia il router come processo separato e ne legge l'output; in caso di errore del router lo stato viene mostrato nella finestra. Non richiede editing manuale di JSON.

![GUI NL Audio Matrix con profilo caricato](ui_profile.png)

Limiti: gli endpoint Windows `NL Mic`/`NL Return` non esistono ancora senza driver; la GUI può già creare i bus e le destinazioni che li useranno. Il driver va sviluppato su VM con WDK.

## 13. Driver NLMatrixAudio (POC VM)

Fork SysVAD MS-PL in `driver/sysvad-nl`:

- `NL Return`: render WaveRT, audio applicazione → ring kernel;
- `NL Mic`: capture WaveRT, ring kernel → audio applicazione;
- controllo `\\.\NLMatrixAudio` con IOCTL `GET_INFO`, `READ_RENDER`, `WRITE_CAPTURE`, `RESET`;
- ring non paged, contatori/peak diagnostici, conversione gestita lato client;
- build locale con WDK NuGet; package test-signed e installato **solo nella VM Windows 11**.

Risultati VM:

```text
device: ROOT\MEDIA\0000  NLMatrix Virtual Audio Device  running
service: sysvad_componentizedaudiosample  RUNNING
inputs:  NL Mic (NLMatrix Virtual Audio Device)
outputs: NLMatrix Virtual Audio Device
ABI 1 render 48000/2/16  capture 48000/1..2/16
```

Test:

1. `NLMatrixDriverTest --tone` → ffmpeg capture NL Mic: RMS ≈ −11 dBFS.
2. Wasapi/JUCE tone su NL Return → `NLMatrixDriverTest --read`: `render_pushed > 0`, `render_peak16=11470`.
3. Catena completa NL Return → `NLMatrixDriverTest --loopback` → NL Mic → ffmpeg: RMS ≈ **−21.9 dBFS**.

Limiti POC:

- una sola coppia render/capture; servono più endpoint indicizzati per sostituire più VB-CABLE;
- il nome visualizzato del render è ancora “Altoparlanti (NLMatrix Virtual Audio Device)” (naming localizzato Windows); GUI/router usano l’alias NL Return;
- conversione formato/rate nel client, non ancora SRC kernel;
- test ASIO+driver nella VM richiede un device ASIO (REAPER/ReaRoute non installati nella VM).

## 14. GUI interattiva, meter e diagnostica router (24/10/2026)

- Canvas: nodi liberamente trascinabili, posizioni `x/y` nel profilo, selezione singola/multipla e drag di gruppo, zoom rotella, pan tasto centrale, Canc, menu contestuali nodo/cavo/sfondo, rename/enable/disable/remove, aggiunta nodi.
- Cavi: hover evidenziato, hit-testing Bézier con `Path::getNearestPoint` e tolleranza 10–16 px, selezione, **pulsante X** sul cavo selezionato, Canc, menu contestuale Disconnect, cambio destinazione trascinando l'estremità, gain/mute; cavi disegnati sopra i nodi per accessibilità. Lista **Connections** con selezione e pulsante Disconnect.
- Bus: porta sinistra input e porta destra output; drag dalla porta destra verificato con mouse reale; bus-source canonica riusata (niente duplicati); più route ingresso/uscita; cavi ricalcolati da `sourceNodeRect`/`destinationNodeRect` con zoom/pan. Un bus interno non crea endpoint Windows.
- Persistenza: write-through su profilo attivo; test verifica posizione, reload layout, rimozione nodo con cleanup cavi.
- Meter: telemetria `METERS` JSON dal router (file, 10 Hz), L/R, peak dBFS, clip, offline. Tono 1 kHz reale: source e destination ≈ −9.1 dBFS (`ui_meters_tone.png`). Gain/mute live verificato su router attivo (destination −100 dB, source invariata, nessun restart).
- Router: sorgenti/destinazioni mancanti (driver, loopback, capture, process, render) restano offline e non chiudono il motore; il vecchio exit 16 da driver assente non si verifica più; un profilo con zero route è valido e l'ultimo Disconnect aggiorna il grafo live. Errori ASIO/grafo/driver mostrati in GUI; stdout letto in thread dedicato e log su file.
- Tray: Show/Hide/Start/Stop/Quit con posizione al cursore; chiusura finestra nasconde e l'engine continua. Callback testate.
- `NLMatrixUI --selftest`: 28 PASS. `--clicktest`: mouse Windows reale 7/7 PASS (hover, click, Canc, router vivo, drag bus, disconnect lista, menu contestuale destro) anche dall'app installata; log `GUI_REAL_MOUSE_TEST.log`.
- Leggibilità/DPI: font e controlli ingranditi, pannello sinistro in Viewport scrollabile, toolbar compatta sotto 1200 px, layout minimo 900×520; coordinate logiche JUCE per-monitor DPI. Non testato su display fisico a 125/150/200%.
- Installer: `installer/build/NLMatrixSetup.exe`; payload app+driver; installazione `/silent` exit 0 sull'ultima build; app installata usata per il clicktest reale. Driver multi-endpoint ABI 2: pool di 4 coppie indipendenti, `filters=4/4`, 8 endpoint Windows online, nuovo hardware ID `Root\NLMatrixVirtualAudio`; uninstaller rimuove entrambe le identità. GUI: pulsanti ? e selettori di tipo leggibili, finestra Virtual devices (screenshot `ui_virtual_devices.png`).

Evidenze: `reports/MVP/GUI_SELFTEST.log`, `GUI_SELFTEST_audio_tone.log`, `ui_meters_tone.png`, `ui_offline.png`.

Non ancora verificato con mouse/tastiera fisici: menu tray e menu contestuali (callback/eventi sintetici testati). Pannelli laterali non richiudibili; meter stereo L/R (multicanale >2 no); una sola coppia endpoint; SRC ASIO↔driver e test Apollo aperti.

## 15. Comandi

```powershell
$probe='K:\AI\NL Audio Matrix\app\build\NLMatrixProbe_artefacts\Release\NLMatrixProbe.exe'
$live='K:\AI\NL Audio Matrix\app\build\NLMatrixLive_artefacts\Release\NLMatrixLive.exe'
$bridge='K:\AI\NL Audio Matrix\app\build\NLMatrixBridge_artefacts\Release\NLMatrixBridge.exe'
$router='K:\AI\NL Audio Matrix\app\build\NLMatrixRouter_artefacts\Release\NLMatrixRouter.exe'

& $probe --list
& $probe --inspect --device "Universal Audio USB"
& $probe --capture --device "Universal Audio USB" --seconds 5 --out "K:\AI\NL Audio Matrix\reports\MVP\apollo"

& $live --live --testtone --output "CABLE Input (VB-Audio Virtual Cable)" --seconds 5

& $bridge --list
& $bridge --inspect-asio "ReaRoute ASIO (x64)"
& $bridge --asio "ReaRoute ASIO (x64)" `
    --bridge "SessionLink->ASIO|win-to-asio|CABLE Output (VB-Audio Virtual Cable)|0,1" `
    --bridge "ASIO->Windows mic|asio-to-win|2,3|CABLE Input (VB-Audio Virtual Cable)" `
    --seconds 5

& $router --list
& $router --config "K:\AI\NL Audio Matrix\app\router_bus_test.json" --seconds 5
& $router --config "K:\AI\NL Audio Matrix\app\router_mixminus_test.json" --seconds 5
& $router --config "K:\AI\NL Audio Matrix\app\router_process_test.json" --seconds 5
& $router --config "K:\AI\NL Audio Matrix\app\profiles\live_doppiaggio_3.json" --validate
& $router --config "K:\AI\NL Audio Matrix\app\profiles\live_doppiaggio_3_process.json" --validate

# GUI
& 'K:\AI\NL Audio Matrix\app\build\NLMatrixUI_artefacts\Release\NLMatrixUI.exe'
```

## Reference review and device-rate conversion (25 Oct 2026)

Read-only analysis of atkAudio Plugin for OBS (AGPL-3.0) is in
`reports/MVP/ATKAUDIO_REFERENCE_ANALYSIS.md`. No AGPL/GPL code or dependency was
introduced.

`NLMatrixRouter` now uses an independently written `StereoRateConverter` between
device-clocked stereo rings and the ASIO graph: declared source/destination
rates, fill-level trim clamped to ±300 ppm, 15 ms target, 64-sample fade-in,
silence-and-reprime on underflow, and 500 ms live format polling for each driver
endpoint. Loopback input is accepted at a rate different from the ASIO graph.

Tests:
- `RESAMPLER_TEST.log`: 4/4 PASS (96→48, 44.1→48, 48→96, 48→48).
- `DRIVER_MULTITONE_10S.log`: four endpoints written simultaneously for 10 s,
  4/4 PASS, per-endpoint byte deltas isolated.
- `SIMULTANEOUS_4_TONES.log`: Return/Mic pairs 1..4 played 500/1000/1500/2000 Hz
  simultaneously; recorded peaks 500/1000/1475/2000 Hz with separate endpoints.
- `PAIR0_RETURN1_MIC1_RMS.log`, `PAIR1_RETURN2_MIC2.log`,
  `PAIR2_RETURN3_MIC3.log`, `PAIR3_HDMI_MIC4_RMS.log`: individual pair
  isolation at 96→48 and 96→16 kHz.
- `ROUTER_4PAIRS_ASIO_HOT.log`: four driver sources/destinations with FlexASIO,
  live route rewrite accepted twice, no restart.

Remaining limits: router graph is still stereo-only, no VST3 host, no
multithreaded DAG, no measured host-clock feed-forward (declared rate + trim
only), and no per-route latency compensation. These are architecture phases, not
features to copy from the AGPL reference.

Final verification (25 Oct 2026, VM):
- GUI selftest: 28/28 PASS (`GUI_SELFTEST.log`).
- Real Windows mouse clicktest: 7/7 PASS (`GUI_REAL_MOUSE_TEST.log`).
- Final installer silent install: exit 0, installed router `--resampler-test` 4/4 PASS (`INSTALLER_FINAL.log`).
- Four-pair simultaneous: peaks 500/1000/1475/2000 Hz on NL Mic 1..4, no cross-channel signal (`SIMULTANEOUS_4_TONES.log`).
- Driver four-endpoint transport 10 s: 4/4 PASS (`DRIVER_MULTITONE_10S.log`).
- Router 4 driver pairs + FlexASIO + live route rewrite: PASS (`ROUTER_4PAIRS_ASIO_HOT.log`).