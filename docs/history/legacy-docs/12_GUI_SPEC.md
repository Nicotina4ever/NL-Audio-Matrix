# 12 — UI/UX della Matrix (F3 dopo prova audio)

## UI minima (non prima di F1/F2)
- Header: device Apollo, ASIO state, sample rate/buffer, WASAPI sink, clock status, **PANIC MUTE** evidente.
- Sidebar: Sources (Apollo inputs 1–10, Windows App input in futuro), Bus (Direct, AUX, SC_SEND, Monitoring?), Destinations (CABLE Input, device Windows, virtual endpoints in F4).
- Tab Matrix: righe sorgente, colonne bus/destinazioni; clic per abilita, gain numerico dB, mute/solo per bus, indicatori stereo.
- Tab Preset: Direct Mic1, Reaper AUX1/2, DUAL AUDIT, Remote Mix-Minus, Custom.
- Tab Diagnostics: ring fill, errors, dropped, realtime CPU load, sample rate conversion, last device reconnect.
- Tab Compatibility: user friendly endpoint hints `CABLE Input` (playback) vs `CABLE Output` (recording).

## Principi
- Di default nessuna route al client prima della selezione esplicita di un preset.
- Se l'utente prova a sommare Mic1 e AUX: warning 'AUX può già contenere la tua voce'.
- Ritorno remoto verso il proprio bus inibito di default.
- Niente meter pesanti calcolati/aggiornati dalla callback realtime; GUI usa snapshots rate limited.
- Mostrare errore ASIO senza auto-modificare il driver.
- Preset esportabili JSON versionati, backup automatico delle impostazioni precedenti.
- Opzionale tray; garantire che Stop/Exit chiuda gli handle audio.

## Out of scope UI
Waveform multitraccia, VST host, emulazione Console UAD, streaming video, social/live capture.
