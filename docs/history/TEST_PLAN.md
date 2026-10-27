# TEST PLAN — NL Audio Matrix v2

**Versione:** 3.0 — scope universale multi-device  
**Data:** 24 ottobre 2026  
**Stato:** P0 — piano di test; nessun test eseguito salvo l'analisi statica read-only.  
**Riferimenti:** `ARCHITECTURE.md`, `IMPLEMENTATION_ROADMAP.md`, `TASKS.md`.

---

## 1. Obiettivi e principi

Il piano verifica che la Matrix:

1. acquisisca **realmente** tutti i canali disponibili, non solo li enumeri;
2. rispetti REAPER, UAD Console e gli output già usati da altre app;
3. instradi N×M con bus dinamici e multicanale, senza limiti artificiali;
4. gestisca più dispositivi con clock indipendenti, rate diversi, drift e hotplug;
5. esponga endpoint virtuali capture/render multipli e configurabili;
6. garantisca mix-minus e anti-feedback;
7. funzioni anche senza ASIO attivo;
8. sia stabile, sicura e disinstallabile.

**Regole:** ogni test ha ID, prerequisiti, procedura, misura, criterio PASS/FAIL e prova (file/log). Le misure non si inventano: se non misurabili, si dichiara `NON MISURATO`. I test distruttivi (driver) solo su VM/snapshot; nessun test driver sul PC di lavoro senza approvazione.

---

## 2. Ambienti

| Ambiente | Uso | Vincoli |
|---|---|---|
| **PC di lavoro (utente)** | ASIO Apollo, REAPER, UAD Console, app reali | Nessun driver sperimentale; nessuna modifica a REAPER/Console; backup |
| **VM Windows 11** | driver SysVAD fork, test signing, PnP, uninstall | Secure Boot off/test signing; snapshot; nessun hardware Apollo |
| **Secondo PC / VM audio** | multi-device, WASAPI, loopback, stress | Può avere una seconda interfaccia ASIO/WASAPI |
| **Offline/CI** | unit test, grafo, SRC, preset, WAV | Nessun device reale; TestBackend |

Versioni da annotare per ogni run: OS build, driver UAD, UAD Console, REAPER, JUCE, app target, scheda audio, buffer/rate, CPU.

---

## 3. Livelli di test

- **Unit:** ring, SRC, controller fill, grafo, snapshot, preset, mix-minus, anti-feedback.
- **Integration offline:** WAV multi-canale → grafo → WAV; null test; 8h simulate.
- **Integration hardware:** ASIO Apollo, WASAPI, multi-device.
- **Driver su VM:** install, endpoint, PCM, PnP, crash, uninstall, Verifier.
- **End-to-end:** app target (Zoom, Source-Connect, SessionLinkPRO, browser) + REAPER/Console.
- **Security:** ACL, IOCTL fuzz, mapping, heartbeat, log privacy.
- **Packaging:** installer, firma, upgrade, rollback, uninstall.

---

## 4. P1 — Acquisizione completa e mappatura canali

### P1-T01 — Enumerazione completa
**Procedura:** eseguire `NLMatrixProbe --enumerate --json` con Apollo collegato.
**Misura:** per ogni device ASIO: nome, input names/count, output names/count, sample type per canale, rate, buffer, latenza.
**PASS:** dump completo, input e output separati, nessuna assunzione di 10/6; differenze a 44.1/48/96/176.4/192 documentate.
**Prova:** `reports/P1/enumerate.json`.

### P1-T02 — Acquisizione separata di tutti gli ingressi
**Procedura:** aprire tutti gli input; `NLMatrixProbe --capture-all --seconds 30 --out reports/P1/wav`; output non scritti.
**Misura:** N WAV, N = canali attivi; contatori callback/frame; RMS/peak per canale; XRUN.
**PASS:** N WAV con dati; nessun crash; nessuna scrittura output; indici coerenti con la maschera.

### P1-T03 — Identificazione canale↔buffer (test obbligatorio)
**Procedura:**
1. Definire la mappa attesa (Mic 1, Mic 2, Monitor L/R, Virtual 1/2, AUX 1/2, altri).
2. Iniettare un segnale distinto per sorgente: tono a frequenza diversa o PRN con seed diverso. Per ingressi fisici: generatore/loopback Console; per ritorni software: tracce di test REAPER/UAD Console separate.
3. Registrare tutti i canali con `--capture-all`.
4. Analizzare ogni WAV: presenza, frequenza dominante/PRN correlation, livello, crosstalk/bleed.
5. Compilare `channel_map.json` con indice assoluto, indice compatto, sorgente attesa/misurata, PASS/FAIL.
6. Ripetere a 48 kHz e 44.1 kHz; se possibile 96 kHz.

**PASS:** ogni sorgente attesa compare nel canale corretto; nessuno scambio; bleed sotto soglia concordata (es. −40 dB, da confermare); mappa riproducibile su due run.
**FAIL:** canale assente/silenziato/scambiato/segnale altrui → indagare, documentare, G1 sospeso.

### P1-T04 — Convivenza REAPER (matrice)
**Combinazioni:** REAPER chiuso; REAPER aperto prima; REAPER aperto dopo; REAPER + OBS-ASIO attivo; buffer 128/256/512; rate 44.1/48/96; cambio rate/buffer.
**Misure:** apertura Matrix, callback counters, errori ASIO, XRUN, stato REAPER, monitor Console (ascolto), latenza dichiarata.
**PASS:** Matrix acquisisce; REAPER mantiene audio/driver; Console invariata; nessun crash. Se una combinazione fallisce, documentare failure mode e decidere input-only/fallback.
**Prova:** `reports/P1/coexistence.md` + log.

### P1-T05 — Uscite ASIO: enumerazione e dry-open
**Procedura:** enumerare tutti gli output; aprire la maschera output senza scrivere (buffer azzerati) per 10 s con REAPER/Console attivi; osservare.
**PASS:** output enumerati; nessun disturbo a REAPER/Console o disturbo documentato → policy input-only di default.
**Prova:** `reports/P1/outputs.md`.

### P1-T06 — Altri device (read-only)
**Procedura:** enumerare ASIO/WASAPI disponibili, capability, loopback; nessuna apertura salvo TestBackend.
**PASS:** inventario completo per P5.

### P1-T07 — Report e gate G1
**PASS G1:** T01–T06 con esito positivo o failure mode accettati e documentati; `channel_map.json` completo; `STATUS.md` aggiornato con prova PC reale.

---

## 5. P2 — Grafo, mixer, SRC, scheduler

### P2-T01 — Ring SPSC
Wrap, capacità, overrun/underrun, memory order, single producer/consumer, stress 10^8 frame.
**PASS:** nessuna corruzione, contatori corretti, nessuna allocazione.

### P2-T02 — Snapshot RCU
Pubblicazione concorrente, lettura per blocco, rampe, nessun lock.
**PASS:** nessun dato parziale, nessun crash, cambi udibili senza click.

### P2-T03 — Grafo dinamico
Configurazioni con 1, 4, 16, 64 bus; canali mono/stereo/8ch; molti-a-molti; rotte dirette; cicli rifiutati.
**PASS:** output atteso, nessun limite fisso, errore chiaro sui cicli.

### P2-T04 — Null test e mix
Sinusoidi/rumore; somma con gain; pan; polarità; mute/solo; rilevazione clipping.
**PASS:** errore numerico entro tolleranza; nessun click; meter coerenti.

### P2-T05 — SRC e controller fill
Rate 44.1↔48↔96; drift simulato ±200 ppm; modalità Low/Balanced/Safe; capacità vs banda.
**PASS:** fill nella banda; underrun/overrun contati; THD/aliasing entro soglia; nessun target fisso 50% assunto.

### P2-T06 — Scheduler e jitter
Timer alta risoluzione; late tick; watchdog; 8h.
**PASS:** jitter misurato e documentato; nessun accumulo; recovery.

### P2-T07 — Mix-minus/anti-feedback
Profili client; return multipli; rotte vietate; cicli.
**PASS:** il client non riceve il proprio return; blocco/avviso corretto.

### P2-T08 — Stress offline
8h simulate, molte configurazioni, memoria stabile.
**PASS:** nessun leak/crash; contatori.

---

## 6. P3/P4 — Driver ed endpoint virtuali (VM)

### P3-T01 — Build e install driver POC
WDK, test signing su VM snapshot; `pnputil`/devcon; Device Manager.
**PASS:** driver caricato, 1 mic + 1 speaker visibili.

### P3-T02 — PCM capture
Tono iniettato dall'engine → `NL Virtual Mic 1` → app terza (registratore) → WAV.
**PASS:** frequenza/livello corretti; nessun rumore; latenza misurata.

### P3-T03 — PCM render
App terza suona su `NL Virtual Speaker 1` → engine cattura → WAV.
**PASS:** audio corretto; nessun drop.

### P3-T04 — Engine kill/heartbeat
Kill engine; endpoint deve andare in silenzio; restart engine; ripristino.
**PASS:** silenzio, nessun rumore; recovery.

### P3-T05 — PnP e uninstall
Disable/enable, unplug virtuale, uninstall; Driver Verifier.
**PASS:** nessun BSoD; uninstall pulito; audio di sistema sano.

### P4-T01 — 4+2 simultanei
4 toni distinti → 4 mic; 2 app → 2 speaker; rotte; nessun cross-talk.
**PASS:** segnali indipendenti; mix-minus; meter.

### P4-T02 — Provisioning
Set 1+1, 4+2, massimo; restart; discovery engine; profilo preservato.
**PASS:** il motore legge il set; nessun hardcode; endpoint mancanti gestiti.

### P4-T03 — App target
Zoom/Source-Connect/SessionLinkPRO/browser su VM; selezione mic/speaker.
**PASS:** riconoscimento e audio; limiti documentati.

### P4-T04 — Mix-minus end-to-end
Client A send/return distinti; verifica assenza del proprio return; client B.
**PASS:** nessun eco; return non nel send.

---

## 7. P5 — Multi-device, clock, hotplug

### P5-T01 — Due device ASIO
Apollo + seconda interfaccia (se disponibile) contemporaneamente; apertura indipendente; conflitti.
**PASS:** entrambi attivi o failure mode documentato; nessun crash.

### P5-T02 — WASAPI input/output/loopback
Mic WASAPI → bus → virtual mic; app → WASAPI loopback → bus; WASAPI render.
**PASS:** audio corretto; loopback senza disturbare l'app; SRC.

### P5-T03 — Clock indipendenti e drift
Device a 44.1 e 48; 8h; fill/ppm; underrun/overrun.
**PASS:** drift compensato; nessun accumulo; soglie concordate.

### P5-T04 — Hotplug e recovery
Unplug/replug ASIO/USB/WASAPI; disable/enable; cambio default; sleep/resume.
**PASS:** mute/rampa, riconnessione, profilo; nessun crash.

### P5-T05 — GraphScheduler failover
Late tick simulato; device perso; watchdog.
**PASS:** mute, recovery, nessun click.

### P5-T06 — Test senza ASIO
Solo WASAPI/virtuali/TestBackend; grafo attivo.
**PASS:** funziona; silenzio esplicito se nessun device.

### P5-T07 — Output non invasivi
Rotte ASIO output con REAPER/Console; conferma; nessuna scrittura non richiesta.
**PASS:** REAPER/Console invariati o conflitto documentato; default off.

---

## 8. P6 — GUI, preset, diagnostica

- **P6-T01** matrice/patchbay: creazione bus, rotte, meter, pan/gain/mute/solo/polarità.
- **P6-T02** preset: salvataggio, backup, migrazione, restore; cambio a caldo senza click.
- **P6-T03** device management: aggiunta/rimozione, stato, conflitti, profili.
- **P6-T04** mix-minus UI e anti-feedback: conferme, blocchi, avvisi.
- **P6-T05** diagnostica: fill, XRUN, late tick, latenza, export log; panic.
- **P6-T06** accessibilità/tastiera/localizzazione; tray/close-to-tray.
**PASS:** tutte le funzioni senza file di config; nessuna regressione audio.

---

## 9. P7 — Affidabilità, sicurezza, packaging

- **P7-T01** stress 8–24h multi-device.
- **P7-T02** sleep/resume, crash engine/UI, driver disable/enable, uninstall.
- **P7-T03** security: ACL, IOCTL fuzz, mapping lifetime, heartbeat, log privacy.
- **P7-T04** installer/firma/attestazione; upgrade; rollback; uninstall su VM pulita.
- **P7-T05** accettazione PC di lavoro con backup/restore point e approvazione.
**PASS:** nessun BSoD; rollback; installer firmato; accettazione.

---

## 10. Matrice multi-device

| Configurazione | Device | Rate | Buffer | Test |
|---|---|---|---|---|
| C1 | Apollo | 48k | 128/256/512 | P1/P5 |
| C2 | Apollo + REAPER | 48k | 256 | P1 |
| C3 | Apollo + OBS-ASIO | 48k | 256 | P1 |
| C4 | Apollo + seconda ASIO | 44.1/48 | 256 | P5 |
| C5 | Apollo + WASAPI input | 48k | n/a | P5 |
| C6 | Apollo + WASAPI loopback | 48k | n/a | P5 |
| C7 | Solo WASAPI | 48k | n/a | P5 |
| C8 | Solo virtuali | 48k | 10 ms | P3/P4 |
| C9 | Tutti insieme | misto | misto | P5/P7 |

---

## 11. Criteri di stop immediato

- Feedback/eco forte, livello improvviso, clipping persistente.
- REAPER perde il driver o UAD Console cambia.
- Windows cambia device predefinito o audio di sistema si interrompe.
- BSoD, crash kernel, driver non disinstallabile.
- Qualsiasi richiesta di installare driver non approvato sul PC di lavoro.

---

## 12. Evidenze e report

Ogni run produce:
- `reports/<fase>/REPORT.md`: ambiente/versioni, procedura, misure, PASS/FAIL, limiti, rollback.
- File di misura: JSON, WAV, log; nessun dato personale.
- `STATUS.md` aggiornato con prova PC sì/no.
- `channel_map.json` per P1; `config`/preset per P4–P6.

**Accettazione finale:** tutti i gate G1–G7 con evidenze; nessun test critico `NON MISURATO`; limiti residui documentati.
