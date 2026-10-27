# MASTER PLAN — NL Audio Matrix v2

**Versione:** 3.0 — matrice audio universale multi-device  
**Data:** 24 ottobre 2026  
**Stato:** P0 — pianificazione completa; nessun codice di produzione, nessun driver installato.  
**Documenti canonici:** `SOURCE_CODE_AUDIT.md`, `ARCHITECTURE.md`, `TECHNICAL_DECISIONS.md`, `IMPLEMENTATION_ROADMAP.md`, `TEST_PLAN.md`, `TASKS.md`, `STATUS.md`.

---

## 1. Missione

Costruire una **matrice audio universale per Windows** che unisca ciò che oggi è diviso tra UAD I/O Matrix, Loopback, un mixer multicanale e un patchbay:

- acquisizione di **tutti i canali** di più interfacce ASIO e di dispositivi WASAPI, con enumerazione distinta di ingressi e uscite;
- routing **N×M dinamico**, bus configurabili e multicanale, molti-a-molti, anche diretto dispositivo→dispositivo;
- **più endpoint Windows virtuali** di capture e render, selezionabili dalle normali applicazioni;
- **mix-minus** indipendenti, anti-feedback, gain/pan/mute/solo/polarità, metering;
- clock indipendenti, sample rate diversi, ricampionamento adattivo, hotplug e recovery;
- preset e profili per dispositivo, con identità persistenti;
- GUI con matrice, patchbay, gestione dispositivi, diagnostica.

**Obiettivo pratico immediato:** eliminare OBS e VB-CABLE dalle sessioni remote, mantenendo REAPER e il monitoraggio UAD Console esattamente come oggi.

**Apollo Solo USB è il primo dispositivo di test**, non il perimetro del prodotto. Nessun documento o modulo deve assumere 2, 8 o 10 canali, 4 bus, 4+2 endpoint o un solo device ASIO.

---

## 2. Utenti e casi d'uso

### Utente primario
Doppiatore/tecnico con Apollo Solo USB su Windows 11, REAPER per la presa, UAD Console per il monitoraggio diretto, Source-Connect/SessionLinkPRO/Zoom/browser per il remoto.

### Casi d'uso
1. **Mandata remota:** Mic 1 e/o AUX 1/2 (o qualsiasi altro canale Apollo) → bus → `NL Virtual Mic 1` → Source-Connect.
2. **Ritorno:** Source-Connect → Apollo direttamente (default), oppure → `NL Virtual Speaker 1` → Matrix → bus/altro device.
3. **Mix-minus:** ogni client riceve il mix degli altri, non il proprio ritorno.
4. **Multi-cliente:** più bus e più microfoni virtuali indipendenti.
5. **Multi-dispositivo:** Apollo + seconda interfaccia ASIO + WASAPI input/output + endpoint virtuali contemporaneamente, con clock indipendenti.
6. **Routing diretto:** ASIO in→ASIO out, virtual speaker→ASIO out, WASAPI in→virtual mic, virtual speaker→virtual mic con protezione.
7. **Uso senza ASIO:** il grafo funziona con soli device WASAPI/virtuali o in modalità test.

---

## 3. Scope e non-goal

### In scope
- DeviceManager multi-backend (ASIO, WASAPI, virtuale, ReaRoute opzionale, test).
- Grafo dinamico con bus configurabili e multicanale, routing molti-a-molti e diretto.
- Mix-minus e anti-feedback.
- Endpoint virtuali capture/render multipli, provisioning configurabile.
- Clock indipendenti, SRC adattivo, ring di disaccoppiamento, latenza selezionabile.
- GUI matrix/patchbay, preset, profili device, diagnostica.
- Packaging, firma, installer/uninstall, rollback.
- Documentazione e test.

### Non-goal
- Sostituire REAPER, UAD Console o il driver UAD.
- Monitoraggio software obbligatorio o latenza zero promessa.
- Creazione/rimozione arbitraria di endpoint Windows a runtime.
- Scrittura forzata su output ASIO già usati da altre app.
- Reverse engineering di UAD/VB-CABLE.
- Streaming di rete, VST host, emulazione Console/DSP.
- Supporto multi-processo engine con lo stesso trasporto nella v1.

---

## 4. Architettura in dieci righe

1. `NLMatrixEngine.exe` headless possiede DeviceManager, GraphScheduler, GraphEngine e trasporto.
2. `NLMatrixUI.exe` è solo GUI/IPC.
3. Backend indipendenti: ASIO (JUCE 9), WASAPI nativo (in/out/loopback), endpoint virtuali (driver), ReaRoute opzionale, Test.
4. Un thread grafo dedicato (timer alta risoluzione + MMCSS) processa a graph rate; i device sono asincroni con ring e SRC adattivo.
5. Ring: capacità di sicurezza vs riempimento operativo adattivo (Low/Balanced/Safe), nessun target fisso 50%.
6. Grafo: nodi device/bus/virtuali, archi molti-a-molti, bus dinamici multicanale, routing diretto.
7. Mix-minus per profilo client; anti-feedback; avviso Mic+AUX.
8. Endpoint virtuali: driver SysVAD fork, massimo di build + provisioning + restart; il motore legge il set.
9. Driver: WaveRT + trasporto non-paged/MDL/IOCTL; silenzio su engine assente.
10. Output policy: enumerare sempre, scrivere solo su scelta esplicita; Apollo output off di default; ritorno diretto ad Apollo supportato.

Dettaglio: `ARCHITECTURE.md`; motivazioni: `TECHNICAL_DECISIONS.md`.

---

## 5. Fasi, gate e risultati

| Fase | Nome | Risultato verificabile | Gate |
|---|---|---|---|
| **P0** | Analisi e pianificazione | Audit + documenti aggiornati; nessun codice | Revisione documenti |
| **P1** | Acquisizione completa e convivenza | Tutti gli ingressi Apollo acquisiti **separatamente** con segnali di test e mappa canale↔buffer verificata; tutte le uscite enumerate; REAPER/Console intatti; failure mode documentati | **G1** |
| **P1.5** | Vertical slice standalone | Probe + matrice runtime + output WASAPI diagnostico verso app terza; VB-CABLE solo banco di prova | **G1.5** |
| **P2** | Routing e mix universale | Grafo dinamico N×M, bus multicanale, rotte dirette, SRC/clock skeleton, test offline; tutti i canali instradabili | **G2** |
| **P3** | Primo endpoint virtuale bidirezionale | 1 capture + 1 render visibili a un'app terza; PCM iniettato/catturato; engine kill → silenzio; su VM | **G3** |
| **P4** | Più endpoint e mix-minus | 4+2 (preset) e set configurabile; segnali indipendenti; mix-minus; app target | **G4** |
| **P5** | Multi-scheda e clock | Apollo + seconda ASIO + WASAPI + virtuali; clock indipendenti; drift; hotplug; failover | **G5** |
| **P6** | GUI completa | Matrice, patchbay, gestione device, preset/profili, diagnostica, panic | **G6** |
| **P7** | Affidabilità e distribuzione | Stress, sleep/resume, installer firmato, rollback, ReaRoute opzionale | **G7** |

Dettaglio task, dipendenze e criteri: `IMPLEMENTATION_ROADMAP.md`; test: `TEST_PLAN.md`; backlog: `TASKS.md`.

---

## 6. Metriche di successo

### Prodotto
- Tutti i canali disponibili di ogni device sono enumerati e instradabili individualmente.
- Almeno 4 capture + 2 render virtuali nel preset iniziale; set espandibile entro il massimo del driver.
- Routing N×M con bus configurabili; nessun limite fisso nel motore.
- Multi-device con clock indipendenti e drift controllato.
- REAPER/Console invariati; nessuna scrittura output non richiesta.
- Mix-minus verificato; nessun feedback.
- Engine kill → silenzio; UI kill → audio continua.
- Uninstall pulito.

### Qualità
- 0 crash/BSOD nei test VM e PC approvati.
- Underrun/overrun entro soglie concordate; late tick misurati.
- CPU stabile con configurazioni target.
- Nessun dato personale nei log di default.

### Accuratezza
- Ogni dichiarazione tecnica classificata VERIFICATO/INFERITO/DA TESTARE.
- Nessuna promessa di latenza o multi-client senza misura.

---

## 7. Rischi principali e mitigazioni

| Rischio | Impatto | Mitigazione |
|---|---|---|
| ASIO non apribile con REAPER | Blocca P1 | Test P1 in tutti gli ordini; fallback input-only/WASAPI/ReaRoute; decisione documentata |
| Coesistenza multi-driver ASIO | Blocca P5 | Test per-driver; stato `Conflict`; nessuna forzatura |
| Clock indipendenti/drift | Artefatti | SRC adattivo, ring, controller fill, stress 8h |
| Jitter del thread grafo | Click/latenza | Timer alta risoluzione, MMCSU, ring, modalità latenza, misure |
| Output Apollo disturbano REAPER | Regressione | Default off, conferma, P1 test, panic |
| Provisioning endpoint/PnP | Endpoint mancanti | Massimo di build, config, restart, test VM |
| Firma driver/HVCI | Driver non caricabile | Test signing VM, attestazione/WHQL, certificato EV |
| Crash kernel | BSoD | VM, Driver Verifier, rollback, nessun test su PC di lavoro |
| Licenze JUCE/ASIO | Blocco release | Decisione prima della distribuzione; nessun codice GPL copiato |
| Scope troppo ampio | Ritardi | P1–P7 con gate; P3/P5 in parallelo; nessun hardcode |

---

## 8. Governance

- **Nessun driver installato sul PC di lavoro** senza approvazione esplicita; P3–P5 su VM/secondo PC.
- **Nessun reverse engineering** di UAD/VB-CABLE.
- **Nessuna modifica** ai repository di riferimento.
- **Nessuna modifica** a REAPER/UAD Console/Windows default come effetto collaterale.
- **Preset conservativi:** nessuna rotta verso il client attiva per default; nessuna somma implicita Mic+AUX.
- **Documenti canonici** nella root; i documenti storici in `docs/` sono integrativi e con banner.
- **Ogni fase aggiorna `STATUS.md`** con file reali, build, test e prova PC sì/no.
- **Change control:** modifiche a scope/architettura passano da `TECHNICAL_DECISIONS.md` e dal piano.

---

## 9. Deliverable P0

- `SOURCE_CODE_AUDIT.md` — evidenze e riferimenti.
- `ARCHITECTURE.md` — componenti, flussi, interfacce.
- `TECHNICAL_DECISIONS.md` — ADR.
- `IMPLEMENTATION_ROADMAP.md` — P1–P7.
- `TEST_PLAN.md` — test e accettazione.
- `TASKS.md` — backlog, P1 eseguibile.
- `STATUS.md` — stato reale.
- `AGENTS.md`, `README.md` — direttive e indice.
- Aggiornamenti a `docs/09,10,15,16,17`, banner storici, `MANIFEST.json`.

---

## 10. Assunzioni e decisioni aperte

**Assunzioni:** Apollo espone 10 ingressi e 6 uscite (P1 verifica); UAD ASIO consente la convivenza con REAPER (P1); WASAPI shared/loopback disponibili su Windows 11; driver con massimo di build e set 4+2; graph rate default 48 kHz; engine/GUI separati; disponibilità VM per driver.

**Decisioni utente da confermare prima della release:** modello di distribuzione/licenza; massimo endpoint del driver; graph rate predefinito; politica output Apollo; UI italiana/inglese; disponibilità hardware per test multi-device; eventuale ReaRoute.

---

## 11. Definizione di successo P0

La fase P0 è completa quando tutti i documenti del §9 esistono, sono coerenti, non contengono limiti artificiali e il backlog P1 è eseguibile senza ulteriori decisioni architetturali. **Nessun codice di produzione viene scritto in P0.**
