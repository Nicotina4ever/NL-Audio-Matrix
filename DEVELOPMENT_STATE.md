# DEVELOPMENT_STATE — NL Audio Matrix

**Updated:** 2026-10-30 15:50 (Europe/Rome)
**Objective:** complete the audio engine, real Focusrite validation and professional GUI without false PASSes.

## Increment 2026-10-30 15:05 — complete NL installer (wizard, diagnostics, verification)

- **Setup wizard (A30)**: `NL-Audio-Matrix-<ver>-Setup-test-signed.exe` is one file
  with a normal Windows wizard: current-state card, install/upgrade/repair/uninstall/
  diagnose, install folder, launch option, explicit test-signing consent, live
  progress log, result page with log/launch/reboot, and a "Guida e requisiti" dialog
  with the OEM/Secure Boot/HVCI instructions and consequences. CLI modes unchanged.
- **Real driver diagnostics (A30)**: SetupAPI PnP identity (instance, hardware ID,
  service, bound INF, problem code), loaded `.sys` path/version/size, DriverStore
  packages with DriverVer, catalog signature (`sysvad.cat`) vs embedded test
  certificate, Code Integrity events, Secure Boot/TESTSIGNING/HVCI/VBS/firmware/
  blocklist, the eight endpoints with names, pending-reboot marker and the setup
  decision. `--mode diagnose` writes `last-diagnose.json/.txt` under
  `%ProgramData%\NL Audio Matrix\reports`.
- **Verification (A30)**: `--mode verify` (+ `/app-selftest`) and `NLMatrixVerify.exe`
  (asInvoker, so no UAC prompt at logon) for the RunOnce check. The app start/stop
  test uses the run-only `--exit-on-close` flag: window shown, normal exit 0.
- **Lifecycle safety (A30)**: upgrade keeps profiles/presets and removes only
  obsolete NL driver packages; repair restores missing file/driver/service/endpoints;
  duplicate NL device instances removed; uninstall removes app/driver/certificate/
  ARP/Start Menu, keeps `%APPDATA%`, writes the pending marker and registers the
  verifier so the next logon proves the removal. Payload/build fails if any private
  key material would be shipped; no third-party driver or Windows audio default is
  touched.
- **VM verification (suite `setupcheck`, 53/53 checks PASS)**: diagnose report,
  wizard window + screenshot, verify with app start/stop, install/repair/upgrade/
  idempotent reinstall with one device and one package, profiles preserved, uninstall
  (exit 20) with every artifact removed after reboot, post-reboot RunOnce verifier
  healthy and marker cleared, clean reinstall, final verify with app start/stop.
  Evidence `reports/setupcheck/2026-10-30-145731/`, `reports/automation/latest-summary.json`.
- **Host preflight (A31)**: `tools/Diagnose-InstallerHost.ps1` — read-only checks of
  OS/build/x64, administrator token, firmware, Secure Boot, TESTSIGNING, HVCI/VBS/
  blocklist, existing NL app/driver/device/endpoints/certificate, free space and
  pending reboot, with verdict READY/BLOCKED/NEEDS-ADMIN and the manual steps.
  Report `reports/host-preflight/2026-10-30-current/`. Nothing is installed or changed.
- **Published (R1)**: pre-release https://github.com/Nicotina4ever/NL-Audio-Matrix/releases/tag/v0.5.0-alpha.3 — tag `v0.5.0-alpha.3` -> commit `eb1e0f7`, 6 assets (setup `14103f62…`, portable `d08f68e8…`, SHA256SUMS, BUILD_INFO, RELEASE_NOTES, GUIDA), every asset re-downloaded and SHA256-verified; `prerelease=true`, no Stable/latest pointer. Publishing tool `tools/Publish-GitHubRelease.ps1` uses the credential git already stores and never prints it.
- **Distribution (A29 refresh)**: full installer renamed `...-Setup-test-signed.exe`
  to state the signing requirement; release adds `GUIDA-INSTALLAZIONE.txt` and
  `SHA256SUMS.txt` covering every asset. Setup SHA256 `14103f62…`, portable
  `d08f68e8…`. Pre-release publication waits for a fresh GitHub token: the previously
  exposed token must be revoked and was not used for this build.

## Increment 2026-10-30 13:05 — responsive mixer, installer hardening, distribution

- **Mixer responsive (C5)**: fixed strip metrics (136 px min width, 96 px min fader,
  26 px buttons, three fixed rows), strips spread to 208 px when there is room, both
  scrollbars when the dock is small. Suite `guimixer` PASS: 7 strips usable at
  1366×768/1600×900/1920×1080, 125%/150% DPI simulation, horizontal scroll, minimum width.
- **Installer (A28)**: `--mode diagnose` read-only report (driver state/INF DriverVer/
  signature vs embedded certificate, trust, staged packages, endpoints, test-signing,
  Secure Boot, decision) and an explicit `/accept-testsigning` gate: setup no longer
  enables test signing without consent (exit 12 with instructions). `devcon install`
  exit 2 + "Device node created" is a reboot-required success, not a failure.
- **Distribution (A29)**: `build-release.ps1` emits the full NL installer and the
  portable user-mode package (Apollo -> VB-CABLE preset, README, SHA256SUMS, BUILD_INFO,
  RELEASE_NOTES). Suite `distcheck` PASS: portable clean payload, VB-CABLE render
  online at -12 dBFS with a -21.1 dBFS browser-side capture and 0 gaps without using NL
  endpoints; installer diagnose/install/repair/upgrade/reinstall 0 with 8 endpoints,
  uninstall 20 + reboot, clean reinstall restores 8 endpoints.
- A21/A22 remain open defects; no driver or Secure Boot change was applied to the host PC.

## Increment 2026-10-30 12:15 — Studio surface, operational mixer, magnetic patchbay

All tests in the VMware VM; the host only compiles and analyses WAV files.

- **Patchbay C4 (magnetic cables).** Input connectors are magnetic drop targets with a
  46 px snap radius (independent of zoom): dragging from a source/bus port draws the
  preview already attached to the nearest connector, highlights the target ring, hover
  shows the connector that would snap, ESC cancels a draft and clears the selection, and
  a double-click on empty canvas opens the add-source/destination/bus menu. Zoom and pan
  were kept. Verified by the canvas interaction test inside the GUI selftest.
- **Mixer strips are model-backed (C5).** Each strip now has gain, pan (sources with
  L/C/R readout), mute, solo, polarity Ø (sources), enable ON/OFF (all kinds), monitor
  MON and mix-minus MM with the excluded-source count (destinations). Controls go through
  the same model + `applyProfile()` path as the left panel, so edits are saved and appear
  in the patchbay; mix-minus opens the source picker anchored to the strip. Seven selftest
  checks prove the callbacks reach the model.
- **Studio surface (A27).** New "Studio" chip and Routing menu entry open
  `studio_panel.h`: return paths **NL Return 1..4 -> ASIO out 1/2** created/removed with
  one click (loopback source + asio-out destination + route), bus list with route count
  and "connect to destination", per-destination Monitor toggles and Mix-minus picker.
  Everything is a normal project edit (profile, patchbay and mixer stay in sync). New
  suite `guistudio` PASS 4/4; the selftest adds 8 Studio checks incl. the profile
  round-trip of mix-minus.
- **Engine proof of mix-minus and bus routing (D6).** `tools/vm/Run-MixMinusProof.ps1`
  (suite `mixminus`) runs three headless 8 s configurations with test sources and reads
  the destination level from the router's own METERS telemetry only (no WAV): two summed
  tones give **-6.0 dBFS**, the same two tones with `mixMinus:["t2"]` give **-12.0 dBFS**,
  and a single tone routed tone -> bus -> destination gives **-12.0 dBFS**; two identical
  phase-aligned tones sum to **-6.0 dBFS** and inverting one cancels to the meter floor
  **-100 dBFS**. All six checks PASS, so the GUI mix-minus/bus/polarity controls drive a
  real engine behaviour.
- **Hidden tray regression fixed.** The selftest's tray test appended its PASS/FAIL lines
  after the SUMMARY, so the coordinator never counted them: "engine survives window hide"
  had been failing silently. Root cause: a selftest fixture profile was deleted while the
  model still referenced it, and `saveLiveProfile()` then refused to start the engine.
  Fixed both sides: the fixture restores `currentProfileFile`, a profile whose folder
  disappeared no longer blocks engine start (stale reference dropped), and the coordinator
  now counts tray lines too. GUI selftest **98/98**, tray failures 0; lifecycle 8/8.

## Increment 2026-10-30 12:05 — Quick Connect verified from the GUI, preset isolation, engine safety

All tests in the VMware VM; the host only compiles and analyses WAV files.

- **Quick Connect is now parametric (A25).** The chip and the Routing menu open a dialog
  that selects the ASIO device, one single channel (1-based) and the NL Mic endpoint,
  plus the bidirectional option (NL Return -> ASIO output 1/2). A one-click item keeps the
  canonical ASIO ch1 -> NL Mic 1. The generated project is written to its own preset
  (`%APPDATA%\NL Audio Matrix\quickconnect\quick_connect_last.json`), never over the
  loaded Studio profile: the Studio profile file stays byte-identical, the current
  profile is not adopted/replaced, and while Quick Connect runs the last session file
  keeps the **Studio** project, so a restart never loses it. Menu "Ripristina sessione
  Studio" restores the backup captured before Quick Connect.
- **GUI end-to-end proof (A26).** `--quickconnecttest` runs the same production action
  from the GUI; `tools/vm/Run-QuickConnectGuiRealClient.ps1` then lets the guest ffmpeg
  capture the NL Mic endpoint via DirectShow. Suite `guiquickconnect` run `qc-gui4` PASS:
  GUI checks 9/9, 12 s capture at RMS -15.1 dBFS, FFT 999.96 Hz, 0 gaps, 0 splices,
  drift -3.3 ppm, preset `asio=FlexASIO channel0=2 endpoint0=0` (channel 3, i.e. not the
  default channel 1). An earlier run (`qc-gui3`) had a clean GUI but 12 phase jumps in the
  capture (A22-class capture flakiness): the GUI criteria stay deterministic, the audio
  criterion stays strict, and the coordinator does not auto-retry.
- **Engine safety (A24).** The GUI no longer terminates NLMatrixRouter processes it does
  not own: a leftover engine of this application is asked to stop through the graceful
  `--control` channel, and an engine started by anything else is left running with an
  explicit recoverable Error ("non lo interrompo"). Lifecycle suite 8/8: exactly one
  engine process, engine alive in tray, resources released on exit, foreign engine keeps
  running / not terminated (deterministic renamed-ping fixture).
- **Endpoint criteria kept separate.** `smoke` = 4 capture endpoint names + driver ABI;
  `endpoints8` = all 8 NLMatrix endpoint identities via PnP (mics=4 returns=4, PASS);
  `endpoints` = per-endpoint audio verification, still blocked by A22. The smoke suite is
  never reported as eight-endpoint verification.
- **Coordinator (A23)**: new suites `guiquickconnect` and `endpoints8`; fixed
  `Start-Process -PassThru` exit-code reporting (cache `$p.Handle` before waiting, else a
  passing child looks like a failure). GUI selftest 79/79.

## Increment 2026-10-29 23:55 — all 8 NL endpoints: root cause, structural fix, real-audio verification

Driver DriverVer `10/29/2026,22.51.28.416`; installer rebuilt (`NLMatrixSetup.exe`); all
tests in the VMware VM. Commit with this document.

- **Diagnosis (no repair by guessing).** The two "missing" render endpoints were never
  absent: MMDevices held four active render records, but two carried stale fallback
  names. Evidence chain: INF publishes `NL Return 1..4` in MediaCategories and the KS
  interface `FriendlyName`; the bound driver binary contains `SINK_DESC_STR L"NL Return 4"`
  and nothing resembling "SinkDescription Sample"; MMDevices records
  `{0c0cfc52}` (Return 1) and `{6cfbab01}` (Return 4) had `DeviceDesc` = "Altoparlanti"
  and "SinkDescription Sample"; their endpoint `FriendlyName` value was absent; the
  records were created by an older driver build and Windows never re-derives names of
  existing endpoint records. Direct registry repair is impossible on current Windows:
  Administrators (and even SYSTEM) get E_ACCESSDENIED on `MMDevices\...\Properties`
  and on `IMMDevice::OpenPropertyStore(STGM_READWRITE)->Commit`, while Delete is
  granted only to Audiosrv/AudioEndpointBuilder/TrustedInstaller (verified by trying
  ACL grant + `reg delete` as SYSTEM and the MMDevice property-store path).
- **Structural fixes.**
  1. *Return 4 (HDMI):* the current binary already uses `NL Return 4` as HDMI sink
     description; the stale record only needed a fresh endpoint identity.
  2. *Return 1 (speaker):* Windows forces the localized "Speakers"/"Altoparlanti"
     display name for pins whose category is `KSNODETYPE_SPEAKER`, ignoring both the
     interface `FriendlyName` and a `KSPIN_DESCRIPTOR.Name` GUID (tested: adding the
     custom-name GUID to both speaker topology pins did not change it). The endpoint is
     now exposed as `KSNODETYPE_LINE_CONNECTOR`, which lets the MediaCategories name
     win (verified: "NL Return 1 (NLMatrix Virtual Audio Device)").
  3. *Fresh endpoint identity:* KS subdevice reference strings bumped in
     `minipairs.h` + INF (`WaveSpeakerA4`/`TopologySpeakerA4`, `WaveHdmiA2`/`TopologyHdmiA2`)
     so Windows creates new endpoint records instead of reusing the stale ones. Old
     records remain as unplugged/hidden entries, which is harmless.
- **Verified in the VM after driver remove + install (installer payload) + reboot:**
  eight active endpoints, correctly named: **NL Mic 1..4** and **NL Return 1..4**;
  one stale unplugged "Altoparlanti" record remains hidden. `NLMatrixDriverAudioTest`
  and the router report the NL Return endpoints at their real mix rates (44100/48000).
- **Real-audio verification (tools/vm/Run-EndpointAudioVerification.ps1; guest ffmpeg
  records each endpoint):**
  - NL Mic 1..4: four distinct tones (1000/1040/1080/1120 Hz) routed per channel,
    ~12 s continuous, RMS -17 dBFS, frequency within 0.4 Hz, no cross-talk
    (each endpoint carried only its own tone).
  - NL Return 1..4: a real WASAPI playback client played a distinct tone into each
    return endpoint; the router captured it with its native WASAPI loopback source and
    routed Return N -> Mic N. All four returns carried continuous audio (RMS about
    -12 dBFS, 12 s), so the return endpoints are electrically/functionally live.
  - **Open anomaly (not silence):** the loopback return path shows a frequency offset
    on some channels - Return1 999.8/1000, Return2 1079.7/1040, Return3 1071.4/1080,
    Return4 1139.7/1120 Hz. The tones are present and continuous; the offset points at
    the loopback capture/rate-conversion path of the return direction (to be
    investigated), not at the endpoints themselves. Tracked in TASKS A21.
- **Test-environment note:** FlexASIO's `FlexASIO.toml` in the VM referenced the old
  endpoint name `Altoparlanti (NLMatrix Virtual Audio Device)`; after the rename it was
  updated to `NL Return 1 (NLMatrix Virtual Audio Device)` (test harness only, not a
  product dependency).
## Increment 2026-10-29 22:10 — Quick Connect with a real Windows client, GUI format bar, installer modes

Commit `01c6f2d`. All audio/driver tests below ran **inside the VMware Player VM**;
the host only compiles binaries and runs the WAV analyzer.

- **Quick Connect verified with a real third-party Windows client** (the missing
  acceptance criterion): `tools/vm/Run-QuickConnectRealClient.ps1` runs the guest's
  ffmpeg 9.0.2 as a DirectShow capture client on the NL Mic 1 endpoint while the
  router feeds it (`tools/vm/configs/quick_connect_nlmic1.json`, single ASIO channel
  in -> NL Mic 1 ch1). Negative control with no router: silent (-98 dBFS). Positive,
  VM, after a full driver reinstall: 12.2 s tone at RMS -17.0 dBFS, FFT 999.98-1000.02 Hz,
  0 dropouts, 0 real/sub-sample splices, phase drift -2.6..-5.7 ppm, router exit 0,
  analyzer `analyze_continuity.py` 1.2.0 PASS. Repeated PASS on three builds/installs
  (before reinstall, after reinstall, after clean install). Evidence (local, git-ignored):
  `reports/quickconnect/2026-10-29/` (`quickconnect_summary.json`, WAV, analyzer JSON,
  control WAV). A router that merely stays alive is no longer accepted as proof.
- **GUI audio format bar** (`app/src/ui_main.cpp`): master ASIO row plus one row per
  extra ASIO driver on the main surface, each with rate combo, Auto/Manuale switch,
  buffer combo and actual ("Attivo") column from METERS; engine mode
  Low/Balanced/Safe/Avanzate (Advanced exposes its numeric fields) selectable without
  opening Preferences; "Applica scheda audio" performs graceful stop/restart.
  `rateAuto` now round-trips profile/preferences/last-session and devices[] JSON.
  VM GUI selftest **67/67 PASS** (56 before; 11 new checks), guiverify 8/8.
- **Per-device ASIO format negotiation** (`app/src/router_main.cpp`): `rateAuto` is now
  honoured for every extra ASIO device, not only the master. Auto picks the nearest
  supported rate / smallest supported buffer >= request; Manual never silently changes
  the rate (pre-check when the driver publishes rates, post-open check otherwise).
  Buffer size is a request rounded to a supported value and reported, never silent.
  METERS `devices[]` now carry `rateAuto`, `rateFallback`, `bufferFallback`,
  `effectiveBuffer`, `availableRates`, `availableBuffers` for the GUI and diagnostics.
  VM matrix (`tools/vm/configs/asio_*_unavailable.json`): FlexASIO buffer 256 -> 960
  reported `bufferFallback=true`; ReaRoute extra 128 -> 1024 same; both VM drivers accept
  22050 Hz and publish an empty rate list, so the rate-fallback branch could not be
  exercised here (it degrades to the post-open strict check) - hardware/driver with a
  real rate list still pending.
- **Installer modes verified in the VM** with `installer/build/NLMatrixSetup.exe`
  hash `6f0ffa28...` (`--mode install|repair|upgrade|uninstall /silent`):
  upgrade = 0 (app files replaced, driver package current, endpoints re-checked),
  repair = 0 (deleted `NLMatrixUI.exe` restored, 1 item repaired, presets kept),
  uninstall = 20 reboot-required (device removed, 7 driver packages queued, 2 certs
  removed, app dir + ARP gone; after guest reboot: no NLMatrix PnP device, DRVSTORE
  packages 0, ARP absent, 6 stale *inactive* endpoint nodes remain), clean install
  after reboot = 0 (device installed, service running, test cert trust, 6 active
  endpoints), second install = 0 idempotent. Forced driver reinstall
  (`devcon remove` + install mode) bound image SHA256 `cd95712e...`, service RUNNING.
  Logs: `reports/installer/2026-10-29/NLMatrixSetup-*.log` (local).
- **Open defect found and confirmed**: only **6 of 8** NL endpoints activate -
  NL Mic 1..4 and NL Return 2/3 are active, **NL Return 1 and NL Return 4 never
  activate** (stable across upgrade, repair, full uninstall+reboot+clean install).
  This is driver endpoint provisioning, not the installer. It blocks the 4+2 preset
  and A6/A7. Next driver task: enumerate the return endpoints in the INF/device list.

## Increment 2026-10-29 — installer devcon presence fix + rate fallback / multi-ASIO VM verification

- Root cause of the reported `devcon update failed` fixed in
  `installer/NLMatrixInstaller.cs`: `devcon status <hwid>` exits 0 even when no
  device matches, so the pre-fix presence check (`Run(...) == 0`) treated an
  absent device as present and routed fresh installs to the `devcon update`
  branch. Fix: output-based `DeviceExists` helper (RunWithOutput + "No
  matching devices" check). New installer build hash `05195009...` (Alpha
  0.5.0-alpha, buildId `20261028-0113-7adcedb`); deployed to the VM; retest:
  device removed → installer → install branch (`devcon install`) EXITCODE=0 →
  device present + service RUNNING; reuse branch OK; service kernel not
  stoppable so the update branch was not re-exercised directly (symmetric logic).
- Router Priorità 0 fixes verified in the VM with the current build (FlexASIO
  master + ReaRoute extra):
  - Auto fallback (`rateAuto`, FlexASIO master 96 kHz): EXITCODE=0, engineHealthy,
    rate 96000 opened.
  - Manual strict (`rateAuto=false`): EXITCODE=0, no silent fallback.
  - Multi-ASIO (FlexASIO master + ReaRoute extra 16/16): EXITCODE=0, second ASIO
    opened, engineHealthy, no exit 23; ReaRoute d1 starved as expected without a
    client.
  - Quick Connect source→NL Matrix end-to-end with the current build: headless
    router run (`--config tools/vm/configs/flex_test_256.json --seconds 20`) source
    FlexASIO test tone → NL Mic 1: EXITCODE=0, auto-stop, engineHealthy, asioError
    false, source underruns 0, destination writerStarved 0 (2026-10-29).

## Increment 2026-10-27 23:45 — multi-ASIO channel domain, async clocks, 1-based channels

Driven by the OBS-ASIO / atkAudio comparison (behavioural reference only; both
are GPL/AGPL, no code copied) and by the Apollo per-channel requirement:

- **Extra-ASIO channel domain fixed**: every ASIO source/destination bound to a
  non-master device is now validated against that driver's own
  `getOpenedInputs()/getOpenedOutputs()`, not against the master's channel count.
  Before this, a 16-channel extra device could only use channels that also
  existed on the master. VM structural proof: FlexASIO master + ReaRoute extra
  16/16, source/destination channels 15/16 accepted, router exit 0
  (`app/profiles/tests/multi_asio/extra_device_channels.json`).
- **Always-on adaptive SRC for independent clocks**: extra-ASIO sources,
  extra-ASIO destinations and async WASAPI/process/driver sources now always run
  through `MatrixRateConverter` (previously direct ring reads when the nominal
  rate matched, which silently accumulated clock drift). METERS nodes expose
  `deviceId`, `rate`, `converterTrimPpm`, `converterUnderruns`,
  `asyncSource`; the ReaRoute extra source shows an adaptive trim ≈ +185 ppm.
- **ASIO channels are 1-based in the UI** (driver convention) and 0-based only in
  the profile/config model, through one explicit conversion boundary. The
  Channel Picker already listed real names and arbitrary single channels; the
  old 1-based tooltip vs 0-based text field ambiguity is gone.
- New engine build SHA256 `5B5C2495...`, UI `307193D2...`; GUI selftest
  **56/56 PASS** (new checks for 1-based display/parse), channel-split
  deterministic matrix still PASS. Hardware Apollo/per-channel audio and a
  live second-ASIO consumer remain pending.

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

## Workspace cleanup (2026-10-31)

- Cartelle esterne al progetto rimosse da `K:\AI`: `_usbip_eval`, `_delivery_tests`
  e `_delivery_tools` spostate in `da_eliminare/2026-10-31-cleanup/esterni/`
  (109 file, 303 MB; non referenziate da script o documenti).
- Scratch della VM archiviato in `.../vm-scratch/` (530 file, 65 MB di log, WAV,
  screenshot e batch di sessioni concluse). Restano in `vm/` solo VM (dischi e
  stato sospeso), `baseline/`, `engine/`, `scripts/`, `caches/`,
  `channel_split_evidence/`, `admin.txt` e `installers/` (installer Focusrite e
  FlexASIO).
- `app/build-gui/` (238 MB, non citato da script), `tools/__pycache__` e il
  residuo `reports/tmp_listing.txt` spostati in `.../build-archiviati/` e
  `.../varie/`.
- Totale archiviato: 839 file / 606 MB in un'unica cartella datata, cancellabile
  a mano per liberare spazio. Nessun file tracciato da Git è stato toccato; i
  percorsi usati da harness, build e release restano validi.

## Open / next (current)

1. **Hardware Focusrite** — il passthrough USB della VM non trasmette (ASIO non
   apre, WASAPI 0 frame) dopo re-plug, cambio porta, xHCI, reset/power-cycle:
   limite VMware, non del prodotto. Le righe hardware (A/B legacy vs FINAL5 +
   latenza marker + B7/B8 separazione Output 1/2 e bus split) sono pronte per un
   PC fisico. Il test deterministico B7/B8 è già PASS in VM (600 Hz → NL Mic 1,
   900 Hz → NL Mic 2, mute/gain/swap e duplicazione bus verificati);
   configurazioni in `app/profiles/tests/channel_split/`, runner
   `tools/Run-ChannelSplitTest.ps1`, evidenze in `vm/channel_split_evidence/`.
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
  render endpoint* **ASIO outputs are opt-in and selective**: the master output mask is built
  only from explicit master `asio-out` destinations; a record-only graph opens
  no output stream, and a single-output route opens only that channel. A driver
  that rejects the selective mask triggers a logged full-mask compatibility
  fallback (no destination created). Inputs remain fully enumerated. Evidence:
  `app/profiles/tests/multi_asio/master_single_output.json` (VM).
* Async WASAPI loopback source smoke: CABLE Input (render) → router loopback
  source → NL Mic 1 captured 600 Hz (dominant 599.881 Hz, RMS −12.38 dBFS) with
  no router errors; the always-on adaptive converter therefore runs on the
  async path. Continuity analyzer is not a valid PASS here (player start/stop
  boundaries; 440 ms of gaps concentrated at the edges and a 6.18 Hz clock
  recurrence error). Evidence: `vm/async_loop_mic1.wav`,
  `vm/async_loop_router.err.log`.
.

## Incremento 2026-10-30 01:15 — A21 sospeso, correzioni preservate

Indagine A21 interrotta su richiesta (consumo risorse); A21 resta **aperto** e non blocca lo
sviluppo. Correzioni reali gia' fatte e conservate:

- **Test client** `NLMatrixDriverAudioTest --play`: la fase dell'oscillatore avanzava una
  volta per canale per campione, producendo salti di fase a ogni buffer (il tono 1040 Hz
  mostrava righe spurie a 980/1080 Hz e la lettura "1079.7 Hz"). Ora un solo incremento per
  frame, uguale su tutti i canali.
- **Driver loopback** (`nltransport.cpp`): il pin loopback leggeva il render ring dal cursore
  di lettura, che avanza solo in overrun; riproduceva quindi audio vecchio e faceva scorrere
  la finestra a scatti (l'errore di frequenza vero e proprio). Ora il tap ha un cursore
  consumatore proprio; alla creazione del pin loopback il tap viene azzerato
  (`NlTransportResetRenderTap`) per non ereditare la coda della sessione precedente.
- Diagnostica: il router logga endpoint e formato risolti per ogni source loopback
  (`WasapiLoopbackCapture::getEndpointName`).

Stato VM: driver ricostruito e reinstallato (DriverVer `10/30/2026,0.38.34.360`) + reboot.
Evidenza sintetica gia' raccolta: matrice 4x4 tono su NL Return N -> NL Mic N con frequenze
nominali esatte e nessuna diafonia; loopback standalone di Return 2 = 1040.0 Hz.

**Aperto (A22)**: con piu' client contemporanei (router + piu' ffmpeg/loopback) la cattura
NL Mic puo' consegnare silenzio o frame spuri; `Run-EndpointAudioVerification.ps1` fallisce
in quel contesto. Da riprendere con test deterministici e un solo client per endpoint.

Prossimo lavoro: funzionalita' Quick Connect in GUI, routing bidirezionale, mixer/Studio.

## Incremento 2026-10-30 11:20 — Test coordinator async, GUI modulare, ciclo di vita motore

- **Coordinatore test** `tools/Run-TestSuite.ps1` (nuovo, usa gli harness esistenti): avvio
  detached (o Task Scheduler one-shot), build hash-pinned, mutex/lock, guardia sessione audio
  in guest, timeout con kill dell'albero, `summary.json` compatto + `latest-summary.json`,
  log dettagliati solo quando utili, retention 5 run. Suite: smoke, guiselftest,
  guilifecycle, quickconnect, endpoints, matrix. Verificato: smoke PASS, launcher detached
  PASS, task Scheduler create/remove OK, guilifecycle PASS (5/5, risorse rilasciate),
  guiselftest 67/67.
- **GUI modulare**: patchbay centrale invariato; barra formati ASIO spostata nella sezione
  "Dispositivi" (nascosta di default, persistita); strip sezioni Routing/Mixer/Quick Connect/
  Devices/Diagnostics; barra di stato compatta con stato reale del motore (Starting/Running/
  Degraded/Stopping/Stopped/Error), rate/buffer effettivi e apertura impostazioni.
- **Ciclo di vita motore**: autostart predefinito ON (voce di menu), avvio solo con profilo
  completo e almeno una rotta attiva; device non disponibile -> Error recuperabile, mai falso
  Running; istanza GUI unica; router orfano terminato prima dell'avvio; uscita reale con stop
  graceful e rilascio risorse; comportamento X configurabile (tray vs uscita).
- **Quick Connect in GUI**: chip + voce menu che costruisce il progetto minimo ASIO ch1 ->
  NL Mic 1, lo salva come ultima sessione e avvia il motore.
- A21/A22 restano aperti e sospesi; nessuna nuova indagine sui toni.

## Incremento 2026-10-31 15:20 — Test signing: stato live del kernel, causa del blocco host

- **Causa del fallimento installazione host** (service 577, device Code 52, CodeIntegrity 3004):
  il test signing era configurato in bcdedit (`testsigning Yes`) ma il kernel in esecuzione non era
  partito con esso attivo. Code Integrity live: host `0x00202201` (bit `TESTSIGN` assente), VM
  `0x00280207` (attivo). Ultimo avvio host 29/10 07:55, prima dell'abilitazione del test signing.
- **Pacchetto driver corretto**: catalogo v2 SHA-256 e firma incorporata verificati offline
  (`signtool verify /pa /c` -> "File is signed in catalog") e installati nella VM (stesso build
  26300) con driver caricato e 8/8 endpoint attivi.
- **Setup**: `NlDevices.TryGetCodeIntegrityOptions` / `IsTestSigningActive` leggono lo stato live
  via `NtQuerySystemInformation(SystemCodeIntegrityInformation)`. Se il test signing e' configurato
  ma non attivo nel boot corrente, il setup si ferma prima di toccare driver o servizio (exit 12)
  spiegando che serve solo un riavvio; `sc start` fallito registra lo stato Code Integrity; la
  diagnostica/verify espone `enabled | reboot-required | disabled | unknown`.
- Verifica VM: `NLMatrixSetup.exe --mode install /silent` -> exit 0, driver 10/31 14.52.34.330,
  8/8 endpoint; log `Kernel Code Integrity: options=0x00280207 testSigningActive=True`.
- **Riavvio pendente**: il wizard non blocca piu'. Se il test signing e' configurato ma non ancora
  attivo, il setup installa l'applicazione, lascia intatti driver/servizio e termina con exit 20
  offrendo «Riavvia ora»; dopo il riavvio un nuovo avvio del setup completa driver e 8 endpoint.
- Nota Fast Startup: su Windows lo spegnimento/accensione non rilegge la configurazione di avvio;
  serve un riavvio completo perche' `testsigning` diventi attivo (verificato sull'host: nessun
  riavvio dal 29/10 07:55, nessun watermark Test Mode).


