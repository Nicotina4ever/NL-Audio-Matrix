# 04 — Grafo audio, matrici e mix-minus

## Nomi e regole
- **IN1 MIC1**, IN2 MIC2; **IN3/4 MON L/R**; **IN5/6 VIRTUAL1/2**; **IN7/8 AUX1 L/R**; **IN9/10 AUX2 L/R** (mappatura predefinita a sample rate normali).
- Gli **input MON/VIRTUAL/AUX sono ritorni software** dal mixer Console: possono contenere voci o segnali già presenti su MIC1.
- Un **bus** è un mix interno, **non** un nuovo microfono Windows fino alla presenza di un endpoint virtuale WDM.
- L'uscita `CABLE Input` è dispositivo Windows **playback**; Source-Connect vede `CABLE Output` come dispositivo Windows **recording**.

## Preset sperimentali, NON routing attuale garantito
**P_DIRECT:** MIC1 -> bus SC_SEND (mono, da convertire in stereo se endpoint richiesto). AUX NON connessi.  
**P_REAPER_AUX:** AUX1 L/R (IN7/8) -> bus SC_SEND stereo. MIC1 NON connesso.  
**P_DUAL_AUDIT:** MIC1 -> bus Direct_AUDIT; AUX1/2 -> bus AUX_AUDIT; **nessuna somma** verso client fino a consenso utente.  
**P_MIX_MINUS:** Client_A send = fonti selezionate **meno** ritorno Client_A; ritorno Client_A disponibile soltanto per ascolto e, se voluto, su bus degli altri client.

## Grafi vietati per default
- Client A return -> Client A send (eco).
- MIC1 + AUX che contiene già MIC1 in parallelo, senza compensazione ritardo (comb filtering/doppia voce).
- Inviare MON L/R interi alla chiamata quando includono l'interlocutore.
- Somma stereo -> mono senza controllo gain/clip.
- Salti di guadagno/route senza rampa/gestione anti-pop.

## Esempio di matrice di permessi
| Sorgente | SC_SEND | LOCAL_MONITOR | OBS_TEST | REAPER_BRIDGE |
|---|---|---|---|---|
| Mic1 | off (preset AUX) | **non usato: UAD Console** | optional | optional |
| AUX1 L/R | on (preset AUX) | off | optional | optional |
| SourceConnect Return | **BLOCCATO** | **Apollo via app** | optional | optional |
| Windows system audio | off | off | optional | optional |

## No monitoraggio locale
Il progetto non deve richiedere al doppiatore di ascoltare il suo microfono via WASAPI, buffer software o REAPER. Il monitoring diretto fisico/UAD resta identico. L'output su CABLE Input è solo **mandata remota**.

## Schema JSON
Vedi `../config/preset_schema_example.json`. È una convenzione proposta che l'agente dovrà validare/versionare e interpretare, non un file accettato automaticamente da software esistenti.
