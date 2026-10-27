> **NOTA v2:** riferimento storico/integrativo. Per il prodotto finale valgono README.md, AGENTS.md e docs/15–17: endpoint virtuali proprietari sono deliverable **prioritario**, VB-CABLE è solo un possibile test temporaneo.

# 06 — Bridge WASAPI vs veri driver audio virtuali

## Livello A (F2) — utilità reale senza nuovo driver
Apollo ASIO -> JUCE/C++ Capture -> bus -> WASAPI render su **VB-CABLE CABLE Input**. L'app remota usa **CABLE Output** come mic. Questo elimina OBS ma NON crea un nuovo endpoint virtuale di nostra proprietà. Necessita di VB-CABLE installato (già presente sull'host).

## Livello B (F4) — nuovi dispositivi Windows
Esporre device distinti come **NL Mic Direct**, **NL Mic REAPER**, **NL Remote Return** richiede endpoint di cattura/render WDM/WASAPI reali oppure dipendenza da un driver virtuale già esistente. Non basta un bus interno JUCE o un nome in GUI.

**Base ufficiale:** Microsoft `SysVAD` dimostra la creazione di dispositivi virtuali WDM/WaveRT. Il campione **non è un bridge funzionante pronto**, serve integrazione PCM dai bus audio user-mode al driver, buffer/transport, meccanismi di sincronizzazione, gestione lifecycle e firma.

## Livello C — ASIO virtuale
ReaRoute è un driver ASIO virtuale fornito da REAPER. Un nostro ASIO virtuale può essere un modulo opzionale separato dalla parte WDM, con proprie API e licenza. Non confonderlo con endpoint Windows per browser/Zoom.

## Milestone F4: prerequisiti
- F2 e F3 completi e stabili.
- Prototipo separato su PC/VM di test, mai installare il driver sperimentale sul PC da lavoro senza rollback.
- Windows WDK compatibile, firma valida, policy attuale Microsoft.
- Specifica IPC/trasporto tra processo user-mode e kernel (ring buffer, privilegi, formato, timing), threat model e possibilità di disinstallazione.
- Test a freddo/riavvii, sleep/wake, crash motore user-mode, comportamento quando app chiude e quando device viene rimosso.
- Consenso espresso prima di privilegio amministratore, firma test, modifica modalità di boot o dispositivo audio predefinito.

## Il confronto con VB-Matrix
VB-Audio Matrix è un riferimento delle funzionalità possibili, **NON** un prerequisito per la soluzione dell'utente. È stato già testato senza soddisfare i vincoli. Nessuna supposizione che una semplice modifica della sua griglia risolva l'apertura ASIO della Apollo.
