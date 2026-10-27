# FINAL STATUS - engine continuity acceptance (FINAL5, 2026-10-27)

Honest status summary for the engine continuity work. Verdicts are split:
**engine/writer**, **WAV end-to-end continuity**, **clock/environment**.
A harness FAIL caused only by test-clock characterisation or a documented
startup transient never turns the first two into a FAIL; both are reported in
`reports/engine/final_acceptance_v5/FINAL_ACCEPTANCE.md` (current; FINAL4 raw
evidence archived in `da_eliminare/2026-10-27-consolidation/reports-superseded/`).

ACCEPTANCE STATEMENT (current, honest): the engine-side A/B evidence (9B4E1D65
vs legacy, section a) is complemented by the **FINAL5 2AE82FEB** acceptance run
on the accurate NLMatrix-virtual clock: safe x2, balanced and late-consumer PASS
engine/writer + WAV continuity (0 steady kernel dropped/underrun deltas, 0 steady
real splices/gaps/phase jumps, writerStarved=0), and the FINAL3 balanced
ring-overrun defect eliminated in FINAL4. Only multi-ASIO remains FAIL/pending
(ReaRoute extra ASIO with no client: d1 starves, d0 steady underrun in that
scenario; documented extra-ASIO clock limitation, no hang). Focusrite hardware
rows remain BLOCKED by the VM USB isochronous path. One non-corrupting scheduler
transient (writer_starved=1, no kernel drop, analyzer PASS) was observed once in
13 FINAL4 validation runs; documented, not hidden. No false PASS. FINAL4/FINAL3
sections below are retained as history.

## (a) Engine A/B verdict - VALID (full-rate window)

Window: ABI3 driver, frozen binaries, back-to-back legacy vs engine-v2, host
load 6-27% recorded per run. Evidence: reports/engine/ENGINE_AB_REPORT.md,
reports/engine/legacy/ (6 runs), reports/engine/engine2/ (6 runs),
reports/engine/engine2_safe/ (1 run), reports/engine/quiet_window/.

| criterion | legacy engine (E0C4553C, ABI3) | engine v2 (9B4E1D65) |
|---|---|---|
| destination underruns per run | 13-37 / 20 s, 63 / 60 s soak | 0 in every run |
| destination ring drops (dropped) | 0 | 0 |
| engineState / engineHealthy / asioError | running / true / false | running / true / false |
| WAV real splices (analyzer 1.2.0, steady) | 1-10 per run | 0-6 per run (0 in safe / low) |
| safe mode | n/a | analyzer PASS: 0 steady gaps, 0 steady real splices, 0 phase jumps, tone err -0.15 Hz |
| estimatedPipelineMs | n/a (old METERS) | 85 ms balanced/low/advanced, 175 ms safe |
| limiting factor | fixed 30 ms ring target + paced writer | upstream FlexASIO callback deficit 0.16-1.73% measured in this VM (environment; recorded separately, not an engine failure) |

Engine-side weakness eliminated: engine v2 never underruns or drops destination
audio on the identical driver/clock, while the legacy control loses 13-37
buffers per 20 s run.

## (b) FINAL4 binary AB5E363F - frozen and acceptance run completed

* Build: SHA256
  AB5E363F237E5D292492DCAB89988AFE0823B6BF7D0DD3C4D61EB25244DB090B
  (FINAL3 + servo now writes EXACTLY the measured deficit with no 1 ms
  minimum-chunk floor; trigger threshold one 1 ms chunk in bytes). Frozen as
  vm/baseline/NLMatrixRouter_engine_v4_FINAL.exe (+sha256).
* Targeted validation (accurate NLMatrix-virtual clock, analyzer 1.2.0,
  13 runs in da_eliminare/2026-10-27-consolidation/reports-superseded/final4_validation-FINAL4/): balanced x3 PASS, GUI x7 (6
  PASS), safe x1 PASS, late-consumer x1 PASS, latency markers x1 PASS.
  Every run: steady kernel dropped delta 0, steady kernel underrun delta 0,
  steady real splices 0, steady phase jumps 0, steady gaps 0, WAV analyzer
  PASS; only startup kernel underrun deltas (864-12576 B) and the documented
  single scheduler transient below. The balanced-ring-overrun defect of FINAL3
  (10-50 B steady drops + sample splices) is GONE in 3/3 balanced runs.
* Scheduler transient (documented, not hidden): one GUI validation run
  (gui_rep1) had writer_starved=1 at uptime 8.7 s under host load, with NO
  kernel drops, NO WAV dropout/splice (analyzer PASS) and zero steady deltas;
  the other 12 validation runs had starved=0. It is a non-corrupting servo
  scheduling transient under host load, not a continuity failure - noted as
  such, not claimed as a clean counter row.
* Marker latency (FINAL4 + recorder F9A0E61D, accurate clock, 10 s, 8 markers):
  start_ms base median 38.96 ms (min 30.06 / max 47.94); first-capture-packet
  base median identical 38.96 ms (min 30.06 / max 47.94); ringFill last/median
  16-21/10 ms, driverUsed 50 ms, ring target 60 ms. This is ~39-45 ms, NOT the
  85-110 ms ring model: a queue-model discrepancy of ~35-45 ms remains
  unresolved (timestamp mapping vs physical capture path), and this number is
  therefore NOT presented as the definitive product latency - the physical /
  hardware measurement is still pending. The external click-path number
  (1484.7 ms in the acceptance run) includes the player WASAPI + NL Return
  loopback path and is NOT graph latency.
* Full acceptance with FINAL4 (accurate clock, click/latency/GUI configs,
  analyzer 1.2.0, steady-vs-startup kernel verdict and split verdicts) -
  6/7 required scenarios PASS; all seven continuity rows have
  engine/writer PASS and WAV PASS except multi-ASIO engine/pending:

| scenario | engine/writer | WAV continuity | clock/env | non-zero counters / notes |
|---|---|---|---|---|
| final_internal_safe_rep1 | PASS | PASS | WARN(startup) | startup kernel underrun 10560 B; steady 0/0; starved 0; 0 gaps/splices |
| final_internal_safe_rep2 | PASS | PASS | WARN(startup) | startup kernel underrun 12576 B; steady 0/0; starved 0 |
| final_internal_balanced_rep1 | PASS | PASS | WARN(startup) | startup kernel underrun 1152 B; steady 0/0; starved 0; partialWrites 19; 0 gaps/splices |
| final_late_consumer_15s | PASS | PASS | PASS | starved 0; steady 0/0; ringFillMaxIdle 80 ms (measured after 1 s drain settle); consumerActive false->true |
| final_latency | PASS | n/a by design | WARN | starved 0; start/steady deltas 0/0; startup underrun 480 B; external click 1484.7 ms (not graph latency; marker row is 38.96 ms) |
| final_gui_profile_12s | PASS | PASS | WARN(startup) | starved 0; steady 0/0; startup underrun 960 B; "Buffer mode"/ring-target lines OK |
| final_multi_asio_15s | FAIL (pending valid extra-ASIO clock) | PASS | PASS | both ASIO devices opened, no asioError; d1 ReaRoute out starved 1251 (needs ReaRoute client); d0 steady kernel underrun delta 10080 B in this scenario; no hang (~54 s) |

Non-zero counters in the whole FINAL4 acceptance: startup-only kernel underrun
deltas (480-12576 B, classified clock/env WARN as before) and the multi-ASIO
scenario's d1 starved=1251 + d0 steady underrun 10080 B. Every other counter is
zero: no steady kernel drops, no steady underruns, no writer starvation on
continuity rows, no steady splices/jumps/gaps. The earlier FINAL3 defect is
fixed.
* UI row: the suite runs the OLD deployed GUI build (2 known selftest failures);
  the NEW GUI build (sha fc77b9ef..., selftest 37/37, 8/8 layout) was verified
  separately by Lead/gui-professional and is not part of this audio suite.

## (b2) Analyzer 1.2.0 - startup-window classification (semantic change)

Lead inspected the dev3 safe WAV: the single "real splice" + phase jump sit at
3.5-4.7 ms after onset (rel. samples 449/450 and 497/498), i.e. an onset
transient, not mid-stream. Analyzer 1.2.0 classifies gap/frozen/phase-jump/
real-splice events starting within --startup-window-ms (default 50 ms) after
onset as STARTUP TRANSIENTS: kept with timestamps in JSON details and reported
as warnings (e.g. startup_phase_jump_at_onset=1, startup_splice_at_onset=1,
startup_gap_at_onset=1), while PASS requires zero STEADY-STATE events of every
type. JSON keeps startup_* and steady_* counters. All engine/A-B/baseline/
acceptance evidence was re-analyzed with 1.2.0 (reanalyze_evidence.py, no VM) and
the HARNESS_ENGINE_*/HARNESS_BASELINE tables were regenerated; the analyzer
self-test is 10/10 (semantics refinement documented in ENGINE_AB_REPORT.md and
tools/vm/README.md).

## (c) Clock characterisation and layer attribution (CLOCK_ISOLATION.md)

FINAL2/3 router-only 10 s probes, one FlexASIO.toml at a time (two campaigns):

| variant | FlexASIO config | cb/s | frames/s vs 48 kHz |
|---|---|---|---|
| a | WASAPI shared, HD Audio, 960 | 51.10 | +2.19% |
| b | WASAPI shared, HD Audio, 480 | 102.16 | +2.16% |
| c | WASAPI shared, NLMatrix virtual render, 960 | 50.09 | +0.18% |
| d | WASAPI shared, VB-Cable input, 960 | 50.11 | +0.22% |
| e | MME, HD Audio, 960 | - | FlexASIO build has no MME host API |
| f | DirectSound, HD Audio, 960 | 51.17 | +2.33% |
| g | WASAPI exclusive, HD Audio, 960 (control) | 51.13 | +2.27% |

* The earlier 16:06-16:20 2x-period anomaly was the guest shared-mode WASAPI
  render engine (device-independent, bypassed by exclusive); it is no longer
  reproducible since the 16:36:45 guest boot - shared mode is real-time now.
* Residual: VMware virtual HD-Audio endpoint clock ~+2.2% fast vs QPC (shared,
  exclusive and DirectSound alike); virtual endpoints are accurate. This
  produces the clock/env WARNs of the HD-Audio campaign; it is not an engine
  drop.
* Clean acceptance therefore uses FlexASIO on the accurate NLMatrix virtual
  render endpoint (vm/engine/FlexASIO/clock_accurate_nlvirtual_960.toml):
  probe 498 callbacks / 9.97 s = 49.95 cb/s, frameDeficit +0.105% (<0.2%),
  engineState running, ringFill 60 ms, underruns/starved/dropped 0. Note:
  FlexASIO plays into NL Return with no consumer, so render-ring overrun
  counters on that endpoint are harmless - only the clock matters here.

## (d) Focusrite USB ASIO rows - BLOCKED

The Scarlett composite enumerates (FocusriteUsb 4.150.0, status OK) but its
audio function interfaces (MI_00 MEDIA, MI_03 Focusrite Control) stay
Present=False: WASAPI capture frames=0, ASIO open "Device didn't start
correctly". Guest reboots, a clean user re-plug, and host-side USB work did not
fix it; Lead is attempting a last xHCI (USB 3.0) VM configuration. This is the
VMware EHCI isochronous path, not the guest driver. No Focusrite number is
claimed; the engine-only A/B (legacy E0C4553C vs FINAL4, focus_master_tone.json,
focus_master_tone_two_dest.json, focus_balanced.json metrics-only) and the
Focusrite marker latency (latency_focusrite_markers.json, expected ~85-110 ms on
the hardware clock) are prepared and not run.

## (e) Reproduction

    # engine A/B (full-rate window, ABI3 driver)
    pwsh -File tools\vm\Run-EngineMatrix.ps1 -Profile legacy
    pwsh -File tools\vm\Run-EngineMatrix.ps1 -Profile engine2

    # accurate-clock FINAL4 acceptance (split-verdict report regenerated automatically)
    pwsh -File tools\vm\Run-FinalAcceptance.ps1 -RouterExe "K:\AI\NL Audio Matrix\vm\baseline\NLMatrixRouter_engine_v4_FINAL.exe" -ClickWav "K:\AI\NL Audio Matrix\vm\engine\click_3s.wav" -LatencyConfig "K:\AI\NL Audio Matrix\vm\engine\latency_loop.json" -GuiConfig "K:\AI\NL Audio Matrix\vm\engine\gui_profile_smoke.json"
    python tools\vm\make_acceptance_report.py reports\engine\final_acceptance

    # clock isolation matrix + analyzer reanalysis
    pwsh -File tools\vm\Run-ClockIsolation.ps1
    python tools\vm\reanalyze_evidence.py --max-shortfall 480 reports\engine\legacy reports\engine\engine2 ... 

    # marker latency (dev router with MARKER lines + new recorder)
    python tools\measure_engine_latency.py --wav <rec.wav> --router <router.log> --rec <recorder.log> --json latency.json

## (f) Backlog

1. Multi-ASIO valid extra-device clock: run multi_asio.json with a ReaRoute
   client (REAPER) running, or use the Focusrite as the extra ASIO output when
   the USB path is repaired; report both devices' callback counts/health + WAV.
   This is the only non-PASS continuity row of the FINAL4 acceptance.
2. Focusrite streaming: closed by Lead as a VM USB-passthrough limitation; the
   engine-only A/B (legacy E0C4553C vs FINAL4, focus_master_tone + two_dest,
   focus_balanced metrics-only) and marker latency are prepared for a
   physical-PC test, not claimed here.
3. Reconcile the measured marker graph latency (~39-45 ms) with the ring-model
   estimate (85-110 ms): FINAL4 keeps ring fill ~10-20 ms, so the old model no
   longer applies; a ~35-45 ms timestamp-mapping/queue-composition discrepancy
   remains and the physical/hardware measurement is still pending. Do not
   present 39-45 ms as the definitive product latency.
4. GUI new build acceptance row (fc77b9ef, 37/37 selftest) including
   last-session profile "engine" object check (the audio suite keeps the old
   deployed GUI build).
5. Keep the documented single non-corrupting scheduler transient
   (gui validation rep1: writer_starved=1, no kernel drop, analyzer PASS) in
   mind for long-soak/field data; no action unless it recurs with kernel drops.

## FINAL5 (2026-10-27 late) - GUI integration engine API, accepted continuity

FINAL5 = FINAL4 engine + the engine-side API requested for the real GUI
sample-rate/buffer feature and click-free reconfiguration:
`vm/baseline/NLMatrixRouter_engine_v5_FINAL.exe`
SHA256 `2AE82FEBEF4DF4BB7DE9F7D2C149F2A28202CC7F4C73A7EC2EC58D43EEED0B86`.

Engine changes: `--control <file>` graceful stop (control thread polls the
command file, driver destinations ramp out/in over 20 ms, process exits 0);
`sameTopology()` now treats device sample-rate/buffer changes as
restart-required (no silent ignore); extra ASIO devices honour
`devices[].buffer`; `destinations[].driverTargetMs` per-endpoint override
(0 = global tuning); METERS top-level `devices` array with requested vs actual
rate/buffer/I/O counts; exit code 24 when a requested sample rate cannot be
opened (buffer rounding is a warning because drivers legitimately round).

Verification: control stop exit 0 and "Control: graceful stop requested." in the
VM; devices telemetry verified (FlexASIO requested 48k/256 -> actual 48k/960);
per-destination target 80 ms verified in METERS; live config format change logs
restart-required; 10 s balanced regression PASS (0 gaps, 0 real splices, steady
kernel drop/underrun delta 0, writerStarved 0); full acceptance on the accurate
NLMatrix clock in `reports/engine/final_acceptance_v5/`: internal safe x2,
internal balanced and late-consumer = engine/writer PASS + WAV PASS
(analyzer 1.2.0, startup transients only); multi-ASIO remains the documented
ReaRoute-client limitation (d1 starved, no hang). Latency: FINAL5 marker run
(`reports/engine/final5_markers/`) median 43.2 ms (29.7-48.3 ms) after pairing
offset alignment; the first marker burst is suppressed by the destination
connect fade and is reported as offset 1 rather than hidden. Physical/hardware
measurement remains pending.

GUI integration (G1-G4) uses this contract: legibility, collapsible left panel,
canvas<->left-panel selection sync, real sample-rate/buffer settings with
graceful stop+restart, capability validation, requested-vs-actual reporting,
per-device persistence and Diagnostics block; GUI deployment hash and
integration evidence are in the final session report/TEST_STATUS.

## Artefacts and links

* reports/engine/ENGINE_AB_REPORT.md - full A/B report (analyzer 1.2.0 note)
* reports/engine/legacy/HARNESS_ENGINE_legacy.md,
  reports/engine/engine2/HARNESS_ENGINE_engine2.md,
  reports/engine/engine2_safe/HARNESS_ENGINE_engine2_safe.md,
  reports/engine/quiet_window/{engine2,legacy}/HARNESS_ENGINE_quiet_*.md
* reports/engine/baseline/HARNESS_BASELINE.md
* reports/engine/CLOCK_ISOLATION.md + reports/engine/clock_isolation/
* da_eliminare/2026-10-27-consolidation/reports-superseded/final3_dev3/ - FINAL3 candidate batch (historical, moved out of active reports)
* da_eliminare/2026-10-27-consolidation/reports-superseded/final4_validation-FINAL4/ -
  FINAL4 targeted validation (historical; balanced x3, GUI x7, safe, late-consumer)
* da_eliminare/2026-10-27-consolidation/reports-superseded/final_acceptance-FINAL4/ -
  FINAL4 acceptance raw evidence (split-verdict table, per-scenario run dirs,
  analyzer JSON, WAVs/logs; historical, reversible archive)
* da_eliminare/2026-10-27-consolidation/reports-superseded/blocked_halfrate_5E3BEFED/ - retained half-rate attempt + FINAL2 delta semantic evidence (historical)
* reports/engine/final_acceptance_v5/FINAL_ACCEPTANCE.md + reports/engine/final5_markers/ -
  FINAL5 acceptance (safe x2, balanced, late-consumer) and marker latency evidence
* Frozen binaries (all with .sha256 in vm/baseline/):
  * NLMatrixRouter_pre_fix.exe - E05BBCFA6D9E72ED227833DA34D0969C0496BA129736629444DAC6E0BA31C9AE
  * NLMatrixRouterLegacy_ABI3.exe - E0C4553C6CF2B7411A0CBB423D349542FFEA89B9839E186F1049CE1A585A8B6B
  * NLMatrixRouter_engine_v2_FINAL.exe (9B4E1D65) - 9B4E1D652AA523B348DE629A7F1DE6F0081A4048383F1103EEE96D084388F5FB
  * NLMatrixRouter_engine_v2_ACCEPT.exe (5E3BEFED, superseded) - 5E3BEFED287963A3542055DED64CF54D2152D3EBD214ABB99BA6974822C8EFDA
  * NLMatrixRouter_engine_v2_ACCEPT2.exe (FINAL2 38B4F6C6) - 38B4F6C6205165D8615F3B1C2B8C5C61CC151C19FBFBD4A0E5A8420D2E9CBF7D
  * NLMatrixRouter_engine_v3_FINAL.exe (FINAL3 7BA50557, superseded) - 7BA5055782A60ED3A00C9B8BCCE87940B4411114B87712B52106A5911F6B393F
  * NLMatrixRouter_engine_v4_FINAL.exe (FINAL4 AB5E363F) - AB5E363F237E5D292492DCAB89988AFE0823B6BF7D0DD3C4D61EB25244DB090B
  * NLMatrixRouter_engine_v5_FINAL.exe (FINAL5 2AE82FEB, current accepted engine for GUI integration) - 2AE82FEBEF4DF4BB7DE9F7D2C149F2A28202CC7F4C73A7EC2EC58D43EEED0B86
  * NLMatrixDriverAudioTest_final.exe (frozen recorder 66DF009D) - 66DF009D67B82E48FEFED85CD7BFB398967C956483B200D396408BAA3ED9CD11
  * new recorder used only for marker runs: F9A0E61D2840CEF9FFB42B760714A75E11EFD6B1A76D79EC925D413743939599 (app/build, not frozen)
* Tools: tools/vm/Run-ContinuityTest.ps1 (steady-vs-startup kernel verdict,
  partial-writes telemetry, trimmed late-consumer analysis), Run-EngineMatrix.ps1,
  Run-FinalAcceptance.ps1, Run-ClockIsolation.ps1, make_acceptance_report.py,
  reanalyze_evidence.py, trim_wav.py; tools/analyze_continuity.py 1.2.0;
  tools/measure_engine_latency.py; tools/vm/README.md.
