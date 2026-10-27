# 07 — REAPER, ReaRoute, Source-Connect, SessionLinkPRO

## REAPER nel workflow
- Conservare driver ASIO Apollo e ingresso Mic 1 in REAPER.
- Non aggiungere VST su Master, non aggiungere send obbligatori su ogni traccia, non richiedere monitoraggio software.
- L'utente conserva il routing esistente che genera segnali nei mix Console AUX; Matrix **li legge** dagli ingressi ASIO.
- Quando serve una modalità con audio REAPER, attivare preset Matrix che riceve AUX1/2 e non somma Mic1 diretto automaticamente.

## ReaRoute opzionale
- Integrazione ReaRoute può consentire flussi aggiuntivi quando l'host è REAPER, ma non è necessario in MVP e non deve spostare il driver audio utilizzato da REAPER senza test specifici.
- ReaRoute ASIO non è un endpoint browser/WDM di per sé.
- Verificare disponibilità reale di ReaRoute nell'installazione utente e supporto multi-client senza dare nulla per scontato.

## Source-Connect
- Prima versione attraverso endpoint Windows: input **CABLE Output**, ritorno **Apollo Solo**.
- Configurazioni plugin Source-Connect Link 3.x / I/O 4.x sono **alternative**, non un obbligo e non il sostituto della richiesta di routing universale.
- Potranno essere invitate sessioni esterne di terzi. Evitare dipendenze da un host/plug-in di un progetto specifico.

## SessionLinkPRO / piattaforme browser
- Una piattaforma browser normalmente sceglie i device di registrazione esposti a Windows (WDM/WASAPI), non i singoli canali ASIO Apollo.
- Bridge F2 userà **CABLE Output**; F4 vuole offrire device selezionabili per nome indipendenti.
- Ogni browser/app può imporre limiti e DSP (AEC/AGC/noise suppression). Durante test confrontare con/senza elaborazioni e indicare quando sono disabilitabili.

## Mix-minus
- Se Source-Connect restituisce il suo audio sulla Apollo, **non collegare input MON L/R** ad SC_SEND perché potrebbero contenerlo.
- Se si vogliono mix-minus con più partecipanti, acquisire ritorni virtuali distinti in bus indipendenti: ognuno riceve il mix degli altri escluso sé stesso.
