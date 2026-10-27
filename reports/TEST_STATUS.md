# TEST_STATUS — NL Audio Matrix

**Documento unico di stato test.** Aggiornare questo file soltanto dopo un test
significativo; non creare report duplicati. Le evidenze restano nei log indicati.

- **Ultimo aggiornamento:** 2026-10-27 21:25 (Europe/Rome)
- **Build verificata:** Alpha 0.4.0-alpha. Engine **FINAL5** router SHA256 `2AE82FEB...` (frozen `vm/baseline/NLMatrixRouter_engine_v5_FINAL.exe`) con API per la GUI (--control graceful stop, METERS devices[], exit 24, driverTargetMs), GUI build sha256 `dbd69a86...` (selftest **54/54**, layouts 8/8, cambio formato reale 48k→44.1k→48k via graceful stop), driver ABI3 installato in VM (image SHA256 CD95712E...). Evidenze engine: `reports/engine/final_acceptance_v5/` + `reports/engine/FINAL_STATUS.md`; GUI: `reports/MVP/gui_verify_20261027/`.
- **Evidenze storiche (cleanup 2026-10-27):** i file di primo livello di `reports/MVP/` (screenshot/WAV/log 24-27/10) sono archiviati in `da_eliminare/2026-10-27-consolidation/reports-superseded/MVP-legacy/` (stessi nomi); restano attive le sottocartelle `reports/MVP/audio/`, `reports/MVP/gui_verify_20261027/`, `reports/MVP/DIAG_HOST_TEST/`. Accettazione e validazione FINAL4 sono in `da_eliminare/2026-10-27-consolidation/reports-superseded/` (`-FINAL4`); evidenze correnti: `reports/engine/final_acceptance_v5/`, `reports/engine/final5_markers/`. Il materiale archiviato è reversibile e consultabile.
- **Laboratorio principale:** VM Windows 11 x64 con FlexASIO, ReaRoute ASIO x64,
  NLMatrix Virtual Audio Device (4 coppie), VB-CABLE diagnostico.
- **Hardware fisico:** nessun Apollo collegato in questa sessione. Non dichiarare
  verificato nulla che richieda Apollo/Windows fisico.
- **Regola:** nessuna installazione driver kernel o modifica test signing sul PC
  principale; i test driver restano confinati alla VM.

## Legenda

| Stato | Significato |
|---|---|
| **PASS** | Misura o test automatico superato, con log verificabile. |
| **FAIL** | Test eseguito ma risultato non conforme. |
| **LIMITATO** | Funzione implementata/parzialmente esercitata, ma la verifica è incompleta o dipende dall'ambiente. |
| **NON TESTATO** | Non eseguito, oppure richiede hardware/ambiente non disponibile. |

## Riepilogo

| Area | Stato complessivo |
|---|---|
| Matrice N×M sintetica | PASS |
| SRC / drift converter | PASS |
| Bus e mix-minus | PASS |
| Routing live | PASS |
| Loopback WASAPI reale | PASS |
| ASIO ReaRoute runtime | PASS (test sources, non REAPER) |
| Process Loopback audio | LIMITATO (nessun pacchetto nella VM) |
| GUI / discovery / profili | PASS |
| Driver virtuale NLMatrix | PASS in VM (POC) |
| Hot-plug / recovery | PASS in VM |
| Apollo reale / Windows fisico | NON TESTATO — PC principale |
| Multi-ASIO contemporaneo | LIMITATO — architettura per-device in engine v2; verifica runtime multi-device pendente VM |
| **Motore audio v2 FINAL5 (engine/writer)** | **PASS — FINAL4 + API GUI e fade anti-click; accettazione su clock accurato (`vm/baseline/NLMatrixRouter_engine_v5_FINAL.exe`, 2AE82FEB...): safe x2, balanced e late-consumer = engine/writer PASS + WAV PASS; steady drops/underruns/starvation 0; --control graceful stop verificato exit 0; devices[] requested-vs-actual verificato; regressione 10 s PASS. Multi-ASIO resta limitazione documentata (ReaRoute client)** |
| **GUI: sample rate / buffer size reali (G4)** | **PASS — verifica end-to-end nella VM: 48000/256 → 44100/256 → 48000/256 via apply di produzione; rate applicati davvero (44100 e 48000), buffer arrotondato dal driver a 1024 e mostrato come tale; graceful stop --control usato, nessun crash, persistenza profilo/preferenze, exit 24 mappato; selftest 54/54** |
| **GUI: leggibilità, pannello comprimibile, selezione canvas↔pannello (G1-G3)** | **PASS in VM (screenshot 20 PNG + layout 8/8): font +1 pt e contrasto, frecce disegnate con Path, pannello sinistro comprimibile con stato persistito, selezione nodo/cavo sincronizzata col pannello sinistro** |
| Continuità assoluta WAV in VM | **PASS** — analyzer 1.2.0, split verdict engine/WAV/clock; scenari balanced/safe/late-consumer/GUI PASS (0 gaps, 0 real splices steady, tono ~1000 Hz); residui startup-only counter classificati WARN |
| Driver ABI v3 (target capture + contatori) | PASS in VM (installato/verificato) |
| Buffer modes Low/Balanced/Safe/Advanced + GUI | **PASS** — persistenza/round-trip verificata (selftest 40/40, preferences/profilo), telemetria buffering in Diagnostics |
| Latenza end-to-end reale | LIMITATO — metodo marker validato; mediana 39–46 ms sul clock NLMatrix accurato, discrepanza ~35–45 ms rispetto al modello di coda irrisolta; misura fisica (Focusrite/PC reale) pendente |
| Multi-ASIO contemporaneo | LIMITATO — architettura per-device in engine v2; ReaRoute extra richiede un client ReaRoute attivo (senza: starved, nessun hang); test con secondo device reale (Focusrite) bloccato dal passthrough USB VM |
| Focusrite post-fix (voce reale) | BLOCKED — passthrough USB VMware: dopo re-plug fisico e xHCI il device enumera con endpoint WASAPI ma lo streaming resta a 0 frame e l'ASIO non apre; A/B hardware pronto per PC fisico/VM riparata |

## Accettazione engine FINAL4 (2026-10-27, VM)

Router congelato `vm/baseline/NLMatrixRouter_engine_v4_FINAL.exe` SHA256
`AB5E363F237E5D292492DCAB89988AFE0823B6BF7D0DD3C4D61EB25244DB090B`.
Test clock: FlexASIO (backend WASAPI, 960 frame) su endpoint render virtuale
NLMatrix (clock accurato, +0.18% vs QPC); analizzatore 1.2.0 con classificazione
startup (50 ms) vs steady. Evidenze storiche (FINAL4):
`da_eliminare/2026-10-27-consolidation/reports-superseded/final_acceptance-FINAL4/`
(FINAL_ACCEPTANCE.md, acceptance_summary.json, cartelle per scenario) e
`.../final4_validation-FINAL4/` (13 run mirati).

| scenario | engine/writer | continuità WAV | note |
|---|---|---|---|
| internal_safe_rep1/rep2 | PASS | PASS | 0 starved, 0 drop/underrun steady, 0 gap/splice; startup-only kernel 10560/12576 B |
| internal_balanced_rep1 | PASS | PASS | 0 starved, 0 drop/underrun steady, 0 splices; partialWrites 19 |
| late_consumer_15s | PASS | PASS | consumerActive false→true, drain corretto, 0/0 steady |
| latency / latency_markers | PASS | n/a | marker: mediana 38.96–45.46 ms (30.06–50.55); ringFill 10–21 ms, driverUsed 50 ms |
| gui_profile_12s | PASS | PASS | 0/0 steady, startup 960 B |
| multi_asio_15s | FAIL (documentato) | PASS | d1 ReaRoute starved 1251 senza client ReaRoute; d0 steady underrun 10080 B in questo scenario; nessun hang |

Verdetto: il difetto del writer bilanciato di FINAL3 (minimo chunk che
oltrepassava il target del ring kernel → 10–50 B di drop steady e splice) è
eliminato (servo a deficit esatto). Residui: transitori di startup classificati
WARN; multi-ASIO richiede un client/device extra reale; latenza definitiva da
misurare su percorso fisico.

## Accettazione engine FINAL5 + integrazione GUI (2026-10-27, VM)

Router congelato `vm/baseline/NLMatrixRouter_engine_v5_FINAL.exe` SHA256
`2AE82FEBEF4DF4BB7DE9F7D2C149F2A28202CC7F4C73A7EC2EC58D43EEED0B86`.
FINAL5 = FINAL4 + API richieste dalla GUI: `--control` graceful stop (fade
20 ms sui driver destination, exit 0), `devices[]` in METERS con
requested-vs-actual rate/buffer/I/O, exit 24 per sample rate non disponibile,
`destinations[].driverTargetMs`, buffer per device ASIO extra, sameTopology che
tratta i cambi di formato come restart-required.

| verifica | esito | evidenza |
|---|---|---|
| Accettazione continuità (safe x2, balanced, late-consumer) | engine/writer PASS + WAV PASS, clock WARN solo startup transients | `reports/engine/final_acceptance_v5/` |
| multi-ASIO | FAIL documentato: ReaRoute senza client -> d1 starved 1251, nessun hang | `reports/engine/final_acceptance_v5/final_multi_asio_15s/` |
| --control graceful stop | PASS exit 0, log "Control: graceful stop requested.", "Stopped." | smoke test VM 2026-10-27 |
| devices[] requested-vs-actual | PASS: FlexASIO richiesto 48k/256 -> attivo 48k/960; richiesta 44100 -> attivo 44100 | telemetria VM |
| driverTargetMs per endpoint | PASS: override 80 ms riportato in METERS | smoke test VM |
| regressione 10 s balanced (FINAL5) | PASS: 0 gaps, 0 real splices, steady drop/underrun delta 0, starved 0 | `vm/dev5_reg_a0.wav` + analisi |
| latenza marker FINAL5 | marker mediana 43.2 ms (29.7-48.3); primo burst soppresso dal fade di connessione (pairing offset 1 riportato) | `reports/engine/final5_markers/latency.json` |
| GUI cambio formato reale | PASS 54/54 selftest: 48000/256 -> 44100/256 -> 48000/256 via apply di produzione, graceful stop, actual 44100/48000, buffer arrotondato 1024 mostrato | `reports/engine/gui_acceptance/ui_selftest_g4e.log`, `ui_app_log_g4e.log` |

## Test sintetici

| Funzionalità | Stato | Ambiente | Backend/dispositivi | Build | Risultato misurabile | Log / evidenza | Problemi aperti |
|---|---|---|---|---|---|---|---|
| Matrice 4×8 → 4×8 deterministica | PASS | Simulazione/VM | test sources, ring, converter | Router 2026-10-25 | `PASS`, `max_error=0`, `underflows=0` | `reports/MVP/MATRIX_NxM_MAP_TEST.log` | Nessuno noto |
| Matrice multicanale N×M | PASS | Simulazione/VM | test sources + null/ASIO | Router 2026-10-25 | 8 sorgenti e 8 destinazioni, canali attivi 0.2, non connessi 0.0 | `RUNTIME_ISOLATION_METRICS.log`, `reports/MVP/RUNTIME_ISOLATION.log` | Test sintetico, non segnale acustico reale |
| SRC 96→48, 44.1→48, 48→96, 48→48 | PASS | Simulazione | test tones 1/2 kHz | Router 2026-10-25 | PASS su tutti i rate, frequenze 1001/2001 Hz | `reports/MVP/RESAMPLER_TEST.log` | Mancano test di drift a lungo termine con clock indipendenti |
| Bus interno | PASS | Simulazione + VM | test source, bus | Router 2026-10-25 | bus somma 600+900 Hz | `reports/MVP/audio/REAROUTE_BUS_MIXMINUS.log` | Verifica con hardware Apollo assente |
| Mix-minus | PASS | Simulazione + VM | test source, NL Mic | Router 2026-10-25 | 600 Hz attenuato a ~−64.7 dB, 900 Hz presente | `REAROUTE_BUS_MIXMINUS.log` | Isolamento non perfetto; misura diagnostica |
| Routing live gain/mute | PASS | VM | ReaRoute ASIO, test tone, NL Mic | Router 2026-10-25 | 0→−20 dB: RMS −29.54→−49.62 dBFS, delta −20.08 dB, nessun restart | `reports/MVP/audio/LIVE_GAIN_TEST.log` | Test breve 2+2 s; manca soak con UI |

## Test audio reali nella VM

| Funzionalità | Stato | Ambiente | Backend/dispositivi | Build | Risultato misurabile | Log / evidenza | Problemi aperti |
|---|---|---|---|---|---|---|---|
| Schema progetto v3, gain e mute | IMPLEMENTATO / PASS (VM) | VM | JSON v3, FlexASIO, NL Mic | Router/UI 16:57 | deviceId, devices, gainDb source/bus/dest, muted; import JSON legacy; live gain e mute applicati | `MULTI_ASIO_SINGLE_ENGINE.log`, `LIVE_GAIN_MUTE.log` | UI multi-device per deviceId non ancora completa |
| Selezione device ASIO per nodo nella GUI | IMPLEMENTATO / LIMITATO | VM | GUI 17:05, profilo v3 | UI 17:05 | combo device per source/dest, deviceId persistito, devices scritti; router apre adapter solo se referenziato | `MULTI_ASIO_SINGLE_ENGINE.log`, `FINAL_GUI_SELFTEST_ALL.log` | Test GUI di routing su device extra non automatizzato; browser dispositivi dedicato ancora mancante |
| Gain live source/bus/dest con rampe | PASS | VM | FlexASIO -> NL Mic 1, tono 1 kHz | Router 16:57 | destination meter 0 dB: −20.0 dBFS; live −6 dB: −26.0 dBFS (Δ −6.0); source invariato −20.0; nessun restart | `LIVE_GAIN_MUTE.log` | Rampe non misurate con click detector; output extra a rate diverso non testato |
| Mute live | PASS | VM | FlexASIO -> NL Mic 1 | Router 16:57 | destination mute: meter −100 dBFS; unmute ripristina −20 dBFS | `LIVE_GAIN_MUTE.log` | Mute via contesto GUI non automatizzato |
| Multi-ASIO nello stesso motore (same rate) | PASS (output) / LIMITATO (input) | VM | FlexASIO master + ReaRoute ASIO extra | Router 16:57 | entrambi aperti; s0 −26.02 -> d0 −26.02; s0 -> d1 ReaRoute −32.02 (route −6); 296 callback; input extra silenzioso senza sorgente esterna | `MULTI_ASIO_SINGLE_ENGINE.log`, `MULTI_ASIO_SINGLE_ENGINE_RAW.log` | Manca prova con segnale esterno su ReaRoute; UI per assegnare deviceId; SRC output extra |
| Mixer dock + fader/mute/solo/meter | PASS (motore) / LIMITATO (interazione GUI) | VM | GUI 17:21, FlexASIO | Router 17:10 / UI 17:21 | strip per source/bus/destination, gain dB + valore numerico, mute, solo, meter; solo agisce solo su destinazioni monitor; normal return resta A+B (-34.4 dBFS) con A solo (-40 su monitor) | `MIXER_ABOUT_IMPLEMENTATION.log`, `SOLO_MONITOR_TEST.log`, `GUI_MIXER_DOCK3.png` | Fader/mute/solo dalla GUI non automatizzati con click reali; salvataggio profilo verificato dal codice, non da test interattivo |
| Browser dispositivi | LIMITATO | VM | GUI 17:33 | UI 17:33 | finestra con ricerca, categorie ASIO/WASAPI/NL, scelta sorgente/destinazione e creazione nodo; apertura da Devices | `GUI_DEVICE_BROWSER.png` | Aggiunta nodo dalla GUI non automatizzata con click reali; non ancora dockato nel layout |
| Extra ASIO output con SRC | PASS (connessione) / LIMITATO (cattura esterna) | VM | ReaRoute master 48k + FlexASIO extra 44.1k | Router 17:37 | FlexASIO aperto a 44.1 kHz, graph d1 −26.02 dBFS, 299 callback, nessun crash; converter 48→44.1 esercitato | `EXTRA_ASIO_SRC_TEST.log`, `EXTRA_ASIO_SRC_RAW.log` | Non è stata registrata l'uscita fisica FlexASIO; SRC input già testato |
| GUI ripristinata dopo layout | PASS (stato VM) | VM | UI 01:08 Alpha, profilo canvas_test | GUI 01:15 | il mixer era nascosto dal layout `mixerVisible:false` salvato dai test; ripristinato layout e caricato profilo, mixer dockato visibile | `GUI_VM_FINAL2.png`, `GUI_VM_MIXER_RESTORED_FINAL.png`, `GUI_FINAL_WITH_MIXER.png` | Stato della VM, non test automatico |
| Alpha distribution audit | PASS | Host/VM | UI/router/tools 0.4.0-alpha, installer 0.4.0-alpha | 2026-10-26 12:20 | UI ProductVersion 0.4.0-alpha, installer 0.4.0-alpha; portable 5.6 MB senza driver; ZIP/hash finali; contenuti privati esclusi | `da_eliminare/2026-10-27-consolidation/dist-0.4.0-alpha/NL-Audio-Matrix-0.4.0-alpha/SHA256.txt` (archiviato), `portable/portable-manifest.json` | Console tools hanno risorsa base 0.4.0; Alpha storica archiviata, non più pacchetto corrente |
| Diagnostica UAD read-only | PASS (host, sola lettura) | Host Windows fisico | Universal Audio USB/Thunderbolt ASIO presenti | 2026-10-26 01:13 | script `Diagnose-NL-Audio.ps1` eseguito su host: enumera 14 device ASIO, UAD USB/Thunderbolt, REAPER/processi; nessuna modifica | `reports/MVP/DIAG_HOST_TEST/` | Non è un test di routing Apollo; nessuna installazione driver |
| Focusrite voce reale → NL Mic | FAIL continuità | VM | Focusrite USB ASIO, 2 in/2 out | 2026-10-27 12:10 | NL Matrix 48 kHz mono PCM16: peak −9,01 dBFS, 21 silenzi digitali ≥5 ms, 327 ms totali, max 40 ms a 3,242 s. REAPER diretto PCM24: peak −7,32 dBFS, nessun buco comparabile | `FOCUSRITE_REAL_VOICE_TEST.log`, `FOCUSRITE_MIC_TEST_NLMATRIX.wav`, `FOCUSRITE_MIC_TEST_REAPER_DIRECT.wav` | Test microfonico realmente eseguito; continuità NON superata; VM USB può contribuire |
| Separazione canali ASIO Scarlett Output 1/2 + bus split (B7/B8) | TESTING — deterministic VM PASS; hardware loopback pending | VM (deterministic) / PC fisico (hardware) | FINAL5; FlexASIO master + NL Mic virtual destinations for the deterministic matrix; Focusrite ASIO configs ready | 2026-10-27 23:15 | Deterministic: 600 Hz → NL Mic 1, 900 Hz → NL Mic 2; mute Output 1 = silence; Output 2 −12 dB applied; swapped mapping verified; bus A → NL Mic 1 = 600 Hz, bus B → NL Mic 2 + NL Mic 3 = 900 Hz; all PASS. Hardware: two independent output channels, analog loopback and cross-talk not yet measured | `app/profiles/tests/channel_split/`, `vm/channel_split_evidence/`, `tools/Run-ChannelSplitTest.ps1` | Focusrite USB passthrough unavailable in VM; do not retry indefinitely. A software tap/METERS is NOT an analog loopback; Focusrite is not proof for Apollo. Hardware pending on physical PC |
| Router heartbeat/underrun | TESTING | VM | Focusrite buffer 512, writer target 30 ms | 2026-10-27 12:35 | METERS includes framesProcessed, lastCallbackAgeMs, engineHealthy, asioError; destination underruns 1 with 30 ms target (was 4 at 15 ms) | `focus_buf512.log` | GUI stall detection and recovery not yet exercised with a real stalled callback |
| Quattro coppie NL Mic simultanee | PASS (forward, con riserva) | VM | 4 sorgenti test, 4 destinazioni driver | 2026-10-26 18:40 | pacing writer corretto; RMS 0.1715/0.1755/0.1751/0.1766; peak 0.2499; dominanti 440.24/550.00/659.96/770.13; restano 5047/1014/1438/22 coppie duplicate | `FOUR_PAIR_VALIDATION_CORRECTED.log`, `FOUR_PAIR_PACED_NL_MIC_1..4.wav` | Le vecchie frequenze 449.7/548/666.6/773.9 non sono accettate; isolamento e continuità ancora da chiudere |
| Quattro coppie NL Return inverso | FAIL (primo stadio) | VM | loopback NL Return 2/3, driver NL Mic 1/2 | 2026-10-26 18:40 | player invia 550/660 Hz a NL Return 2/3; router sorgenti restano −100 dBFS; WAV NL Mic silenziosi | `REVERSE_ROUTER_RAW.log`, `reverse_mic1.log`, `reverse_mic2.log` | Primo stadio fallito: loopback capture del render endpoint NL Return, non la scrittura driver |
| WASAPI loopback indipendente | CABLE PASS / NL Return FAIL | VM | helper standalone, shared mode | 2026-10-26 18:55 | CABLE Input controllo: frames 240000, RMS 0.188, peak 0.35; NL Return 2: frames 239520, RMS 0, peak 0 durante playback 550 Hz | `LOOPBACK_CONTROL_CABLE.log`, `LOOPBACK_NL_RETURN2.log` | Il render endpoint NL Return non consegna audio al loopback; difetto del driver/render, non del router |
| Build identity UI | PASS (display) | VM 1920x1080 | UI build 0.4.0-alpha, compile stamp 26 Oct 2026 22:42 | 2026-10-26 22:45 | titolo finestra e About mostrano Build ID/timestamp; hash UI host=guest AD24... (build precedente), screenshot About/Main | `UI_BUILD_ID.txt`, `ABOUT_LIGHT_SCREENSHOT.png` | Compatibilità automatica GUI/router/driver non ancora implementata |
| Tema Light | PARZIALE | VM 1920x1080 | preferenza theme.txt=light | 2026-10-26 22:45 | superfici e testi principali passano a chiaro; restano bande scure e pannelli non adattati; menu/System non collaudati | `MAIN_LIGHT_SCREENSHOT.png` | Palette hardcoded residue, mixer non ancora leggibile |
| Layout GUI 1920x1080 | FAIL | VM | ultima build distribuita | 2026-10-26 22:45 | pannello sinistro troncato, mixer vuoto/non operativo, About migliorato ma email/link da rifinire; 1366/1600/DPI non testati | `MAIN_LIGHT_SCREENSHOT.png`, `ABOUT_LIGHT_SCREENSHOT.png` | Serve rifacimento layout left panel/mixer || Driver endpoint audit | FAIL / IN CORSO | VM | Due device MEDIA, 7 OEM stale | 2026-10-26 16:15 | rimossi i due nodi MEDIA NLMatrix e 7 pacchetti ComponentizedAudioSample stale; reinstallato INF corrente; 8 endpoint presenti, ma NL Return 1 mostra Altoparlanti e NL Return 4 mostra SinkDescription Sample | `ENDPOINT_AUDIT_BEFORE.txt`, `ENDPOINT_AUDIT_AFTER.txt`, `ENDPOINT_AUDIT.log`, `DRIVER_PACKAGES_CLEANUP.log` | Causa: SinkDescription hardcoded in hdmitopo (sorgente corretto ma cache endpoint VM) e fallback speaker; nessuna modifica VB/UAD/default |
| Isolamento test GUI | PASS | VM | GUI 01:08 Alpha, selftest 33/33 | 2026-10-26 01:15 | hash del layout utente identico prima/dopo il self-test (36871c…aeb); session persistence disabilitata per i test | `ALPHA_LAYOUT_BEFORE.txt`, `ALPHA_LAYOUT_AFTER.txt` | Verificato layout; profilo/preferenze utente non ancora hashati |
| Layout pannelli/finestra | PASS (clamp/isolation) / LIMITATO | VM | GUI 01:08 Alpha | UI 01:08 | finestra clampata a userArea con margini; mixerHeight limitato; test GUI non modifica più Last session.json | `GUI_FINAL_WITH_MIXER.png`, `ALPHA_LAYOUT_BEFORE.txt`/`ALPHA_LAYOUT_AFTER.txt` | Mixer cut su 1600×900 ancora da verificare con click; nessun docking drag-and-drop |
| Preferenze audio e buffer | LIMITATO | VM | GUI 17:42, FlexASIO | UI 17:42 / Router 17:37 | finestra Semplice/Avanzata, profilo latenza, sample rate/buffer; scrittura su devices/preferences e restart motore; FlexASIO ha ignorato 44.1/512 aprendo 48k/960 | `PREFERENCES_BUFFER_TEST.log`, `PREFERENCES_BUFFER_RAW.log` | Il driver può imporre il formato; verifica Apollo/PC fisico necessaria; manca test interattivo dei controlli |
| Diagnostica / telemetria | IMPLEMENTATO / LIMITATO | VM | GUI 17:39, router 17:37 | GUI/Router 2026-10-25 | telemetria con rate/buffer/callback/drop/underrun; finestra Tools > Diagnostics; refresh 2 Hz | `DIAGNOSTICS_IMPLEMENTATION.log` | Latenza/drift sono stime; manca calibrazione impulso end-to-end |
| About / autore / licenze | PASS (implementato) | VM | UI 17:21 | UI 17:21 | autore Nicola Lanci, email, sito, GitHub verificato, versione/build, crediti e licenze terze; metadata installer corretti | `GUI_ABOUT_FIXED4.png`, `MIXER_ABOUT_IMPLEMENTATION.log` | Link cliccabili non automatizzati; metadata exe/installer verificati (ProductName NL Audio Matrix, Company Nicola Lanci, 0.4.0-alpha) |
| Pan e polarità sorgente | PASS (motore) / LIMITATO (click GUI) | VM | Router 18:19, FlexASIO | 2026-10-25 | pan hard-left: L −9,12 / R −100 dBFS; due toni coerenti con polarità opposta si annullano a −100 dBFS; campi GUI e persistenza profilo aggiunti | `PAN_POLARITY_TEST.log`, `PAN_TEST_RAW.log`, `POLARITY_TEST_RAW.log` | Test automatici motore; manca click GUI e pan su segnali reali |
| Mix-minus per destinazione | PASS (motore) / LIMITATO (click GUI) | VM | Router 17:37 | 2026-10-25 | dNorm vede A+B (-11.46/-10.47 dBFS); dMinus con mixMinus A vede solo B (-26.02 dBFS); combo UI e profilo presenti | `MIX_MINUS_TEST.log`, `MIX_MINUS_RAW.log` | Selezione mix-minus dalla GUI non automatizzata con click |
| Driver target-latency | IMPLEMENTATO / NON VALIDATO | VM | Driver VM target 30 ms | 2026-10-25 23:45 | target capture 30 ms distinto dai 256 KB; il percorso test tone noto funziona (RMS −12,99 dBFS); la misura impulso con loopback non è valida (sorgente non drenata/endpoint) | `PREFERENCES_BUFFER_RAW.log` | Mancano occupazione/età campioni strumentate e una misura end-to-end ripetibile con consumer attivo; non è PASS |
| Test WAV / misura latenza | LIMITATO / NON VALIDATO | VM | Router Alpha, driver target 30 ms | 2026-10-26 | CABLE diretto 116,5 ms; vecchie misure capacity: 256 KB 1885,6 ms, 16 KB 473,4 ms, 4 KB 434,7 ms; il target 30 ms non è ancora misurato end-to-end | `LATENCY_PROBE_TEST.log`, `LATENCY_RING_CAPACITY_TEST.log`, `LATENCY_DIRECT_RAW.log`, `LATENCY_RING_256K_RAW.log`, `LATENCY_RING_16K_RAW.log`, `LATENCY_RING_4K_RAW.log` | Il ring spiega la coda stale; il target 30 ms è distinto dalla capacità e non è latenza totale. Misura impulso valida ancora mancante; nessuna installazione sul PC principale |
| REAPER con WASAPI -> ReaRoute -> NL Matrix -> NL Mic | **PASS** | VM | REAPER 7.83 WASAPI + Routing Matrix, extra ASIO ReaRoute 16 in/out | 2026-10-26 12:02 | REAPER con traccia tone1k.wav e HWOUT 512 (ReaRoute 1/2); aperto prima il client ReaRoute in NL Matrix; NL Mic WAV: RMS 0.1156, peak 0.2999, dominante 1000.0 Hz; meter sorgente router max L −10.46 dB | `REAROUTE_E2E_PASS.log`, `REAROUTE_TONE_CAPTURE.wav`, `REAROUTE_ROUTER_METERS.log` | Test diretto riuscito; bus/mix-minus e percorso inverso non ancora inclusi |
| Unity gain 0 dB end-to-end | PASS (peak) / LIMITATO (RMS) | VM | loopback CABLE Input → NL Mic 1, 48 kHz PCM16 | Router 2026-10-25 | source/dest peak −9.1186 dBFS; registrato −9.120 dBFS; peak preservato; RMS −12.92 vs CABLE diretto −12.20 dBFS (Δ −0.72), picco invariato | `reports/MVP/audio/UNITY_GAIN_LOOPBACK.log`, `LEVEL_COMPARISON.log`, `unity_cable.wav` | Differenza RMS ~0.72 dB attribuita a loopback/downmix; nessun gain compensativo applicato |
| Soak 60 s con bus | PASS | VM | ReaRoute ASIO, bus, NL Mic | Router 2026-10-25 | start RMS −29.5286, end −29.5198 dBFS (Δ +0.009 dB), nessun crash/overflow | `reports/MVP/audio/SOAK_60S_REAROUTE.log`, `soak_rearoute_stdout.log` | Soak >60 s e latenza non misurata |
| Loopback WASAPI reale end-to-end | PASS | VM | endpoint “Altoparlanti” → NL Mic 1 | Router 2026-10-25 | tono 1 kHz registrato, peak 1000 Hz, RMS −8.55 dBFS | `reports/MVP/audio/LOOPBACK_REAL_AUDIO_METRICS.log`, `nlmic_record.log` | Solo VM; non Apollo |
| Due driver ASIO simultanei in processi separati | PASS | VM | FlexASIO + ReaRoute ASIO, NL Mic 1/2 | Router 2026-10-25 | entrambi Running, meter −40 dBFS e callbacks attivi; 960/512 buffer | `reports/MVP/MULTI_ASIO_TWO_PROCESSES.log` | Non è un unico motore multi-device: è una prova di fattibilità OS/driver |
| ASIO ReaRoute runtime 16 canali | PASS | VM | ReaRoute ASIO (x64), 48 kHz, 1024 | Router 2026-10-25 | due toni separati 1 kHz / 1.5 kHz, registrazioni separate; isolamento 26–50 dB | `REAROUTE_SEPARATION_ANALYSIS.log`, `rearoute_rec1/2.log`, WAV in `reports/MVP/audio/` | Test sources, non REAPER come generatore; un solo client ASIO |
| Bus + mix-minus con ReaRoute | PASS | VM | ReaRoute ASIO + NL Mic | Router 2026-10-25 | bus 600+900 Hz; mix-minus 600 Hz a ~−64.7 dB | `REAROUTE_BUS_MIXMINUS.log` | Non verificato con Apollo |
| Hot-plug 4 coppie driver NLMatrix | PASS | VM | NL Matrix driver, FlexASIO | Router 2026-10-25 | 4 source e 4 destination ricontattate | `reports/MVP/ROUTER_4PAIRS_ASIO_HOT.log` | Solo VM |
| Driver virtuale NLMatrix multi-endpoint | PASS | VM | `NLMatrixAudio.sys` POC | Driver package 2026-10-25 | log multi-endpoint/multitone; coppie attive | `reports/MVP/DRIVER_MULTI_ENDPOINT.log`, `DRIVER_MULTITONE_10S.log`, `PAIR*` | POC non firmato, non production; nessun test Apollo |
| Process Loopback (Application Loopback) | LIMITATO | VM | processo audio + NL Mic | Router 2026-10-25 (PCM16/event e tentativo float32) | API attivata, nodo online, ma nessun pacchetto (`GetBuffer`/event); meter −90 dBFS | `reports/MVP/RUNTIME_PROCESS_CAPTURE.log` (storico), test del 2026-10-25 | Nessun audio catturato; endpoint non mutati (mute=0); probabile limite API/driver audio VM. Non dichiarare verificato |
| Process picker / discovery app | PASS | VM | GUI, Win32 process list | UI 2026-10-25 | running application discovery PASS; GUI self-test 33/33 | `reports/MVP/FINAL_GUI_PROCESS_SELFTEST.log` | Toggle “processi di sistema (avanzato)” non ancora misurato con un’app di sistema |
| Enable/disable sorgente + stop capture thread | LIMITATO | VM | router, source JSON | Router 2026-10-25 | routing disabilitato senza rimuovere nodo/rotte; codice arresta il capture | test del 2026-10-25 in VM | Misura del thread stop non strumentata; Process Loopback già senza pacchetti |
| Coesistenza ReaRoute con processo REAPER | LIMITATO | VM | ReaRoute ASIO x64, REAPER 7.83 (WASAPI) | Router 2026-10-25 | router apre ReaRoute 16 canali mentre REAPER è aperto; REAPER era su WASAPI, quindi non prova multi-client ASIO | probe del 2026-10-25 | Serve configurare REAPER su ReaRoute e ripetere con progetto REAPER attivo |
| REAPER + ReaRoute come generatore/registratore | NON TESTATO | VM | REAPER 7.83, ReaRoute 16×16 | REAPER/ReaRoute | ReaRoute enumerato 16 in/16 out; nessun progetto REAPER multicanale eseguito | `reports/MVP/REAPER_REAROUTE_INFO.log` | Un solo client ASIO: coordinare REAPER/router; test su PC fisico |

## Test GUI / profili / diagnostica

| Funzionalità | Stato | Ambiente | Backend/dispositivi | Build | Risultato misurabile | Log / evidenza | Problemi aperti |
|---|---|---|---|---|---|---|---|
| Dialog responsive / pulsanti accessibili | PASS (implementato) / LIMITATO (test) | VM | GUI, schermo VM | UI 15:45 | dialog clampati all area utile; picker canali/processi con OK/Annulla visibili; self-test 33/33 | `FINAL_GUI_MENU_SELFTEST.log` | Da verificare su monitor piccolo/DPI diversi |
| Menu applicativa e proprietà nodo | LIMITATO | VM | GUI 2026-10-25 15:40 | 33/33 self-test; menu File/Routing/Devices/View/Help e doppio clic → pannello proprietà compilati e avviati | `reports/MVP/FINAL_GUI_MENU_SELFTEST.log`, `GUI_MENU_PRO2.png` | Interazione menu non automatizzata; test umano finale consigliato |
| GUI self-test completo | PASS | VM | canvas/profili | UI 2026-10-25 | 33/33 PASS | `reports/MVP/FINAL_GUI_PROCESS_SELFTEST.log` | Test headless/VM, non interazione umana completa |
| Discovery canali ASIO/WASAPI | PASS | VM | FlexASIO, ReaRoute, endpoints | UI 2026-10-25 | ASIO input/output PASS; ReaRoute 16 canali PASS | `FINAL_GUI_DISCOVERY_SELFTEST.log` | Discovery su Apollo reale non eseguita |
| Profili save/reload e layout | PASS | VM | JSON, canvas | UI 2026-10-25 | posizioni e route ripristinate nel self-test | `FINAL_GUI_SELFTEST.log` | Manca test con profili Apollo reali |
| Picker applicazioni con icone/ricerca | PASS | VM | Win32 processi visibili | UI 2026-10-25 | lista app visibili + ricerca; running app discovery PASS | `GUI_PROCESS_PICKER.png`, `FINAL_GUI_PROCESS_SELFTEST.log` | Icone dipendono dalla shell; app senza finestra richiedono modalità manuale |
| Processi di sistema (opzione avanzata) | LIMITATO | VM | Win32 Toolhelp | UI 2026-10-25 | opzione implementata; non misurata su liste grandi | codice `ui_main.cpp` | Validazione UI su processi di sistema ancora da fare |

## Test Apollo / hardware fisico (PC principale)

| Funzionalità | Stato | Ambiente | Dispositivi | Build | Risultato | Log / evidenza | Note |
|---|---|---|---|---|---|---|---|
| Enumerazione driver `Universal Audio USB` | NON TESTATO (solo enumerazione passiva) | VM | driver UAD registrato, Apollo non attivo | Probe 2026-10-25 | 0 input, 0 output, rate 0, buffer 0 | `reports/MVP/REPORT.md`; `docs/05_APOLLO_ASIO.md` | Non è una conclusione sul driver UAD |
| Apertura/cattura 10 ingressi Apollo | NON TESTATO | PC principale | Apollo Solo USB + UAD Console | — | — | — | Richiede Apollo collegato e REAPER coordinato |
| Output ASIO Apollo / Console invariance | NON TESTATO | PC principale | Apollo/UAD | — | — | — | Non aprire output senza conferma; non modificare Console |
| Multi-client ASIO REAPER + OBS-ASIO | NON TESTATO | PC principale | Apollo, REAPER, OBS-ASIO | — | — | — | Da verificare sul PC principale |
| Multi-ASIO in un unico motore | PASS (rate diversi) / LIMITATO (audio) | VM | ReaRoute 48 kHz + FlexASIO richiesto 44,1 kHz | Router Alpha 01:07 | log: `ASIO rate 48000`, `Extra ASIO: FlexASIO at 44100 Hz`; due clock realmente differenti aperti; ingresso Flex misurato solo a rumore | `MULTI_ASIO_SINGLE_ENGINE.log`, `MULTI_ASIO_DIFFERENT_RATE.log` | Rate diversi verificati, ma il percorso audio extra non è ancora alimentato/verificato |
| Stress/latency su Windows fisico | NON TESTATO | PC principale | Apollo/ASIO | — | — | — | Non dichiarare benchmark |

## Problemi aperti che richiedono intervento/verifica esterna

1. **Apollo fisico:** collegare Apollo e ripetere `--inspect`, poi cattura con REAPER
   chiuso/coordinato. Non è stato verificato nulla sull’hardware reale.
2. **Process Loopback:** su questa VM non arrivano pacchetti; verificare su Windows
   fisico con un’app reale prima di considerarla una funzione prodotto.
3. **REAPER/ReaRoute multicanale:** serve un progetto REAPER e una strategia per il
   singolo client ASIO; coordinare apertura router/REAPER.
4. **Multi-ASIO:** richiede evoluzione architetturale; non è un test da VM.
5. **Sicurezza driver:** nessuna installazione kernel/test signing sul PC principale;
   ogni prova driver resta in VM isolata.

## Aggiornamento 2026-10-27 — Engine v2 (buffering, clock, telemetria, recovery)

Architettura e schema config: `docs/ENGINE_V2.md`. Analisi completa:
`reports/engine/ENGINE_AB_REPORT.md`. Stato finale/dettaglio blocker:
`reports/engine/FINAL_STATUS.md` e `reports/engine/final_acceptance_v5/FINAL_ACCEPTANCE.md` (evidenze FINAL5; FINAL4 archiviato in `da_eliminare/2026-10-27-consolidation/reports-superseded/final_acceptance-FINAL4/`).

**Metodo A/B.** Stesso driver ABI3 (immagine SHA256 CD95712E...), stessa VM,
stessi config e harness, run back-to-back: "legacy engine" (sorgenti router
pre-refactor ricompilate solo per ABI3, SHA256 E0C4553C...) contro "engine v2"
(SHA256 9B4E1D65... finestra a clock pieno; FINAL2 successivo 38B4F6C6...).
Il deficit di callback FlexASIO (frame non consegnati dal driver ASIO) è misurato
e riportato separatamente come metrica d'ambiente.

| Prova | Legacy engine | Engine v2 | Esito |
|---|---|---|---|
| Underrun destinazione (20 s; 60 s soak) | 13/21/13/37; 63 | **0** in ogni run | PASS engine |
| Frame persi nel ring destinazione | 0 | **0** | PASS engine |
| Gap silenziosi WAV (10 s, post-onset) | 70–196 ms per run | Balanced/Low: residui = deficit ASIO a monte (0.16–1.73 %); **Safe: 0 gap** (analyzer PASS, 0 splice reali, tono 999.85 Hz) | PASS engine; limite ambiente documentato |
| Latenza stimata pipeline | n/d | 85 ms balanced, 175 ms safe | stimata |
| Stato motore | asioError=false, healthy | engineState=running, healthy, asioRestarts=0, watchdog startup senza falsi positivi | PASS |

**Driver ABI3 (VM).** Installato e verificato (boot 14:51:04): `NLMATRIX_ABI_VERSION=3`,
8 endpoint OK, SET_CAPTURE_TARGET readback 30→50 ms con clamp 10/500, contatori
pulled/underrun/dropped/render-overrun funzionanti; stall iniettato di 200 ms
visibile sia nei contatori kernel sia nel WAV. Rollback:
`driver/package-known-good-20261027-1350/`.

**Blocker ambientali registrati (non fallimenti del prodotto):**

1. Dopo un power-cycle completo dell'host, il clock del motore audio WASAPI guest
   (FlexASIO, render condiviso) è sceso a metà velocità: buffer da 960 frame ogni
   40 ms anziché 20 ms, per qualunque binario router, anche con destinazione
   "null" e senza writer; cattura e QPC guest restano real-time. Esclusi: reset
   VM, power-cycle guest/VM, pin del backend audio host su Realtek, piano High
   Performance, restart servizi audio, pin dispositivo FlexASIO. La ri-esecuzione
   di accettazione del binario finale è quindi **BLOCKED**, non FAIL.
2. Passthrough USB Focusrite incastrato (enumera, ASIO non parte, cattura WASAPI
   0 frame) dopo streaming intenso; serve re-plug fisico dell'operatore. Le righe
   Focusrite post-fix (legacy vs v2) sono pronte nell'harness.

**Copertura non eseguita in questo ciclo:** misura impulso della latenza reale,
multi-ASIO runtime, interazione GUI buffer modes/selftest, recovery con
disconnessione reale, registrazione vocale post-fix su Focusrite. Vedi `TASKS.md`
(A4/A5/A9/A10/A11/B4/C11).

## Protocollo di aggiornamento

- Aggiornare solo questo file dopo un test significativo; non creare file di stato
  paralleli.
- Inserire sempre: stato, ambiente, backend, build, misura, percorso log, problemi.
- Un test senza misura/log = **NON TESTATO**, non PASS.
- Un test su VM non sostituisce un test Apollo/Windows fisico.
- Non inserire credenziali, certificati, percorsi di VM o dati personali.

## Evidenze archiviate (2026-10-27)

La documentazione storica è in `docs/history/`. Le evidenze superate (FINAL3 e
batch intermedi, run baseline raw, probe coda, helper GUI, backup, dist alpha,
cache oggetti build) sono state spostate in
`da_eliminare/2026-10-27-consolidation/` senza cancellarle; la VM e le baseline
FINAL2/FINAL3/FINAL4/FINAL5 in `vm/baseline/` non sono state toccate.