> **AGGIORNAMENTO 24/10/2026 — documento storico.** Commit, licenze e riferimenti verificati sono in `SOURCE_CODE_AUDIT.md` §3. Nota: SysVAD upstream è MS-PL; JUCE locale è 9.0.4 AGPL/commerciale; OBS-ASIO è GPL-3.0 (solo riferimento comportamentale). Lo scope è universale multi-device.

# 17 — Link di riferimento verificati

## Apollo e OBS-ASIO (priorità assoluta)
- OBS-ASIO repository: https://github.com/Andersama/obs-asio
- Branch JUCE: https://github.com/Andersama/obs-asio/tree/asio-juce
- Source modulo callback: https://github.com/Andersama/obs-asio/blob/asio-juce/src/asio-input.cpp
- Release funzionante v3.2.1f: https://github.com/Andersama/obs-asio/releases/tag/v3.2.1f
- Wiki uso canali e limiti: https://github.com/Andersama/obs-asio/wiki
- Manuale Apollo Solo USB (vedi Driver I/O Table): https://media.uaudio.com/support/manuals/v11.0.0-y2Bz6/Apollo%20Solo%20USB%20Manual.pdf
- Pannello I/O Matrix ufficiale UAD (riferimento UI): https://help.uaudio.com/hc/en-us/articles/25403673923220-I-O-Matrix-Settings-Panel

## Driver virtuali / API
- Microsoft SysVAD: https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad
- Microsoft SysVAD doc: https://learn.microsoft.com/en-us/samples/microsoft/windows-driver-samples/sysvad-virtual-audio-device-driver-sample/
- SweetlyAudioDrivers, MIT-0: https://github.com/Whitestagconcepts/SweetlyAudioDrivers
- MicyWMD, MIT: https://github.com/Mithronn/MicyWMD
- JUCE: https://github.com/juce-framework/JUCE
- Steinberg ASIO SDK: https://www.steinberg.net/asiosdk

## Confronto funzioni, NON dipendenze
- VB Audio Matrix: https://vb-audio.com/Matrix/
- VB CABLE: https://vb-audio.com/Cable/
- ReaRoute: https://www.reaper.fm/

## Requisiti di audit agent
Studiare autenticità del repository, architettura, build, compatibilità Windows, licenza corrente, affidabilità e correttezza del trasporto PCM. Non assumere che un repository poco diffuso sia produzione-ready o che la qualità del suo driver sia verificata; usarlo come punto di partenza del codice.
