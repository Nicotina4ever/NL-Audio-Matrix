> **AGGIORNAMENTO 24/10/2026 — documento storico.** L'audit canonico è `SOURCE_CODE_AUDIT.md`. La verifica del tag OBS-ASIO è chiusa: `src/` è byte-identico a `v3.2.1f` (HEAD = tag + solo README). Lo scope non è più Apollo-only: matrice universale multi-device; 4+2 è solo un preset. Vedi `ARCHITECTURE.md` e `TECHNICAL_DECISIONS.md`.

# 14 — Evidenze verificabili e ipotesi ancora aperte

| Affermazione | Stato | Evidenza / come verificare |
|---|---|---|
| I/O Matrix ufficiale solo Apollo Thunderbolt | CONFERMATO | UAD I/O Matrix support |
| Apollo Solo USB default 10 input ASIO e 6 output | CONFERMATO su manuale | Apollo USB Solo Driver I/O Table |
| 7/8 AUX1 L/R, 9/10 AUX2 L/R | CONFERMATO su manuale | stessa tabella |
| OBS-ASIO branch `asio-juce` usa JUCE e callback ASIO | CONFERMATO sul branch | `src/asio-input.cpp`, CMake |
| CMake di OBS `asio-juce` include fork `Andersama/JUCE open_asio_juce` | CONFERMATO sul branch | `CMakeLists.txt` |
| Il codice del branch è esattamente quello del tag v3.2.1f | **DA VERIFICARE** | fetch/tag/diff esatto |
| OBS-ASIO 3.2.1f funziona nel setup REAPER simultaneo | OSSERVATO DALL'UTENTE | test sul suo PC, non misurato nel kit |
| Una nuova app JUCE apre Apollo con REAPER senza conflitti | **NON VERIFICATO** | milestone F1; niente garanzie |
| Un bridge standalone invia correttamente ad VB-CABLE | **NON VERIFICATO** | milestone F2 |
| Il mix Mic1+AUX non duplica voce | **NON VERIFICATO** | audizione routing Console, T09 |
| Senza driver nostro possiamo rendere nuovo NL Mic in Zoom | FALSO per endpoint reale | serve un driver virtuale già esistente o F4 |
| SysVAD è pronto da installare come nostro driver | FALSO | è un esempio da sviluppare e testare |
| JUCE upstream presenta JUCE_ASIO/JUCE_WASAPI | CONFERMATO | juce_audio_devices.h |
| Source-Connect Link/I/O sono alternative commerciali da progetto | CONFERMATO esistenza, NON sostitutive | quick sheets ufficiali |

## Fatti da non spacciare per certi
- Latency numerica.
- Numero massimo di client ASIO aperti da processi indipendenti.
- Compatibilità della GUI/SDK JUCE già installata sul PC.
- Esistenza di endpoint ASIO o WDM proprio.
- Capacità di ricreare il plugin OBS direttamente fuori OBS senza dipendenze.
- Successo dei test di callback, output o driver prima di ricevere log reali.
