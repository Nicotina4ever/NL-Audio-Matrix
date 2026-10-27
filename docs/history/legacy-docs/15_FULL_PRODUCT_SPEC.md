> **AGGIORNAMENTO 24/10/2026 — documento storico/integrativo.** La specifica canonica è `MASTER_PLAN.md` + `ARCHITECTURE.md`. Scope aggiornato: matrice universale multi-device (ASIO/WASAPI/virtuali), bus dinamici, routing bidirezionale, endpoint provisionabili. Apollo è il primo device di test, non il limite. 4+2 è solo un preset.

# 15 — Specifica funzionale: il prodotto definitivo

## Concetto
Un mixer software a matrice per sorgenti e destinazioni audio Windows/ASIO, che offra ciò che la I/O Matrix UAD Thunderbolt non offre alla Solo USB. È un prodotto **autonomo**: non modifica il driver UAD e non richiede OBS, DAW o Virtual Cable in funzione.

## Sorgenti
- Apollo Solo USB ingressi ASIO 1–10 con nomi dal driver; a 176.4/192 kHz il modello può esporre meno canali (manuale).
- Più avanti driver ASIO diversi, REAPER tramite ReaRoute opzionale, input dai virtual render endpoint e da altri WDM input.
- Ritorni delle app acquisiti da `NL Virtual Speaker N` (endpoint di riproduzione Windows) tramite il bridge PCM.

## Routing
- Matrix N sorgenti × M bus, pulsante ON/OFF su crosspoint, guadagno dB per crosspoint, attenuazione/pan/opzionale inversione fase e meter.
- Bus MONO/STEREO con nomi personalizzabili, invii pre/post, mute/solo, limitatore opzionale solo sui bus di trasmissione.
- Assegna uno o più bus a un dispositivo virtuale specifico, uno stesso input può andare a più bus.
- Routing salvato in preset JSON, cambio con transizione anti-click; meter senza congestione GUI.
- Anti-feedback e mix-minus per destinazione: client X riceve voce e riproduzioni scelte, NON il proprio ritorno. Protezione esplicita da mic diretto+mic AUX elaborato duplicato.

## Dispositivi Windows
- `NL Virtual Mic 1..4` (capture): visibili nelle impostazioni audio e come selezione microfono nelle app.
- `NL Virtual Speaker 1..2` (render): selezionabili come uscita dalle app; il motore ne riceve i dati per restituirli verso UAD o altri bus configurati.
- Numero iniziale indicativo, espandibile nella progettazione. Configurazione di endpoint persistenti e nomi stabili.
- ASIO virtuale/ReaRoute facoltativi, NON rimpiazzo degli endpoint Windows.

## Esempio flusso
`Apollo ASIO IN 7/8 (Console AUX1) -> Matrix BUS "TAKE PROCESSATA" -> NL Virtual Mic 1 -> Source-Connect input`.
Ritorno `Source-Connect output -> Apollo` come oggi, oppure opzionalmente `Source-Connect output -> NL Virtual Speaker 1 -> NL Matrix -> destinazione scelta`.
`REAPER` continua a gestire il proprio microfono e le uscite UAD/Console. Nessun insert, routing Master o audio monitoring REAPER imposto.

## Requisiti indispensabili
- Continua a funzionare con REAPER aperto (come la baseline OBS-ASIO, da verificare su nuova implementazione).
- UAD Console rimane il monitor a bassa latenza.
- Driver UAD originale intatto; nessuna manipolazione firmware/DLL.
- Se il motore si chiude, gli endpoint diventano silenziosi senza feedback o rumori forti.
- Meter clipping/XRUN, pulsante PANIC e ripristino esplicito.
