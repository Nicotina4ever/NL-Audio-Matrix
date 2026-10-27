> **NOTA v2:** riferimento storico/integrativo. Per il prodotto finale valgono README.md, AGENTS.md e docs/15–17: endpoint virtuali proprietari sono deliverable **prioritario**, VB-CABLE è solo un possibile test temporaneo.

# 02 — Requisiti e criteri di accettazione

## MUST — MVP F1/F2
R01. Windows 11 x64, app standalone GUI minima o CLI diagnostica, C++/JUCE.
R02. Rileva il driver ASIO UAD, mostra tutti gli ingressi nominativi e relativi indici assoluti.
R03. Apre e acquisisce selettivamente Mic 1, AUX 1 L/R, AUX 2 L/R (e gli altri presenti), **senza OBS**.
R04. Mantiene REAPER operativo col proprio driver e conserva il monitoraggio UAD Console: test reale richiesto.
R05. Gestisce MIC 1 e AUX come bus indipendenti, senza mix di default che duplichi la stessa voce.
R06. Emette bus mono/stereo su **CABLE Input** come renderer WASAPI; client vede **CABLE Output** come microfono.
R07. Supporta 44.1 e 48 kHz dove consentito, con negoziazione, gestione errori e drift tra clock.
R08. Start/Stop manuali, mute universale (panic), meter e contatori XRUN/drop/dropout.
R09. Salvataggio configurazione, nessuna modifica globale al driver o Console.
R10. Fallback OBS senza necessità di disinstallazione o riconfigurazione critica.
R11. Vincolo realtime nel callback, senza allocazioni/blocchi o scritture su file.
R12. Ritorno Source-Connect/APOLLO non presente nel bus di invio per default (mix-minus).

## SHOULD — F3
R13. Matrix N→M con guadagni, mute, mono/stereo, bus e nomi leggibili.
R14. Preset separati Direct / REAPER AUX / sessione remota / prova silenziosa.
R15. Blocco grafi con feedback involontario; avviso di duplicazione MIC/AUX e clipping.
R16. Audio endpoint change/reconnect e recupero errori UAD.
R17. Diagnostica esportabile (senza dati privati).
R18. App tray; avvio opzionale ma **mai forzare device predefiniti Windows**.

## FUTURE — F4 da autorizzare
R19. 2+ endpoint di registrazione WDM/WASAPI realmente distinti, generati da driver proprio.
R20. Endpoint di riproduzione WDM/WASAPI opzionali e ASIO virtuale separato.
R21. Bridge ReaRoute facoltativo; non prerequisito di funzionamento.
R22. Distribuzione firmata/installer con rollback e disinstallazione sicura.

## NON OBIETTIVI
- Sbloccare la I/O Matrix Thunderbolt nel firmware/driver Apollo Solo USB.
- Emulare Console, DSP Unison, controlli UAD o modificare hardware.
- Ricostruire/repackaging del plugin OBS come se fosse una DLL indipendente.
- Fare streaming/routing in rete come requisito MVP.
- Sostituire REAPER, cambiare i suoi bus o le catene audio.

## Gate oggettivi
G1: ASIO Probe enumerazione corretta + acquisizione canali **con REAPER aperto** e Console invariata; identificare failure mode.
G2: Bridge senza OBS fa arrivare audio a CABLE Output e all'app remota; ritorno non rientra nella mandata; catture di test confrontate.
G3: Test multiora con drop=0 o soglia concordata, latenza monitor UAD identica, riconnessioni controllate.
G4: Solo dopo, approvazione esplicita per progettazione driver kernel.
