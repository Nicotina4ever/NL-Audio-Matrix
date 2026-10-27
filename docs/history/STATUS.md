# STATUS — NL Audio Matrix v2

> **Nota (2026-10-27):** questo documento descrive la fase P0/MVP (24-25 ottobre).
> Lo stato verificato corrente, i risultati engine v2 e i blocker sono in
> `DEVELOPMENT_STATE.md`, `reports/TEST_STATUS.md`, `docs/ENGINE_V2.md` e
> `reports/engine/ENGINE_AB_REPORT.md`, `reports/engine/FINAL_STATUS.md`.

**Data:** 24 ottobre 2026  
**Fase:** P0 completata; vertical slice MVP operativa (probe, live, bridge bidirezionale).  
**Scope:** matrice audio universale multi-device; Apollo Solo USB è il primo dispositivo di test.

## Fatto
- Audit read-only completato sui quattro repository + JUCE: `SOURCE_CODE_AUDIT.md` con commit, licenze, file/righe, componenti riutilizzabili e limiti.
- Documenti canonici prodotti/aggiornati: `MASTER_PLAN.md`, `ARCHITECTURE.md`, `TECHNICAL_DECISIONS.md` (incl. D23 vertical slice), `IMPLEMENTATION_ROADMAP.md`, `TEST_PLAN.md`, `TASKS.md`, `AGENTS.md`, `README.md`, `STATUS.md`.
- Verificato: OBS-ASIO locale = v3.2.1f (tag + solo README); JUCE 9.0.4 stock; SysVAD multi-endpoint senza trasporto user-mode; Micy/Sweetly non basi di prodotto.
- Toolchain: CMake 3.31, VS 2022 Community/BuildTools, Windows SDK e WDK x64 10.0.26100.6584 via NuGet locale in `tools/wdk-packages`; nessuna installazione globale aggiuntiva.
- Driver `NLMatrixAudio.sys` compilato con la toolchain locale; installato **solo nella VM Windows 11 di test** (test signing), mai sul Windows principale. Repository di riferimento non modificati.

## Decisione di percorso
Si parte da una **vertical slice** con JUCE, non dall'architettura completa:
1. `NLMatrixProbe`: enumerazione ASIO, cattura separata di tutti i canali, verifica canale↔buffer.
2. `NLMatrixLive`: matrice semplice nel callback ASIO + output WASAPI verso endpoint di diagnostica.
3. `NLMatrixBridge`: multi-bridge bidirezionale Windows ↔ canali ASIO, con endpoint e ring indipendenti.
4. Poi driver SysVAD per sostituire gli endpoint diagnostici e architettura completa.

VB-CABLE, se usato nella slice, è **solo diagnostica temporanea**, non dipendenza finale. VB-Matrix e ReaRoute non sono usati come dipendenze: il codice accetta qualsiasi endpoint/canale disponibile e non hardcoda nomi di prodotti terzi.

## Risultati MVP già ottenuti
- Build `NLMatrixProbe.exe` riuscita con CMake 3.31 + VS 2022 BuildTools + JUCE 9.0.4 (Ninja, x64 Release).
- `--list`: 14 driver ASIO enumerati, inclusi `Universal Audio USB`, `ReaRoute ASIO (x64)`, `VB-Matrix`, `ASIO4ALL`, `FlexASIO`, `Source Elements Audio Driver`.
- `--inspect` su `Universal Audio USB`: 0 canali, rate 0. Il dato è registrato **senza assumere la causa** (hardware non attivo, driver non inizializzato, device busy, ecc.); la verifica P1 va ripetuta sul PC con Apollo.
- `--inspect` su `ReaRoute ASIO (x64)`: 4 input + 4 output, nomi corretti.
- `--capture` su ReaRoute: 4 WAV separati, 48.000 campioni ciascuno, report con mappa canale↔buffer; output ASIO non aperti (input-only accettato).
- Il percorso di cattura, la mappa compatta→assoluta e la scrittura WAV/report sono verificati su un driver ASIO virtuale; resta la verifica reale Apollo.
- `NLMatrixLive` compilato: matrice stereo CLI nel callback ASIO + uscita WASAPI shared, ring SPSC, meter e panic iniziale.
- Verifica end-to-end senza Apollo: tono 440 Hz → `CABLE Input` → `CABLE Output` → cattura; peak −20 dB, RMS −23.1 dB. WAV in `reports/MVP/cable_loopback.wav`.
- ASIO + WASAPI simultanei provati con `ReaRoute ASIO (x64)` + `CABLE Input`: nessun crash; input silenzioso atteso. Il ring si riempie senza SRC/drift control: è il prossimo limite da affrontare.
- `NLMatrixBridge` compilato: bridge Windows→ASIO e ASIO→Windows, più bridge indipendenti nello stesso processo; config con nome, endpoint e canali 0-based.
- Test bridge Windows→ASIO: tono da `CABLE Output` → canali ASIO ReaRoute, meter `asioOut −20 dB`; test bridge ASIO→Windows attivo in parallelo. Nessun Apollo richiesto.
- Il bridge usa endpoint Windows **esistenti**. La creazione di nuovi endpoint proprietari con nome è il driver P3; il bridge è già pronto a riceverli.
- VB-Matrix è stato provato come coppia virtuale ma non ha mostrato passthrough senza la sua app: non è usato e non è una dipendenza. ReaRoute è usato solo come sorgente/destinazione ASIO di test.
- **Loopback WASAPI nativo** (`app/src/wasapi_loopback.*`): cattura direttamente qualsiasi endpoint di render Windows, senza `CABLE Output` né driver terzi. Test: tono su `CABLE Input` → loopback → ASIO `-20 dB`, senza aprire la coppia capture.
- **`NLMatrixRouter`**: router N×M configurabile via JSON, con sorgenti ASIO/loopback/capture, destinazioni ASIO/render, route esplicite e guadagni. Test: una sorgente loopback → due destinazioni ASIO con guadagni 0 e −6 dB (`-20`/`-26 dB` misurati).
- **Core matrice N×M (fase 1)**: `app/src/matrix_engine.h` con ring SPSC multicanale, convertitore N-canali e tap matrix; `NLMatrixRouter --matrix-test` esegue 4 sorgenti × 8 canali a 48/96/44.1 kHz verso 4 destinazioni × 8 canali con rotazioni di canale, 20 s, 32/32 controlli di isolamento PASS, crosstalk < −30 dB, underflow diagnostici riportati. Il percorso hardware ASIO/WASAPI del router resta stereo: il prossimo passo è collegare il core N×M ai device fisici. Il grafo salta lettura/mix quando non ci sono route attive (`IDLE_SUSPEND_TEST.log`).
- **Runtime N×M (2026-10-25)**: il callback reale usa `MatrixRing`/`MatrixRateConverter` N-canali, `channelMap` per route, bus con `channels` configurabile e `mixMinus` per destinazione. Verificato sul runtime in VM: 8 canali indipendenti (una sola sorgente attiva per volta, canali non collegati a zero), bus 8ch con somma e mappa invertita, mix-minus con esclusione (d0=0.2, d1=0.1, d2=bus 0.15), routing live con gain cambiato in corsa (0.2→0.02). Il percorso fisico disponibile in VM rimane 2ch (FlexASIO) o 2/1ch (driver NL Matrix): nessuna verifica hardware >2ch/Apollo.
- **Crosstalk**: i valori −33/−42 dB del test sintetico erano leakage della DFT a finestra finita; nel runtime reale, con una sola sorgente e canali non collegati, i canali misurano esattamente 0.0. Il percorso ring+converter prefillato dà 0 underflow; gli underflow del vecchio generatore threaded sono di scheduling del banco sintetico, non del converter.
- **Idle**: salto DSP senza route attive; `--idle-exit-seconds` chiude ASIO/WASAPI e termina il router (testato: 2 s, exit 0); GUI sospende/riprende il router quando l'ultima route viene rimossa/aggiunta.
- **GUI discovery / patchbay UX (2026-10-25)**: etichetta esplicita "Motore ASIO (unico driver)"; pulsanti "Scegli canali..." per sorgenti/destinazioni con elenco reale dei canali ASIO/WASAPI, nomi riportati dal driver, scelta mono/stereo/tutti e fallback manuale quando il device è occupato/offline; generazione automatica nomi nodo; bus "Mix Bus N" con numero canali; profili preservano `channelMap`, `mixMinus`, canali bus e campi test. Self-test GUI 30/30 PASS.
- **Laboratorio VM**: REAPER 7.83 installato via installer ufficiale con ReaRoute ASIO x64 (16 in/16 out, registrato); VB-CABLE già presente (CABLE Input/Output). Il percorso GUI è stato verificato con FlexASIO; ReaRoute è enumerato dal router e disponibile per i test multicanale.
- **Process Capture UX (2026-10-25)**: selettore applicazioni visuale e ricercabile con icone, titolo, exe e PID; refresh automatico solo mentre il dialog è visibile; toggle "Includi processi figli"; sorgente Enable/Disable senza perdere nodo o collegamenti; `processTitle` persistito per distinguere più istanze e riconnettersi dopo chiusura/riapertura senza dipendere dal PID; fallback offline/manuale per cattura non disponibile. Verifica runtime: sorgente processo `msedge.exe` risolta e catturata (PID riportato, nodo online) in `RUNTIME_PROCESS_CAPTURE.log`.
- **Audio reale / ReaRoute (2026-10-25)**: verificato loopback WASAPI reale end-to-end: tono 1 kHz da app → loopback "Altoparlanti" → driver NL Mic 1, registrato a 48 kHz (peak 1000 Hz, RMS −8.55 dBFS). Verificato runtime con ASIO **ReaRoute ASIO (x64)** 16 canali, 48 kHz/1024: due toni separati instradati a NL Mic 1/2 e registrati separatamente; analisi 1000/1500 Hz con isolamento 26–50 dB. Verificato bus 600+900 Hz e mix-minus che rimuove 600 Hz da NL Mic 2. File WAV/log in `reports/MVP/audio/`.
- **Process Capture (limite VM)**: il selettore applicazioni, il matching per eseguibile+`processTitle`, l'enable/disable e l'arresto del capture thread sono implementati e compilati; su questa VM l'API Windows Process Loopback attiva il flusso ma non restituisce pacchetti audio (`GetNextPacketSize=0`), quindi i meter restano a −90 dB. Non viene presentato come verificato: la cattura reale è verificata via loopback WASAPI, non via Process Loopback. Il bug di modalità EXCLUDE (che avrebbe catturato tutte le altre app) è stato corretto in modalità INCLUDE tree.
- **Live gain / routing realtime (2026-10-25)**: test automatico con ReaRoute ASIO, tono test 1 kHz a livello 0.05, registrazione NL Mic 1 prima/dopo modifica live da 0 a −20 dB: RMS −29.54 → −49.62 dBFS (delta −20.08 dB), router rimasto attivo senza restart. Evidenza: `reports/MVP/audio/LIVE_GAIN_TEST.log`.
- **Process Loopback event-driven**: aggiunto `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` + `SetEventHandle` e lettura a eventi; sulla VM il flusso resta senza pacchetti. Il limite è documentato nel pacchetto di condivisione; non viene presentato come test audio riuscito.
- **Riferimento atkAudio Plugin for OBS (AGPL)**: analisi read-only di sincronizzazione multi-device, drift/priming, SRC, graph runtime e VST3; nessun codice AGPL/GPL incorporato, nessuna dipendenza OBS/atkAudio. Report: `reports/MVP/ATKAUDIO_REFERENCE_ANALYSIS.md`.
- **Router device-rate aware (implementazione indipendente)**: `StereoRateConverter` streaming per sorgenti/destinazioni con clock diverso, trim ±300 ppm, target 15 ms, fade-in 64 campioni, rilevamento live dei cambi formato endpoint ogni 500 ms; loopback non più rifiutato per rate diverso. Test `--resampler-test` 4/4 PASS (96→48, 44.1→48, 48→96, 48→48). Verifica reale: quattro coppie NL Return/NL Mic pilotate a 96/48/16 kHz, quattro toni simultanei 500/1000/1500/2000 Hz con peak corretti e canali isolati (`SIMULTANEOUS_4_TONES.log`, `PAIR*.log`).
- **Verifica quattro coppie**: transport multitone 10 s su tutti e quattro gli endpoint, 4/4 PASS (`DRIVER_MULTITONE_10S.log`); routing 4 coppie con FlexASIO, hot-route live e formati 96/48 kHz (`ROUTER_4PAIRS_ASIO_HOT.log`).
- **Bus interni riutilizzabili**: un bus può essere alimentato da sorgenti e usato come sorgente da altre route/destinazioni, senza ri-catturare un endpoint. Test: sorgente → bus → due destinazioni ASIO, guadagni indipendenti.
- **Microsoft Application Loopback** (`app/src/process_loopback.*`): sorgente `process` che cattura un processo e i suoi figli indipendentemente dall'endpoint di riproduzione. Test: processo `NLMatrixLive.exe` → ASIO, meter −20 dB. Questo permette di separare Source-Connect/SessionLinkPRO/Nexus anche se condividono lo stesso dispositivo.
- Profilo `app/profiles/live_doppiaggio_3_process.json` validato: usa process loopback per i ritorni e mantiene solo i futuri `NL Mic` come destinazioni; 4 bus, 8 sorgenti, 7 destinazioni, 20 route.
- **GUI `NLMatrixUI`** compilata e avviata: profilo, selezione device ASIO, sorgenti ASIO/loopback/capture/process/bus, destinazioni ASIO/render, bus, route con gain/enable, meter dal router, Start/Stop/Apply, save/load profilo. Screenshot: `reports/MVP/ui5.png`.
- La GUI lancia `NLMatrixRouter.exe` come processo audio separato: la UI non deve mai bloccare il thread audio e il router continua a funzionare anche se la finestra viene chiusa in modo anomalo (il processo figlio viene terminato alla chiusura).
- **GUI visuale**: canvas interattivo con nodi trascinabili, selezione multipla, zoom/pan, cavi bezier; **hover evidenziato**, click selezione, Canc, pulsante X sul cavo, menu contestuale con Disconnect, lista Connections con selezione/Disconnect; bus sia destinazione (porta sinistra) sia sorgente (porta destra). Tipo sorgente/destinazione leggibili (ASIO input, WASAPI loopback/capture, Process capture, Internal bus, NL virtual endpoint) e pulsanti **?** con spiegazioni. Finestra **Virtual devices...** elenca gli 8 endpoint reali, stato e assegnazioni, con apertura Impostazioni audio Windows.
- **Meter reali** da telemetria router su file (`METERS` JSON, 10 Hz): L/R, peak, clip e offline per sorgenti/bus/destinazioni; test con tono 1 kHz ≈ −9.1 dBFS su source e destination.
- **Diagnostica router**: stdout letto da thread dedicato, log file, errori ASIO/driver/grafo comprensibili; endpoint mancanti restano offline senza chiudere il motore. Il vecchio exit 16 da driver assente è stato eliminato.
- **Test GUI automatico** `--selftest`: 28 PASS su canvas/mouse, profilo/layout, route gain/mute, router start/stop, meter, tray; `--snapshot` salva il render UI. Test virtual devices e multi-endpoint: `reports/MVP/ui_virtual_devices.png`, `DRIVER_MULTI_ENDPOINT.log`, `DRIVER_ENDPOINTS.log`.
- **Test input Windows reale** `--clicktest`: hover, click sul cavo, Canc, router vivo dopo disconnect, drag reale bus→destinazione, Disconnect dalla lista Connections, menu contestuale destro. 7/7 PASS. Log: `reports/MVP/GUI_REAL_MOUSE_TEST.log`, render: `reports/MVP/ui_real_mouse.png`. Tray nativo: `GUI_TRAY_TEST.log`.
- **Hot-routing**: `Apply live` riscrive il profilo e il router lo ricarica ogni 200 ms senza riavvio (matrice/gain/mute). Modifiche topologiche (aggiunta/rimozione device) riavviano solo il motore.
- **Hot-plug**: sorgenti `process` diventano offline e si riconnettono automaticamente quando l'app parte/rientra; loopback e render endpoint hanno watchdog di riconnessione. Testato: router avviato senza processo, avvio successivo dell'app, cattura audio RMS ≈ −12.1 dBFS.
- **Tray**: menu nativo Win32 `TrackPopupMenu` ancorato al cursore (non più popup JUCE ancorato alla GUI), gestione del WM_RBUTTONUP che Shell_NotifyIcon può inviare senza WM_RBUTTONDOWN, click sinistro/doppio per mostrare. Menu verificato con click reale sulle voci native; il click fisico sull'icona non è verificabile in VM perché l'icona è nel pannello overflow.
- **Installer dev** `installer/build/NLMatrixSetup.exe` (embedded payload, driver test-signed, scorciatoia, registro di disinstallazione); `NLMatrixUninstall.exe` rimuove app/driver/endpoint. Testato in VM in modalità `/silent` (install exit 0, uninstall exit 0). Per VM con UAC è ricostruibile la variante asInvoker con `build-installer.ps1 -Dev`.
- **Driver `NLMatrixAudio.sys`** (fork SysVAD MS-PL, WaveRT, KMDF): sorgenti in `driver/sysvad-nl`, trasporto `driver/sysvad-nl/nltransport.*`, ABI `nlmatrix_ioctl.h`, package firmato in `driver/package`.
- Installato e caricato nella VM Windows 11 (`ROOT\MEDIA\0000`, servizio `sysvad_componentizedaudiosample` RUNNING). Endpoint creati:
  - `NL Mic (NLMatrix Virtual Audio Device)` capture, verificato con ffmpeg (RMS ≈ −21.9 dBFS con catena NL Return→NL Mic);
  - render `NLMatrix Virtual Audio Device` (Windows mostra ancora il nome localizzato “Altoparlanti (NLMatrix Virtual Audio Device)”); il nome visualizzato è un limite noto di denominazione degli endpoint render, non del driver.
- Trasporto kernel↔user mode `\\.\NLMatrixAudio` con IOCTL GET_INFO/READ_RENDER/WRITE_CAPTURE/RESET, ring non paged, contatori e peak per diagnostica. Testati:
  - tono in NL Mic → cattura ffmpeg;
  - tono WASAPI su NL Return → lettura render (`render_peak16 > 0`);
  - catena completa NL Return → `NLMatrixDriverTest --loopback` → NL Mic → ffmpeg (RMS ≈ −21.9 dBFS).
- Test client/strumenti: `NLMatrixDriverTest.exe`, `NLMatrixDriverAudioTest.exe`, `NLMatrixEndpointVolume.exe`; script di build/install package in `driver/`.
- Router e GUI conoscono i tipi `driver` come sorgente/destinazione; il collegamento runtime completo richiede un device ASIO nella VM (non presente) o un bridge driver-only, quindi è il prossimo passo.
- Profilo `app/profiles/live_doppiaggio_3.json` validato: 4 bus, 8 sorgenti, 7 destinazioni, 20 route. Mix-minus espresso esplicitamente (ogni attore non è routato al proprio mic bus); registrazione isolata via coppie ASIO distinte.
- Test mix-minus esplicito `app/router_mixminus_test.json`: bus altri `-20 dB`, bus proprio ritorno `-100 dB`; destinazioni ASIO corrispondenti `-20`/`-100 dB`.
- Report dettagliato: `reports/MVP/REPORT.md`.

## Categorie di dispositivi
- **Fisici/driver**: ASIO e WASAPI enumerati dinamicamente; qualsiasi interfaccia compatibile è una sorgente/destinazione.
- **Virtuali esterni opzionali**: VB-CABLE, VB-Matrix, ReaRoute, Console Virtual I/O. Usabili per test, ma **non dipendenze**: il codice non contiene i loro nomi come requisiti e il loopback non richiede una coppia capture.
- **NLMatrix proprietari**: router, bus, loopback nativo, Application Loopback, profili; endpoint Windows proprietari (`NL Mic ...`, `NL Return ...`) in arrivo con il driver. Finché il driver non c'è, i profili usano endpoint esistenti come binding temporaneo.
- Con Application Loopback i **return** delle applicazioni possono essere catturati come processi senza endpoint virtuali dedicati; restano necessari i **NL Mic** virtuali per inviare audio alle applicazioni.

## Stato gate
| Gate | Stato | Nota |
|---|---|---|
| P0 documenti | DONE | Documenti canonici; legacy in `docs/` da riconciliare dopo la slice |
| G1.5 slice | IN CORSO | Probe, live, bridge, router, loopback, process loopback e GUI compilati; routing N×M, bus, mix-minus e GUI verificati; mancano Apollo reale e driver endpoint |
| G1 acquisizione reale | READY | Procedura in `TEST_PLAN.md`; richiede Apollo collegato + REAPER |
| G3 driver | POC VM FATTO | `NLMatrixAudio.sys` ABI 2 caricato, 4 coppie NL Mic/NL Return, 8 endpoint Windows online; trasporto per-endpoint e binding GUI/router implementati; resta provisioning dinamico |

## Prossima attività
1. Driver POC: più coppie NL Mic/NL Return con indice endpoint nel trasporto, provisioning e binding GUI/router; testare il collegamento ASIO+driver reale. Limiti GUI: scaling Windows testato a 100%; layout compatto/minimo verificato, non ancora a 125/150/200% con display fisico; menu tray verificato tramite menu nativo e callback shell, non con click fisico sull'icona in overflow; pool endpoint fisso a 4 coppie (aggiunta/rimozione dinamica non ancora implementata); nomi Windows di NL Return 1/4 da rinominare in Impostazioni audio; meter stereo L/R (multicanale >2 da fare).
2. Verifica Apollo reale: collegare l'interfaccia, `--inspect`/`--capture`, mappa canali e prova del profilo 3 attori.
3. SRC/drift control input↔output (osservato ring che si riempie su ReaRoute) e test 44.1/48/96.
4. Consolidare la GUI: valori dei meter con nomi leggibili, edit diretto delle route, preset recenti e gestione errori del router.

## Blocker / attenzioni
- Apertura ASIO con REAPER: da verificare in P1, non assumere multi-client.
- Output Apollo: off di default; nessuna scrittura forzata.
- WDK disponibile localmente in `tools/wdk-packages`; il driver è installato solo nella VM di test. Restano da implementare più endpoint e il binding definitivo alla GUI.
- Licenze JUCE/ASIO da decidere prima della distribuzione.

## Documenti
Canonici: `SOURCE_CODE_AUDIT.md`, `MASTER_PLAN.md`, `ARCHITECTURE.md`, `TECHNICAL_DECISIONS.md`, `IMPLEMENTATION_ROADMAP.md`, `TEST_PLAN.md`, `TASKS.md`, `STATUS.md`. Storici/integrativi: `docs/`.
## Aggiornamento 2026-10-25 (implementazione professionale)

- Schema progetto v3 con `devices`/`deviceId` e import legacy; gain di sorgente, bus e destinazione; rampe anti-click; mute separato da enable; aggiornamenti live senza restart.
- Primo percorso multi-ASIO nello stesso motore: FlexASIO master + ReaRoute ASIO aggiuntivo, ring SPSC per input/output, clock master e callback indipendenti. Verificato l'output same-rate; input extra, SRC output e UI multi-device restano da completare.
- GUI: menu applicativa, proprietà nodo con doppio clic/contesto, clamp dialog all'area utile, campi gain source/dest/bus, mixer con fader/mute/meter, selettore profili e diagnostica meter, selezione device ASIO per sorgenti/destinazioni (deviceId).
- Test VM: self-test GUI 33/33; gain live 0→−6 dB misurato −6.0 dB; mute live −100 dBFS; solo limitato alle destinazioni monitor; unity peak preservato; soak 60 s ReaRoute stabile.
- Mixer dock con strip source/bus/destination, solo/mute/meter; About con autore, contatti, GitHub, crediti/licenze e metadata exe/installer (NL Audio Matrix, Nicola Lanci, 0.3.0).
- Layout: salvataggio/caricamento nominato, ripristino ultimo layout, round-trip PASS.
- Preferenze Semplice/Avanzata per profilo latenza, sample rate e buffer; il driver può imporre il formato reale.
- Diagnostica Tools con rate/buffer/callback/drop/underrun; multi-ASIO output con SRC e test WAV/latenza in corso.
- Mix-minus per destinazione: esclusione di una sorgente dal singolo ritorno, verificata (destinazione normale A+B; destinazione mix-minus solo B); combo nella GUI e persistenza profilo.
- Pan e polarità sorgente: pan hard-left e annullamento di due toni in controfase verificati nel motore; campi GUI pan (-1..1)/Invert polarity e persistenza profilo.
- ReaRoute: sonda REAPER 7.83 con WASAPI mostra solo 2 uscite e nessun hardware output ReaRoute; percorso REAPER→ReaRoute non ancora verificato.
- Latenza VM: il ring del driver virtuale è un contributo dominante; 256 KB=1885,6 ms, 16 KB=473,4 ms, 4 KB=434,7 ms (CABLE diretto 116,5 ms). Driver sorgente/package ripristinato a 256 KB; nessuna modifica sul PC principale. Restano ~400 ms da isolare.
- Limiti aperti: Apollo fisico non disponibile; Process Loopback senza pacchetti nella VM; REAPER/ReaRoute end-to-end non ancora eseguito; multi-ASIO input/UI/SRC da completare; livello RMS va misurato con trimming del transitorio di avvio.
- Evidenze in `reports/TEST_STATUS.md` e `reports/MVP/`; nessuna installazione driver kernel sul PC principale.
