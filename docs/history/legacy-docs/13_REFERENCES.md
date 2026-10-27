# 13 — LINK ORIGINALI, UFFICIALI E CODICE DI RIFERIMENTO

Consultati e verificati nelle ricerche del 24/10/2026, dove accessibili. I collegamenti a manuali PDF potrebbero essere protetti da CDN o cambiare; usare il sito dell'editore per la versione corrente. Non includiamo copie non autorizzate.

## A. Link trasmessi dall'utente (da preservare)
1. UAD I/O Matrix Thunderbolt-only — https://help.uaudio.com/hc/en-us/articles/25403673923220-I-O-Matrix-Settings-Panel
2. VB-Audio Matrix — https://vb-audio.com/Matrix/
3. OBS-ASIO GitHub — https://github.com/Andersama/obs-asio
4. OBS-ASIO releases — https://github.com/Andersama/obs-asio/releases
5. OBS-ASIO v3.2.1f — https://github.com/Andersama/obs-asio/releases/tag/v3.2.1f

## B. Sorgente rilevante per OBS-ASIO v3.2.1f
6. Branch asio-juce (riferimento, VERIFICARE il commit tag) — https://github.com/Andersama/obs-asio/tree/asio-juce
7. src/asio-input.cpp — https://github.com/Andersama/obs-asio/blob/asio-juce/src/asio-input.cpp
8. CMakeLists.txt — https://github.com/Andersama/obs-asio/blob/asio-juce/CMakeLists.txt
9. Fork JUCE utilizzato dal CMake OBS — https://github.com/Andersama/JUCE/tree/open_asio_juce
10. Licenza OBS-ASIO — https://github.com/Andersama/obs-asio/blob/asio-juce/LICENSE

**Cosa cercare:** `AudioIODeviceType::createAudioIODeviceType_ASIO`, `AudioIODeviceCallback`, `audioDeviceIOCallback`, input channel mask, selezione canali, distribuzione delle sorgenti. Il README usa “multi-client” per più sorgenti OBS, NON per garantire multi-apertura multi-processo.

## C. Universal Audio
11. Apollo Solo USB — Manuale ufficiale (mappatura 10 IN, 6 OUT) — https://media.uaudio.com/support/manuals/v11.0.0-y2Bz6/Apollo%20Solo%20USB%20Manual.pdf
12. UAD I/O Matrix impostazioni — https://help.uaudio.com/hc/en-us/articles/25403673923220-I-O-Matrix-Settings-Panel

## D. JUCE / ASIO / WASAPI
13. JUCE upstream — https://github.com/juce-framework/JUCE
14. JUCE AudioIODevice — https://docs.juce.com/master/classjuce_1_1AudioIODevice.html
15. JUCE AudioIODeviceType — https://docs.juce.com/master/classjuce_1_1AudioIODeviceType.html
16. JUCE Windows ASIO/WASAPI macros — https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_devices/juce_audio_devices.h
17. Licenza ASIO files bundled with JUCE — https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_devices/native/asio/LICENSE.txt
18. Microsoft WASAPI overview — https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi
19. Microsoft IAudioClient — https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioclient

## E. Endpoint driver virtuali (SOLO F4)
20. Microsoft SysVAD repository — https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad
21. Microsoft SysVAD README — https://github.com/microsoft/Windows-driver-samples/blob/main/audio/sysvad/README.md
22. Microsoft driver samples guide — https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/sample-audio-drivers
23. Microsoft test driver signing — https://learn.microsoft.com/en-us/windows-hardware/drivers/install/test-signing

## F. Riferimenti confronto, non soluzioni confermate
24. VBMatrix user guide (PDF) — https://vb-audio.com/Matrix/VBMatrix_UserManual.pdf
25. VB-CABLE official — https://vb-audio.com/Cable/
26. REAPER official — https://www.reaper.fm/
27. Source Elements: Quick Sheets, Source-Connect Link 3 e Source-Connect I/O 4 — https://support.source-elements.com/quick-sheets
28. Source-Connect 4 Talent + I/O — https://support.source-elements.com/sc4-talent
29. SessionLinkPRO sito — https://sessionlinkpro.com/

## Note sui link
- `v3.2.1f` è la release riferita come funzionante dall'utente. Verificare se il branch `asio-juce` corrente e il commit `ef995b7` risultante dalla release GitHub coincidono davvero, prima di citare linee di codice come prova del binario specifico.
- Il manuale VB-Matrix pubblico reperito può mostrare una versione precedente rispetto al download sul sito.
- Non confondere i canali ASIO IN (alcuni sono monitor virtuali) e ASIO OUT della Apollo.


## Aggiunte v2 — basi codice per endpoint indipendenti
- https://github.com/Whitestagconcepts/SweetlyAudioDrivers (MIT-0, due cavi virtuali)
- https://github.com/Mithronn/MicyWMD (MIT, microfono Windows con IOCTL)
- https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad (sample ufficiale)
- Vedi anche `docs/17_REFERENCE_PROJECTS.md`.
