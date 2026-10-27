> **NOTA v2:** riferimento storico/integrativo. Per il prodotto finale valgono README.md, AGENTS.md e docs/15–17: endpoint virtuali proprietari sono deliverable **prioritario**, VB-CABLE è solo un possibile test temporaneo.

# 03 — Architettura tecnica C++ / JUCE

## Blocchi separati
```text
         Universal Audio ASIO Driver                      Windows Audio Engine
                 |                                                 ^
      [ApolloAsioCapture]                                     [WASAPI Render]
                 |                                                 ^
         timestamp / frames / channels                         [OutputSink]
                 |                                                 ^
          [CaptureRing/SPSC] -> [Bus Mixer] -> [Drift/SRC] -> [PlaybackRing]
                                      ^                            |
                                      |                            +-> VB-CABLE CABLE Input
                                  [Router Graph]
                                      ^
                                 [GUI/Presets]

                  [Future WDM virtual endpoints / ASIO bridge]
                    add in F4, isolated from core engine
```

## Principali interfacce C++ (design, non API definite)
- `IDeviceEnumerator`: deviceType, humanName, stableId, channel descriptors, rates, buffers.
- `IAsioCapture`: `open(config)`, `start(callback)`, `stop()`, `close()`, status.
- `IPlaybackSink`: `open(endpoint, format)`, `pushFrames()`, error metrics.
- `AudioBlock`: frames, channel map, sample time/monotonic timestamp, planar/interleaved flag, float32, sequence.
- `RouterGraph`: source, bus, destination, gain/mute, channel mapping; immutable snapshot used on audio threads.
- `ClockBridge`: buffering/latency model, sample-rate conversion and drift adjustment where clocks differ.
- `Diagnostics`: atomics counters, snapshot accessed outside callbacks, file export from worker.
- `PresetStore`: serialize/configure/validate schema with atomic swap outside callback.

## Concorrenza e realtime
- Separare callback ASIO, thread rendering WASAPI e GUI/event loop.
- Allocare blocchi audio e ring buffer **prima** di `start()`.
- Evitare mutex contesi, callback await, file I/O, log o `std::vector::resize` in thread audio.
- Rendere il routing utilizzabile tramite snapshot immutabili/scambio atomic pointer + gestione lifetime verificabile.
- Non riutilizzare mai memoria audio ancora in lettura; evitare data races e dangling pointers.
- Se sink è in ritardo: contatore overrun e policy deterministica di perdita/ricampionamento, mai fermare callback ASIO.
- Gestire la creazione/chiusura dei dispositivi esclusivamente dal thread di controllo.

## Clocks
La Apollo è clock ASIO; CABLE Input è endpoint WASAPI con propria cadenza. Anche a 48k nominali gli oscillatori possono divergere. Una FIFO fissa senza gestione deriva nel lungo periodo; misurare riempimento e drift e usare ASRC o strategia equivalente documentata. Per MVP registrare FIFO fill, overrun e underrun; qualità videochiamata non deve mascherare drop.

## Gestione errori
- Device unavailable: disabilitare route interessate e informare utente senza lasciare audio non previsto.
- Sample rate change: stop ordinato, rinegoziazione, riavvio esplicito o auto solo se safe.
- Sink lost: mute e reconnect, nessuna escalation di livello.
- Bus mute/panic sempre raggiungibile, senza riaprire driver UAD.

## Gestione 1-to-many e mix-minus
Ogni app riceve bus dedicato. Non dare per scontato che Source-Connect o il browser consenta di selezionare un ASIO driver; usa endpoint WDM disponibili come `CABLE Output` oppure futuro virtual mic.
