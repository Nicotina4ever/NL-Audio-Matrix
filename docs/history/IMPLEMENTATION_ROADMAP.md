# IMPLEMENTATION ROADMAP — NL Audio Matrix v2

**Versione:** 3.0 — scope universale multi-device  
**Data:** 24 ottobre 2026  
**Stato:** P0 completato (documentazione); P1 pronto all'esecuzione.  
**Riferimenti:** `ARCHITECTURE.md`, `TECHNICAL_DECISIONS.md`, `TEST_PLAN.md`, `TASKS.md`.

---

## 0. Struttura di progetto prevista

```
K:\AI\NL Audio Matrix\
  app\
    CMakeLists.txt
    CMakePresets.json
    external\JUCE\              # 9.0.4 pinnato (submodule/FetchContent)
    src\
      core\                     # device manager, graph, scheduler, ring, src, preset, diagnostics, ipc
      backends\asio\            # JUCE ASIO host
      backends\wasapi\          # WASAPI nativo (in/out/loopback)
      backends\virtual\         # trasporto driver endpoint
      backends\test\            # tono/file/clock offline
      engine\                   # NLMatrixEngine main
      ui\                       # NLMatrixUI main (JUCE)
      tools\                    # NLMatrixProbe, transport_probe
    tests\
  driver\
    NLMatrixAudio.sln
    NLMatrixAudio\              # fork SysVAD ridotto
    EndpointsCommon\
    Package\
    provisioning\
  docs\                         # documenti storici/integrativi
  tools\                        # script build/install/uninstall
  reports\                      # log e risultati
```

Nessun file dei repository di riferimento viene modificato; il fork SysVAD è una copia vendorizzata e tracciata.

---

## P0 — Analisi e documentazione (completato)

**Obiettivo:** chiudere l'audit read-only e produrre la pianificazione universale.  
**Deliverable:** `SOURCE_CODE_AUDIT.md`, `MASTER_PLAN.md`, `ARCHITECTURE.md`, `TECHNICAL_DECISIONS.md`, `IMPLEMENTATION_ROADMAP.md`, `TEST_PLAN.md`, `TASKS.md`, `STATUS.md`, `AGENTS.md`, `README.md`, aggiornamenti `docs/` e `MANIFEST.json`.  
**Criterio di uscita:** documenti coerenti, nessun limite artificiale, backlog P1 eseguibile.  
**Prova PC:** nessuna. **Driver:** nessuno.

---

## P1 — Acquisizione completa e convivenza con REAPER

**Obiettivo:** dimostrare che la Matrix può acquisire **tutti** i canali di ingresso effettivamente esposti da Apollo (attesi 10) e enumerare **tutte** le uscite ASIO, senza disturbare REAPER e UAD Console. Non è sufficiente enumerare: ogni canale deve essere acquisito e identificato.

**Dipendenze:** P0.  
**Può partire in parallelo a:** P2 (motore offline), P3 (driver su VM).  
**Driver richiesto:** no.  
**Prova PC reale:** sì, obbligatoria.

### P1.1 Setup progetto e toolchain
- Creare `app/` CMake con JUCE 9.0.4 pinnato, `JUCE_ASIO=1`, target `NLMatrixProbe`, `NLMatrixCore` minimale.
- Verificare VS 2022 BuildTools, CMake (da installare), SDK Windows; annotare versioni.
- Build x64 Release/Debug; nessun driver, nessuna installazione.

**File:** `app/CMakeLists.txt`, `app/CMakePresets.json`, `app/src/tools/NLMatrixProbe.cpp`, `app/src/backends/asio/*`.  
**DoD:** build riproducibile documentata; nessuna modifica di sistema.

### P1.2 Enumerazione completa device/canali
- Elencare tutti i driver ASIO (`AudioIODeviceType::scanForDevices`), per ognuno: nome, CLSID/descrizione se disponibile, input/output channel names, count, sample type per canale, sample rate supportati, buffer sizes, latenza dichiarata.
- Distinguere esplicitamente input e output; salvare JSON.
- Elencare anche dispositivi WASAPI (input/output/loopback) e altri device ASIO presenti, in sola lettura, per il futuro multi-device.
- Non assumere 10/6: riportare i conteggi reali e le differenze a 44.1/48/96/176.4/192 kHz.

**File:** `AsioHost`, `DeviceDescriptor`, `NLMatrixProbe --enumerate --json`.  
**DoD:** dump completo, riproducibile, con nomi/indici; output separati dagli input.

### P1.3 Acquisizione separata di tutti i canali
- Aprire il device Apollo con maschera **tutti gli input**; output non scritti (maschera output disabilitata di default).
- Callback realtime: copia planare, timestamp, contatori callback/frame/XRUN; nessuna allocazione/log nel callback.
- Registrare un WAV separato per ogni canale attivo, con nome che include indice assoluto e nome driver; livelli RMS/peak; durata configurabile; metadata JSON.
- Verificare che il numero di canali registrati corrisponda al numero di canali attivi e che l'indice `i` del buffer compatto corrisponda all'i-esimo bit della maschera attiva.

**File:** `AsioCaptureRecorder`, `NLMatrixProbe --capture-all --seconds N --out reports\P1`.  
**DoD:** N WAV separati, nessun crash, nessuna scrittura output; contatori coerenti.

### P1.4 Identificazione canale↔buffer con segnali di test
Procedura obbligatoria:
1. Definire una mappa attesa sorgente→canale Apollo (Mic 1, Mic 2, Monitor L/R, Virtual 1/2, AUX 1/2, altri reali).
2. Iniettare un segnale distinto per sorgente: tono a frequenza diversa, oppure rumore PRN con seed diverso; per gli ingressi fisici usare un generatore esterno o il loopback UAD Console; per i ritorni software usare REAPER/UAD Console con tracce di test separate.
3. Registrare tutti i canali con `--capture-all`.
4. Analizzare ogni WAV: presenza, frequenza dominante, livello, correlazione con il segnale atteso; rilevare bleed/crosstalk.
5. Compilare `channel_map.json`: `{driverName, absoluteChannel, compactIndex, expectedSource, measuredSource, pass/fail, note}`.
6. Ripetere almeno una volta a 48 kHz e una a 44.1 kHz (se supportato) e, se possibile, a 96 kHz; documentare i canali che scompaiono ad alta frequenza.

**Criterio PASS:** ogni canale atteso è presente nel canale giusto; nessun canale scambiato; bleed sotto la soglia concordata; mappa riproducibile su due run.  
**Criterio FAIL:** canale assente, scambiato, silenzioso o con segnale di un'altra sorgente → investigare e documentare; G1 non passa.

**File:** `NLMatrixProbe --verify-map channel_map.json`, `reports/P1/REPORT.md`.  
**DoD:** report per-canale PASS/FAIL con misure e file WAV.

### P1.5 Convivenza con REAPER e Console
Matrice di test:
- REAPER chiuso → apertura Matrix → cattura → chiusura.
- REAPER aperto e armato su Mic 1 → apertura Matrix (dopo e prima) → cattura → chiusura.
- REAPER aperto + OBS-ASIO attivo (baseline utente) → apertura Matrix; verificare se il driver consente una terza apertura.
- Buffer 128/256/512, rate 44.1/48/96; cambio rate/buffer con device aperto.
- Verificare che il monitor UAD Console non cambi e che REAPER non perda il driver.
- Raccogliere errori ASIO, callback counters, XRUN, latenza dichiarata.

**File:** `reports/P1/COEXISTENCE.md`, log.  
**DoD:** esito per ogni combinazione; failure mode documentato; nessuna modifica a REAPER/Console.

### P1.6 Uscite ASIO: enumerazione e test non invasivo
- Enumerare tutti gli output (nomi, indici, count, sample type) e salvarli.
- Testare l'apertura con maschera output **senza scrivere** (buffer azzerati) in una finestra controllata, con REAPER/Console aperti; misurare se il driver/Console/REAPER subiscono disturbi.
- Se l'apertura output disturba: documentare e mantenere input-only di default; nessuna forzatura.
- Non implementare ancora rotte output; P2/P5 le abiliteranno con conferma.

**File:** `NLMatrixProbe --outputs --dry-open`, `reports/P1/OUTPUTS.md`.  
**DoD:** elenco completo; esito dry-open; policy confermata.

### P1.7 Altri dispositivi (read-only)
- Elencare ASIO e WASAPI disponibili; capability; eventuali conflitti noti.
- Non aprire device diversi da Apollo in P1, salvo TestBackend.

**DoD:** inventario per P5.

### P1.8 Report e gate G1
- `reports/P1/REPORT.md`: ambiente, versioni, comandi, misure, PASS/FAIL, limiti.
- Aggiornare `STATUS.md` con prova PC sì/no.
- **G1:** tutti i canali disponibili acquisiti e mappati; REAPER/Console invariati; uscite enumerate; failure mode chiari. Solo dopo G1 si può considerare P2 sul device reale.

**Rischi P1:** driver UAD non multi-client; output aperti che disturbano; canali software non alimentabili con segnali distinti; sample rate diversi. **Mitigazione:** input-only, ordini di apertura, test con REAPER, documentare.

---

## P1.5 — Vertical slice standalone (diagnostica)

**Obiettivo:** ottenere una prima Matrix standalone che acquisisce i canali Apollo, applica una matrice semplice e raggiunge un endpoint Windows selezionabile da un'app terza, **senza OBS**. Usa VB-CABLE solo come banco di prova diagnostico; il driver proprietario lo sostituirà in P3.

**Dipendenze:** P1-01…P1-05; P1.5 può procedere in parallelo a P1-06/P1-07.  
**Driver:** no. **Prova PC:** sì (Apollo + REAPER + app target).  
**Perché:** valida in fretta il percorso end-to-end e la mappa canali, prima di investire nel driver e nel grafo completo.

### Attività
1. `NLMatrixLive` (JUCE console o GUI minima): un device ASIO input + un device WASAPI output, due `AudioIODevice` indipendenti, ring SPSC tra callback.
2. Configurazione runtime: canali scoperti dal driver; route `{sourceChannel → bus L/R, gain}`; numero di bus/canali non hardcoded; default prudente (nessuna somma Mic+AUX).
3. Matrice nel callback ASIO; output WASAPI verso l'endpoint scelto (diagnostica); zero output ASIO.
4. Meter/peak, contatori underrun/overrun, panic; log fuori callback.
5. Test end-to-end: Apollo input → mix → CABLE Input → CABLE Output → Source-Connect/Zoom; REAPER/Console invariati.
6. Report con misure, latenza percepita, drop, limiti; decisione su cosa tenere nella versione definitiva.

**Gate G1.5:** mix stabile verso endpoint diagnostico, canali mappati, REAPER/Console invariati, app terza riceve audio; documentato che VB-CABLE è solo diagnostica.

---

## P2 — Motore di routing e mixaggio universale

**Obiettivo:** grafo dinamico N×M, bus configurabili/multicanale, routing molti-a-molti e diretto, SRC/clock skeleton, test offline completi. Nessun endpoint virtuale ancora.

**Dipendenze:** P0; P1 consigliato per i dati reali, ma il motore può essere sviluppato con TestBackend/WAV.  
**Driver:** no. **Prova PC:** parziale (TestBackend/offline), poi con Apollo.

### Attività
1. `RingBuffer` SPSC con atomici, capacità configurabile, contatori, test di wrap/overrun.
2. `GraphScheduler` dedicato: timer alta risoluzione, MMCSS, scadenze assolute, late tick, modalità Low/Balanced/Safe; test di jitter.
3. `GraphEngine`: nodi/archi, bus dinamici 1..N canali, routing diretto, somma, gain/pan/mute/solo/polarità, rampe, snapshot RCU.
4. `AdaptiveResampler` + controller fill (capacità vs banda operativa, ±200 ppm, nessun 50%).
5. `DeviceManager` skeleton con backend Test e ASIO; apertura/chiusura fuori dal thread audio.
6. `PresetStore` v1 (schema, validazione, backup, migrazione).
7. Test unitari e integrazione offline: WAV multi-canale → grafo → WAV; null test; mix-minus; anti-feedback; 8h simulata; stress con molti bus/canali.
8. Integrazione con Apollo (dopo G1): tutti i canali instradabili; nessun output scritto di default.

**File:** `app/src/core/{ring,graph,scheduler,resampler,preset,device_manager}`, `app/tests/`.  
**Gate G2:** routing verificato con tutti i canali; null test; 8h senza drift/leak; nessun limite fisso di bus/canali; UI minima di test opzionale.  
**Prova PC:** sì per la parte Apollo; offline per il resto.

---

## P3 — Primo endpoint virtuale bidirezionale

**Obiettivo:** un `NL Virtual Mic` e un `NL Virtual Speaker` funzionanti su VM, con PCM iniettato/catturato dal motore, visibili in un'app terza. Nessun test su PC di lavoro.

**Dipendenze:** P0; in parallelo a P1/P2. **Driver:** sì, su VM. **Prova PC:** no (VM).

### Attività
1. Vendor upstream `Windows-driver-samples` pinnato; fix build (WIL, INX, LICENSE); fork SysVAD ridotto a 1 capture + 1 render.
2. Control device KMDF + IOCTL versionati; trasporto non-paged + MDL mapping; heartbeat; cleanup.
3. Inserimento in `WriteBytes`/`ReadBytes`; reset in `SetState`; contatori/underrun.
4. INF: nomi `NL Virtual Mic 1` / `NL Virtual Speaker 1`; formato 48k/2ch/float; componentized base; niente APO.
5. `transport_probe` e integrazione engine: iniezione tono, cattura playback; engine kill → silenzio.
6. Test su VM: install test-signed, Device Manager, Windows Sound, app terza, uninstall, Driver Verifier.
7. Documentare il provisioning (massimo, config, restart).

**Gate G3:** endpoint visibile e funzionante in app terza su VM; PCM corretto; silenzio su engine kill; install/uninstall puliti.  
**Rischi:** firma/test signing; WaveRT timing; mappatura MDL; PnP. **Mitigazione:** VM snapshot, Verifier, rollback.

---

## P4 — Più endpoint virtuali e mix-minus

**Obiettivo:** set 4+2 (preset) e configurabile; segnali indipendenti; mix-minus; app target.

**Dipendenze:** P3; P2 per il grafo. **Driver:** sì, VM. **Prova PC:** VM + app target; PC di lavoro solo dopo approvazione.

### Attività
1. Estendere tabelle/INF a 4 capture + 2 render (e massimo di build 8+8 da validare); provisioning configurabile; nomi stabili.
2. Engine: discovery dinamica del set; assegnazione bus→endpoint; profili client; mix-minus; anti-feedback.
3. Test: 4 toni distinti → 4 mic; 2 app → 2 speaker → rotte; mix-minus; nessun cross-talk; engine/UI restart.
4. Test app: Zoom/Source-Connect/SessionLinkPRO/browser su VM; documentare limiti.
5. Endpoint provisioning: aggiunta/rimozione con restart; profilo preservato.

**Gate G4:** 4+2 simultanei, indipendenti, mix-minus corretto, app target, uninstall.  
**Rischi:** PnP/INF, formati app, latenza. **Mitigazione:** test matrix, fallback 1+1.

---

## P5 — Multi-scheda ASIO/WASAPI, clock e drift

**Obiettivo:** più dispositivi contemporaneamente, clock indipendenti, SRC adattivo, hotplug, failover.

**Dipendenze:** P2 + P3. **Driver:** sì per endpoint; **Prova PC:** sì (hardware multi-device).

### Attività
1. `WasapiBackend` nativo: input, output, loopback; notifiche; formati; SRC.
2. Multi-istanza ASIO; apertura/chiusura indipendente; conflitti; stato.
3. GraphScheduler multi-device: ring per device, controller fill, modalità latenza, measure jitter.
4. Hotplug: ASIO/WASAPI/PnP; reconnect; profili; mute/ramp.
5. Failover GraphScheduler (late tick/watchdog), cambio graph rate, device perso.
6. Test: Apollo + seconda ASIO + WASAPI input + loopback + virtuali; 8h; rate diversi; unplug/replug; sleep/resume (VM/PC test).
7. Documentare limiti reali (JUCE 16, driver ASIO, WASAPI).

**Gate G5:** multi-device stabile, clock indipendenti, drift controllato, hotplug, nessuna regressione REAPER/Console.  
**Rischi:** driver ASIO non coesistenti; jitter; CPU; loopback. **Mitigazione:** gate per device, modalità conservative, log.

---

## P6 — GUI completa, patchbay, preset, gestione dispositivi

**Obiettivo:** l'utente costruisce e controlla tutto senza file di configurazione.

**Dipendenze:** P4 + P5. **Driver:** sì. **Prova PC:** sì.

### Attività
1. Matrice dinamica (righe/colonne dal grafo), patchbay a nodi, bus strips, meter.
2. Pannello dispositivi: aggiunta/rimozione, stato, canali, rate/buffer, profili, conflitti.
3. Client/mix-minus UI, anti-feedback, conferme, panic, mute.
4. Preset/profili: salvataggio, backup, migrazione, restore al reconnect.
5. Diagnostica: fill, XRUN, late tick, latenza, log export; tray; close-to-tray.
6. Accessibilità/tastiera, localizzazione IT/EN, tema.
7. Test UI automatizzati dove possibile; test manuale app target.

**Gate G6:** routing completo via GUI, preset persistenti, device management, diagnosi, panic.  
**Rischi:** complessità UI, performance meter. **Mitigazione:** snapshot 30Hz, virtualizzazione griglia.

---

## P7 — Ottimizzazione, affidabilità, packaging, integrazioni

**Obiettivo:** prodotto distribuibile.

**Dipendenze:** P6. **Prova PC:** sì, con approvazione.

### Attività
1. Stress 8–24h, sleep/resume, crash engine/UI, driver disable/enable, uninstall.
2. Ottimizzazione CPU/latenza; modalità clock-locked PLL opzionale; tuning SRC.
3. Installer app + driver package; attestazione/WHQL; certificato EV; rollback; uninstall.
4. Sicurezza: ACL, audit, limiti, fuzz IOCTL, Verifier.
5. ReaRoute/ASIO virtuale opzionale; altri backend.
6. Documentazione utente, troubleshooting, privacy log.
7. Accettazione finale su PC di lavoro con backup/ripristino.

**Gate G7:** installer firmato su VM pulita; accettazione PC; rollback verificato.  
**Rischi:** firma esterna, Windows Update, hardware. **Mitigazione:** lead time, test matrix, restore point.

---

## Dipendenze e parallelizzazione

```
P0 ── P1 ── P1.5 ───────┐
   ├─ P2 ───────────────┼─ P5 ── P6 ── P7
   └─ P3 ── P4 ─────────┘
```

- P1 (hardware ASIO) e P2 (motore offline) possono procedere in parallelo.
- P3 (driver su VM) può partire dopo P0, in parallelo a P1/P2.
- P4 richiede P3 + P2.
- P5 richiede P2 + P3; P6 richiede P4 + P5.
- P7 richiede P6 e il lead time di firma.

## Stime indicative

| Fase | Stima (persona-settimane) | Note |
|---|---:|---|
| P1 | 1–2 | dipende dal driver UAD e dalla disponibilità segnali |
| P2 | 2–4 | motore, test, performance |
| P3 | 2–4 | driver POC su VM, firma test |
| P4 | 1–2 | endpoint multipli/mix-minus |
| P5 | 2–4 | multi-device, clock, hotplug |
| P6 | 3–5 | GUI completa |
| P7 | 2–4 + lead time firma | hardening/packaging |

Le stime sono indicative e saranno riviste dopo G1/G3; non sono impegni.

## Regole di fase

- Nessuna fase avanza senza gate e `STATUS.md` aggiornato.
- Nessun driver sul PC di lavoro senza approvazione e backup.
- Nessuna modifica ai repository di riferimento.
- Nessun output ASIO scritto senza rotta esplicita e conferma.
- Nessuna promessa di latenza/multi-client senza misura.
