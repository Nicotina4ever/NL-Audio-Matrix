# SOURCE CODE AUDIT — NL Audio Matrix v2

**Data:** 24 ottobre 2026  
**Stato:** P0 — analisi read-only completata; nessun codice compilato, nessun driver installato, nessuna modifica ai repository di riferimento.  
**Ambito:** `obs-asio` (v3.2.1f), `JUCE` 9.0.4, `Windows-driver-samples\sysvad`, `SweetlyAudioDrivers`, `MicyWMD`.  
**Lente:** prodotto universale multi-dispositivo; Apollo Solo USB è il primo dispositivo di test, non il limite del prodotto.

> Classificazione delle affermazioni: **[V]** = verificato direttamente nel codice/file; **[I]** = inferito dal codice o dalla struttura; **[T]** = richiede test sul PC e non può essere deciso staticamente.

---

## 1. Sintesi esecutiva

1. **OBS-ASIO locale è autentico e corrisponde alla release v3.2.1f**: il branch `asio-juce` è al commit `a250623…`, cioè il tag `v3.2.1f` (`ef995b7…`) più un solo commit che modifica `README.md`. Il contenuto di `src/` è byte-identico al tag. [V]
2. **OBS-ASIO dimostra il fan-out intra-processo, non il multi-client ASIO cross-process.** Un solo `juce::AudioIODevice` per nome device, un `AudioCB` condiviso e N `AudioListener` che leggono lo stesso ring. Non esiste alcun meccanismo di condivisione tra processi: la coesistenza con REAPER dipende interamente dal driver UAD. [V]
3. **Il codice OBS-ASIO non è compilabile con il JUCE 9.0.4 locale**: il plugin usa il fork `Andersama/JUCE` branch `open_asio_juce` (JUCE 7.0.2 + openASIO), API callback precedente a JUCE 7.0.3. Il JUCE locale è stock 9.0.4. [V]
4. **SysVAD è la base corretta per gli endpoint virtuali**: è PortCls/WaveRT, espone già 4 render + 4 capture statici con tabelle dichiarative e INF componentizzato, ma **non ha alcun trasporto PCM kernel↔user-mode**. Capture = tono generato, render = scartato o dump su file. [V]
5. **MicyWMD e SweetlyAudioDrivers non sono basi di prodotto**:
   - Micy: un solo endpoint capture, ring globale di 30.720 B, IOCTL non versionato, nessun render, bug di overflow/OOB, controlli di accesso inadeguati. [V]
   - Sweetly: WaveCyclic/MSVAD legacy, FIFO statica per immagine driver (al massimo 2 percorsi audio indipendenti), nessun trasporto user-mode, difetti INF e side effect su `C:\`. [V]
6. **Il routing universale richiede un motore diverso da quello di OBS-ASIO**: DeviceManager multi-backend (ASIO, WASAPI nativo, endpoint virtuali, ReaRoute, test), grafo dinamico N×M, clock indipendenti con SRC adattivo, ring di disaccoppiamento, scheduler del grafo dedicato (funziona anche senza ASIO attivo). [I]
7. **Licenze**: OBS-ASIO GPL-3.0 (solo riferimento comportamentale); JUCE 9 AGPLv3/commerciale; ASIO SDK GPLv3/Steinberg; SysVAD MS-PL; Micy MIT su base Microsoft MS-PL; Sweetly MIT-0 dichiarata su codice Microsoft MS-PL (re-licensing non valido per i file Microsoft). [V]

---

## 2. Perimetro, metodo, file analizzati

**Metodo:** lettura diretta con strumenti read-only (`read`, `grep`, `glob`), query git read-only (`log`, `show`, `diff`, `describe`, `tag`, `ls-tree`, `hash-object`), nessuna build, nessuna esecuzione di binari, nessuna installazione. I repository non sono stati modificati; `git status --porcelain` è vuoto per `obs-asio`, `SweetlyAudioDrivers`, `MicyWMD`; `Windows-driver-samples` non contiene `.git`.

**Ambiente verificato:** Windows 11 Enterprise 26300; VS 2022 BuildTools presente in `D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`; **WDK non installato** (manca `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\km`); CMake non presente nei path standard; git presente. Registry ASIO: presenti `Universal Audio USB`, `ReaRoute ASIO (x64)`, `ASIO4ALL v2`, `FlexASIO`, `VB-Matrix VASIO-*`, `Source Elements Audio Driver`, altri.

**File principali letti integralmente o in modo estensivo:**

- `obs-asio/src/asio-input.cpp` (926 righe), `CMakeLists.txt`, `buildspec.json`, `LICENSE`, `README.md`, `cmake/**`, `data/locale/en-US.ini`.
- `JUCE/modules/juce_core/system/juce_StandardHeader.h`, `JUCE/CMakeLists.txt`, `JUCE/LICENSE.md`, `JUCE/modules/juce_audio_devices/native/juce_ASIO_windows.cpp` (1.676 righe), `.../native/asio/LICENSE.txt`, `juce_AudioDeviceManager.{h,cpp}`, `juce_Messaging_windows.cpp`, `juce_WASAPI_windows.cpp` (grep mirato).
- `Windows-driver-samples/sysvad`: `adapter.cpp`, `common.cpp`, `common.h`, `hw.cpp`, `minwavert.cpp`, `minwavertstream.cpp`, `TabletAudioSample/minipairs.h`, `TabletAudioSample/*wavtable.h`, `ComponentizedAudioSample.inx`, `README.md`; `EndpointsCommon/**` in modo mirato.
- `SweetlyAudioDrivers`: `basewave.{h,cpp}`, `basedma.cpp`, `adapter.cpp`, `msvad.h`, `msvad.inf`, `simple/{minwave.cpp,mintopo.cpp,simple.h,wavtable.h,toptable.h}`, `savedata.cpp`, `README.md`, `LICENSE`.
- `MicyWMD`: `Source/Main/adapter.cpp`, `minwavertstream.cpp`, `minwavert.cpp`, `common.cpp`, `Source/Inc/{common.h,definitions.h,endpoints.h}`, `Source/Filters/{micarraywavtable.h,micarray1toptable.h,minipairs.h}`, `Source/Main/MicyAudio.inx`, `Test/Sender.cpp`, `README.md`, `LICENSE`.

---

## 3. Identità, versioni, licenze

| Repository | Identità verificata | Licenza effettiva | Note |
|---|---|---|---|
| `obs-asio` | Branch `asio-juce`; HEAD `a250623e56debca699d5c60e1d6ff676aaa1002e` (2024-05-21, "Update README.md"); tag `v3.2.1f` = `ef995b7d5a6f149492b2806587b392ab3cd32af1`; `git describe` = `v3.2.1f-1-ga250623`; `project(obs-asio VERSION 3.2.1)`; `buildspec.json` version 3.2.1 | **GPL-3.0** (`LICENSE` riga 1-2); header sorgente dichiara GPL-2.0-or-later; credits "GPL v3" | `src/` identico al tag: blob `3851c4ad787e9b1d504ec6292d2e873ebec3775b` sia in `git ls-tree v3.2.1f` sia in HEAD e nel working tree; `git diff v3.2.1f..HEAD` = solo `README.md`. Il suffisso `f` esiste solo nel tag/release, non nelle versioni interne. |
| `JUCE` | 9.0.4 (`juce_StandardHeader.h:42-44`; `CMakeLists.txt:37`); nessun `.git` | **AGPLv3 / commerciale JUCE 9** (`LICENSE.md:1-53`); SDK ASIO Steinberg **GPLv3 / licenza proprietaria** (`native/asio/LICENSE.txt:9-27`) | Stock, non il fork `open_asio_juce`. Il fork usato da OBS-ASIO è JUCE 7.0.2 + openASIO (esterno, non presente localmente). |
| `SysVAD` | Subset locale di `Windows-driver-samples/audio/sysvad`, senza `.git`; README campione moderno; INF target Windows 11 22H2 (`ComponentizedAudioSample.inx:28`) | **MS-PL** (licenza del repo upstream Microsoft, verificata via `raw.githubusercontent.com/microsoft/Windows-driver-samples/main/LICENSE`); **nessun file LICENSE nel subset locale** | Il subset manca anche di `wil/`, della INX legacy `TabletAudioSample.inx` e del repo root; non è buildabile così com'è. |
| `SweetlyAudioDrivers` | `main`, HEAD `bf5c11c035ca0e48452238fae969bca4d0961845` (2025-11-19, "Update license to MIT-0"), 2 commit | Testo **MIT-0** (`LICENSE`), ma i file derivano dal sample MSVAD Microsoft con header Microsoft; upstream **MS-PL** | Il re-licensing MIT-0 dei file Microsoft è un difetto di compliance; non usare come base senza verifica legale. |
| `MicyWMD` | `main`, HEAD `c5d2e11b734fea48d1ebfaf9f450e01fba6634df` (2025-11-18), 4 commit | **MIT** (`LICENSE:1-3`), su base Microsoft SimpleAudioSample (MS-PL) | Conservare notice Microsoft; la parte Micy è MIT, la base è MS-PL. |

---

## 4. OBS-ASIO v3.2.1f — analisi

### 4.1 Autenticità della release

- Tag `v3.2.1f` = commit `ef995b7…`, lightweight tag; il commit precedente `38857df…` è il tag `v3.2.1`.
- `git diff --stat v3.2.1..v3.2.1f` = `src/asio-input.cpp | 44 +++++-----` (23 inserzioni, 21 eliminazioni), solo formattazione/clang-format. [V]
- `git diff --stat v3.2.1f..HEAD`: solo `README.md | 2 +-` (badge CI). [V]
- `git ls-tree -r v3.2.1f -- src` e HEAD restituiscono lo stesso blob `3851c4ad…`; `git hash-object src/asio-input.cpp` coincide. [V]
- **Conclusione:** il codice locale è la release funzionante v3.2.1f; l'unica differenza è documentale. La verifica richiesta dai documenti precedenti (`docs/14`) è quindi chiusa.

### 4.2 Architettura e classi

Un solo file, `src/asio-input.cpp` (926 righe), che mescola OBS, JUCE e Qt.

| Simbolo | Righe | Ruolo |
|---|---:|---|
| `deviceTypeAsio` | 48 | unica `juce::AudioIODeviceType` ASIO per tutta la DLL |
| `callbacks` | 58 | registro process-wide `std::vector<AudioCB*>`: un `AudioCB` per nome device |
| `AudioCB` | 136-476 | proprietario del device e del ring condiviso; implementa `juce::AudioIODeviceCallback` |
| `AudioCB::AudioListener` | 161-296 | consumatore per sorgente OBS; `TimeSliceClient`; route 0-based |
| `AudioCB::audioDeviceIOCallback` | 331-346 | callback realtime: copia tutti i canali input nel ring |
| `AudioCB::audioDeviceAboutToStart` | 378-439 | alloca il ring, il buffer silenzioso, avvia il thread globale |
| `AudioCB::add_client/remove_client` | 348-376 | aggancio/distacco delle sorgenti |
| `ASIOPlugin` | 480-722 | una sorgente OBS; proprietà, update, route |
| `ASIOPlugin::update` | 587-689 | lookup `AudioCB`, creazione device, apertura, start, applicazione route |
| `fill_out_devices` | 838-866 | enumerazione e creazione `AudioCB` per device |
| `obs_module_load/unload` | 868-926 | inizializzazione globale, `scanForDevices`, distruzione |

### 4.3 Flusso dati e threading

```
OBS source create/update
  -> ASIOPlugin::update                         src:587-689
     -> AudioCB lookup per nome device          src:594-609
     -> deviceTypeAsio->createDevice(...)       src:601
     -> device->open(all inputs, all outputs)   src:622-645
     -> device->start(callback)                 src:650-651
        -> AudioCB::audioDeviceAboutToStart     src:378-439
  -> ASIO callback (thread driver)
     -> AudioCB::audioDeviceIOCallback          src:331-346
        -> buffers[_write_index].buffer.copyFrom(...)
        -> _write_index = (_write_index + 1) % N
  -> TimeSliceThread globale
     -> AudioListener::useTimeSlice             src:271-295
        -> route[ch] -> obs_source_output_audio
```

- Il ring è un `std::vector<AudioBufferInfo>` con `count = max(8, AUDIO_OUTPUT_FRAMES*2 / buf_size)` (src:387-406), cioè circa 2 frame OBS (≈42,7 ms a 48 kHz) più il ritardo del `TimeSliceThread` (≈10 ms, src:293). [V/I]
- Il callback non alloca, ma `_write_index` è un `int` semplice letto dal thread di consumo (src:140, 275-276, 341): data race secondo il modello C++; nessun atomico, nessun contatore di drop/overwrite. [V/I]
- Il ring è multi-consumer con un `read_index` per sorgente; una sorgente lenta viene superata dal producer. [V]
- Dimensione buffer/canali: `ch_count = device->getActiveInputChannels().countNumberOfSetBits()` (src:390); il buffer è `AudioBuffer<float>(ch_count, buf_size)`. La callback copia `numInputChannels` (compatto) e non i 10 canali nominali. [V]

### 4.4 Canali, route e formati

- `getInputChannelNames()` restituisce i nomi ASIO, indice `StringArray` = indice canale 0-based (JUCE `reloadChannelNames`). [V]
- La UI route ha "Mute" = -1 e poi i nomi con valore 0-based (`fill_out_channels_modified`, src:761-773). [V]
- La route è applicata per canale di uscita OBS: `out.data[i] = data[route[i]]` se valido, altrimenti buffer silenzioso (src:179-206). Non c'è somma, gain o pan: è una mappa 1:1 o mute. [V]
- Layout OBS limitato a Mono..7.1 (src:124-134); il numero di route è limitato ai canali del layout registrato (src:661-671). Non è un modello adatto a una matrice multicanale. [V]
- Formato interno: `AUDIO_FORMAT_FLOAT_PLANAR` (src:184, 397). [V]
- Apertura: **tutti** gli input e **tutti** gli output (`in.setRange(0, in_chs.size(), true); out.setRange(0, out_chs.size(), true)`, src:622-627), poi `open(in, out, getCurrentSampleRate(), getCurrentBufferSizeSamples())` (src:631-633). Gli output non vengono scritti (`UNUSED_PARAMETER(outputChannelData)`, src:344-345). [V]

### 4.5 Condivisione tra sorgenti e tra processi

- Due sorgenti OBS che scelgono lo stesso device trovano lo stesso `AudioCB` nel registro `callbacks` (src:594-609) e quindi lo stesso `AudioIODevice` e lo stesso ring; ogni sorgente ha il proprio `AudioListener` e il proprio `read_index` (src:271-295). **Fan-out, non apertura per sorgente.** [V]
- Non c'è reference counting: il device resta aperto fino a `obs_module_unload` (src:904-926); `remove_client` non chiude il device. [V]
- **Non esiste condivisione cross-process**: nessuna shared memory, named mutex, IPC o arbitraggio. Ogni processo chiama `init`/`createBuffers` sul driver; il multi-client è una proprietà del driver UAD. [V]
- Il README del plugin usa "multi-client" per indicare più sorgenti OBS sullo stesso device (README:15-20); non è una garanzia di multi-apertura multi-processo. [V]

### 4.6 Limiti e difetti rilevanti

1. **Data race sul ring** (`_write_index` e buffer condivisi senza atomici) — non copiare questo schema. [V/I]
2. **Nessuna gestione della riconnessione**: `audioDeviceStopped/Error` segnano i client come disconnessi, ma non riaprono; dopo un reset/rate change la cattura può restare morta finché non si cambia device o si ricrea la sorgente. [V/I]
3. **Nessun hotplug**: `scanForDevices()` solo al load (src:875). [V]
4. **Apertura di tutti gli output**: può aumentare i conflitti con REAPER/altri host; il codice non li scrive, ma li richiede. [V/T]
5. **Nessuna compensazione di latenza ASIO**: `getInputLatencyInSamples()` non è usato. [V]
6. **CMake non riproducibile**: `FetchContent` punta al branch `open_asio_juce` (CMakeLists:8-14), non a un commit/tag; `qt_add_resources(... ${win-asio_QRC})` usa una variabile non definita (`win-asio_QRC` invece di `obs-asio_QRC`, CMakeLists:51 e 67); `cmake_minimum_required(3.5)` è troppo vecchio per JUCE 7/9. [V]
7. **Build OBS-dipendente**: modulo OBS (`OBS_DECLARE_MODULE`, `obs_source_output_audio`, Qt main window) e GPL-3.0; non è un host standalone. [V]

### 4.7 Riuso per NL Audio Matrix

**Riusare come comportamento/API, non come codice:**
- un solo device ASIO per driver, tutti i canali enumerati e selezionabili singolarmente;
- indice canale 0-based distinto dall'etichetta UAD 1-based;
- apertura di tutti gli input, output trattati con cautela;
- callback planare `float**`;
- pannello di controllo del driver.

**Reimplementare:**
- DeviceManager multi-backend, non un registro globale per un solo tipo;
- ring SPSC atomici per device, non un ring condiviso multi-consumer;
- routing N×M con somma, gain, pan, mute, solo, polarità, bus dinamici;
- clock indipendenti, SRC adattivo, disaccoppiamento;
- lifecycle, hotplug, recovery, diagnostica;
- driver/endpoint virtuali.

---

## 5. JUCE 9.0.4 — capacità e limiti

### 5.1 Versioni e licenza

- `JUCE_MAJOR_VERSION 9`, `JUCE_MINOR_VERSION 0`, `JUCE_BUILDNUMBER 4` (`juce_StandardHeader.h:42-44`); `project(JUCE VERSION 9.0.4)` (`CMakeLists.txt:37`).
- Licenza duale **AGPLv3 / commerciale** (`LICENSE.md:1-53`). Il README (righe 120-123) avverte che un assistente AI deve segnalare la possibile necessità di licenza commerciale. [V]
- SDK ASIO incluso in `modules/juce_audio_devices/native/asio/` con licenza Steinberg **GPLv3 o proprietaria**; distribuzione proprietaria richiede accordo Steinberg. [V]

### 5.2 ASIO

- `maxNumASIODevices = 16`, `currentASIODev[16]` (`juce_ASIO_windows.cpp:317-319`); `createDevice` cerca uno slot libero (`:1521-1530`). Più device ASIO sono possibili **nello stesso processo**, ma ogni driver decide se e come condividerli. [V]
- Un `currentCallback` per device (`:772`); `start()` sostituisce il callback precedente (`:665-687`). JUCE non fa multiplexing: `AudioDeviceManager` è il livello che espone `addAudioCallback` e somma gli output (`juce_AudioDeviceManager.cpp:1089-1156`). [V]
- Layout callback: ASIO double-buffered per canale → conversione a float planare `float**`, `numActiveInputChans`, `numSamples` (`juce_ASIO_windows.cpp:1349-1360`). [V]
- Nomi canali: `getInputChannelNames()` costruito da `ASIOChannelInfo.name`; indice `i` = canale ASIO 0-based (`:799-833`). [V]
- Reset/rate change: JUCE gestisce `kAsioResetRequest` con timer 500 ms, `close(); open(...); start(oldCallback)` (`:718-747`, `:1389-1393`, `:1429-1433`). [V]
- `MessageManager` crea un hidden window e imposta `juce_messageWindowHandle` (`juce_Messaging_windows.cpp:56-57`); un engine console che inizializza `MessageManager` può quindi passare un HWND valido ad ASIO. [V]

### 5.3 WASAPI

- Sono presenti `IMMNotificationClient` (`juce_WASAPI_windows.cpp:170-185`), `IAudioClient3` (`:252`), `IAudioCaptureClient` (`:259`), notifiche endpoint (`:1904`). [V]
- **Non è presente `AUDCLNT_STREAMFLAGS_LOOPBACK`** nel backend JUCE (grep senza risultati). Per il loopback capture serve un backend WASAPI nativo. [V]
- `AudioDeviceManager` gestisce un solo device per volta; per più device servono host dedicati. [V]

### 5.4 Limiti da documentare

- 16 slot ASIO per processo (limite JUCE, superabile con patch/fork o più processi, non con il routing engine).
- Nessun multi-client: il driver ASIO/X decide.
- `AudioDeviceManager` non è pensato per più device simultanei; il progetto userà un DeviceManager proprio.
- WASAPI JUCE non copre il loopback; backend nativo necessario.
- Licenza AGPL/commerciale e SDK ASIO da risolvere prima della distribuzione.

---

## 6. SysVAD — analisi

### 6.1 Identità e stato del subset

- Sample Microsoft **SysVAD Virtual Audio Device Driver**, WDM/PortCls con WaveRT e offload; README:3, 19-21. [V]
- Il subset locale contiene solo `sysvad/` (188 file, ~2,2 MB), senza repo root, **senza LICENSE**, **senza WIL** e **senza `TabletAudioSample.inx`** legacy; non è buildabile così com'è. [V]
- Licenza upstream **MS-PL** (verificata sul repo Microsoft); conservare notice e obblighi. [V]
- Target progetto: VS2019/v142, WDK, `DriverType=KMDF`, `WindowsKernelModeDriver10.0`; INF target Windows 11 22H2. [V]

### 6.2 Endpoint già disponibili

`TabletAudioSample/minipairs.h:510-533`:

- render: `SpeakerMiniports` (`:164-187`), `SpeakerHpMiniports` (`:212-234`), `HdmiMiniports` (`:259-...`), `SpdifMiniports`;
- capture: `MicInMiniports` (`:352-...`), `MicArray1/2/3Miniports` (`:395-...`, `:439-...`, `:483-503`);
- `g_MaxMiniports = (4+4)*2` (`:539`).

Ogni endpoint è una coppia topo+wave installata da `CAdapterCommon::InstallEndpointFilters` (`common.cpp:2492-2640`), con `CLSID_PortWaveRT` (`:2571-2586`), interfaccia `KSCATEGORY_AUDIO` (`:1669-1673`) e `PcRegisterSubdevice` (`:1863-1868`). I nomi `TopoName`/`WaveName` devono corrispondere alle `KSNAME_*` dell'INF (`common.cpp:2543, 2572`). [V]

### 6.3 Percorso PCM

- Buffer WaveRT: `AllocateAudioBuffer` / `AllocateBufferWithNotification` allocano pagine via PortCls e le mappano in kernel (`minwavertstream.cpp:528-605`, `:785-840`); `m_pDmaBuffer` è la vista kernel del buffer che l'audio engine legge/scrive. [V]
- Clock: posizione simulata da QPC; `UpdatePosition` calcola `ByteDisplacement` dal tempo (`:1397-1493`); timer da 1 ms in `SetState(RUN)` (`:1312-1363`); `TimerNotifyRT` (`:1729-1854`) segnala gli eventi di notifica. [V]
- Capture: `UpdatePosition` → `WriteBytes` (`:1497-1523`) → `m_ToneGenerator.GenerateSine(...)` (`:1519`). Il capture è un **tono**, non audio reale; il loopback è anch'esso un tono (README:209-213). [V]
- Render: `ReadBytes` (`:1527-1554`) scrive su `CSaveData` solo se `!g_DoNotCreateDataFiles`; default `g_DoNotCreateDataFiles=1` (`adapter.cpp:252-253`), quindi il render viene **scartato**. [V]
- **Nessun trasporto kernel↔user-mode**: grep su tutto il tree per `IRP_MJ_DEVICE_CONTROL`, `IoCreateDevice`, `WdfIoQueueCreate`, `ZwCreateSection`, `MmMapLockedPages`, `WdfControlDeviceInitAllocate` → nessun risultato. [V]

### 6.4 Punti di inserimento per NL Audio Matrix

| File:funzione | Righe | Modifica |
|---|---:|---|
| `minwavertstream.cpp:WriteBytes` | 1497-1523 | capture: leggere dal ring del nostro endpoint invece del tone generator; silenzio su underrun |
| `minwavertstream.cpp:ReadBytes` | 1527-1554 | render: scrivere il DMA nel ring invece di `CSaveData`; drop-oldest su overrun |
| `minwavertstream.cpp:TimerNotifyRT` | 1729-1854 | aggiornare heartbeat/contatori e segnalare il trasporto |
| `minwavertstream.cpp:SetState` | 1149-1372 | reset ring su STOP/RUN; binding stream↔endpoint |
| `minwavertstream.cpp:AllocateAudioBuffer` | 785-840 | registrare buffer/dimensione/notifiche presso l'endpoint |
| `minwavert.cpp:NewStream` | 633-751 | associare lo stream all'endpoint e allo stato di trasporto |
| `common.cpp:InstallEndpointFilters` | 2492-2640 | creare/distruggere il trasporto per endpoint |
| `common.cpp:Init/Cleanup` | 743-844 / 2327 | inizializzazione globale del trasporto |
| `adapter.cpp:DriverEntry/StartDevice` | 498-610 / 1012-1148 | control device e init trasporto |
| `common.h:ENDPOINT_MINIPAIR` | 377-412 | aggiungere ID/trasporto all'endpoint descriptor |

### 6.5 Build, firma, limiti

- Richiede VS + WDK, ATL v142, WIL; test signing con Secure Boot disabilitato; distribuzione con attestazione/WHQL e certificato EV. [V/I]
- Il sample è "Universal" e non implementa mixing/loopback reali (README:207-213); non promette HLK. [V]
- Il numero di endpoint è statico rispetto al set compilato/INF; per aumentarlo servono tabelle + INF + rebuild/reinstall. Non c'è creazione arbitraria a runtime. [V/I]
- Licenza MS-PL: derivati sorgente sotto MS-PL; conservare copyright; nessun uso del nome/trademark Microsoft. [V]

---

## 7. MicyWMD — analisi

### 7.1 Identità e endpoint

- Derivato Microsoft **SimpleAudioSample** (PortCls/WaveRT), non SysVAD; un solo endpoint **capture** "MicyAudio Microphone" (`MicyAudio.inx:127-134`, `:209-210`). [V]
- Formato fisso: 48 kHz, 2 canali, 32-bit PCM, `WAVE_FORMAT_EXTENSIBLE` (`micarraywavtable.h:21-24`, `:33-61`); nessuna SRC. [V]
- `MAX_INPUT_STREAMS 1`; una sola istanza adapter (`common.cpp:371-380`). [V]
- Il render "MicyAudio Speaker" esiste solo come stringa/INF orfana; nessun subdevice render installato (`adapter.cpp:734-756`; `minipairs.h:98-104`). [V]

### 7.2 Trasporto PCM

- Control device `\Device\MICY` + symlink `\DosDevices\MicyAudio` (`adapter.cpp:26-31`, `:505-552`); IOCTL `CTL_CODE(29, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)` (`:30-31`), gestito da `DeviceControlHandler` (`:927-1029`). [V]
- Ring globale in non-paged pool di `7680*4 = 30.720` byte (`adapter.cpp:552`, struct `:78-88`), protetto da spinlock; `UserPcmBuffer_Write` (`:131-158`), `Read` (`:160-180`), `Clear` (`:188-197`). [V]
- Consumo: `CMiniportWaveRTStream::WriteBytes` (`minwavertstream.cpp:1404-1442`) legge dal ring e riempie il DMA; silenzio sul residuo. [V]
- Render: `ReadBytes` è commentato (`:1446-1473`); nessun percorso render→user. [V]

### 7.3 Difetti critici

1. **OOB write/OOB read nel ring**: `UserPcmBuffer_Write` non clampa `toWrite`; se `length > capacity` la copia può superare il buffer; `count` può superare `capacity`, e letture successive possono copiare oltre il buffer verso il DMA. Il demo invia 48.011 byte su 30.720. [V/I]
2. **Device object check commentato** (`adapter.cpp:965-973`): l'IOCTL può essere accettato su oggetti audio; unknown IOCTL inoltrato a `PcDispatchIrp` su un control device senza device extension (`:1015-1018`) → rischio crash. [V/I]
3. **SDDL World** nel INF (`MicyAudio.inx:146-153`) e `FILE_ANY_ACCESS`: superficie di iniezione ampia. [V]
4. **Nessun controllo di formato/versione**: i byte sono copiati grezzi; header del demo interpretato come PCM; 16-bit del demo vs 32-bit dell'endpoint. [V]
5. **Nessun feedback di underrun/overrun o pacing**: ring da 80 ms, drop del più vecchio, nessun evento all'app. [V]
6. **Nessun render**: inadatto come base per `NL Virtual Speaker`. [V]

### 7.4 Riuso

- Solo riferimento per: control device/IOCTL, ring non-paged, inserimento in `WriteBytes`.
- Non riusare: sicurezza, overflow, formato, singolo endpoint, globalità, demo.

---

## 8. SweetlyAudioDrivers — analisi

### 8.1 Identità e modello

- Derivato **MSVAD** legacy (WaveCyclic + `IDmaChannel`), non WaveRT; 2 commit, import unico. [V]
- Quattro device model (`Root\SWLYSessionOutput/Input/OutputB/InputB`), due servizi/binari (`vadsimpl.sys`, `vadsimpl_b.sys`) (`msvad.inf:21-24`, `:38-39`, `:84-85`, `:184`, `:191`). [V]
- Ogni device è un adapter con topo+wave; ogni wave filter ha un pin capture e uno render, con limite di 1 stream per direzione (`simple/simple.h:26-36`). [V]
- **FIFO statica per immagine driver** (`basewave.h:161-164`, `basewave.cpp:9`): una per `vadsimpl.sys`, una per `vadsimpl_b.sys`; tutti gli endpoint di un binario condividono lo stesso ring. Al massimo 2 percorsi audio indipendenti. [V]

### 8.2 Loopback

- Render `CopyTo`: copia client→DMA e scrive il ring (`basewave.cpp:1219-1232`).
- Capture `CopyFrom`: legge dal ring nel DMA e copia al client (`:1234-1249`); `Silence` scrive zeri nel ring quando il render è attivo (`:1210-1216`). [V]
- Ring 65.536 B (`basewave.cpp:7`, `:22-28`), circa 341 ms a 16-bit/48k o 171 ms a float/48k; nessun controllo di drift/latenza; drop del più vecchio su overflow (`:75-84`), silenzio su underflow (`:124-127`); nessun reset su stop/start. [V]
- Formato: 2 canali, 16-bit o float32, solo 48 kHz (`simple/simple.h:26-36`, `simple/wavtable.h:18-66`); il ring non ha tag formato → PCM16 e float mischiati si corrompono. [V]
- Render stream creation esegue `CSaveData::Initialize` e crea `C:\STREAM_<n>.wav` anche se il dump è disabilitato (`basewave.cpp:745-757`; `savedata.cpp:41, 451, 520-563`); se la creazione fallisce, lo stream fallisce. [V]

### 8.3 INF e installazione

- `%PKEY_AudioEndpoint_Supports_EventDriven_Mode%` usato ma **non definito** in `[Strings]` (`msvad.inf:50,73,96,119`); proprietà EP nel key del device, non nell'interfaccia; `KSNAME_*` (`SweetlyWaveSessionOut`, ecc., `:228-236`) non corrisponde ai nomi subdevice `Wave`/`Topology` (`common.cpp:1151,1194`). [V]
- Due binari dalla stessa sorgente; il progetto di default produce solo `vadsimpl_b.sys` (`simple/vadsimpl.vcxproj:175-177`), il packaging non include l'INF (`Package/package.VcxProj:146-153`). [V]
- Licenza MIT-0 dichiarata su codice Microsoft MSVAD (MS-PL): compliance dubbia. [V]

### 8.4 Riuso

- Solo riferimento per l'idea di loopback render→capture e per i difetti da evitare.
- Non è una base valida per 4+2 endpoint indipendenti: il ring statico e il modello WaveCyclic non scalano.

---

## 9. Confronto basi driver

| Criterio | SysVAD | MicyWMD | Sweetly |
|---|---|---|---|
| Modello | PortCls/WaveRT, multi-endpoint | SimpleAudioSample/WaveRT, 1 capture | MSVAD/WaveCyclic, 2 FIFO |
| Endpoint già disponibili | 4 render + 4 capture | 1 capture | 2 percorsi (2 binari) |
| Multi-endpoint indipendenti | Sì, tabelle/INF | No | No (FIFO condivisa) |
| Trasporto user-mode | Assente | IOCTL + ring globale, insicuro | Assente |
| Render→user | Assente (scarto/dump) | Assente | Loopback kernel, non user-mode |
| Formati/rate | Tabelle, 44.1/48/96 | 48k/2ch/32-bit | 48k/2ch/16 o float |
| Licenza | MS-PL | MIT + base MS-PL | MIT-0 dichiarata + base MS-PL |
| Build | WDK, richiede WIL/INX upstream | VS2022+WDK, KMDF | VS2022+WDK, packaging rotto |
| Verdetto | **Base scelta** | Riferimento per ring/IOCTL | Riferimento negativo |

---

## 10. Requisiti universali e implicazioni tecniche

| Requisito | Implicazione | Evidenza/limite |
|---|---|---|
| Tutti i 10 ingressi Apollo acquisibili e identificabili | Enumerare `getInputChannelNames()`, aprire tutti gli input, mappare indice compatto↔canale assoluto, registrare/verificare ogni canale | OBS-ASIO apre tutti gli input (`src:622-627`); la mappa route è 0-based (`:761-773`). Il conteggio effettivo va verificato in P1. |
| Tutte le uscite ASIO enumerate e distinte | `getOutputChannelNames()`, descrittori separati input/output, nessuna scrittura forzata | JUCE espone output names; OBS-ASIO apre tutti gli output ma non scrive (`src:622-627`, `:344-345`). |
| Multi-scheda ASIO/WASAPI | DeviceManager con N backend; nessun device globale; apertura indipendente; gestione conflitti | JUCE consente 16 device ASIO in-process (`juce_ASIO_windows.cpp:317-319`); nessuna gestione multi-client. WASAPI nativo per loopback (JUCE non ha `AUDCLNT_STREAMFLAGS_LOOPBACK`). |
| Clock indipendenti, rate diversi | Scheduler dedicato + ring per device + SRC adattivo; nessun clock comune assunto | SysVAD usa QPC; ASIO usa clock hardware; WASAPI endpoint clock; tutti possono derivare. |
| Bus dinamici, N×M, multicanale | Grafo con bus configurabili e matrice canali; niente 4 fisso | Il modello OBS-ASIO è 1:1/mute e limitato ai layout (`src:179-206`, `:124-134`): inadeguato. |
| Endpoint virtuali multipli e provisioning | Driver SysVAD con set configurabile; motore legge il set, nessun hardcode | SysVAD ha endpoint statici da tabelle/INF (`minipairs.h:510-533`); aggiunta = rebuild/reinstall, non runtime. |
| Routing bidirezionale e mix-minus | Grafo con sorgenti/sink di device, bus, virtuali; anti-feedback; output non invasivi | SysVAD non ha trasporto; Micy non ha render; Sweetly ha solo loopback kernel. |
| Funziona senza ASIO | Scheduler del grafo indipendente dal device; TestBackend e device WASAPI/virtuali | OBS-ASIO dipende dal device ASIO; il nuovo motore non deve. |

---

## 11. Componenti riutilizzabili e licenze

| Componente | Uso | Licenza/condizione |
|---|---|---|
| JUCE 9.0.4 (ASIO, GUI, core) | App engine/GUI | AGPLv3 o commerciale; SDK ASIO GPLv3/Steinberg |
| SysVAD (WaveRT, endpoint, INF) | Fork driver | MS-PL; conservare notice; nuovi file compatibili |
| Pattern OBS-ASIO (fan-out, route 0-based, apertura canali) | Riferimento comportamentale | GPL-3.0: **nessun codice copiato** |
| Micy (ring→WaveRT) | Riferimento per `WriteBytes` | MIT + base MS-PL; non copiare il trasporto insicuro |
| Sweetly (loopback) | Riferimento negativo / idea | MIT-0 dichiarata + base MS-PL; non riusare |
| WASAPI nativo | Backend input/output/loopback | Windows SDK, nessuna licenza aggiuntiva |
| ReaRoute | Backend opzionale | Licenza REAPER/Cockos; verificare prima dell'uso |

---

## 12. Criticità e limiti effettivamente riscontrati

| # | Gravità | Problema | Evidenza |
|---|---|---|---|
| 1 | Bloccante (P1) | Coesistenza ASIO con REAPER non garantita dal codice; dipende dal driver UAD | OBS-ASIO non ha condivisione cross-process (`src:594-609`, `:904-926`); serve test P1 |
| 2 | Alta | OBS-ASIO non compilabile con JUCE 9; fork 7.0.2/openASIO | `src:331`, `:438`; JUCE 9 usa `audioDeviceIOCallbackWithContext`, `startThread(Priority)` |
| 3 | Alta | SysVAD non ha trasporto user-mode; capture tono, render scarto | `minwavertstream.cpp:1497-1554`; grep IOCTL zero |
| 4 | Alta | Micy: OOB/overflow nel ring; device check commentato; SDDL World | `adapter.cpp:131-158`, `:965-973`; `MicyAudio.inx:146-153` |
| 5 | Alta | Sweetly: FIFO statica condivisa, solo 2 percorsi; INF rotto; `C:\STREAM` | `basewave.h:161-164`; `msvad.inf:50`; `savedata.cpp:451` |
| 6 | Media | JUCE ASIO max 16 device; nessun multi-client | `juce_ASIO_windows.cpp:317-319` |
| 7 | Media | JUCE WASAPI senza loopback | grep `LOOPBACK` assente |
| 8 | Media | Subset SysVAD non buildabile (WIL/INX/LICENSE mancanti) | listing; README:66-74, 110, 189-195 |
| 9 | Media | Toolchain: WDK/CMake mancanti | verifica ambiente |
| 10 | Media | Licenze da decidere prima della distribuzione | LICENSE file; JUCE/Steinberg/MS-PL |

---

## 13. Architettura consigliata sulla base del codice

1. **DeviceManager multi-backend** con `AsioBackend` (JUCE 9), `WasapiBackend` nativo, `VirtualEndpointBackend` (driver), `ReaRouteBackend` opzionale, `TestBackend`.
2. **GraphScheduler dedicato** (thread ad alta priorità, timer ad alta risoluzione, MMCSS): il grafo funziona anche senza ASIO; i device sono asincroni con ring e SRC adattivo. Confronto con callback master in `TECHNICAL_DECISIONS.md`.
3. **Ring per device** con capacità di sicurezza e riempimento operativo adattivo (non un target fisso del 50%); latenza selezionabile.
4. **Grafo dinamico** con nodi device/bus/virtuali, routing molti-a-molti, bus multicanale configurabili, mix-minus e anti-feedback.
5. **Driver SysVAD fork** con endpoint capture/render provisionabili (massimo di build + set iniziale 4+2), trasporto non-paged + MDL + IOCTL; nessun hardcode nel motore.
6. **Politica output non invasiva**: Apollo output enumerati ma non scritti di default; nessuna sottrazione forzata a REAPER/Console; rotte output solo esplicite; WASAPI come percorso bidirezionale sicuro; ritorno piattaforme direttamente ad Apollo resta il default.
7. **Licenze**: nessun codice GPL copiato; JUCE/ASIO da decidere; driver MS-PL.

Dettaglio completo in `ARCHITECTURE.md` e `TECHNICAL_DECISIONS.md`.

---

## 14. Domande aperte e test sul PC

1. Con REAPER aperto, l'app Matrix può aprire il device UAD in input-only e in input+output? In quali ordini e con quali buffer/rate? (P1)
2. I 10 canali Apollo sono realmente tutti presenti a 48 kHz? Quali nomi/indici? A 96/176.4/192 kHz quanti e quali? (P1)
3. Ogni canale acquisito corrisponde al segnale di test atteso? Verifica con toni/PRN separati e cross-correlazione. (P1)
4. Aprire gli output Apollo (anche solo con maschera, senza scrivere) disturba REAPER/Console? (P1)
5. Quanti device ASIO diversi possono essere aperti contemporaneamente sullo stesso PC? (P5)
6. WASAPI loopback e shared mode funzionano con le app target (Zoom/Source-Connect/browser)? (P5/P6)
7. Quale massimo di endpoint virtuali è praticabile su questa macchina (driver/PnP/audio engine)? (P3/P4)
8. Qual è il jitter reale del timer ad alta risoluzione e la latenza percepita nei modi low/balanced/safe? (P2/P5)
9. Il driver SysVAD fork è stabile con Driver Verifier, sleep/resume e unplug? (P3/P7)
10. Quale combinazione di licenze (JUCE/Steinberg) è compatibile con la distribuzione scelta? (decisione utente)

---

## 15. Appendice — comandi e hash di verifica

```powershell
git -C 'K:\AI\NL Audio Matrix\obs-asio' rev-parse HEAD
# a250623e56debca699d5c60e1d6ff676aaa1002e
git -C 'K:\AI\NL Audio Matrix\obs-asio' describe --tags --always
# v3.2.1f-1-ga250623
git -C 'K:\AI\NL Audio Matrix\obs-asio' diff --stat v3.2.1f..HEAD
# README.md | 2 +-
git -C 'K:\AI\NL Audio Matrix\obs-asio' ls-tree -r v3.2.1f -- src
# 100644 blob 3851c4ad787e9b1d504ec6292d2e873ebec3775b src/asio-input.cpp
git -C 'K:\AI\NL Audio Matrix\obs-asio' hash-object src/asio-input.cpp
# 3851c4ad787e9b1d504ec6292d2e873ebec3775b
```

Riferimenti runtime: Windows 11 26300; VS 2022 BuildTools; WDK assente; CMake assente; registry ASIO con Universal Audio USB, ReaRoute, ASIO4ALL, FlexASIO, VB-Matrix, Source Elements.

---

**Esito:** l'audit conferma che la documentazione precedente era corretta sull'identità OBS-ASIO ma incompleta su multi-device, clock, scheduling, provisioning endpoint e limiti JUCE/WASAPI. Le decisioni aggiornate sono in `TECHNICAL_DECISIONS.md`; l'architettura in `ARCHITECTURE.md`; il piano in `MASTER_PLAN.md` e `IMPLEMENTATION_ROADMAP.md`; i test in `TEST_PLAN.md`; il backlog in `TASKS.md`.
