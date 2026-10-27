> **AGGIORNAMENTO 24/10/2026 — documento storico/integrativo.** La progettazione canonica è in `ARCHITECTURE.md` §6–§7 e `TECHNICAL_DECISIONS.md` D11–D14. Gli endpoint sono multipli e provisionabili entro il massimo del driver; il motore non hardcoda 4+2. La base driver è SysVAD (MS-PL), non Micy/Sweetly.

# 16 — Endpoint audio Windows: implementazione concreta

## Cosa va implementato davvero
Un bus JUCE non diventa da solo un dispositivo Windows. Windows deve enumerare uno o più endpoint `eCapture` e `eRender` reali (MMDevice/WASAPI). La soluzione completa richiede quindi un componente driver virtuale appropriato e un trasporto dati PCM verso il motore user-mode.

### Due flussi distinti
A. `Apollo ASIO input → Bus Mixer user-mode → PCM bridge → Driver virtual capture → Source-Connect microfono`.
B. `Source-Connect output → Driver virtual render → PCM bridge → Bus Mixer user-mode → destinazione scelta`.

### Riferimenti da valutare
1. Microsoft SysVAD https://github.com/microsoft/Windows-driver-samples/tree/main/audio/sysvad — base ufficiale WDM/WaveRT per endpoint multipli, non un cavo completo.
2. SweetlyAudioDrivers https://github.com/Whitestagconcepts/SweetlyAudioDrivers — due cavi virtuali con ring buffer e loopback render/capture, repo MIT-0; audit tecnico necessario.
3. MicyWMD https://github.com/Mithronn/MicyWMD — microfono virtuale con iniezione PCM/IOCTL, repo MIT; audit tecnico necessario.

### Target iniziale
`VirtualMicProbe`: un endpoint capture Windows visibile in Impostazioni audio, alimentato da segnale PCM test (sinusoide a livello basso, non clipping) tramite componente user-mode; un'app generica lo registra correttamente. Poi collegamento ai blocchi audio provenienti dalla ASIO callback.

### Architettura orientativa
- User mode: Engine C++ con FIFO per ogni bus e backend `VirtualCaptureWriter`/`VirtualRenderReader`.
- Bridge di processo: interfaccia dati e control plane versionati; ring buffer + signaling (esattamente come implementati dipende dal driver scelto), policy lifecycle/rate/channel e disconnessione.
- Driver: capture/render endpoints con formati supportati, render/capture data path, buffering, silenzio se engine assente.
- Ricampionamento / drift tra clock Apollo e client endpoint gestiti in user-mode.

### Cosa NON fare
- Non pretendere che un semplice VST/EXE JUCE produca microfoni di Windows senza un componente audio virtuale installato.
- Non usare come soluzione definitiva `VB-CABLE -> CABLE Output` anche se utile per diagnosticare il capture engine.
- Non reverse engineer VB-CABLE come prerequisito: sorgenti pubblici già disponibili.
- Non installare sul PC produttivo build kernel non testate senza consenso esplicito; prima compilare, testare e documentare disinstallazione.
