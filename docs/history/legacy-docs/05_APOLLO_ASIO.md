# 05 — Apollo Solo USB: driver, canali e prova di apertura

## Driver I/O Table ufficiale, mappatura predefinita
Fonte: **Apollo Solo USB Manual — Driver I/O Table**, v11 (link in 13_REFERENCES).

| ASIO IN | Descrizione | ASIO OUT | Descrizione |
|---:|---|---:|---|
| 1 | MIC/LINE/Hi-Z 1 | 1 | MON L |
| 2 | MIC/LINE 2 | 2 | MON R |
| 3 | MON L (software output) | 3 | VIRTUAL 1 |
| 4 | MON R (software output) | 4 | VIRTUAL 2 |
| 5 | VIRTUAL 1 (software output) | 5 | HP L |
| 6 | VIRTUAL 2 (software output) | 6 | HP R |
| 7 | AUX1 L (software output) | — | — |
| 8 | AUX1 R (software output) | — | — |
| 9 | AUX2 L (software output) | — | — |
| 10 | AUX2 R (software output) | — | — |

Nota ufficiale: a **176.4 e 192 kHz** AUX2 e VIRTUAL possono non essere disponibili. Quindi la mappa è una BASELINE, ma in runtime enumerare sempre numeri e nomi restituiti dal driver; i canali in alto sample rate variano.

**Non confondere:** AUX1/2 compaiono negli ingressi ASIO visibili dalla DAW come mix del Console. NON sono output ASIO numerati 7–10. Non modificare routing REAPER già funzionante sulla base di una supposizione.

## Domande F0 obbligatorie da risolvere via test, senza pretesa di risposta a priori
1. Quale versione driver UAD e UAD Console? Quale sample rate e buffer?
2. ASIO nominalmente multi-client? REAPER aperto + OBS-ASIO funzionante non garantisce equivalenza di una nuova app.
3. Con OBS e REAPER attivi: quanti client ASIO fisici? Il callback è condiviso o lato software? L'host JUCE deve poter aprire canali e/o output correttamente.
4. Quali maschere input/output erano selezionate dall'implementazione OBS-ASIO? Alcuni driver potrebbero richiedere apertura di un output dummy: testare senza produrre audio e senza toccare i monitor.
5. L'ordine dei canali ritornato da JUCE corrisponde a inputChannelData[] quando la mask è sparsa? Usare tabella esplicita mask→buffer index.
6. Quanti campioni per buffer e formato float? Stabilità dopo cambio sample rate o riapertura REAPER?

## Esperimento F1 — AsioProbe
- Enumerazione read-only di device ASIO e descrittori canali.
- Test con **REAPER chiuso** per baseline; poi ripetere **REAPER aperto** e attivo sul driver originale.
- Aprire input Mic1 (+ possibilmente AUX1/2) con output silenziati/non connessi secondo API senza alterare le selezioni della DAW.
- Copiare i frame in ring buffer e calcolare RMS/peak **fuori callback**; nessun playback/microfono remoto in F1.
- Mostrare ingressi e livelli separati, contatori callback/buffer, timestamp e device errors.
- Uscita/chiusura pulita; OBS-ASIO resta installato e funzionale.

## Criterio successo
In F1 Mic1 e AUX differenziati su misura, nessun errore REAPER, nessuna alterazione ascolto UAD, nessuna periferica modificata. Fallimento documentato (code/errori/versioni) = evidenza utile, non autorizzazione a reverse engineering invasivo.
