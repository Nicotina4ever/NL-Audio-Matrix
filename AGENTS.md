# AGENTS.md — NL Audio Matrix v2 (scope universale multi-device)

## Missione, non negoziabile
Una **matrice audio universale per Windows** ispirata a UAD I/O Matrix + Loopback + mixer/patchbay:
- acquisisce **tutti i canali** di più dispositivi ASIO e WASAPI, con ingressi e uscite enumerati e distinti;
- instrada **N×M dinamico**, bus configurabili e multicanale, molti-a-molti e diretto dispositivo→dispositivo;
- espone **più endpoint Windows virtuali** capture/render, selezionabili dalle normali applicazioni;
- offre gain, pan, mute, solo, polarità, metering, **mix-minus** e anti-feedback;
- gestisce clock indipendenti, sample rate diversi, SRC adattivo, hotplug e recovery;
- conserva preset/profili per dispositivo con identità persistenti.

**Apollo Solo USB è il primo dispositivo di test, non il limite del prodotto.** Nessun modulo o documento deve assumere 2, 8 o 10 canali, 4 bus, 4+2 endpoint o un solo device ASIO.

**NL = Nicola Lanci.** Mantenere `NL` maiuscolo in nomi prodotto, eseguibili, driver e documenti visibili (`NLMatrix...`, `NL Audio Matrix`). Gli identificatori C++ interni possono seguire le convenzioni del linguaggio.

**Obiettivo pratico immediato:** eliminare OBS e VB-CABLE dalle sessioni remote, preservando REAPER e il monitoraggio UAD Console esattamente come oggi.

## Vincoli di prodotto
- REAPER e UAD Console restano sovrani: nessuna sostituzione del driver UAD, nessun plugin obbligatorio, nessun cambio del routing Console, nessun monitoraggio software obbligatorio.
- Gli output ASIO già usati da altre app non vanno né sottratti né sovrascritti: enumerazione sempre, scrittura solo su rotta esplicita e conferma. Per Apollo output off di default.
- Il ritorno delle piattaforme direttamente verso Apollo resta un percorso supportato e preferito.
- Funzionamento anche senza ASIO attivo.
- Nessun limite artificiale di schede, canali, bus o endpoint nel routing engine; solo capacità reali dei backend e guardie di risorsa documentate.
- Driver virtuale proprietario, senza dipendenza finale da VB-CABLE/OBS.
- VB-CABLE, VB-Matrix, ReaRoute e Console Virtual I/O sono **percorsi diagnostici o opzionali**, non dipendenze di prodotto. Il codice deve usare endpoint e canali generici, mai nomi hardcoded di prodotti terzi.

## Organizzazione tecnica
A) `DeviceManager` + backend indipendenti: ASIO (JUCE 9), WASAPI nativo (in/out/loopback), endpoint virtuali, ReaRoute opzionale, Test.
B) `GraphScheduler` dedicato + `GraphEngine` N×M + ring SPSC + SRC adattivo; clock di grafo indipendente dai device.
C) `NLMatrixEngine.exe` headless + `NLMatrixUI.exe` JUCE; IPC versionata; l'audio sopravvive alla chiusura della GUI.
D) Driver `NLMatrixAudio.sys` (fork SysVAD MS-PL, WaveRT) + trasporto non-paged/MDL/IOCTL; endpoint provisionabili.
E) GUI: matrice, patchbay, gestione dispositivi, preset/profili, diagnostica, panic.

## Priorità di implementazione
1. **P1** — enumerazione completa di tutti i 10 ingressi Apollo e di tutte le uscite ASIO; **acquisizione reale e verifica canale↔buffer** con segnali di test distinti; convivenza con REAPER/Console.
2. **P2** — routing indipendente di tutti i canali e motore di mixaggio dinamico; test offline.
3. **P3** — primo endpoint Windows virtuale bidirezionale su VM; driver POC.
4. **P4** — più endpoint virtuali e mix-minus; set 4+2 come preset, provisioning configurabile.
5. **P5** — più schede ASIO/WASAPI contemporaneamente; clock, drift, hotplug, failover.
6. **P6** — GUI completa, patchbay, preset, gestione dispositivi, diagnostica.
7. **P7** — ottimizzazione, affidabilità, packaging, firma, integrazioni avanzate.

Le fasi possono essere riordinate solo rispettando le dipendenze e mantenendo l'espandibilità.

## Disciplina audio
- Thread del grafo dedicato, timer ad alta risoluzione, MMCSS; i device sono asincroni con ring e SRC.
- Nessuna allocazione, lock bloccante, I/O, log o syscall nel percorso audio.
- Ring SPSC con atomici e contatori; capacità di sicurezza distinta dal riempimento operativo; target adattivo (Low/Balanced/Safe), non 50% fisso.
- Snapshot RCU per il routing; rampe anti-click; panic sempre raggiungibile.
- Nessuna assunzione di clock condiviso; SRC adattivo per device; drift misurato.
- Canali ASIO 1-based vs indici buffer 0-based: mappa esplicita e verificata.
- Evitare la somma implicita Mic1 + AUX (comb filtering); mix-minus/anti-feedback.
- Meter fuori dal thread audio; GUI a 30 Hz.

## RE, licenze, proprietà terze
- OBS-ASIO è GPL-3.0: solo riferimento comportamentale, nessun codice copiato.
- JUCE 9.0.4 AGPLv3/commerciale; ASIO SDK GPLv3/Steinberg; driver SysVAD MS-PL; Micy/Sweetly base Microsoft MS-PL.
- Non ridistribuire DLL proprietarie UAD/VB-CABLE; non fare reverse engineering del driver UAD o VB-CABLE come percorso di default.
- Decisione di distribuzione/licenza prima della release.

## Sviluppo sul PC
- P0–P2: nessun driver; solo app/test; è permesso scrivere sorgenti e compilare in `app/`.
- P3–P5: driver solo su VM/secondo PC con test signing e snapshot; nessuna installazione sul PC di lavoro senza approvazione esplicita.
- Non modificare i repository di riferimento; non cambiare default Windows/REAPER/UAD Console.
- Backup/restore point prima di qualsiasi test driver sul PC di lavoro.
- Aggiornare `TASKS.md`, `DEVELOPMENT_STATE.md` e `reports/TEST_STATUS.md` con risultati reali.

## Risultati richiesti ogni fase
File creati; build ripetibile; criterio dimostrabile; prova PC sì/no; prossima parte realizzabile. Non fermarsi alla sola pianificazione se si può scrivere codice senza violare i vincoli.

## Documenti canonici
`README.md`, `TASKS.md`, `DEVELOPMENT_STATE.md`, `reports/TEST_STATUS.md`, `ARCHITECTURE.md`, `docs/ENGINE_V2.md`, `docs/08_TOOLCHAIN_BUILD.md`, `docs/11_RISKS_LICENSES.md`, `driver/sysvad-nl/README.md`, `tools/vm/README.md`. `docs/history/` contiene documenti superati (audit, piani, ADR, roadmap, vecchio STATUS) solo per consultazione storica: non sono autorità. In caso di conflitto prevalgono i documenti canonici elencati sopra.
