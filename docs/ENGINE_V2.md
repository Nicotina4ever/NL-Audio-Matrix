# ENGINE V2 — buffering, clocking, telemetry and recovery

Status: implemented 2026-10-27. This document describes the audio-engine architecture
that replaced the original fixed-pacing writer. It is the reference for the buffer
modes exposed in the GUI and for the METERS fields.

## Problem it solves

The original router clocked the graph from the master ASIO callback and fed device
outputs with fixed 10 ms sleeps and a fixed 30 ms ring target. Under Windows / VM
scheduling jitter the destination ring starved, the converter zero-filled whole
blocks and the recorded product showed periodic digital-silence gaps
(real voice test: 21 gaps / 327 ms in 10 s; destination underruns 13-63 per run in
the A/B control). The fix is architectural, not "bigger buffers everywhere":

* the device feeder thread runs as a level servo fed back by the kernel queue,
  so it never assumes a scheduling deadline it cannot meet;
* the buffer mode is an explicit latency budget chosen at start (Low / Balanced /
  Safe / Advanced), primed once, never silently grown (growing a primed buffer
  requires inserting silence);
* every stage exposes counters so a dropout can be attributed to the engine, the
  kernel transport or upstream (ASIO driver / VM), instead of guessing.

## Data path

    source device threads            graph (master ASIO callback)         destination device threads
    ASIO input  ──────────────┐                                          ┌── master ASIO outputs
    extra ASIO input ─ ring ──┤   N×M matrix, buses, mix-minus,          ├── extra ASIO outputs (ring)
    WASAPI loopback ─ ring ───┼─  gain/pan/mute/solo, metering,   ───────┼── WASAPI render (ring)
    WASAPI capture  ─ ring ───┤   test generators, route snapshot       └── NLMatrix driver capture
    process capture ─ ring ───┤                                          (ring + kernel queue)
    driver render   ─ ring ───┘

* The master ASIO callback is the graph clock. Every device that is not the
  master exchanges audio through SPSC rings; its callback only moves samples.
* The callback performs no allocation (pointer views are pre-allocated per node),
  no I/O, no logging; FP flush-to-zero is enabled on the audio thread.
* The routing snapshot is built by the control thread and published atomically;
  callbacks only read it (bounded live-update latency, no locks in the audio path).

## Two-stage destination buffering

1. graph ring (producer = graph callback / extra-ASIO callback, consumer = device):
   * target = `ringTargetMinMs` floored by 2.5x the real ASIO buffer period
     (a 20 ms FlexASIO period yields a 50 ms floor; Focusrite 256 yields 60 ms
     from the profile). This is the total buffered audio of the path.
   * conversion to the device rate uses linear interpolation plus a +/-300 ppm
     fill trim. The trim target is `ringTarget - kernelQueue`, i.e. the
     steady-state ring occupancy; targeting the full ring would slowly increase
     latency for minutes after start (observed and fixed).
2. NLMatrix driver kernel queue (consumer = Windows capture engine via WaveRT):
   * target = `driverTargetMs`, set per endpoint with
     `NLMATRIX_IOCTL_SET_CAPTURE_TARGET` (ABI v3; default 30 ms if unset).
   * the feeder thread wakes every `servoIntervalMs` (2 ms), reads the kernel
     queue state from GET_INFO (`CaptureUsedBytes` is authoritative, updated on
     write and on DMA pull), and writes exactly the deficit, capped at
     `writeChunkMs` per iteration. A late wake therefore causes a catch-up write
     instead of a silence gap.
   * MMCSS "Pro Audio", THREAD_PRIORITY_HIGHEST, 1 ms timer resolution.
   * if the kernel reports dropped bytes (queue over target), the servo backs off
     one tick instead of pushing more audio.
   * if no capture client is draining the endpoint for >500 ms, the graph ring is
     drained to the target so a later-opened application starts near the
     configured latency instead of receiving up to a ring-capacity of stale audio.
   * on driver (re)connect the ring is drained to target and the converter reset;
     the capture target is re-applied.

WASAPI render destinations use the same ring + a callback-driven converter with
pre-roll; additional ASIO devices use per-device rings with the same policy.

## Buffer modes (defaults, milliseconds)

| mode | ring min..max | driver target (max) | extra ASIO | render | write chunk | servo |
|---|---|---|---|---|---|---|
| low | 20..60 | 25 (80) | 20 | 20 | 5 | 2 |
| balanced | 60..200 | 50 (250) | 50 | 50 | 5 | 2 |
| safe | 150..400 | 100 (400) | 100 | 100 | 10 | 2 |
| advanced | `ringTargetMinMs`, `ringTargetMaxMs`, `driverTargetMs`, `driverTargetMaxMs`, `extraAsioTargetMs`, `renderTargetMs`, `writeChunkMs`, `servoIntervalMs` explicit; 0/missing keeps the balanced value |

Profile JSON (v3) carries the policy in a top-level object. Missing object means
balanced; changing it is a topology change (the engine restarts, it is not applied
live):

    "engine": {
      "bufferMode": "balanced",
      "ringTargetMinMs": 60, "ringTargetMaxMs": 200,
      "driverTargetMs": 50, "driverTargetMaxMs": 250,
      "extraAsioTargetMs": 50, "renderTargetMs": 50,
      "writeChunkMs": 5, "servoIntervalMs": 2
    }

Latency budget: the graph ring target is the total buffered audio; the kernel
queue is carved out of it, it is not an additional delay. METERS reports, per
destination, `estimatedPipelineMs = ringTargetMs + ASIO period + 5 ms` (measured
values in the VM: 85 ms balanced, 175 ms safe). A true impulse-based end-to-end
measurement is prepared (click WAV + player/recorder start_ms) and listed in the
backlog.

## Xrun policy (no masking)

* Zero-fill happens only when data genuinely does not exist (upstream ASIO
  callback not delivered, device disconnected). The engine never conceals missing
  frames as if they were processed.
* No automatic buffer growth after start: increasing a primed buffer would
  require inserting silence. If the measured callback jitter exceeds the selected
  mode's ring margin, the router logs a `Buffer warning` and METERS exposes
  `callbackGapMaxMs`, so the operator can choose a safer mode.
* Every stage is counted: destination converter underruns, feeder starved ticks,
  kernel dropped/underrun bytes, ring dropped frames, upstream frame deficit.

## Telemetry (METERS JSON, 10 Hz, also written to the telemetry file)

Top level: `callbacks`, `framesProcessed`, `expectedFrames`, `frameDeficit`
(positive = ASIO callback frames the driver never delivered), `lastCallbackAgeMs`,
`callbackGapMaxMs` (max interarrival over the last second), `engineHealthy`,
`engineState` (starting/running/restarting/error), `asioError`, `asioRestarts`,
`engineRate`, `engineBuffer`, `bufferMode`, `tuning`, `uptimeS`.

Per destination node: `underruns` (converter), `writerStarved`, `writes`,
`maxWakeMs`/`lastWakeMs` (feeder wake gap), `maxWriteUs`, `getInfoUs`,
`reconnects`, `ringFillMs`/`ringTargetMs`, `driverTargetMs`/`driverUsedMs`,
`driverDroppedBytes`, `driverUnderrunBytes`, `driverPulledBytes`,
`driverConsumerActive`, `estimatedPipelineMs`.

## Recovery and disconnection handling

* Control-loop watchdog (10 Hz): no callback for >2 s or ASIO error -> stop/start
  the ASIO device (minimum 5 s between attempts), state machine
  starting/running/restarting/error reported in METERS. `lastCallbackMs == 0`
  is "starting", not "stalled".
* Driver client reconnect with 250 ms retry; stale ring audio drained on connect.
* WASAPI loopback/capture/render endpoints and process sources have reconnect
  watchdogs with stale-audio drain on reconnect (endpoint removal, USB, app
  restart).
* Engine continues with remaining devices while one is offline; GUI shows
  per-node online state.

## Verification status (2026-10-27)

* Engine-only A/B on the same ABI3 driver and back-to-back VM state
  (legacy pre-refactor router vs engine v2, full-rate VM window):
  destination underruns 13-37/20 s and 63/60 s (legacy) vs **0** (engine v2);
  ring dropped frames **0** in every engine-v2 run; safe mode is the only
  configuration with zero silent gaps / zero real splices under this VM's
  measured upstream callback deficit (0.16-1.73 %). Evidence:
  `reports/engine/ENGINE_AB_REPORT.md`, `reports/engine/{legacy,engine2,engine2_safe}/`.
* Final acceptance re-run of the last binary (watchdog-startup fix + no-consumer
  drain) is blocked by a VM audio-clock regression that appeared after a host
  full power cycle (FlexASIO delivers 960-frame buffers every 40 ms = half
  real-time; independent of router binary, device, power plan, host sound
  backend pinning; null-destination run shows it too). Recorded as environment
  blocker, not PASS: `reports/engine/FINAL_ACCEPTANCE.md`,
  `reports/engine/FINAL_STATUS.md`.
* Focusrite physical path: blocked in this VM instance (VMware USB passthrough
  wedged: endpoint enumerates, ASIO open fails, WASAPI capture 0 frames).
  Requires a physical USB re-plug by the operator; then run the Focusrite rows
  of `tools/vm/Run-ContinuityTest.ps1` for both the legacy and engine-v2
  binaries. Historical pre-fix evidence remains the real-voice recording
  (21 gaps / 327 ms in 10 s).
* GUI buffer-mode controls and buffering diagnostics: implemented and linked
  (`app/src/preferences_panel.h`, `app/src/ui_main.cpp`); runtime interaction
  test pending VM repair.

## Multi-ASIO channel domain and adaptive clocking (2026-10-27)

* Every ASIO device is an independent backend instance with its own opened
  channel inventory. A source/destination bound to a non-master device is
  validated against that device's `getOpenedInputs()/getOpenedOutputs()`; the
  master's channel count is not used as a global limit.
* Sources fed by an extra-ASIO or asynchronous WASAPI/process/driver clock and
  destinations consumed by an extra-ASIO clock always pass through
  `MatrixRateConverter`, even when nominal rates match. The converter's
  adaptive trim absorbs clock drift; direct same-rate ring reads would drift
  until a drop/underrun.
* METERS exposes per-node `deviceId`, `rate`, `converterTrimPpm`,
  `converterUnderruns` and `asyncSource` so independent clocks are measurable,
  not inferred.
* **ASIO output ownership is explicit**: the master output mask is built from
  the `channels` of master `asio-out` destinations; no output stream is opened
  for record-only graphs. Selective masks are retried as a logged full-mask
  compatibility fallback if the driver rejects them. Inputs remain fully
  enumerated.
* UI channel selection is 1-based (driver convention); the profile/config model
  is 0-based. The conversion lives in one pair of UI helpers and is covered by
  GUI selftest checks. Arbitrary single channels remain selectable.

Evidence: GUI selftest 56/56 (VM), deterministic channel-split matrix PASS,
extra-device structural test with FlexASIO master + ReaRoute 16/16 accepting
channels 15/16 (`vm/extra_device_channels_meters.txt`). Real audio/clock proof
still requires a live second ASIO consumer or a second physical device.