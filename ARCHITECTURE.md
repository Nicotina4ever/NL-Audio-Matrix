# ARCHITECTURE — NL Audio Matrix v2

**Versione:** 3.0 (scope universale multi-device)  
**Data:** 24 ottobre 2026 (aggiornato 27 ottobre 2026)  
**Stato:** architettura di riferimento; implementazione corrente engine **FINAL5** + GUI (vedi `docs/ENGINE_V2.md`, `TASKS.md`, `reports/TEST_STATUS.md`).  
**Documenti collegati:** `docs/ENGINE_V2.md`, `TASKS.md`, `DEVELOPMENT_STATE.md`, `reports/TEST_STATUS.md`; materiale storico in `docs/history/`.

---

## 1. Contesto e principi

NL Audio Matrix è una **matrice audio universale per Windows**: più dispositivi ASIO e WASAPI, canali dinamici, bus dinamici, routing bidirezionale, endpoint virtuali proprietari, mix-minus. Apollo Solo USB è il **primo dispositivo di test**, non il limite del prodotto.

Principi non negoziabili:

1. **REAPER e UAD Console restano sovrani.** La Matrix non sostituisce il driver UAD, non cambia il routing Console, non forza l'uso di plugin REAPER e non sottrae output già utilizzati da altre applicazioni.
2. **Nessun limite artificiale** di schede, canali, bus o endpoint nel motore di routing. I conteggi sono dati di configurazione; gli eventuali limiti sono capacità dichiarate dei backend/driver o guardie di risorsa documentate.
3. **Nessuna assunzione di clock condiviso.** Ogni dispositivo ha il proprio clock; il grafo è un dominio asincrono con ricampionamento adattivo.
4. **Il grafo funziona anche senza ASIO attivo.** Lo scheduler è indipendente dal device; senza dispositivi il silenzio è esplicito, non un crash.
5. **Realtime discipline:** nel percorso audio niente allocazioni, lock bloccanti, I/O, log o syscall; snapshot immutabili, ring SPSC, buffer preallocati.
6. **Output non invasivi:** gli output ASIO sono enumerati e instradabili solo su scelta esplicita; per Apollo la scrittura output è disabilitata di default. Il ritorno delle piattaforme direttamente verso Apollo resta un percorso supportato e preferito.
7. **Licenze pulite:** nessun codice GPL di OBS-ASIO copiato; driver derivato SysVAD sotto MS-PL; JUCE/ASIO da licenziare prima della distribuzione.

---

## 2. Modello di processo

```
┌───────────────────────────────┐        IPC locale versionata        ┌────────────────────────────────────┐
│ NLMatrixUI.exe                │◄───────────────────────────────────►│ NLMatrixEngine.exe                 │
│ JUCE 9 GUI                    │  comandi, preset, stato, meter      │ headless, nessuna GUI              │
│ nessun thread audio           │                                     │ DeviceManager + GraphScheduler +   │
│ nessuna chiamata al driver    │                                     │ GraphEngine + TransportClient      │
└───────────────────────────────┘                                     └───────────────┬────────────────────┘
                                                                                       │ IOCTL + ring condivisi
                                                                       ┌───────────────▼────────────────────┐
                                                                       │ NLMatrixAudio.sys                  │
                                                                       │ PortCls/WaveRT                     │
                                                                       │ endpoint capture/render            │
                                                                       │ provisionabili                     │
                                                                       └───────────────┬────────────────────┘
                                                                                       │ WaveRT
                                                                       ┌───────────────▼────────────────────┐
                                                                       │ Windows audio engine               │
                                                                       │ NL Virtual Mic/Speaker <n>         │
                                                                       └────────────────────────────────────┘
```

- **Engine singleton** per sessione utente (mutex nominato). Possiede DeviceManager, GraphScheduler, GraphEngine, trasporto endpoint, preset e diagnostica.
- **UI client**: può chiudersi/riaprirsi senza interrompere l'audio. Nessun thread audio nella GUI.
- **IPC**: named pipe `\\.\pipe\nlmatrix-engine-<sid>`, messaggi length-prefixed JSON (o binario compatto), versione protocollo, ACL sull'utente corrente. La GUI può avviare l'engine se assente.
- **Nessun servizio in sessione 0**: ASIO richiede un contesto utente e un message window; l'engine è un processo utente headless con `MessageManager` inizializzato (hidden window JUCE).
- **Crash engine**: il driver rileva heartbeat scaduto, mette in silenzio i capture e ignora i render; nessun rumore. Al riavvio l'engine riapre il trasporto e ripristina il preset.
- **Crash UI**: l'audio continua; la UI si riconnette.

---

## 3. DeviceManager e backend

### 3.1 Backend

| Backend | Implementazione | Direzioni | Note |
|---|---|---|---|
| `AsioBackend` | JUCE 9.0.4 `AudioIODeviceType` ASIO, una istanza per device | input/output | Apre il device indipendentemente; pannello driver; canali con nomi/indici assoluti; output opzionali |
| `WasapiBackend` | WASAPI nativo COM (`IMMDeviceEnumerator`, `IAudioClient3`, `IAudioCaptureClient`, `IAudioRenderClient`, `IMMNotificationClient`) | input/output/loopback | Shared mode di default; loopback capture degli endpoint di render; conversione a float planare; SRC verso il graph rate |
| `VirtualEndpointBackend` | Trasporto diretto col driver `NLMatrixAudio.sys` | capture/render virtuali | Non passa da WASAPI per evitare doppio mix; discovery dal control device; ring condivisi |
| `ReaRouteBackend` | Istanza ASIO del driver ReaRoute | input/output | Opzionale, P7; non prerequisito |
| `TestBackend` | Sorgente/sink di test (tono, file, clock) | input/output | Consente P1–P2 e test offline senza hardware |

Regole:
- Un backend può avere **N istanze**; nessun device ASIO globale.
- Ogni istanza ha il proprio stato (`Closed/Opening/Streaming/Stopping/Error/Conflict/Unavailable`) e il proprio thread/callback.
- Il DeviceManager non assume capacità: legge descrittori reali.
- Apertura output: maschera configurabile; default nessun output per Apollo; output abilitati solo da rotte esplicite.
- Un device già aperto da un'altra app può rifiutare l'apertura: stato `Conflict` con messaggio; nessun tentativo distruttivo o retry loop.

### 3.2 Descrittori e identità

```cpp
enum class BackendType { Asio, Wasapi, VirtualEndpoint, ReaRoute, Test };

struct PortDescriptor {
    int index;                 // indice nel device
    std::string name;          // nome del driver/endpoint
    int channels;              // 1..N
    SampleType sampleType;
    bool isLoopback;
    std::vector<std::string> channelNames;
};

struct DeviceDescriptor {
    std::string stableId;      // hash backend+deviceId+nome+firma canali
    BackendType type;
    std::string displayName;
    std::string vendor, model, driverVersion;
    std::string clockDomainId;
    std::vector<double> sampleRates;
    std::vector<int> bufferSizes;
    bool exclusiveCapable;
    std::vector<PortDescriptor> inputPorts;
    std::vector<PortDescriptor> outputPorts;
    DeviceState state;
    std::string lastError;
};
```

- `stableId` è persistente per quanto possibile; se la firma canali/rate cambia, il profilo viene marcato **“da rivedere”** e non applicato automaticamente in modo distruttivo.
- Identificatori: nome device ASIO, endpoint ID WASAPI, instance/interface ID degli endpoint virtuali; fallback su nome + numero canali.
- Il numero di porte/canali è dinamico; nessun 2/8/10 assunto.

### 3.3 Ciclo di vita e hotplug

- Enumerazione all'avvio e su richiesta; refresh dal pannello dispositivi.
- Listener: JUCE device type per ASIO; `IMMNotificationClient` per WASAPI; PnP/control device per gli endpoint virtuali.
- Aggiunta/rimozione: il DeviceManager crea/distrugge il backend su thread di controllo; mai dal thread grafo.
- Device perso: le rotte interessate vanno in `Muted/Unavailable`, meter a zero, evento alla UI; riconnessione con ripristino del profilo se `stableId` e firma coincidono.
- Cambio sample rate/buffer: stop controllato, riapertura, re-sync dei ring, rampa; nessun cambio a caldo dentro il callback.

### 3.4 Limiti dei driver e multi-client

- ASIO: multi-client e multi-device dipendono dal driver. JUCE consente fino a 16 device object per processo; il driver UAD può accettare o rifiutare una seconda apertura. Il DeviceManager espone lo stato reale e non promette ciò che il driver non fa.
- WASAPI: shared mode consente più client; loopback non interferisce con l'app che suona; exclusive mode non è il default.
- Endpoint virtuali: Windows mixing permette più app sullo stesso endpoint; l'engine vede un solo stream per endpoint e non distingue i client (il mix-minus si basa sui profili utente, non sull'identità delle app).
- Output ASIO: l'apertura per scrittura può entrare in conflitto con REAPER; policy non invasiva (§7.3).

---

## 4. GraphScheduler, clock e ring buffer

### 4.1 Perché un thread del grafo dedicato

Sono stati confrontati due modelli:

| Criterio | Callback master (un device esegue il grafo) | Thread del grafo dedicato (scelto) |
|---|---|---|
| Funziona senza ASIO | No, dipende dal device master | **Sì**, indipendente dai backend |
| Clock | Legato al device master | QPC del sistema; i device sono asincroni |
| Multi-device | Il master impone rate/block; gli altri asincroni | Tutti i device asincroni e uniformi |
| Latenza | Minima sul master, ma il master è un single point of failure | Un blocco di ritardo in più; jitter assorbito dai ring |
| Failover | Complesso: cambiare master cambia il clock | Semplice: il grafo continua; si riconfigurano i device |
| SRC | Nessun SRC sul master, SRC sugli altri | SRC su ogni device (o ottimizzazione clock-locked futura) |
| Jitter del timer | Nessuno sul master | Mitigato da timer ad alta risoluzione + ring + target adattivo |

**Decisione:** thread del grafo dedicato, con timer ad alta risoluzione e priorità audio. È l'unico modello che soddisfa il requisito “funziona anche senza ASIO” e il multi-device uniforme. Un'ottimizzazione futura (P7) potrà introdurre un modo **clock-locked** che aggancia il periodo del grafo a un device di riferimento via PLL, senza tornare al callback master.

**Eccezione controllata per la vertical slice:** la prima implementazione (`NLMatrixProbe`/`NLMatrixLive`, §15) può eseguire la matrice direttamente nel callback ASIO e usare un ring verso un output WASAPI. È un percorso di validazione, non l'architettura finale; quando si aggiungono più device o il requisito “senza ASIO”, si passa al thread dedicato.

### 4.2 Scheduler

- Thread `GraphScheduler` con `AvSetMmThreadCharacteristicsW(L"Pro Audio")` e priorità critica; timer `CreateWaitableTimerExW` con `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, scadenze assolute (no drift cumulativo).
- Periodo nominale = `blockFrames / graphRate`; `blockFrames` iniziale 128 (configurabile 64/128/256), `graphRate` iniziale 48 kHz (44.1/48/96 selezionabile).
- Un blocco per tick; se il tick è in ritardo non si accumulano blocchi di recupero (niente burst): si processa un blocco e si incrementa `lateTicks`; i ring assorbono.
- `GraphEngine::process(blockFrames)` legge le sorgenti, mixa, scrive i sink. Se non ci sono device attivi, produce silenzio e mantiene il clock.
- Watchdog: se il thread non completa entro una soglia, gli output vanno in mute con rampa e viene richiesta la riconfigurazione; nessun crash.
- Il thread grafo non apre/chiude device e non chiama il driver per controllo: solo memoria condivisa/ring.

### 4.3 Ring buffer: capacità vs riempimento operativo

- Ogni device ha un **ring di ingresso** (device→grafo) e uno di **uscita** (grafo→device), SPSC, preallocati.
- **Capacità massima** = protezione di sicurezza (default 250 ms per ring, configurabile; memoria preallocata). Non è il target di latenza.
- **Riempimento operativo** = banda target scelta dalla modalità latenza e adattata dal controller:
  - `Low`: target iniziale ≈ 8 ms, banda 6–12 ms;
  - `Balanced`: ≈ 16 ms, banda 10–24 ms;
  - `Safe`: ≈ 32 ms, banda 20–48 ms.
- Il controller PI misura il fill e corregge il rapporto di SRC (±200 ppm, clamp) per mantenerlo nella banda; non assume un 50% fisso. Se il fill scende sotto la banda → silenzio/underrun contato; sopra → drop/trim contato.
- La UI mostra capacità, fill min/avg/max, target, banda, ppm, underrun/overrun; l'utente può cambiare modalità.
- La capacità può essere aumentata senza cambiare il codice (configurazione), ma la memoria è preallocata all'apertura.

### 4.4 Flusso dati

```
Device capture callback/thread
  -> ring ingresso (rate nativo)
  -> AdaptiveResampler (rate nativo -> graphRate, drift)
  -> GraphEngine (sorgenti, bus, mix-minus)
  -> AdaptiveResampler (graphRate -> rate nativo)
  -> ring uscita
  -> Device render callback/thread
```

- I device non-clock non bloccano mai: se il ring è pieno/vuoto, drop/silenzio e contatori.
- Il device che suona non viene mai fermato dal grafo.
- Gli endpoint virtuali usano lo stesso schema: il graph thread legge/scrive i ring del driver; il driver ha il proprio clock WaveRT.

---

## 5. GraphEngine e routing dinamico

### 5.1 Modello

**Nodi:**
- `DeviceInputPort` — canale/porta di un device (ASIO/WASAPI/virtuale);
- `DeviceOutputPort` — canale/porta di uscita;
- `Bus` — mix a 1..N canali, configurabile;
- `VirtualCaptureSink` — endpoint `NL Virtual Mic <n>`;
- `VirtualRenderSource` — endpoint `NL Virtual Speaker <n>`;
- `MonitorSink` opzionale (solo meter/ascolto interno, mai monitoraggio obbligatorio).

**Archi:**
```cpp
struct Connection {
    NodeId from;
    NodeId to;
    int fromChannel;          // -1 = tutti
    int toChannel;            // -1 = mappa 1:1 o somma
    float gainDb;
    float pan;                // -1..+1, per sorgenti mono
    bool mute;
    bool invertPolarity;
};
```
- Molti-a-molti: più archi possono convergere su un bus/porta; la somma è esplicita.
- Bus multicanale: canali 1..N; la matrice di canali è parte dell'arco.
- Routing diretto device→device senza bus (es. ASIO in→ASIO out, virtual speaker→ASIO out, WASAPI in→virtual mic).
- Bus→bus consentito solo se aciclico e esplicito; il compilatore rileva cicli.
- Nessun numero fisso di bus, canali o archi. Guardie di risorsa (memoria/CPU) configurabili e documentate, non assunzioni di prodotto.

### 5.2 Compilazione e realtime

- La configurazione viene compilata in un `GraphSnapshot` immutabile: lista topologica di nodi, per ogni bus la lista di contributi, per ogni sink la lista di sorgenti, rampe.
- Pubblicazione a 3 slot con contatore di generazione (RCU): il thread grafo legge lo snapshot attivo per l'intero blocco; la UI costruisce lo slot inattivo e pubblica. Nessun lock nel percorso audio.
- Gain/route changes: rampa 5–10 ms; panic: rampa breve e mute; niente click.
- Meter: atomici per porta/bus; la UI li legge a 30 Hz.
- Il grafo non alloca durante il process; tutti i buffer scratch sono preallocati alla dimensione massima del blocco.

### 5.3 Mix-minus e anti-feedback

- `ClientProfile { id, name, sendBus/sink, returnSources[] }`.
- Il compilatore esclude dai send del client i suoi return (e i bus che li contengono), salvo override esplicito con conferma.
- Return multipli supportati (es. Source-Connect + browser).
- **Anti-feedback:** blocco di default delle rotte `VirtualSpeaker_i → VirtualMic_j` assegnate allo stesso client; tra client diversi è consentito solo con conferma e avviso. Il grafo rileva cicli interni; non può rilevare loop esterni alle app (documentato).
- Avviso non automatico per Mic1 + AUX che possono contenere la stessa voce; nessuna somma implicita.

### 5.4 Capacità e validazione

- Ogni arco è validato contro canali, rate e formato reali del device.
- Output non disponibile/occupato: errore o avviso, mai scrittura silenziosa su un output altrui.
- Se un device cambia canali/rate, le rotte che non sono più valide vengono disabilitate e segnalate; il preset resta salvato.

---

## 6. Endpoint virtuali e provisioning

### 6.1 Modello

- Driver `NLMatrixAudio.sys` derivato da SysVAD (MS-PL), con un **massimo di endpoint definito dal build** (proposta: 8 capture + 8 render, da confermare in base a PnP/test) e un **set provisionato iniziale 4+2**.
- Ogni endpoint è una coppia topo+wave WaveRT, con nome stabile `NL Virtual Mic <n>` / `NL Virtual Speaker <n>`, formato iniziale 48 kHz/2 canali/float32, event-driven.
- Il set effettivo è configurato dal provisioning (installer/registry); il driver installa solo i subdevice abilitati. Aggiungere/rimuovere endpoint richiede modifica configurazione + riavvio device/driver (disable/enable o reboot). Non si promette creazione/rimozione arbitraria a runtime: è un limite di PnP/INF documentato.
- Il motore legge il set dal control device (`GET_INFO`) e non hardcoda nulla; un endpoint può essere disabilitato senza cambiare il routing engine (nodo `EndpointLost`).

### 6.2 Perché non endpoint dinamici a runtime

Windows enumera endpoint tramite driver/INF e AudioEndpointBuilder; la creazione/rimozione arbitraria richiede un modello di child device/PnP e un'INF coerente, con rischi di stato inconsistente. La scelta è:
- massimo di build generoso;
- provisioning configurabile;
- restart controllato;
- discovery dinamica lato engine.

Documentato storicamente in `docs/history/TECHNICAL_DECISIONS.md` (D12).

### 6.3 Formati e compatibilità

- Formato iniziale fisso: 48 kHz, 2 ch, float32, WAVEFORMATEXTENSIBLE. Le app a 44.1/96 vengono ricampionate dal Windows audio engine; l'engine ricampiona dal proprio graph rate.
- Il set di formati è una tabella del driver, estendibile in build successive (44.1/96, più canali) senza cambiare il motore.
- Nomi e ID persistenti; il profilo utente li referenzia per ID, non per ordine di enumerazione.

---

## 7. Driver, trasporto PCM e output non invasivi

### 7.1 Driver

- Fork SysVAD ridotto: niente APO, BT/USB/A2DP/KWS; endpoint da `TabletAudioSample/minipairs.h` e INF componentizzato; niente tone/savedata nel percorso di produzione.
- WaveRT:
  - capture: `CMiniportWaveRTStream::WriteBytes` legge dal ring e riempie il DMA; silenzio su underrun;
  - render: `ReadBytes`/completamento pacchetto scrive il DMA nel ring; drop-oldest su overrun;
  - `SetState` resetta i ring; `TimerNotifyRT` aggiorna heartbeat/contatori; le posizioni WaveRT restano invariate.
- Punti di inserimento verificati: `minwavertstream.cpp:1497-1554`, `:1149-1372`, `:1729-1854`; `minwavert.cpp:633-751`; `common.cpp:2492-2640`.

### 7.2 Trasporto

- Blocco **non-paged** del driver: header + un ring per endpoint (capienza 100 ms), MDL bloccato e mappato nel processo engine via IOCTL.
- Header per endpoint: magic/versione/ID/direzione, formato, capienza, contatori monotoni `writeFrames`/`readFrames`, heartbeat, underrun/overrun/xrun, stato.
- Control device KMDF separato (PortCls possiede il FDO audio), device interface + symbolic link, SDDL ristretto; IOCTL versionati (`GET_INFO`, `MAP`, `UNMAP`, `GET_STATS`, `SET_HEARTBEAT`), validazione di ID, lunghezze, offset; nessun puntatore utente fidato.
- Cleanup su chiusura handle/crash: unmap, stato disconnesso, silenzio; su rimozione driver l'engine va in `EndpointsLost` e chiude il trasporto.
- Nessuna allocazione/attesa nel percorso dati; sincronizzazione SPSC con barriere; il thread grafo non fa syscall.

### 7.3 Politica output non invasiva (REAPER/Console)

1. Gli output ASIO sono **enumerati** sempre, ma **non aperti per scrittura di default**; per Apollo la maschera output è disabilitata salvo scelta esplicita.
2. Una rotta verso un output ASIO richiede: capacità del device, conferma utente, verifica che il device sia aperto dalla Matrix, avviso su possibile conflitto con REAPER/Console.
3. Il percorso preferito per il ritorno delle piattaforme resta **client → Apollo direttamente**, come oggi; la Matrix non lo forza e non lo intercetta.
4. WASAPI output in shared mode è il percorso sicuro per il routing bidirezionale (nessuna sottrazione esclusiva).
5. In P1 si testa l'apertura output (anche solo maschera, senza scrivere) per verificare che non disturbi REAPER/Console; se disturba, si resta input-only e si documenta.
6. Panic/mute sempre disponibili; nessuna scrittura su output non configurati.

---

## 8. Clock, SRC, latenza

- **Dominio grafo:** QPC + timer ad alta risoluzione; graph rate configurabile (default 48 kHz).
- **Dominio device:** ASIO hardware clock, WASAPI endpoint clock, WaveRT virtuale (QPC del driver). Nessun clock condiviso assunto.
- **SRC adattivo per device:** windowed-sinc 16 tap iniziali (fallback linear), rapporto corretto dal controller di fill; compensa sia rate diversi sia drift ppm.
- **Latenza:** buffer device + fill operativo + blocco grafo + buffer uscita. Modalità `Low/Balanced/Safe`; valori misurati in P2/P5, nessuna promessa. Il monitor locale resta UAD Console.
- **Meter diagnostici:** fill min/avg/max, target/banda, ppm, underrun/overrun, late tick, callback time, latenza dichiarata dal driver, XRUN.
- **Failover:** device perso → rotte in mute, ring svuotati, riconnessione; il grafo continua. Cambio graph rate → stop/restart coordinato, rampa.

---

## 9. GUI, IPC, preset, persistenza

- GUI JUCE 9: matrice dinamica, patchbay a nodi, pannello dispositivi, bus strips, client/mix-minus, meter, panic, preset, diagnostica.
- Layout derivato dal grafo: righe/colonne dinamiche, filtri, gruppi, zoom; nessun 4×N fisso.
- IPC: pipe con messaggi versionati; la UI riceve stato/meter a 30 Hz e invia comandi; engine autorevole.
- Preset: grafo completo + assegnazioni device + profili per dispositivo + modalità latenza; schema versionato; backup; migrazione.
- Persistenza: `%LOCALAPPDATA%\NLAudioMatrix\` (config, log, backup); niente PII nei log di default.
- Riconnessione: match per `stableId`; se la firma cambia, profilo “da rivedere” e nessuna applicazione automatica distruttiva.
- Tray/close-to-tray; Quit esplicito; panic sempre raggiungibile.

---

## 10. Threading e regole realtime

| Thread | Responsabilità | Regole |
|---|---|---|
| GraphScheduler | processa il grafo, SRC, meter | niente alloc/lock/I-O; priorità audio; buffer preallocati |
| ASIO callback | copia input→ring, ring→output, contatori | niente lavoro pesante; niente open/close; niente log |
| WASAPI capture/render | event-driven, ring | niente alloc; gestione errori fuori callback |
| Virtual endpoint | graph thread legge/scrive ring driver | niente syscall nel percorso; heartbeat |
| Control/Message | apertura/chiusura device, preset, IPC | può allocare/bloccare; mai nel percorso audio |
| UI | presentazione | nessun accesso diretto all'audio |

- Snapshot RCU; ring SPSC; atomici con memory order esplicito; nessun `shared_ptr` copiato nel callback.
- Denormali disabilitati; NaN guard; rampe anti-click.
- Watchdog e contatori per diagnosticare late tick/underrun.

---

## 11. Failure mode e recovery

| Evento | Comportamento |
|---|---|
| Device ASIO non apribile / conflict | stato `Conflict`, rotte disabilitate, messaggio; nessun retry distruttivo |
| Device perso durante lo streaming | mute rampa, ring svuotati, riconnessione; grafo continua |
| Cambio rate/buffer | stop coordinato, reopen, re-sync, rampa |
| Graph thread in ritardo | un blocco, `lateTicks++`; se persistente, mute + recovery |
| Ring underrun/overrun | silenzio/drop contati; mai blocco |
| Engine crash | driver heartbeat scaduto → silenzio; UI mostra engine assente |
| Driver/endpoint rimosso | engine `EndpointsLost`, rotte virtuali disabilitate, profilo salvato |
| Endpoint virtuale disabilitato | nodo `EndpointLost`, rotte in mute, ri-enumerazione |
| UI crash | audio continua; riconnessione |
| Output ASIO occupato | errore/avviso, nessuna scrittura forzata |

---

## 12. Build, deploy, firma

- **App:** CMake + VS 2022 BuildTools + CMake (da installare); JUCE 9.0.4 pinnato; `NLMatrixCore`, `NLMatrixEngine`, `NLMatrixUI`, `NLMatrixProbe`, `transport_probe`, test.
- **Driver:** MSBuild/VS + WDK 10.0.26100 (da installare); vendor upstream `Windows-driver-samples` pinnato (MS-PL, WIL, INX); fork in `driver/`; script build/install/uninstall; test signing solo VM; attestazione/WHQL per release.
- **Nessuna installazione driver sul PC di lavoro in P0–P2.** P3 su VM; P4/P5 su VM; P6/P7 con build firmata e approvazione esplicita.
- CI: build/test app; driver build su runner con WDK o locale; nessun deploy automatico.

---

## 13. Espandibilità e limiti interni documentati

### 13.1 Motore di routing

- Nessun limite fisso di device/canali/bus/archi nel modello.
- Guardie di risorsa configurabili (memoria, CPU, numero archi) per evitare config malformate; sono limiti di sicurezza, non assunzioni di prodotto.
- Il compilatore del grafo e i buffer sono dimensionati sulla configurazione; il percorso realtime non alloca.

### 13.2 JUCE

- ASIO: `maxNumASIODevices = 16` per processo (`juce_ASIO_windows.cpp:317-319`). Superabile con patch/fork di JUCE o con più processi engine in versioni future; non è un limite del routing engine.
- `AudioDeviceManager` gestisce un device per volta; il progetto usa un DeviceManager proprio con più host.
- WASAPI JUCE non ha loopback; backend nativo.

### 13.3 ASIO

- Numero di canali, rate, buffer, multi-client e multi-device sono proprietà del driver, non standardizzate. L'enumerazione è l'unica fonte di verità.
- Alcuni driver non consentono due device simultanei o due aperture dello stesso device; il DeviceManager lo mostra e non lo maschera.

### 13.4 SysVAD / driver

- Gli endpoint sono statici rispetto al set compilato/INF; il massimo è una scelta di build. Aumentarlo = estendere tabelle `minipairs.h`/`*wavtable.h`, INF/`AddInterface`, rebuild/reinstall.
- Provisioning configurabile + restart; nessuna creazione arbitraria a runtime.
- Capacità del trasporto (nonpaged pool) proporzionale al numero di endpoint × capienza; documentata e configurabile.

### 13.5 Come aumentare in futuro

1. Più endpoint: estendere tabelle/INF e massimo di build; provisioning; test PnP.
2. Più device ASIO: patch del limite JUCE o più processi engine (futuro).
3. Più bus/canali: configurazione; nessuna modifica al motore, solo guardie di risorsa.
4. Più formati/rate endpoint: tabelle driver; SRC del motore già pronto.
5. Clock-locked PLL: ottimizzazione P7 per ridurre SRC sul device di riferimento.
6. Backend aggiuntivi (ReaRoute, altri): nuova implementazione di `IAudioBackend`.

---

## 14. Non-obiettivi

- Sostituire REAPER, UAD Console o il driver UAD.
- Monitoraggio software obbligatorio o latenza zero promessa.
- Creazione/rimozione arbitraria di endpoint Windows a runtime.
- Routing verso output ASIO occupati senza consenso.
- Reverse engineering di UAD/VB-CABLE.
- Supporto a più processi engine con lo stesso trasporto nella v1.

---

## 15. Percorso MVP: vertical slice prima dell'architettura completa

La documentazione descrive il target; l'implementazione parte da una **vertical slice** per ridurre il rischio e verificare presto le capacità reali.

### 15.1 Sequenza

1. **Probe ASIO** (`NLMatrixProbe`, JUCE 9 console):
   - `--list`: enumerazione device ASIO senza aprire buffer;
   - `--capture`: apertura di tutti gli input, cattura separata per canale, WAV + JSON;
   - report canali/rate/buffer e mappa canale↔buffer.
2. **Live diagnostica** (`NLMatrixLive`):
   - matrice semplice gestita nel callback ASIO, con numero di canali/bus scoperto a runtime;
   - output WASAPI verso un endpoint di diagnostica (VB-CABLE solo come banco di prova, non soluzione finale);
   - meter, panic, log; verifica end-to-end con Source-Connect/Zoom.
3. **Driver** (P3): sostituisce l'endpoint diagnostico con `NL Virtual Mic/Speaker`; la matrice non cambia concettualmente.
4. **Consolidamento**: scheduler dedicato, DeviceManager multi-backend, grafo dinamico, mix-minus, GUI, multi-device.

### 15.2 Componenti evitati nella slice

| Componente target | Perché rimandato | Cosa si usa ora |
|---|---|---|
| Host ASIO custom | JUCE copre enumerazione/open/callback | JUCE 9 `AudioIODeviceType` ASIO |
| Plugin/runtime OBS | Non è il prodotto | API JUCE diretta |
| SDK ASIO alternativo/openASIO | Non necessario con JUCE 9 stock | SDK incluso in JUCE (licenza da decidere) |
| Driver virtuale | Richiede VM/WDK e non serve per validare il routing | WASAPI verso endpoint diagnostico |
| Scheduler dedicato | La slice ha un solo device | Callback ASIO + ring |
| DeviceManager multi-backend | La slice ha un solo device ASIO + un output WASAPI | Due `AudioIODevice` JUCE |
| Grafo dinamico compilato | La slice ha una matrice semplice | Array di route runtime |
| GUI completa | Non serve per P1 | CLI/report |
| WASAPI loopback nativo | Serve per i ritorni in P5 | WASAPI JUCE output |
| Engine/UI split | Non necessario per la slice | Un processo |

### 15.3 Cosa resta valido del target

- Nessun hardcode di canali/bus: la slice legge i canali reali e accetta una configurazione.
- Output non invasivi: Apollo output off di default.
- REAPER/Console intatti; nessuna modifica al setup.
- Licenze: nessun codice GPL copiato; JUCE/ASIO da licenziare.
- Espandibilità: la slice è un modulo della futura architettura, non un fork usa-e-getta.

### 15.4 Criterio di uscita della slice

- Tutti i canali Apollo acquisiti separatamente e mappati (P1).
- Un mix stereo raggiunge un endpoint WASAPI selezionabile da un'app terza (diagnostica).
- REAPER/Console invariati.
- Report con comandi, misure, limiti e prossima azione.
- Solo dopo: driver e architettura completa.

---

## 16. Riferimenti interni

- Architettura/tuning engine v2: `docs/ENGINE_V2.md`.
- Backlog unico: `TASKS.md`; stato per riprendere: `DEVELOPMENT_STATE.md`.
- Risultati verificati: `reports/TEST_STATUS.md`.
- Documenti storici (audit, decisioni, roadmap, test plan): `docs/history/`.
