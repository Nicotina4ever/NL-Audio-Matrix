> **AGGIORNAMENTO 24/10/2026 — documento storico.** Il piano canonico è `TEST_PLAN.md`, con verifica reale di tutti i canali, multi-device, clock, output non invasivi e driver su VM.

# 10 — Piano test verificabile

## Preparazione
- Annotare stato driver/UAD Console/REAPER, fare screenshot o esportazioni locali delle configurazioni, lasciare OBS installato.
- Usare tracce di test/test call, evitare sessione cliente durante debugging.
- Avviare livelli bassi e mute attivo su bus, volume cuffie sicuro.
- Non registrare audio di clienti/interlocutori senza permesso.

## Test per milestone
| ID | Fase | Test | Pass/Fail |
|---|---|---|---|
| T01 | F1 | Enumerazione driver e ingressi 1–10 a 48k | Nomi/indices documentati |
| T02 | F1 | Mic1 senza AUX: meter solo sul canale scelto | Nessun bleed inatteso |
| T03 | F1 | AUX1 con Mic1 muto in Matrix | Canale AUX rilevato indipendentemente |
| T04 | F1 | REAPER ASIO aperto durante acquisizione | Entrambi mantengono audio e callback |
| T05 | F1 | Console monitor non cambia | Verifica pratica con cuffia |
| T06 | F1 | Stop/close/re-open, REAPER ancora aperto | Nessun crash / canali sbloccati |
| T07 | F2 | ASIO→CABLE Input→CABLE Output catturato | File test / meter coerente |
| T08 | F2 | Client return su Apollo senza ritorno verso send | Mix-minus verificato con segnale distinto |
| T09 | F2 | Test Mic1 vs AUX: niente somma duplice | Preset route verificati |
| T10 | F2 | 44.1k/48k, buffer differenti | Nessun drift patologico |
| T11 | F2 | 30–60 minuti audio, FIFO/counter | Nessun crash; drop documentati |
| T12 | F3 | Cambi preset a caldo | Niente picchi improvvisi |
| T13 | F3 | Connessione/scollegamento dispositivo | Mute e recovery controllati |
| T14 | F3 | Source-Connect, SessionLinkPRO/browser test | Microfono appropriato riconosciuto |
| T15 | F3 | Panic mute | Interruzione mandata immediata/definita |
| T16 | F4 | Driver crash/reboot/uninstall su test PC | Sistema sano dopo rollback |

## Misure da catturare
Sample rate ASIO/WASAPI; block size; callback duration/max; CPU; RMS/peak per canale; FIFO fill min/avg/max; underruns/overruns; error codes; device IDs; clock correction; latency loopback se misurabile. Dichiarare metodi e condizioni, non inventare millisecondi.

## Test anti-eco
1. Emissione A (microfono) e B (tono/test audio lato client) distinguibili.
2. Il bus in uscita contiene A (o AUX selezionato) ma **non B**.
3. Disattivare DSP automatici browser quando possibile per isolare il comportamento del router.
4. Controllare che mic diretto e AUX non sommino la stessa voce ritardata.

## Criteri di stop immediato
Eco forte, feedback, livello improvvisamente elevato, drop continui, REAPER perde driver, monitor UAD cambia, Windows cambia device predefinito, qualsiasi richiesta di installare un driver non approvato.
