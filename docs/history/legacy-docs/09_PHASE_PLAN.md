> **AGGIORNAMENTO 24/10/2026 — documento storico.** Il piano canonico è `IMPLEMENTATION_ROADMAP.md` (P1–P7 + vertical slice). Le fasi Apollo-only/4+2 sono superate: matrice universale multi-device, bus dinamici, endpoint provisionabili.

# 09 — Piano sprint (v2: full Matrix, NON cable bridge)

## Sprint A: acquisizione ASIO (thread 1)
1. CMake C++/JUCE compilabile x64.
2. Enumerare Apollo, 10 canali di input, callback, meter e record/ring-buffer solo test.
3. Test con REAPER e Console contemporanei; preservare comportamento base OBS-ASIO v3.2.1f.

## Sprint B: microfono virtuale Windows (thread 2, in parallelo)
1. Audit Microsoft SysVAD / SweetlyAudioDrivers / MicyWMD.
2. Build `VirtualMicProbe` che espone capture endpoint a Windows e riceve PCM da un test app.
3. Dimostrare ricezione reale del segnale in app Windows terza; evitare l'uso definitivo di VB-CABLE.

## Sprint C: integrare A+B
1. Buffer/rate/clock/format; `Apollo ASIO IN 1 -> NL Virtual Mic 1`.
2. Dimostrazione live app/Source-Connect; poi `AUX 1 L/R -> NL Virtual Mic 2` contemporaneo.
3. Endpoint output virtuale per ritorni app → ingresso Matrix, routing UAD selezionabile.

## Sprint D: GUI Matrix
1. N→M crosspoints; nomi bus, gain, mute/solo, meter e presets.
2. Mix-minus per client, blocco feedback, switch senza pop e cattura mono/stereo.
3. Tray, auto-recovery, no cambi ai default device Windows.

## Sprint E: qualità e installazione
1. Tests lunghi e invarianti REAPER/Console, stop/crash/reboot, drift, hotplug.
2. Pacchetto driver/installazione/disinstallazione, firma e validazione.
3. ReaRoute e ASIO virtuali solo dopo sistema principale.

**NB:** Il test via VB-CABLE può accelerare Sprint A, ma è una verifica temporanea; non è una fase obbligatoria e non sostituisce Sprint B. Prima di installare driver kernel sul PC da lavoro, chiedere consenso. Non serve autorizzazione per progettare o compilare codice.
