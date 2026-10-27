# TECHNICAL DECISIONS — NL Audio Matrix v2

**Versione:** 3.0 — scope universale multi-device  
**Data:** 24 ottobre 2026  
**Stato:** P0 — decisioni di pianificazione; nessuna implementazione.  
**Formato:** ADR (Architecture Decision Record). Stati: `ACCETTATA`, `PROPOSTA`, `DA CONFERMARE`, `RINVIATA`.

---

## D01 — Scope universale, Apollo come dispositivo di test

**Contesto:** i documenti precedenti trattavano Apollo Solo USB come perimetro del prodotto e fissavano 4 bus, 4 capture + 2 render, un solo device ASIO.  
**Decisione:** il prodotto è una matrice audio universale Windows multi-dispositivo, multicanale e multi-bus. Apollo è il primo dispositivo di test; nessun limite di prodotto deriva da Apollo.  
**Alternative:** restare Apollo-only (più semplice, ma non soddisfa la richiesta); supportare solo ASIO (esclude WASAPI e i ritorni delle piattaforme).  
**Conseguenze:** DeviceManager multi-backend, grafo dinamico, clock indipendenti, provisioning endpoint. I documenti non devono contenere “4 bus”, “4+2” o “Apollo-only” come vincoli; solo come preset/dataset iniziale.  
**Stato:** ACCETTATA.  
**Evidenza:** richiesta utente; `SOURCE_CODE_AUDIT.md` §10.

---

## D02 — JUCE 9.0.4 stock per ASIO e GUI

**Contesto:** il JUCE locale è 9.0.4 stock; OBS-ASIO usa un fork JUCE 7.0.2 + openASIO, non disponibile localmente e non compilabile con l'API JUCE 9.  
**Decisione:** usare JUCE 9.0.4 stock per ASIO, GUI, core; nessun codice OBS-ASIO copiato; callback `audioDeviceIOCallbackWithContext`; `startThread(Priority)`/API moderne.  
**Alternative:** fork JUCE 7.0.2/openASIO (GPL, vecchio, non allineato al locale); Qt6 + host ASIO custom (più lavoro, doppio stack).  
**Conseguenze:** il codice OBS-ASIO è solo riferimento comportamentale; l'ASIO backend va scritto ex novo; licenza JUCE AGPL/commerciale da decidere.  
**Stato:** ACCETTATA.  
**Evidenza:** `JUCE/juce_StandardHeader.h:42-44`, `juce_ASIO_windows.cpp:317-319`, `obs-asio/src/asio-input.cpp:331,438`, `CMakeLists.txt:8-14`.

---

## D03 — DeviceManager con backend indipendenti, non un device ASIO globale

**Contesto:** OBS-ASIO ha un registro process-wide di `AudioCB` per nome device, ma non un DeviceManager multi-backend; REAPER/WASAPI non sono integrati.  
**Decisione:** `IDeviceManager` con N istanze di backend: ASIO (una per device), WASAPI nativo, endpoint virtuali, ReaRoute opzionale, Test. Ogni backend ha stato, thread/callback, capacità e ciclo di vita indipendenti.  
**Alternative:** un solo `AudioDeviceManager` JUCE (un device per volta); un solo backend ASIO con fan-out (non universale).  
**Conseguenze:** apertura/chiusura su thread di controllo; conflitti e multi-client gestiti per device; hotplug; nessuna assunzione di clock comune.  
**Stato:** ACCETTATA.  
**Evidenza:** `SOURCE_CODE_AUDIT.md` §4.5, §5.2; `juce_ASIO_windows.cpp:317-319`; `juce_AudioDeviceManager.cpp:1089-1156`.

---

## D04 — WASAPI nativo per input, output e loopback

**Contesto:** JUCE 9 espone WASAPI con `IMMNotificationClient`, `IAudioClient3`, `IAudioCaptureClient`, ma non implementa `AUDCLNT_STREAMFLAGS_LOOPBACK`.  
**Decisione:** backend WASAPI nativo: capture da input endpoint, render su output endpoint, loopback capture degli endpoint di render; shared mode di default; `IAudioClient3` per buffer flessibili; conversione a float planare e SRC verso il graph rate.  
**Alternative:** WASAPI JUCE (senza loopback); esclusiva solo (rischiosa); VB-CABLE (vietato come soluzione finale).  
**Conseguenze:** più codice COM/Windows, ma controllo completo su loopback, notifiche e formati; nessun percorso obbligato da VB-CABLE.  
**Stato:** ACCETTATA.  
**Evidenza:** `juce_WASAPI_windows.cpp:170-185, 252, 259, 1904`; grep `LOOPBACK` assente; requisito ritorni piattaforme.

---

## D05 — Backend endpoint virtuali su trasporto diretto

**Contesto:** gli endpoint virtuali sono visibili anche via WASAPI, ma passarci creerebbe un doppio mix (Windows audio engine + Matrix).  
**Decisione:** il backend `VirtualEndpointBackend` usa il trasporto diretto col driver (ring/IOCTL), non WASAPI, per capture e render virtuali.  
**Alternative:** WASAPI loopback/render sui nostri endpoint (doppio buffering, latenza, mix indesiderato).  
**Conseguenze:** il motore controlla il PCM esatto; gli endpoint restano selezionabili dalle app; il driver è l'unica fonte di verità sul set.  
**Stato:** ACCETTATA.  
**Evidenza:** `ARCHITECTURE.md` §6–§7; `SOURCE_CODE_AUDIT.md` §6.4.

---

## D06 — Thread del grafo dedicato, non callback master

**Contesto:** con più device indipendenti e requisito “funziona anche senza ASIO”, un device master che esegue il grafo è un single point of failure e impone il proprio clock/rate.  
**Decisione:** `GraphScheduler` dedicato: thread ad alta priorità, timer ad alta risoluzione, MMCSS “Pro Audio”, scadenze assolute. I device sono asincroni con ring e SRC adattivo. Il grafo funziona senza ASIO; se non ci sono device, produce silenzio e mantiene il clock.  
**Alternative:** callback master (latenza minima sul master, ma dipendenza dal device, failover complesso, clock imposto); timer thread + clock master device (ibrido, più complesso).  
**Conseguenze:** un blocco di latenza in più sul device di riferimento; SRC su ogni device; jitter assorbito dai ring; failover semplice; uniformità multi-device. In P7 si potrà aggiungere un modo clock-locked (PLL) per ridurre l'SRC sul device di riferimento.  
**Stato:** ACCETTATA.  
**Evidenza:** `ARCHITECTURE.md` §4.1; requisito “Matrix senza ASIO”; confronto in TEST_PLAN (jitter/latenza).

---

## D07 — Graph rate e clock di riferimento

**Contesto:** QPC è disponibile sempre; ASIO/WASAPI hanno clock hardware indipendenti.  
**Decisione:** graph rate configurabile (default 48 kHz, 44.1/48/96 selezionabili); clock del grafo = QPC/timer; i device sono slave via SRC adattivo. Nessuna assunzione di clock condiviso.  
**Alternative:** graph rate = clock del primo device (dipende dal device); clock esterno word clock (non disponibile su tutti).  
**Conseguenze:** SRC su ogni device; qualità affidata al resampler; possibilità futura di clock-locked.  
**Stato:** ACCETTATA.  
**Evidenza:** `ARCHITECTURE.md` §4, §8.

---

## D08 — Capacità ring vs riempimento operativo; target adattivo, non 50%

**Contesto:** un target fisso del 50% è arbitrario e peggiora la latenza; la capacità del ring è una protezione, non un target.  
**Decisione:** capacità massima configurabile (default 250 ms) preallocata; riempimento operativo in banda definita dalla modalità `Low/Balanced/Safe` (es. 8/16/32 ms iniziali), controller PI che corregge il rapporto SRC (±200 ppm) per restare nella banda; nessun 50% assunto.  
**Alternative:** target fisso 50%; ring minimo senza controller (drift); ring enorme (latenza).  
**Conseguenze:** latenza selezionabile e misurabile; underrun/overrun contati; l'utente vede capacità, target, banda e fill.  
**Stato:** ACCETTATA.  
**Evidenza:** requisito utente; `ARCHITECTURE.md` §4.3.

---

## D09 — Grafo dinamico, bus dinamici, nessun limite artificiale

**Contesto:** i documenti precedenti fissavano 4 bus e 4+2 endpoint; il routing deve essere N×M espandibile.  
**Decisione:** nodi/archi dinamici; bus in numero e canali configurabili (mono/stereo/multicanale); routing molti-a-molti e diretto; nessun numero fisso nel motore. Solo guardie di risorsa configurabili (memoria/CPU) documentate come sicurezza, non come limite di prodotto.  
**Alternative:** 4 bus fissi (non universale); matrice densa precablata (memoria inutile).  
**Conseguenze:** compilatore del grafo, snapshot, UI dinamica; test di stress con configurazioni grandi.  
**Stato:** ACCETTATA.  
**Evidenza:** requisito utente; `ARCHITECTURE.md` §5.

---

## D10 — Snapshot RCU a 3 slot

**Contesto:** la GUI deve modificare il routing senza lock nel percorso audio.  
**Decisione:** 3 slot immutabili + contatore di generazione; la UI scrive lo slot inattivo, il thread grafo legge lo slot attivo; rampe anti-click.  
**Alternative:** mutex (inaccettabile); doppio buffer a 2 slot (rischio di sovrascrittura mentre il thread legge); shared_ptr atomico (costo/allocazioni).  
**Conseguenze:** nessun lock audio; memoria costante; pubblicazione atomica.  
**Stato:** ACCETTATA.  
**Evidenza:** `ARCHITECTURE.md` §5.2, §10.

---

## D11 — Base driver SysVAD

**Contesto:** SysVAD è l'unico sample locale con multi-endpoint WaveRT e INF moderno; Micy/Sweetly non scalano.  
**Decisione:** fork SysVAD (MS-PL), ridotto a endpoint capture/render, senza APO/BT/USB/A2DP/KWS; trasporto PCM aggiunto.  
**Alternative:** Micy (1 capture, ring globale insicuro, no render); Sweetly (WaveCyclic, FIFO statica, solo 2 percorsi); driver da zero (tempo/rischio).  
**Conseguenze:** riuso di tabelle/INF/miniport; lavoro su trasporto/sicurezza; obblighi MS-PL.  
**Stato:** ACCETTATA.  
**Evidenza:** `SOURCE_CODE_AUDIT.md` §6, §9.

---

## D12 — Provisioning endpoint: massimo di build + configurazione + restart

**Contesto:** Windows non crea/rimuove endpoint audio arbitrariamente a runtime; SysVAD installa subdevice statici da tabelle/INF.  
**Decisione:** massimo di endpoint definito dal build (proposta 8+8, da validare); set provisionato iniziale 4+2; provisioning configurabile (installer/registry); modifica del set = restart device/driver (disable/enable o reboot); il motore legge il set effettivo e non hardcoda nulla.  
**Alternative:** creazione dinamica runtime (complessa, PnP/INF fragile); endpoint fissi 4+2 (non espandibile).  
**Conseguenze:** documentare la procedura; test PnP; nessuna promessa di creazione arbitraria; espansione futura via tabelle/INF/rebuild.  
**Stato:** ACCETTATA (massimo numerico PROPOSTO, da confermare in P3).  
**Evidenza:** `SOURCE_CODE_AUDIT.md` §6.2, §6.5; `minipairs.h:510-539`; requisito utente.

---

## D13 — Trasporto: ring non-paged + MDL mappato + IOCTL

**Contesto:** il percorso audio non può toccare memoria paginabile a DISPATCH_LEVEL; servono ring per endpoint e controllo.  
**Decisione:** blocco non-paged del driver (header + ring per endpoint), MDL bloccato e mappato nel processo engine via IOCTL; control device KMDF separato; IOCTL versionati; heartbeat; cleanup su chiusura handle.  
**Alternative:** sezione condivisa paginabile (rischio fault a DISPATCH); IOCTL read/write per blocco (overhead/syscall); memoria utente mappata in kernel (rischio sicurezza).  
**Conseguenze:** sicurezza e realtime migliori; lifecycle da testare su crash/rimozione; capacità limitata dal nonpaged pool (documentata).  
**Stato:** ACCETTATA.  
**Evidenza:** `SOURCE_CODE_AUDIT.md` §6.4; `ARCHITECTURE.md` §7.2.

---

## D14 — Formato endpoint iniziale

**Contesto:** le app target usano 48 kHz; Windows shared mode ricampiona; il driver SysVAD ha tabelle di formato.  
**Decisione:** iniziale 48 kHz, 2 canali, float32, WAVEFORMATEXTENSIBLE, event-driven; formato per-endpoint in tabella, estendibile (44.1/96, più canali) senza cambiare il motore.  
**Alternative:** formato variabile subito (più test); 16-bit (perdita qualità); multicanale subito (compatibilità app).  
**Conseguenze:** semplicità P3/P4; SRC nel motore; documentare l'estensione.  
**Stato:** ACCETTATA per il prototipo; estensione PROPOSTA.  
**Evidenza:** `SOURCE_CODE_AUDIT.md` §6.2; `ARCHITECTURE.md` §6.3.

---

## D15 — Routing bidirezionale e output non invasivi

**Contesto:** l'utente richiede ASIO in→out, virtual speaker→ASIO out, WASAPI in→virtual mic, virtual speaker→virtual mic; REAPER/Console non devono essere disturbati.  
**Decisione:** il grafo supporta tutti i percorsi. Policy output: enumerare sempre gli output ASIO; non aprirli per scrittura di default; per Apollo output disabilitati salvo scelta esplicita; ogni rotta output richiede capacità, conferma e avviso; WASAPI shared è il percorso sicuro; il ritorno piattaforme→Apollo diretto resta supportato e preferito.  
**Alternative:** apertura output sempre (rischio conflitti); nessun routing output (non soddisfa la richiesta).  
**Conseguenze:** P1 testa l'apertura output senza scrivere; P2/P5 implementano le rotte; documentazione chiara sui conflitti.  
**Stato:** ACCETTATA.  
**Evidenza:** requisito utente; `ARCHITECTURE.md` §7.3; `SOURCE_CODE_AUDIT.md` §4.4.

---

## D16 — Mix-minus e anti-feedback

**Contesto:** ogni client deve ricevere un mix senza il proprio ritorno; i loop virtual speaker→virtual mic possono creare feedback.  
**Decisione:** `ClientProfile` con send e return multipli; il compilatore esclude i return del client dal suo send; blocco di default delle rotte virtual speaker→virtual mic dello stesso client; conferma per client diversi; rilevamento cicli interni; avviso Mic1+AUX duplicato. Loop esterni non rilevabili documentati.  
**Alternative:** mix-minus manuale (errore umano); nessun blocco (feedback).  
**Conseguenze:** UI di conferma, diagnostica, test anti-eco.  
**Stato:** ACCETTATA.  
**Evidenza:** requisito utente; `ARCHITECTURE.md` §5.3.

---

## D17 — Engine e GUI separati

**Contesto:** la GUI non deve influire sul realtime; l'audio deve sopravvivere alla chiusura della UI.  
**Decisione:** `NLMatrixEngine.exe` headless + `NLMatrixUI.exe`; IPC named pipe; engine autorevole.  
**Alternative:** processo unico (più semplice, ma crash UI = audio perso; difficile test headless).  
**Conseguenze:** IPC da versionare; lifecycle engine; test di riconnessione.  
**Stato:** ACCETTATA.  
**Evidenza:** `ARCHITECTURE.md` §2, §9.

---

## D18 — Persistenza, stable ID, profili

**Contesto:** device ricollegati possono cambiare nome/ordine; i preset devono essere ripristinabili.  
**Decisione:** `stableId` da backend+ID+nome+firma canali; profili per device; match per ID, fallback nome; se firma cambia, profilo “da rivedere” e applicazione non distruttiva.  
**Alternative:** match per nome (fragile); per ordine (fragile).  
**Conseguenze:** UI di revisione; test hotplug.  
**Stato:** ACCETTATA.  
**Evidenza:** requisito utente; `ARCHITECTURE.md` §3.2, §9.

---

## D19 — Licenze e distribuzione

**Contesto:** OBS-ASIO GPL-3.0; JUCE 9 AGPL/commerciale; ASIO SDK GPLv3/Steinberg; SysVAD MS-PL; Micy/Sweetly base MS-PL.  
**Decisione:** nessun codice OBS-ASIO copiato; JUCE 9 stock; ASIO SDK con licenza da scegliere; driver MS-PL con notice; sviluppo privato finché la distribuzione non è decisa. Per closed-source: JUCE commerciale + Steinberg. Per open: GPLv3/AGPL con obblighi.  
**Alternative:** GPLv3 subito (vincola tutto); ignorare le licenze (inaccettabile).  
**Conseguenze:** decisione di business prima della release; architettura che isola ASIO.  
**Stato:** ACCETTATA come impostazione; scelta finale DA CONFERMARE dall'utente.  
**Evidenza:** `SOURCE_CODE_AUDIT.md` §3, §11.

---

## D20 — Toolchain, test, firma

**Contesto:** VS BuildTools presente; WDK e CMake mancanti; il driver non può essere testato sul PC di lavoro.  
**Decisione:** app CMake + VS; driver MSBuild/WDK su VM; test signing su VM; release con attestazione/WHQL e certificato EV; nessuna installazione driver sul PC di lavoro in P0–P2; P3–P5 su VM.  
**Alternative:** test diretto sul PC di lavoro (vietato); driver non firmato (non distribuibile).  
**Conseguenze:** installare WDK/CMake con approvazione; VM/secondo PC; tempi di firma.  
**Stato:** ACCETTATA.  
**Evidenza:** verifica ambiente; `SOURCE_CODE_AUDIT.md` §2, §6.5.

---

## D21 — P1: verifica dell'acquisizione reale, non solo enumerazione

**Contesto:** l'utente richiede che P1 verifichi l'acquisizione effettiva di tutti i 10 canali Apollo e la corrispondenza canale ASIO↔buffer, non solo l'elenco.  
**Decisione:** P1 include: dump di nomi/indici/rate/buffer di input e output; apertura di tutti gli input; registrazione separata di ogni canale; iniezione di segnali di test distinti (toni/PRN) per sorgente; analisi di presenza, frequenza, livello e cross-correlazione; mappa attesa→canale; report PASS/FAIL per canale; test con REAPER aperto/chiuso.  
**Alternative:** solo enumerazione (insufficiente); test manuale non ripetibile (insufficiente).  
**Conseguenze:** `NLMatrixProbe` con modalità `--capture-all`, `--verify-map`, WAV per canale; gate P1 bloccante.  
**Stato:** ACCETTATA.  
**Evidenza:** requisito utente; `TEST_PLAN.md` §P1; `TASKS.md` P1.

---

## D22 — ReaRoute e altri backend: opzionali, non prerequisiti

**Contesto:** ReaRoute è utile se l'host è REAPER, ma non è un endpoint browser/WDM e non è necessario al MVP.  
**Decisione:** backend ReaRoute opzionale in P7; nessun percorso del prodotto dipende da ReaRoute, OBS o VB-CABLE.  
**Alternative:** ReaRoute come integrazione principale (fragile, dipendente da REAPER).  
**Conseguenze:** interfaccia `IAudioBackend` estensibile; test opzionali.  
**Stato:** ACCETTATA.  
**Evidenza:** requisito; `ARCHITECTURE.md` §3.1, §14.

---

## D23 — Vertical slice prima, architettura completa dopo

**Contesto:** la pianificazione precedente costruiva prima DeviceManager multi-backend, scheduler dedicato, grafo dinamico e driver, e solo dopo un risultato end-to-end. Il rischio è spendere settimane senza una Matrix standalone funzionante e senza validare le capacità reali di JUCE/ASIO sul PC.  
**Decisione:** introdurre una **vertical slice** prioritaria, con il massimo riuso delle capacità già disponibili:
1. `NLMatrixProbe` (JUCE 9, console): enumerazione ASIO read-only, apertura di tutti gli input Apollo, cattura separata di ogni canale, WAV + report, mappa canale↔buffer verificata.
2. `NLMatrixLive` (stessa base): matrice semplice e dinamica (tutti i canali → bus stereo configurabili) eseguita nel callback ASIO, uscita **WASAPI** verso un endpoint di diagnostica (es. VB-CABLE solo come banco di prova, non come soluzione finale).
3. Solo dopo: driver SysVAD per sostituire l'endpoint diagnostico con `NL Virtual Mic/Speaker`, poi multi-device, scheduler dedicato, grafo completo e GUI.

**Perché:** JUCE 9 fornisce già enumerazione, apertura, callback planare e WASAPI; OBS-ASIO dimostra che l'APOLLO funziona con questo approccio. Non serve riscrivere l'host ASIO, il plugin OBS o l'SDK ASIO. Il codice OBS-ASIO è GPL e non va copiato; si riusa il **comportamento**, non l'implementazione.  
**Alternative confrontate:**
- riusare OBS-ASIO dentro OBS: escluso dal prodotto, ma utile come baseline di confronto;
- host ASIO custom: non necessario, JUCE copre enumerazione/open/callback;
- driver-first: percorso più lungo, senza validare prima la mappa canali e il routing;
- WASAPI-only: non accede ai singoli canali Apollo;
- grafo/scheduler completi subito: sovra-progettazione rispetto al risultato concreto.

**Componenti che evitiamo di sviluppare adesso:** plugin/runtime OBS, codice OBS-ASIO, host ASIO custom, SDK ASIO alternativo, driver virtuale, GUI completa, DeviceManager multi-backend, scheduler dedicato, grafo dinamico compilato, WASAPI loopback nativo, engine/GUI split. Restano nell'architettura target e verranno introdotti quando la slice li richiede.

**Conseguenze:** primo risultato end-to-end in tempi brevi; la mappa canali e il routing vengono validati sul PC; il driver subentra dopo; l'architettura non viene vincolata a numeri fissi. La slice usa un numero di canali/bus **scoperto a runtime** e una configurazione semplice, non hardcoded. VB-CABLE, se usato, è dichiarato **diagnostica temporanea** e non dipendenza finale.  
**Stato:** ACCETTATA (cambio di metodologia richiesto dall'utente).  
**Evidenza:** `SOURCE_CODE_AUDIT.md` §4–§5; `JUCE/examples/CMake/ConsoleApp`; build CMake 3.31/VS 2022 disponibile; `TASKS.md` task MVP.

---

## Riepilogo decisioni

| ID | Decisione | Stato |
|---|---|---|
| D01 | Scope universale; Apollo test | ACCETTATA |
| D02 | JUCE 9 stock | ACCETTATA |
| D03 | DeviceManager multi-backend | ACCETTATA |
| D04 | WASAPI nativo/loopback | ACCETTATA |
| D05 | Endpoint virtuali su trasporto diretto | ACCETTATA |
| D06 | Thread grafo dedicato | ACCETTATA |
| D07 | Graph rate QPC | ACCETTATA |
| D08 | Capacità vs fill adattivo | ACCETTATA |
| D09 | Grafo dinamico senza limiti artificiali | ACCETTATA |
| D10 | Snapshot RCU 3 slot | ACCETTATA |
| D11 | Base SysVAD | ACCETTATA |
| D12 | Provisioning endpoint | ACCETTATA (massimo PROPOSTO) |
| D13 | Trasporto non-paged + MDL | ACCETTATA |
| D14 | Formato 48k/2ch/float iniziale | ACCETTATA (estensione PROPOSTA) |
| D15 | Output non invasivi | ACCETTATA |
| D16 | Mix-minus/anti-feedback | ACCETTATA |
| D17 | Engine/GUI separati | ACCETTATA |
| D18 | Stable ID/profili | ACCETTATA |
| D19 | Licenze | ACCETTATA (scelta finale utente) |
| D20 | Toolchain/firma | ACCETTATA |
| D21 | P1 verifica reale canali | ACCETTATA |
| D22 | ReaRoute opzionale | ACCETTATA |
| D23 | Vertical slice prima, riuso JUCE, componenti rinviati | ACCETTATA |

## D24 — Riferimento atkAudio / conversione device-rate (25 ottobre 2026)

`atkAudio/PluginForObsRelease` è studiato solo come riferimento comportamentale
(AGPL-3.0, graph derivato JUCE AGPL, parti OBS GPL): nessun codice, nessun header,
nessuna dipendenza. Le idee generiche adottate sono state reimplementate da zero
in `router_main.cpp`:

- SPSC stereo ring → convertitore fra rate dichiarati della sorgente e del grafo;
- trim lento del livello del ring entro ±300 ppm (latenza target 15 ms), non un
  controller basato su lock o drop;
- fade-in di 64 campioni dopo priming/recovery; underflow = silenzio e
  ri-priming, mai ripetizione dell'ultimo campione;
- polling del formato endpoint ogni 500 ms per seguire le rinegoziazioni Windows;
- loopback accettato anche se il suo rate differisce dal grafo ASIO.

Restano fuori scope per ora: stima del rate rispetto al clock host, FIFO da 1 Mi
campioni, graph multithread, VST3 e routing multicanale. Sono fasi architetturali
separate; non vanno importate dal repository AGPL.