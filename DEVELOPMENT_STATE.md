# DEVELOPMENT_STATE — NL Audio Matrix

**Updated:** 2026-10-27 21:25 Europe/Rome
**Objective:** complete the audio engine, real Focusrite validation and professional GUI without false PASSes.

## Engine v2 (2026-10-27) — main result of this cycle

Architecture and configuration: `docs/ENGINE_V2.md`. Measurements:
`reports/engine/ENGINE_AB_REPORT.md`. Final status: `reports/engine/FINAL_STATUS.md`.

What changed:

- Destination writers rewritten as level servos fed back by the kernel queue
  (NLMatrix driver) or by the device callback (WASAPI render, extra ASIO). The
  old fixed 10 ms sleep / 30 ms ring design is gone.
- Buffer modes Low / Balanced / Safe / Advanced configurable in profile JSON
  (`engine.bufferMode` + numeric overrides) and in the GUI Preferences dialog;
  persisted in preferences.json and profile files, diagnostics show the live
  buffering/health block.
- Driver ABI v3: configurable per-endpoint kernel capture target
  (`NLMATRIX_IOCTL_SET_CAPTURE_TARGET`), accurate pulled/underrun/dropped/render
  overrun counters; installed and verified in the VM.
- Telemetry: frameDeficit/expectedFrames (upstream ASIO callback loss), callback
  gap max, engine state machine, per-destination ring target/fill, kernel queue
  used/target, starved ticks, max feeder wake gap, estimated pipeline latency.
- Recovery: ASIO stall watchdog with restart state machine (startup false
  positive fixed), driver/endpoint reconnect with stale-audio drain, no-consumer
  drain, engine keeps running with remaining devices.
- Real-time discipline: no allocations in the audio callback, FTZ on, MMCSS
  Pro Audio feeder threads, buffer-meter loops merged.

Verified (full-rate VM window, same ABI3 driver, back-to-back A/B):

- Engine-side continuity: destination underruns **0** and ring dropped frames
  **0** in every engine-v2 run, vs **13-37 per 20 s** (and 63 per 60 s soak) for
  the legacy engine on the identical driver/clock. Legacy WAVs show 70-196 ms of
  silent gaps per 10 s window; engine-v2 Balanced/Low are limited only by the
  measured upstream FlexASIO callback deficit (0.16-1.73 % under 6-27 % host
  load), while engine-v2 **Safe is gap-free** (analyzer PASS: 0 silent gaps,
  0 real splices, tone 999.85 Hz) with 175 ms pipeline latency.
- Buffer modes behave as designed: low = smallest ring (floored to 2.5x ASIO
  period), balanced = default, safe = headroom; buffer mode changes restart the
  engine (topology).
- Driver ABI v3 installed in the VM (boot 14:51, image SHA256 CD95712E...),
  SET_CAPTURE_TARGET readback 30->50 ms and clamps verified, kernel underrun
  counters proven with an injected 200 ms stall (visible in COUNTERS and WAV).
- GUI: buffer mode selector + advanced fields + engine persistence + buffering
  diagnostics implemented; **verified in VM with the fixed build**: selftest
  **37/37 PASS**. The two pre-existing failures ("canvas interaction: nodes
  missing" and "GUI parses actual router meter values") were a selftest-ordering
  bug: the test ran before the deferred last-session restore, so the model was
  empty; fixed with a deterministic fixture. New checks verify profile /
  last-session / preferences.json engine persistence and round-trip, legacy
  compatibility and balanced fallback. GUI quality round: fixed the real
  left-panel clipping (sourceEnabled/pan/polarity were never reparented, painted
  as 10 px window slivers), bus-row overflow, mixer bottom clipping, profile-name
  field; selftest now **40/40 PASS** including 3 layout checks; guiverify
  **8/8 layout PASS** at 1900x1020 (Dark/Light/System x IT/EN), 1600x900 and
  1366x768 with 20 PNGs under `reports/MVP/gui_verify_20261027/` (+ layout log).
  Final GUI build deployed in VM: SHA256 313a06b7... (selftest 40/40; 20/20 PNGs;
  layout 8/8); remaining C8 gaps are documented (technical English prose only).
  Localization increments 1-3 (IT/EN live switch + persisted language, menus,
  panels, Preferences/About/Diagnostics/Mixer/help/context menus/statuses, UTF-8
  helpers) and theme fixes (POLLSYSTEMTHEME never called -> System theme did not
  react live; secondary windows now use the palette-aware LookAndFeel; TextEditor
  cached text colour fixed in Light; hardcoded colours -> Palette tokens).
- FINAL2 final acceptance under the WASAPI-exclusive test clock (split-verdict
  report): internal_safe x2, internal_balanced and gui_profile rows =
  **engine/writer PASS + WAV continuity PASS** (0 silent gaps, 0 real splices,
  tone ~1000.2 Hz, underruns 0, ring drops 0, engineState running, restarts 0);
  clock/environment **WARN** only from the test clock: negative frameDeficit
  (-1.1..-1.8 %), ring-fill inflation (exclusive-mode startup burst) and
  startup-only kernel underrun byte deltas (0 after t>=1 s). late_consumer and
  latency rows are affected by the same test-clock inflation; multi-ASIO extra
  device invalid because ReaRoute does not tick without REAPER. Evidence (storica,
  archiviata): `da_eliminare/2026-10-27-consolidation/reports-superseded/final_acceptance-FINAL4/`.
- Latency measurement method implemented: router test-source markers
  (MARKER index/frame/wall_ms), recorder timestamps, analyzer
  `tools/measure_engine_latency.py`. Under the inflated exclusive test clock it
  measured 256 ms = ring fill ~220 ms + driver queue 50 ms (test-clock artefact).
  With the accurate NLMatrix-virtual test clock the raw marker value was
  median 28 ms, which cannot be reconciled with the queued audio (~73 ms), so
  the recorder now also logs first_cb_ms/first_cb_samples and the analyzer
  reports both time bases (latency_ms / latency_firstcb_ms); the definitive
  number comes from the next marker run (and then Focusrite hardware clock).
- FINAL3 (frozen 7BA50557) results under the accurate NLMatrix clock: safe x2
  and late_consumer PASS engine/writer + WAV; analyzer 1.2.0 with startup-window
  classification (steady real splices required 0); writerStarved 0 everywhere
  (partial writes ~196 in balanced); marker latency median 44.46 ms
  (31.5-49.6, first-cb base 45.46 ms). BUT two full acceptance runs exposed an
  intermittent balanced-mode defect: the writer floored small deficits up to a
  1 ms minimum chunk, pushing the kernel ring past its target -> driver dropped
  10-50 B steady and produced 1-4 steady real splices in balanced/GUI rows.
  FINAL3 is therefore NOT accepted for the end-to-end matrix.
- FINAL4 (frozen AB5E363F, accepted): exact-deficit servo fix removed the
  balanced ring overrun. Targeted validation (13 runs) + full acceptance on the
  accurate clock: safe x2, balanced, GUI profile, late consumer, latency and
  latency markers = engine/writer PASS + WAV PASS (0 gaps, 0 steady splices,
  steady kernel drops/underruns/starvation 0; one documented non-corrupting
  scheduler transient). Multi-ASIO remains the documented ReaRoute-client
  limitation. Evidence (storica): `da_eliminare/2026-10-27-consolidation/reports-superseded/`
  (`final_acceptance-FINAL4/`, `final4_validation-FINAL4/`).
- FINAL5 (frozen 2AE82FEB, accepted, current): FINAL4 + the engine API requested
  by the GUI feature: `--control <file>` graceful stop with 20 ms destination
  fades (verified exit 0), METERS `devices[]` with requested-vs-actual
  rate/buffer/I/O (FlexASIO requested 48k/256 -> actual 48k/960; per-endpoint
  driverTargetMs 80 ms verified), exit 24 when a requested rate is unavailable,
  extra-ASIO buffer request, sameTopology treating device formats as
  restart-required. 10 s balanced regression PASS (0 gaps, 0 real splices,
  steady deltas 0, starved 0). Acceptance `reports/engine/final_acceptance_v5/`:
  safe x2, balanced, late-consumer PASS engine/writer + WAV; multi-ASIO
  documented limitation. FINAL5 marker latency measured in
  `reports/engine/final5_markers/`: median 43.2 ms (29.7-48.3) after explicit
  pairing-offset alignment (the first burst is suppressed by the destination
  connect fade, reported rather than hidden); physical/hardware measurement
  still pending.
- GUI increment requested by the operator (2026-10-27 evening) - DELIVERED and
  verified in VM (UI sha256 dbd69a86..., selftest **54/54**, layout 8/8):
  C12 legibility (+1 pt fonts, stronger text/muted/disabled contrast, resized
  rows, no clipping at 1366/1600/1920); C13 collapsible left panel with drawn
  Path triangles (whole panel railed + per-section disclosure, state persisted
  in preferences `ui` object, canvas reflows); C14 bidirectional canvas<->left
  panel selection (node/cable details editable on the left, section auto-expands
  and scrolls, list selection highlights the node); C15 REAL sample-rate/buffer
  settings for the main ASIO engine and extra ASIO devices: capability probing,
  validation, production apply path = validate -> graceful stop via --control ->
  rewrite profile/preferences -> restart -> requested-vs-actual report, exit 24
  mapped to an Italian error, per-device persistence (profile+preferences
  `deviceFormats`), Diagnostics "FORMATO DISPOSITIVI ASIO" block and per-driver
  destination `driverTargetMs`. Integration proof in VM through the production
  path: 48000/256 -> 44100/256 -> 48000/256 with actual rates 48000/44100/48000,
  buffer rounded by the driver to 1024 and shown as "(arrotondato dal driver)",
  graceful stop used, no crash/leftovers; evidence `ui_selftest_g4e.log`,
  `reports/engine/gui_acceptance/ui_app_log_g4e.log`, 20 screenshots in `reports/MVP/gui_verify_20261027/`.
- **FINAL4 accepted and frozen** (sha AB5E363F, `vm/baseline/
  NLMatrixRouter_engine_v4_FINAL.exe`): the servo now writes EXACTLY the measured
  deficit (no minimum-chunk floor; trigger threshold one 1 ms chunk) which
  eliminated the FINAL3 balanced-mode defect (10-50 B steady kernel drops + sample
  splices). Targeted validation (13 runs): balanced x3, GUI x7 (6 PASS), safe,
  late consumer, latency markers - every run steady kernel dropped 0, steady
  underrun 0, steady real splices 0, gaps 0, analyzer PASS; one documented
  non-corrupting scheduler transient (writerStarved=1, no kernel drop, analyzer
  PASS). Full acceptance on the accurate NLMatrix clock (analyzer 1.2.0, split
  verdicts): safe x2, balanced, GUI profile, late consumer, latency and latency
  markers = engine/writer PASS + WAV PASS; multi-ASIO = documented limitation
  (extra ReaRoute ASIO needs a live ReaRoute client: d1 starved 1251 and d0
  steady underrun 10080 B in that scenario; no hang). Remaining non-zero
  counters are startup-only kernel underrun deltas (480-12576 B, clock/env
  WARN). Marker latency FINAL5 median 43.2 ms (29.7-48.3) after pairing-offset
  alignment; the remaining queue-model/timebase uncertainty is documented and
  the definitive measurement is pending on a physical path. Evidence:
  `reports/engine/final_acceptance_v5/`, `reports/engine/final5_markers/`,
  `reports/engine/FINAL_STATUS.md`.
- FINAL2 delta (the only engine code added after the accepted 9B4E1D65 binary:
  watchdog startup-state guard + no-consumer drain) verified semantically in the
  VM with the late-consumer scenario: while no capture client the feeder reports
  `driverConsumerActive=false` and keeps the graph ring at the target
  (~60 ms, max 80-240 ms transient vs 1365 ms capacity) with 0 kernel drops;
  when the recorder opens the flag flips to true, the writer resumes (writes
  0 -> 1398/1596) and the WAV contains the 1 kHz tone; `engineState=running`,
  `asioRestarts=0`, no startup false restart. Two independent runs (Lead and
  vm-harness). Evidence (storica): `da_eliminare/2026-10-27-consolidation/reports-superseded/blocked_halfrate_5E3BEFED/FINAL2_delta_semantic_test.md`.

## Repository consolidation (2026-10-27)

- Documentazione consolidata: README, TASKS, DEVELOPMENT_STATE, reports/TEST_STATUS
  + docs/ENGINE_V2, docs/08_TOOLCHAIN_BUILD, docs/11_RISKS_LICENSES, ARCHITECTURE,
  driver/README, tools/vm/README. I documenti superati sono in `docs/history/`.
- Materiale obsoleto spostato (reversibile) in
  `da_eliminare/2026-10-27-consolidation/`: backup agenti, dist 0.4.0-alpha,
  installer build, 271 MB di oggetti build, report FINAL3/intermedi, baseline run
  raw, helper GUI. La VM (`vm/`, ~32 GB) è esclusa dal repository e non è stata
  toccata.
- Cache oggetti `app/build` rimossa: la prima build ricompila; gli artefatti
  Release correnti (router FINAL5, UI, recorder, legacy) restano presenti.
- `.gitignore` aggiornato. Spazio non liberato finché `da_eliminare` non viene
  svuotata (spostamento sullo stesso disco).

## Open / next (current)

1. **Hardware Focusrite** — il passthrough USB della VM non trasmette (ASIO non
   apre, WASAPI 0 frame) dopo re-plug, cambio porta, xHCI, reset/power-cycle:
   limite VMware, non del prodotto. Le righe hardware (A/B legacy vs FINAL5 +
   latenza marker) sono pronte per un PC fisico.
2. **Latenza definitiva** — marker FINAL5 mediana 43.2 ms (29.7-48.3, pairing
   offset esplicito); misura fisica/loopback hardware ancora da fare.
3. **Multi-ASIO valido** — serve un client ReaRoute reale o un secondo device
   fisico (Focusrite) come extra; senza client il master resta pulito ma d1
   starva (nessun hang).
4. **Recovery reale** (A4): disconnessione device durante routing su hardware/VM.
5. **GUI**: restano prose tecniche inglesi (licenze/dump), DPI 125/150 (C6),
   contrasto fader Light già sistemato.
6. **Release**: installer/firma/licenze (D4-D8), packaging, endpoint persistenti,
   process-loopback su Windows fisico.

## Files touched this cycle

Engine/GUI/driver/harness: `app/src/router_main.cpp` (FINAL5 API: --control
graceful stop, devices[] telemetry, exit 24, fades, driverTargetMs),
`app/src/engine_tuning.h`, `app/src/nl_driver_client.{h,cpp}`,
`app/src/driver_audio_test.cpp`, `app/src/ui_main.cpp`,
`app/src/preferences_panel.h`, `app/src/device_browser.h`,
`app/CMakeLists.txt`, `app/legacy/*`; `driver/*` (ABI3); `tools/vm/*`,
`tools/analyze_continuity.py` 1.2.0, `tools/measure_engine_latency.py`;
evidenze `reports/engine/**`, binari `vm/baseline/*`.

## Tests already done; do not repeat

- ASIO/FlexASIO/ReaRoute enumeration and Focusrite identity (VM).
- Engine A/B legacy vs engine v2 (frozen E0C4553C / 9B4E1D65) with ABI3 driver.
- FINAL4/FINAL5 continuity acceptance on the accurate NLMatrix clock (reports in
  `reports/engine/final_acceptance_v5/`, `reports/engine/FINAL_STATUS.md`).
- ABI3 driver install/verification and SET_CAPTURE_TARGET readback.
- GUI selftest **54/54** + guiverify layout 8/8 + real 48k→44.1k→48k format change
  (UI sha dbd69a86...).
- Latency marker method (analyzer pairing offset): FINAL5 median 43.2 ms.
- Half-rate shared-mode clock investigation and CLOCK_ISOLATION matrix — do not
  repeat host/guest levers; accurate test clock = FlexASIO on NLMatrix virtual
  render endpoint.
